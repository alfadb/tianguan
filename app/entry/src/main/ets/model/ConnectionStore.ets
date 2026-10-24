/**
 * Saved-connection persistence: the connection list (filesDir/connections.json),
 * the fingerprint trust store (filesDir/vnc-pins.json) and passwords (asset
 * key store). VNC passwords keep the historical alias "tianguan:<host>:<port>"
 * so already-saved passwords keep working; RDP passwords use the separate
 * alias "tianguan:rdp:<host>:<port>" so the same host:port cannot clash.
 *
 * Migrated from pages/Index.ets for the connection-manager UI. The old
 * connections.json entries ({name,host,port,savePassword}) are migrated in
 * place on load: id/group/lastConnectedAt get defaults; records without a
 * protocol are VNC, username/domain default to "".
 *
 * id is OPAQUE: new connections get 'c' + time + random (newConnectionId()).
 * "host:port" is only a display value and the fingerprint/password alias key,
 * never the primary key. Legacy records (no id, or id written as host:port by
 * an older build) keep the id they already have verbatim on load, because the
 * runtime session registry and the UI refer to connections by this id.
 */
import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import asset from '@ohos.security.asset';
import { util } from '@kit.ArkTS';

const DOMAIN: number = 0x0000;
const TAG: string = 'TianguanDiag';

// @selfcheck-begin connection-core
/** A record as it may appear on disk: new shape, legacy shape, or partial. */
interface StoredConnection {
  id?: string;
  name?: string;
  host?: string;
  port?: string;
  group?: string;
  savePassword?: boolean;
  noPassword?: boolean;
  lastConnectedAt?: number;
  protocol?: string;
  username?: string;
  domain?: string;
}

/**
 * A saved connection. id is opaque (see file header); "host:port" is display /
 * trust-store key only, so renaming or re-addressing a connection keeps its id.
 */
export interface ConnectionEntry {
  id: string;
  name: string;
  host: string;
  port: string;
  group: string;
  savePassword: boolean;
  /** The server accepts connections without a password (VNC security type None). */
  noPassword: boolean;
  lastConnectedAt: number;
  /** 'vnc' | 'rdp'. RDP is persistence/UI only until the native backend lands. */
  protocol: string;
  /** RDP logon user; '' for VNC. */
  username: string;
  /** RDP domain; '' when unset. */
  domain: string;
}

export const DEFAULT_PORT: string = '5901';
export const DEFAULT_RDP_PORT: string = '3389';
export const UNGROUPED: string = '未分组';

/** Port used for a protocol when the form leaves the port empty. */
export function defaultPortFor(protocol: string): string {
  return protocol === 'rdp' ? DEFAULT_RDP_PORT : DEFAULT_PORT;
}

let idSequence: number = 0;

/** New opaque connection id: 'c' + timestamp + sequence + random suffix. */
export function newConnectionId(): string {
  idSequence += 1;
  return 'c' + Date.now().toString(36) + idSequence.toString(36) +
    Math.floor(Math.random() * 46656).toString(36);
}

/** Legacy id shape (host:port). Only used to migrate id-less records. */
export function connectionIdOf(host: string, port: string): string {
  return host + ':' + port;
}

function storedPort(item: StoredConnection): string {
  return typeof item.port === 'string' && item.port.length > 0 ? item.port : DEFAULT_PORT;
}

/**
 * Parses connections.json. A record carrying "id" is new format: group and
 * lastConnectedAt are kept when legal, missing fields fall back to defaults.
 * Legacy records (no id, written by the single-session UI) are migrated:
 * id=host:port, group="", lastConnectedAt=0.
 *
 * An existing id is NEVER rewritten: legacy records keep their id verbatim so
 * historical references (runtime session -> connection) stay valid.
 */
export function parseConnectionRecords(text: string): ConnectionEntry[] {
  const result: ConnectionEntry[] = [];
  if (text.trim().length === 0) {
    return result;
  }
  let records: StoredConnection[] = [];
  records = JSON.parse(text) as StoredConnection[];
  if (!Array.isArray(records)) {
    return result;
  }
  for (let i = 0; i < records.length; i++) {
    const item = records[i];
    if (item === undefined || typeof item.host !== 'string' || item.host.length === 0) {
      continue;
    }
    const host = item.host;
    const port = storedPort(item);
    const newFormat = typeof item.id === 'string' && item.id.length > 0;
    const id = typeof item.id === 'string' && item.id.length > 0 ? item.id : connectionIdOf(host, port);
    const name = typeof item.name === 'string' && item.name.length > 0 ? item.name : host;
    const group = newFormat && typeof item.group === 'string' ? item.group : '';
    const lastConnectedAt = newFormat && typeof item.lastConnectedAt === 'number' &&
      item.lastConnectedAt >= 0 ? item.lastConnectedAt : 0;
    // Records written before RDP support have no protocol: they are VNC. Any
    // other value is treated as VNC too, so a corrupt field cannot create a
    // protocol nothing understands.
    const protocol = item.protocol === 'rdp' ? 'rdp' : 'vnc';
    const username = typeof item.username === 'string' ? item.username : '';
    const domain = typeof item.domain === 'string' ? item.domain : '';
    const entry: ConnectionEntry = {
      id: id,
      name: name,
      host: host,
      port: port,
      group: group,
      savePassword: item.savePassword === true,
      noPassword: item.noPassword === true,
      lastConnectedAt: lastConnectedAt,
      protocol: protocol,
      username: username,
      domain: domain
    };
    result.push(entry);
  }
  return result;
}
// @selfcheck-end connection-core

let context: common.UIAbilityContext | undefined = undefined;
let connections: ConnectionEntry[] = [];

export function initConnectionStore(abilityContext: common.UIAbilityContext): void {
  context = abilityContext;
  connections = loadConnections();
}

export function savedConnections(): ConnectionEntry[] {
  return connections;
}

export function findConnection(id: string): ConnectionEntry | undefined {
  for (let i = 0; i < connections.length; i++) {
    if (connections[i].id === id) {
      return connections[i];
    }
  }
  return undefined;
}

function filesDir(): string {
  const ctx = context;
  if (ctx === undefined) {
    throw new Error('ConnectionStore 未初始化');
  }
  return ctx.filesDir;
}

function readText(name: string): string {
  try {
    const path = filesDir() + '/' + name;
    if (!fs.accessSync(path)) {
      return '';
    }
    return fs.readTextSync(path);
  } catch (error) {
    return '';
  }
}

function writeText(name: string, text: string): void {
  const file = fs.openSync(filesDir() + '/' + name,
    fs.OpenMode.CREATE | fs.OpenMode.READ_WRITE | fs.OpenMode.TRUNC);
  try {
    fs.writeSync(file.fd, text);
  } finally {
    fs.closeSync(file);
  }
}

function loadConnections(): ConnectionEntry[] {
  const text = readText('connections.json');
  if (text.trim().length === 0) {
    return [];
  }
  try {
    // Accept both the new shape and the legacy one so the upgrade is silent.
    return parseConnectionRecords(text);
  } catch (error) {
    hilog.error(DOMAIN, TAG, '连接列表解析失败：%{public}s', String(error));
    return [];
  }
}

function sameConnection(a: ConnectionEntry, b: ConnectionEntry): boolean {
  return a.id === b.id && a.name === b.name && a.host === b.host && a.port === b.port &&
    a.group === b.group && a.savePassword === b.savePassword &&
    a.noPassword === b.noPassword && a.lastConnectedAt === b.lastConnectedAt &&
    a.protocol === b.protocol && a.username === b.username && a.domain === b.domain;
}

function connectionsRoundTripEqual(expected: ConnectionEntry[],
  actual: ConnectionEntry[]): boolean {
  if (expected.length !== actual.length) {
    return false;
  }
  for (let i = 0; i < expected.length; i++) {
    if (!sameConnection(expected[i], actual[i])) {
      return false;
    }
  }
  return true;
}

function persistConnections(): void {
  try {
    writeText('connections.json', JSON.stringify(connections));
    // Self-check: read the file back so a field lost by save/parse is reported
    // instead of silently disappearing on the next restart.
    const reloaded = loadConnections();
    if (!connectionsRoundTripEqual(connections, reloaded)) {
      hilog.error(DOMAIN, TAG,
        '连接列表保存后回读自检失败：内存 %{public}d 条，读回 %{public}d 条，group/lastConnectedAt/protocol 等字段可能丢失',
        connections.length, reloaded.length);
    }
  } catch (error) {
    hilog.error(DOMAIN, TAG, '连接列表保存失败：%{public}s', String(error));
  }
}

function normalizePort(port: string, protocol: string): string {
  const trimmed = port.trim();
  return trimmed.length > 0 ? trimmed : defaultPortFor(protocol);
}

/**
 * Creates or updates a connection. Matching is by the incoming opaque id: the
 * same id replaces that record (even if host/port/name changed), a different
 * id is appended as a new record. The id is never derived from host:port.
 * Returns the stored entry.
 */
export function upsertConnection(entry: ConnectionEntry): ConnectionEntry {
  const host = entry.host.trim();
  const protocol = entry.protocol === 'rdp' ? 'rdp' : 'vnc';
  const port = normalizePort(entry.port, protocol);
  const id = typeof entry.id === 'string' && entry.id.length > 0 ? entry.id : newConnectionId();
  const noPassword = entry.noPassword === true;
  // username/domain only mean something for RDP; a VNC record keeps them empty.
  const username = protocol === 'rdp' && typeof entry.username === 'string' ?
    entry.username.trim() : '';
  const domain = protocol === 'rdp' && typeof entry.domain === 'string' ?
    entry.domain.trim() : '';
  const normalized: ConnectionEntry = {
    id: id,
    name: entry.name.trim().length > 0 ? entry.name.trim() : host,
    host: host,
    port: port,
    group: entry.group.trim(),
    savePassword: noPassword ? false : entry.savePassword,
    noPassword: noPassword,
    lastConnectedAt: entry.lastConnectedAt,
    protocol: protocol,
    username: username,
    domain: domain
  };
  const kept: ConnectionEntry[] = [];
  let replaced = false;
  for (let i = 0; i < connections.length; i++) {
    if (connections[i].id === id) {
      kept.push(normalized);
      replaced = true;
    } else {
      kept.push(connections[i]);
    }
  }
  if (!replaced) {
    kept.push(normalized);
  }
  connections = kept;
  persistConnections();
  return normalized;
}

export function removeConnection(id: string): void {
  connections = connections.filter((item: ConnectionEntry) => item.id !== id);
  persistConnections();
}

export function markConnected(id: string): void {
  const entry = findConnection(id);
  if (entry === undefined) {
    return;
  }
  const now = Date.now();
  // Coalesce: only rewrite storage when the minute changes.
  if (now - entry.lastConnectedAt > 60000) {
    entry.lastConnectedAt = now;
    persistConnections();
  } else {
    entry.lastConnectedAt = now;
  }
}

/** Distinct groups (non-empty), plus '' meaning ungrouped. */
export function distinctGroups(): string[] {
  const seen = new Set<string>();
  for (let i = 0; i < connections.length; i++) {
    const group = connections[i].group;
    if (group.length > 0) {
      seen.add(group);
    }
  }
  return Array.from(seen);
}

// ---------------------------------------------------------------------------
// Fingerprint trust store.
// ---------------------------------------------------------------------------

export function readTrustedFingerprint(endpoint: string): string {
  try {
    const text = readText('vnc-pins.json');
    if (text.trim().length === 0) {
      return '';
    }
    const pins = JSON.parse(text) as Record<string, string>;
    const pin = pins[endpoint];
    return pin === undefined ? '' : pin;
  } catch (error) {
    return '';
  }
}

export function trustFingerprint(endpoint: string, fingerprint: string): void {
  try {
    let pins: Record<string, string> = {};
    const text = readText('vnc-pins.json');
    if (text.trim().length > 0) {
      pins = JSON.parse(text) as Record<string, string>;
    }
    pins[endpoint] = fingerprint;
    writeText('vnc-pins.json', JSON.stringify(pins));
  } catch (error) {
    hilog.error(DOMAIN, TAG, '保存指纹失败：%{public}s', String(error));
  }
}

// ---------------------------------------------------------------------------
// Passwords in the asset key store.
// ---------------------------------------------------------------------------

function toBytes(text: string): Uint8Array {
  return new util.TextEncoder().encodeInto(text);
}

function fromBytes(bytes: Uint8Array | undefined): string {
  if (bytes === undefined) {
    return '';
  }
  return util.TextDecoder.create('utf-8').decodeWithStream(bytes);
}

function passwordAlias(entry: ConnectionEntry): string {
  const hostPort = entry.host + ':' + entry.port;
  // VNC deliberately keeps the legacy alias: passwords saved by older builds
  // must still load. RDP gets its own namespace so a VNC and an RDP connection
  // on the same host:port cannot overwrite each other's password.
  return entry.protocol === 'rdp' ? 'tianguan:rdp:' + hostPort : 'tianguan:' + hostPort;
}

function aliasQuery(entry: ConnectionEntry): asset.AssetMap {
  const query: asset.AssetMap = new Map();
  query.set(asset.Tag.ALIAS, toBytes(passwordAlias(entry)));
  return query;
}

function loadQuery(entry: ConnectionEntry): asset.AssetMap {
  const query = aliasQuery(entry);
  // Without RETURN_TYPE=ALL a query returns only the queried attributes, so
  // the secret would come back missing.
  query.set(asset.Tag.RETURN_TYPE, asset.ReturnType.ALL);
  return query;
}

export async function savePassword(entry: ConnectionEntry, secret: string): Promise<boolean> {
  try {
    try {
      await asset.remove(aliasQuery(entry));
    } catch (error) {
      // No previous entry is the normal first-save case.
    }
    const map: asset.AssetMap = new Map();
    map.set(asset.Tag.ALIAS, toBytes(passwordAlias(entry)));
    map.set(asset.Tag.SECRET, toBytes(secret));
    map.set(asset.Tag.ACCESSIBILITY, asset.Accessibility.DEVICE_FIRST_UNLOCKED);
    await asset.add(map);
    return true;
  } catch (error) {
    hilog.error(DOMAIN, TAG, '保存密码失败：%{public}s', String(error));
    return false;
  }
}

export async function loadPassword(entry: ConnectionEntry): Promise<string> {
  try {
    const rows: asset.AssetMap[] = await asset.query(loadQuery(entry));
    if (rows.length === 0) {
      return '';
    }
    return fromBytes(rows[0].get(asset.Tag.SECRET) as Uint8Array | undefined);
  } catch (error) {
    hilog.error(DOMAIN, TAG, '读取密码失败：%{public}s', String(error));
    return '';
  }
}

export async function removePassword(entry: ConnectionEntry): Promise<void> {
  try {
    await asset.remove(aliasQuery(entry));
  } catch (error) {
    // No saved entry (asset NOT_FOUND, 24000002) is normal: nothing to delete.
    if ((error as BusinessError).code === asset.ErrorCode.NOT_FOUND) {
      return;
    }
    hilog.error(DOMAIN, TAG, '删除密码失败：%{public}s', String(error));
  }
}
