/**
 * Pure codec for saved connections: connections.json parsing/migration and
 * serialization, id helpers, and the per-protocol key namespaces shared by the
 * fingerprint trust store and the password alias.
 *
 * This file is intentionally plain TypeScript: it must not import any @kit /
 * @ohos SDK module, so the host-side unit test
 * (app/tests/arkts/connection_codec.test.mjs) can compile it with tsc and run
 * it under Node without a device or the ArkTS runtime. ConnectionStore.ets
 * re-exports everything here so existing imports keep working.
 */

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
 * A saved connection. id is opaque (see ConnectionStore.ets); "host:port" is
 * display / trust-store key only, so renaming or re-addressing a connection
 * keeps its id.
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
  /** 'vnc' | 'rdp'. RDP is persistence/UI plus a separate key namespace. */
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
    // JSON arrays may legally hold null, numbers, strings or booleans. Reading
    // item.host on those (null/undefined in particular) throws, and the caller
    // used to drop the WHOLE list on any error, so one bad entry erased every
    // connection. Non-object entries are not connection records: skip them.
    if (item === null || typeof item !== 'object') {
      continue;
    }
    if (typeof item.host !== 'string' || item.host.length === 0) {
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

/** Serializes the connection list exactly as connections.json is written. */
export function serializeConnectionRecords(entries: ConnectionEntry[]): string {
  return JSON.stringify(entries);
}

/**
 * Key of one connection's pin in vnc-pins.json. VNC keeps its historical
 * "host:port" key so fingerprints confirmed by older builds stay valid; RDP
 * uses its own "rdp:host:port" namespace so the two protocols can never
 * overwrite each other's trust anchor on the same address.
 */
export function pinKeyFor(entry: ConnectionEntry): string {
  const hostPort = entry.host + ':' + entry.port;
  return entry.protocol === 'rdp' ? 'rdp:' + hostPort : hostPort;
}

/**
 * Key-store alias of one connection's password. VNC deliberately keeps the
 * legacy alias "tianguan:host:port" so passwords saved by older builds still
 * load; RDP gets "tianguan:rdp:host:port" so the two protocols cannot
 * overwrite each other's secret for the same host:port.
 */
export function passwordAliasFor(entry: ConnectionEntry): string {
  const hostPort = entry.host + ':' + entry.port;
  return entry.protocol === 'rdp' ? 'tianguan:rdp:' + hostPort : 'tianguan:' + hostPort;
}
// @selfcheck-end connection-core
