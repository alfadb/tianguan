/**
 * The ONE wrapper every page and service uses to talk to libtianguan_diag.
 *
 * The native side is being reworked in parallel against docs/napi-contract.md
 * (multi-session + clipboard), so every call here is wrapped in try/catch: a
 * runtime that does not yet export the new symbols produces a safe sentinel and
 * one hilog line instead of a UI crash. Status strings are parsed as JSON into
 * the interfaces below — the UI never parses the legacy "key=value" text.
 */
import diag from 'libtianguan_diag.so';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN: number = 0x0000;
const TAG: string = 'TianguanDiag';

/** optionsJson for createSession (docs/napi-contract.md §会话生命周期). */
export interface SessionOptions {
  host: string;
  port: number;
  password?: string;
  expectedFingerprint?: string;
  name?: string;
}

/** cursor object inside the status JSON. */
export interface CursorInfo {
  w: number;
  h: number;
  hotX: number;
  hotY: number;
  blank: boolean;
  version: number;
}

/** clipboard object inside the status JSON. */
export interface ClipboardInfo {
  version: number;
}

/** sessionStatus(sessionId) JSON (docs/napi-contract.md §状态 JSON). */
export interface SessionStatus {
  id: string;
  state: string;
  error: string;
  errorCode: string;
  width: number;
  height: number;
  generation: number;
  cursor?: CursorInfo;
  clipboard?: ClipboardInfo;
  retryInMs?: number;
  fingerprint?: string;
  pixelBytes?: number;
  clipboardTruncated?: boolean;
}

/** takeFrame metadata JSON (docs/napi-contract.md §画面). */
export interface FrameMeta {
  x: number;
  y: number;
  w: number;
  h: number;
  version: number;
  generation: number;
  full: boolean;
}

/** One entry of the listSessions JSON array. */
export interface ListedSession {
  id: string;
  state: string;
  width: number;
  height: number;
  generation: number;
  profileName: string;
  /** Address the session is currently connecting to; '' when unresolved. */
  host: string;
  /** Port the session is currently connecting to; 0 when unresolved. */
  port: number;
}

export const SESSION_STATE_CONNECTING: string = 'connecting';
export const SESSION_STATE_CONNECTED: string = 'connected';
export const SESSION_STATE_DISCONNECTED: string = 'disconnected';
export const SESSION_STATE_FAILED: string = 'failed';
export const SESSION_STATE_TRUST_REQUIRED: string = 'trust-required';
export const SESSION_STATE_RECONNECTING: string = 'reconnecting';

export function buildOptionsJson(options: SessionOptions): string {
  return JSON.stringify(options);
}

export function parseSessionStatus(text: string): SessionStatus | undefined {
  try {
    const parsed = JSON.parse(text) as SessionStatus;
    if (typeof parsed.id !== 'string' || typeof parsed.state !== 'string') {
      return undefined;
    }
    return parsed;
  } catch (error) {
    return undefined;
  }
}

export function parseFrameMeta(text: string): FrameMeta | undefined {
  try {
    const parsed = JSON.parse(text) as FrameMeta;
    if (typeof parsed.w !== 'number' || typeof parsed.h !== 'number') {
      return undefined;
    }
    return parsed;
  } catch (error) {
    return undefined;
  }
}

/**
 * One listSessions entry, or undefined when the native side left a required
 * field out: callers must never read e.g. profileName.length off a partial
 * object (F30), and a malformed entry must not poison the whole list.
 */
function listedSessionFrom(value: ListedSession): ListedSession | undefined {
  if (typeof value.id !== 'string' || value.id.length === 0) {
    return undefined;
  }
  if (typeof value.state !== 'string' || value.state.length === 0) {
    return undefined;
  }
  if (typeof value.width !== 'number' || typeof value.height !== 'number') {
    return undefined;
  }
  if (typeof value.generation !== 'number') {
    return undefined;
  }
  if (typeof value.profileName !== 'string') {
    return undefined;
  }
  // host/port are additive: an older native library that predates them must
  // still yield a usable row (empty endpoint, never a parse failure).
  const host: string = typeof value.host === 'string' ? value.host : '';
  const port: number = typeof value.port === 'number' ? value.port : 0;
  return {
    id: value.id,
    state: value.state,
    width: value.width,
    height: value.height,
    generation: value.generation,
    profileName: value.profileName,
    host: host,
    port: port
  };
}

/**
 * Parses the listSessions JSON array. Returns undefined when the text is not a
 * JSON array at all (the caller cannot tell "empty list" from "native failed"
 * otherwise); individual malformed entries are skipped with one warn each.
 */
export function parseListedSessions(text: string): ListedSession[] | undefined {
  let parsed: ListedSession[] | null = null;
  try {
    parsed = JSON.parse(text) as ListedSession[] | null;
  } catch (error) {
    warn('listSessions.parse', error);
    return undefined;
  }
  if (parsed === null || !Array.isArray(parsed)) {
    hilog.warn(DOMAIN, TAG, 'session-api listSessions 解析失败：不是 JSON 数组');
    return undefined;
  }
  const result: ListedSession[] = [];
  for (let i = 0; i < parsed.length; i++) {
    const raw: ListedSession | null = parsed[i] as ListedSession | null;
    const item: ListedSession | undefined = raw === null ? undefined : listedSessionFrom(raw);
    if (item === undefined) {
      hilog.warn(DOMAIN, TAG, 'session-api listSessions 跳过无效条目 index=%{public}d', i);
      continue;
    }
    result.push(item);
  }
  if (result.length === 0 && parsed.length > 0) {
    // A non-empty array in which EVERY entry was unusable is a bad read, not
    // an empty session table; report failure so nobody wipes the table (F31).
    hilog.warn(DOMAIN, TAG, 'session-api listSessions 全部条目无效，按读取失败处理');
    return undefined;
  }
  return result;
}

function warn(action: string, error: Object): void {
  hilog.warn(DOMAIN, TAG, 'session-api %{public}s 失败：%{public}s', action, String(error));
}

/**
 * One-shot probe for the multi-session surface. With the legacy .so the new
 * symbol is missing and this throws; the answer is cached so the probe itself
 * costs one call per process.
 */
let multiSessionProbeDone: boolean = false;
let multiSessionAvailable: boolean = false;

export function diagMultiSessionAvailable(): boolean {
  if (multiSessionProbeDone) {
    return multiSessionAvailable;
  }
  try {
    const text = diag.listSessions();
    multiSessionAvailable = typeof text === 'string';
  } catch (error) {
    multiSessionAvailable = false;
    hilog.warn(DOMAIN, TAG, '多会话接口不可用（原生层尚未升级）：%{public}s', String(error));
  }
  multiSessionProbeDone = true;
  return multiSessionAvailable;
}

/** Creates a session; returns '' when the call failed. */
export function createSession(options: SessionOptions): string {
  try {
    const id = diag.createSession(buildOptionsJson(options));
    return typeof id === 'string' ? id : '';
  } catch (error) {
    warn('createSession', error);
    return '';
  }
}

/** Idempotent close per contract. */
export function closeSession(sessionId: string): void {
  try {
    diag.closeSession(sessionId);
  } catch (error) {
    // Closing a session that already vanished is expected; stay quiet.
  }
}

/**
 * Native session table. undefined means "the read failed" and MUST NOT be
 * treated as an empty table: callers would wipe every known session (F31).
 */
export function listSessions(): ListedSession[] | undefined {
  try {
    const text = diag.listSessions();
    if (typeof text !== 'string') {
      hilog.warn(DOMAIN, TAG, 'session-api listSessions 返回非字符串');
      return undefined;
    }
    return parseListedSessions(text);
  } catch (error) {
    warn('listSessions', error);
    return undefined;
  }
}

export function sessionStatus(sessionId: string): SessionStatus | undefined {
  try {
    return parseSessionStatus(diag.sessionStatus(sessionId));
  } catch (error) {
    warn('sessionStatus', error);
    return undefined;
  }
}

export function reconnectSession(sessionId: string): void {
  try {
    diag.reconnectSession(sessionId);
  } catch (error) {
    warn('reconnectSession', error);
  }
}

/** Raw takeFrame result: 'NONE', 'ERROR:…', or frame metadata JSON. */
export function takeFrameRaw(sessionId: string, buffer: ArrayBuffer, afterVersion: number,
  forceFull: boolean): string {
  try {
    return diag.takeFrame(sessionId, buffer, afterVersion, forceFull);
  } catch (error) {
    warn('takeFrame', error);
    return 'NONE';
  }
}

/**
 * Asks for one non-incremental (whole-screen) framebuffer update. The local
 * forceFull flag alone cannot wake an idle desktop, so a freshly mounted or
 * resized Canvas needs this real request.
 */
export function requestFullFrame(sessionId: string): string {
  try {
    return diag.requestFullFrame(sessionId);
  } catch (error) {
    warn('requestFullFrame', error);
    return '';
  }
}

export function sendPointer(sessionId: string, x: number, y: number, mask: number): string {
  try {
    return diag.sendPointer(sessionId, x, y, mask);
  } catch (error) {
    warn('sendPointer', error);
    return 'INVALID_SESSION';
  }
}

export function sendKey(sessionId: string, keysym: number, pressed: number): string {
  try {
    return diag.sendKey(sessionId, keysym, pressed);
  } catch (error) {
    warn('sendKey', error);
    return 'INVALID_SESSION';
  }
}

export function releaseSessionInput(sessionId: string): void {
  try {
    diag.releaseSessionInput(sessionId);
  } catch (error) {
    warn('releaseSessionInput', error);
  }
}

/** Sends local text as ClientCutText; returns the native reply or ''. */
export function setClipboard(sessionId: string, text: string): string {
  try {
    return diag.setClipboard(sessionId, text);
  } catch (error) {
    warn('setClipboard', error);
    return '';
  }
}

/**
 * Takes (and clears) the pending ServerCutText; null when the read failed.
 *
 * An empty string means "nothing new on the native side" and may be consumed
 * as such, while null MUST NOT advance the clipboard version: the caller has to
 * keep the version and retry, or a one-time server cut is lost for good.
 */
export function takeClipboardOrNull(sessionId: string): string | null {
  try {
    const text = diag.takeClipboard(sessionId);
    return typeof text === 'string' ? text : null;
  } catch (error) {
    warn('takeClipboard', error);
    return null;
  }
}

/** Legacy form: '' for both "nothing new" and "read failed"; prefer the above. */
export function takeClipboard(sessionId: string): string {
  const text = takeClipboardOrNull(sessionId);
  return text === null ? '' : text;
}

/**
 * Cursor pixel fetch for this session; the pixels land in buffer as RGBA_8888.
 *
 * Fetch once per cursor.version from sessionStatus: call refreshCursorShape only
 * when the version changes, since the bytes are identical otherwise. Native
 * replies "OK <bytes>"; a session without a cursor image returns "OK 0".
 */
export function copyCursor(sessionId: string, buffer: ArrayBuffer): string {
  try {
    return diag.copyCursor(sessionId, buffer);
  } catch (error) {
    warn('copyCursor', error);
    return '';
  }
}
