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
  sunshineApp?: string;
  sunshinePreset?: string;
  sunshineWidth?: number;
  sunshineHeight?: number;
  sunshineFps?: number;
  sunshineBitrate?: number;
  sunshineCodec?: string;
  sunshineLocalCursor?: boolean;
  sunshineHostCursorHidden?: boolean;
  sunshineFollowWindow?: boolean;
  sunshineVsync?: boolean;
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
  /** 'vnc' | 'rdp' | 'sunshine'; each protocol has its own key namespace. */
  protocol: string;
  /** RDP logon user; '' for VNC and Sunshine. */
  username: string;
  /** RDP domain; '' when unset (and always '' for VNC/Sunshine). */
  domain: string;
  /**
   * Sunshine only: /applist title to launch. '' means the native default
   * ("Desktop"); the native side may launch another app and report it in the
   * status JSON's `app`.
   */
  sunshineApp: string;
  /**
   * Sunshine quality preset: 'smooth' | 'balanced' | 'hd' | 'uhd' | 'custom'.
   * Missing or unknown on disk falls back to 'balanced' (records written before
   * the quality setting existed).
   */
  sunshinePreset?: string;
  /**
   * Sunshine custom quality, used only when sunshinePreset==='custom'.
   * sunshineBitrate is in Mbps — the unit the form shows; connect() converts it
   * to the kbps the native options expect.
   */
  sunshineWidth?: number;
  sunshineHeight?: number;
  sunshineFps?: number;
  sunshineBitrate?: number;
  /** Sunshine video codec: 'auto' (native picks) | 'h264' | 'hevc'. */
  sunshineCodec?: string;
  /**
   * Sunshine only: experimental Moonlight-style local cursor. The host hides its
   * own cursor and this client shows the system pointer, which follows the hand
   * with zero latency but always looks like an arrow. Default false — the remote
   * cursor is shown unless this is explicitly true (records written before this
   * setting existed also get false).
   */
  sunshineLocalCursor?: boolean;
  /**
   * Sunshine only: whether the HOST cursor is currently hidden. The host state
   * is host-wide and survives this app, so it is recorded here to avoid needless
   * toggles on the next connection; the session status's remoteCursorHidden is
   * authoritative and overwrites it whenever it changes.
   */
  sunshineHostCursorHidden?: boolean;
  /**
   * Sunshine only: follow the window size (drag / maximize / fullscreen) by
   * picking the best host mode and restarting the stream. Default true (old
   * records too). false keeps the stream at the configured tier / custom size
   * and the remote page never calls requestDesktopSize for this connection.
   */
  sunshineFollowWindow?: boolean;
  /**
   * Sunshine only: true (default) waits for vertical sync; false disables vsync
   * for the Sunshine zero-copy presentation only (lower latency, may tear).
   * VNC/RDP are never affected.
   */
  sunshineVsync?: boolean;
}

/**
 * One Sunshine quality tier. The numbers live here only, so tuning a tier is a
 * single edit; sunshinePresetFor() is what the connection form and connect()
 * both read. bitrateMbps is the form-facing unit (native wants kbps).
 */
export interface SunshinePreset {
  id: string;
  label: string;
  width: number;
  height: number;
  fps: number;
  bitrateMbps: number;
}

export const SUNSHINE_PRESET_SMOOTH: string = 'smooth';
export const SUNSHINE_PRESET_BALANCED: string = 'balanced';
export const SUNSHINE_PRESET_HD: string = 'hd';
export const SUNSHINE_PRESET_UHD: string = 'uhd';
export const SUNSHINE_PRESET_CUSTOM: string = 'custom';

export const SUNSHINE_CODEC_AUTO: string = 'auto';
export const SUNSHINE_CODEC_H264: string = 'h264';
export const SUNSHINE_CODEC_HEVC: string = 'hevc';

/** The agreed Sunshine quality tiers (流畅/均衡/高清/超清). */
export const SUNSHINE_PRESETS: SunshinePreset[] = [
  { id: SUNSHINE_PRESET_SMOOTH, label: '流畅', width: 1280, height: 720, fps: 60, bitrateMbps: 15 },
  { id: SUNSHINE_PRESET_BALANCED, label: '均衡', width: 1920, height: 1080, fps: 60, bitrateMbps: 20 },
  { id: SUNSHINE_PRESET_HD, label: '高清', width: 2560, height: 1440, fps: 60, bitrateMbps: 40 },
  { id: SUNSHINE_PRESET_UHD, label: '超清', width: 3840, height: 2160, fps: 60, bitrateMbps: 80 }
];

/** 'custom' has no tier numbers of its own: it starts from 均衡. */
export function sunshinePresetFor(id: string | undefined): SunshinePreset {
  for (let i = 0; i < SUNSHINE_PRESETS.length; i++) {
    if (SUNSHINE_PRESETS[i].id === id) {
      return SUNSHINE_PRESETS[i];
    }
  }
  return SUNSHINE_PRESETS[1];
}

export function isSunshinePreset(id: string | undefined): boolean {
  if (typeof id !== 'string') {
    return false;
  }
  if (id === SUNSHINE_PRESET_CUSTOM) {
    return true;
  }
  for (let i = 0; i < SUNSHINE_PRESETS.length; i++) {
    if (SUNSHINE_PRESETS[i].id === id) {
      return true;
    }
  }
  return false;
}

export function isSunshineCodec(codec: string | undefined): boolean {
  return codec === SUNSHINE_CODEC_AUTO || codec === SUNSHINE_CODEC_H264 ||
    codec === SUNSHINE_CODEC_HEVC;
}

// Native limits (parseSessionOptions: width/height 1..4096, fps 1..480,
// bitrate 1..200000 kbps = 1..200 Mbps).
export function isValidSunshineSize(value: number): boolean {
  return Number.isInteger(value) && value >= 1 && value <= 4096;
}

export function isValidSunshineFps(value: number): boolean {
  return Number.isInteger(value) && value >= 1 && value <= 480;
}

export function isValidSunshineBitrateMbps(value: number): boolean {
  return Number.isInteger(value) && value >= 1 && value <= 200;
}

function boundedInt(value: number | undefined, valid: (value: number) => boolean,
  fallback: number): number {
  if (value === undefined || !valid(value)) {
    return fallback;
  }
  return value;
}

/** Fully resolved Sunshine quality (every protocol stores one; defaults for VNC/RDP). */
export interface SunshineQuality {
  preset: string;
  width: number;
  height: number;
  fps: number;
  bitrateMbps: number;
  codec: string;
}

/**
 * Resolves stored quality fields into valid values. Missing/invalid numbers
 * fall back to the record's own tier (balanced for older records), so an old
 * connections.json loads as 均衡 and a corrupt field cannot make createSession
 * fail with "invalid sunshine width".
 */
export function normalizeSunshineQuality(preset: string | undefined,
  width: number | undefined, height: number | undefined, fps: number | undefined,
  bitrateMbps: number | undefined, codec: string | undefined): SunshineQuality {
  const id = isSunshinePreset(preset) && typeof preset === 'string' ?
    preset : SUNSHINE_PRESET_BALANCED;
  const tier = sunshinePresetFor(id);
  return {
    preset: id,
    width: boundedInt(width, isValidSunshineSize, tier.width),
    height: boundedInt(height, isValidSunshineSize, tier.height),
    fps: boundedInt(fps, isValidSunshineFps, tier.fps),
    bitrateMbps: boundedInt(bitrateMbps, isValidSunshineBitrateMbps, tier.bitrateMbps),
    codec: isSunshineCodec(codec) && typeof codec === 'string' ? codec : SUNSHINE_CODEC_AUTO
  };
}

/**
 * Native createSession options `sunshineFormats` for a codec choice: 'auto'
 * sends nothing (native picks H.264 plus HEVC when the device decodes it),
 * H.264 sends 'h264', HEVC sends 'h264,hevc' so the server prefers HEVC and the
 * session still falls back to H.264 when HEVC is unavailable.
 */
export function sunshineFormatsFor(codec: string | undefined): string {
  if (codec === SUNSHINE_CODEC_H264) {
    return 'h264';
  }
  return codec === SUNSHINE_CODEC_HEVC ? 'h264,hevc' : '';
}

export const DEFAULT_PORT: string = '5901';
export const DEFAULT_RDP_PORT: string = '3389';
/** Sunshine/Moonlight plaintext HTTP (nvhttp) port; HTTPS is port - 5 (47984). */
export const DEFAULT_SUNSHINE_PORT: string = '47989';
export const UNGROUPED: string = '未分组';

/** Port used for a protocol when the form leaves the port empty. */
export function defaultPortFor(protocol: string): string {
  if (protocol === 'rdp') {
    return DEFAULT_RDP_PORT;
  }
  return protocol === 'sunshine' ? DEFAULT_SUNSHINE_PORT : DEFAULT_PORT;
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
    // unknown value is treated as VNC too, so a corrupt field cannot create a
    // protocol nothing understands. Only these exact strings select a backend.
    const protocol = item.protocol === 'rdp' ? 'rdp' :
      (item.protocol === 'sunshine' ? 'sunshine' : 'vnc');
    const username = typeof item.username === 'string' ? item.username : '';
    const domain = typeof item.domain === 'string' ? item.domain : '';
    const sunshineApp = typeof item.sunshineApp === 'string' ? item.sunshineApp : '';
    // Quality fields are optional on disk: an old record (no preset) resolves to
    // 均衡 and the numbers follow that tier, so nothing has to be migrated.
    const quality = normalizeSunshineQuality(item.sunshinePreset, item.sunshineWidth,
      item.sunshineHeight, item.sunshineFps, item.sunshineBitrate, item.sunshineCodec);
    // Local cursor is OFF by default for every record (including old ones): the
    // remote cursor is shown unless it was explicitly enabled. The recorded host
    // state defaults to shown.
    const sunshineLocalCursor = item.sunshineLocalCursor === true;
    const sunshineHostCursorHidden = item.sunshineHostCursorHidden === true;
    // Both behaviour switches default to ON; only a literal false turns them off.
    const sunshineFollowWindow = item.sunshineFollowWindow !== false;
    const sunshineVsync = item.sunshineVsync !== false;
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
      domain: domain,
      sunshineApp: sunshineApp,
      sunshinePreset: quality.preset,
      sunshineWidth: quality.width,
      sunshineHeight: quality.height,
      sunshineFps: quality.fps,
      sunshineBitrate: quality.bitrateMbps,
      sunshineCodec: quality.codec,
      sunshineLocalCursor: sunshineLocalCursor,
      sunshineHostCursorHidden: sunshineHostCursorHidden,
      sunshineFollowWindow: sunshineFollowWindow,
      sunshineVsync: sunshineVsync
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
 *
 * Sunshine MUST NOT use this store at all: its server certificate and pin are
 * persisted by the native backend inside sunshineStateDir, so its key is a
 * separate "sunshine:" namespace purely as a guard — even a code path that
 * forgets to branch on the protocol can never read or overwrite a VNC/RDP
 * trust anchor (pinKeyFor is only called after that branch in pages/Index.ets).
 */
export function pinKeyFor(entry: ConnectionEntry): string {
  const hostPort = entry.host + ':' + entry.port;
  if (entry.protocol === 'rdp') {
    return 'rdp:' + hostPort;
  }
  return entry.protocol === 'sunshine' ? 'sunshine:' + hostPort : hostPort;
}

/**
 * Key-store alias of one connection's password. VNC deliberately keeps the
 * legacy alias "tianguan:host:port" so passwords saved by older builds still
 * load; RDP gets "tianguan:rdp:host:port" so the two protocols cannot
 * overwrite each other's secret for the same host:port.
 *
 * Sunshine has no password (the server never asks for one) and its own
 * namespace, so the form never saves an alias for it and a Sunshine record can
 * never delete or overwrite a VNC/RDP secret.
 */
export function passwordAliasFor(entry: ConnectionEntry): string {
  const hostPort = entry.host + ':' + entry.port;
  if (entry.protocol === 'rdp') {
    return 'tianguan:rdp:' + hostPort;
  }
  return entry.protocol === 'sunshine' ?
    'tianguan:sunshine:' + hostPort : 'tianguan:' + hostPort;
}
// @selfcheck-end connection-core
