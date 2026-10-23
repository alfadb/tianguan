/**
 * Bidirectional automatic clipboard sync, per docs/napi-contract.md §剪贴板:
 *   remote ServerCutText  -> local system pasteboard (per-session
 *   clipboardVersion from the status JSON; text taken once per version);
 *   local pasteboard copy -> every session via setClipboard (ClientCutText).
 *
 * Local -> remote is EVENT DRIVEN, never polled with a blocking read. The
 * system pasteboard's 'update' listener only sets a flag and starts ONE
 * asynchronous getData() promise; the compare, the fan-out and the loop guard
 * run when that promise settles. On the diagnostic device getDataSync() blocked
 * the UI thread for ~104 ms per call, which the user saw as a stutter twice a
 * second. An 'update' that arrives while a read is in flight is coalesced into
 * exactly one follow-up read (LocalReadGate). The listener is registered while
 * a session page is visible and removed with the same callback reference on
 * hide/destroy. The first read after the module starts only establishes the
 * baseline (the clipboard's existing content is not pushed to the sessions).
 * If on('update') cannot be registered (unavailable API or ROM), the module
 * falls back to a 500 ms poll that still uses only getData() promises.
 *
 * Loop prevention: every write we make to the system pasteboard is recorded in
 * ClipboardLoopGuard BEFORE setData is called. Whatever the next read returns
 * (an update event or the fallback poll) is classified against that record, so a
 * write that is still in flight (or whose promise resolves after the event) can
 * never be mistaken for a user copy and echoed to the sessions.
 *
 * Ordering: every pasteboard write (remote -> local setData) and every session
 * broadcast (local -> remote setClipboard) runs through one promise-chain queue,
 * so the two directions cannot interleave.
 *
 * Delivery: success is tracked per session (deliveredRevs), never as one global
 * "lastSyncedText". A session that was disconnected while a local text was
 * broadcast gets it on a later read triggered by its own registration, or by the
 * demand-driven retry timer (which never reads the pasteboard); only
 * QUEUED/COALESCED counts as delivered.
 *
 * Remote -> local failures (including a failed takeClipboard read) keep the
 * pending entry for the same demand-driven retry; a version is marked consumed
 * and its entry removed only conditionally, so a newer cut that arrived while a
 * write was in flight is kept and applied immediately instead of being lost.
 * Text above 1 MiB is truncated (UTF-8 safe) and logged plus exposed in
 * clipboardSyncInfo().
 *
 * Text only; \r\n normalized to \n both ways. Failures are logged (a background
 * app cannot read the pasteboard without a system-level permission, so a hidden
 * window simply pauses the local->remote direction until it is visible again —
 * the remote->local writes keep working).
 *
 * Permission: reading the system pasteboard needs
 * ohos.permission.READ_PASTEBOARD (system_basic / user_grant). It is granted at
 * runtime through a dialog and also needs an ACL entry in the signing profile;
 * while the profile ACL is empty the declaration is intentionally commented out
 * in module.json5 (a HAP declaring it fails to INSTALL with 9568289), so restore
 * the declaration together with the ACL and re-sign. Until the permission is
 * granted the module does NOT read at all (no update-driven getData, no fallback
 * poll): every rejected read is still a ~100 ms cross-process call. The dialog
 * is requested at most once per process (clipboardRequestReadPermission) and the
 * read path stays gated off after a denial or a failed request. Writing the
 * pasteboard (remote -> local) needs no permission and keeps working either way.
 */
import { pasteboard } from '@kit.BasicServicesKit';
import { systemDateTime } from '@kit.BasicServicesKit';
import { abilityAccessCtrl, bundleManager, common, Permissions,
  PermissionRequestResult } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { setClipboard, takeClipboardOrNull } from '../diag/SessionApi';

const DOMAIN: number = 0x0000;
const TAG: string = 'TianguanDiag';
// Only used when on('update') cannot be registered: the fallback poll still uses
// the asynchronous getData() promise, never getDataSync().
const FALLBACK_POLL_INTERVAL_MS: number = 500;
// Demand-driven retry for a broadcast/apply that failed. It never reads the
// pasteboard; it only runs while something is actually waiting.
const RETRY_INTERVAL_MS: number = 1000;
// Window for the debug-level "TianguanClip" timing summary.
const STAT_WINDOW_MS: number = 2000;
// Reading the system pasteboard requires this permission (system_basic /
// user_grant on API 26). See clipboardRequestReadPermission().
const READ_PASTEBOARD_PERMISSION: Permissions = 'ohos.permission.READ_PASTEBOARD';

// @selfcheck-begin clipboard-core
/** Contract limit: the native side also caps clipboard text at 1 MiB. */
export const CLIPBOARD_MAX_BYTES: number = 1024 * 1024;

export const CLIPBOARD_READ_OWN: number = 0;
export const CLIPBOARD_READ_UNCHANGED: number = 1;
export const CLIPBOARD_READ_CHANGED: number = 2;

/** A text this module wrote to the system pasteboard, not yet observed. */
export interface PendingOwnWrite {
  token: number;
  text: string;
}

/**
 * Loop guard. markOwnWrite() must be called before setData(): the pasteboard
 * 'update' event may fire before the setData promise resolves, and it must
 * already see the text as ours.
 */
export class ClipboardLoopGuard {
  private writes: PendingOwnWrite[] = [];
  private nextToken: number = 1;

  /** Records an imminent own write; returns its token. */
  markOwnWrite(text: string): number {
    const token = this.nextToken;
    this.nextToken += 1;
    const write: PendingOwnWrite = { token: token, text: text };
    this.writes.push(write);
    return token;
  }

  /** true when the read text is our own write; the marker is consumed. */
  consumeOwnWrite(text: string): boolean {
    for (let i = 0; i < this.writes.length; i++) {
      if (this.writes[i].text === text) {
        // Older markers were overwritten in the pasteboard and can no longer
        // be observed, so drop them together with the matched marker.
        this.writes.splice(0, i + 1);
        return true;
      }
    }
    return false;
  }

  /** Rolls back a marker when the write failed (no update event will come). */
  discardOwnWrite(token: number): void {
    for (let i = 0; i < this.writes.length; i++) {
      if (this.writes[i].token === token) {
        this.writes.splice(i, 1);
        return;
      }
    }
  }

  /** Drops stale markers once a genuine user copy has been observed. */
  clearOwnWrites(): void {
    this.writes = [];
  }

  pendingCount(): number {
    return this.writes.length;
  }
}

/** Pure classification of one local pasteboard read. */
export function classifyLocalRead(guard: ClipboardLoopGuard, lastLocalText: string,
  text: string): number {
  if (guard.consumeOwnWrite(text)) {
    return CLIPBOARD_READ_OWN;
  }
  if (text === lastLocalText) {
    return CLIPBOARD_READ_UNCHANGED;
  }
  return CLIPBOARD_READ_CHANGED;
}

/** UTF-8 byte length (surrogate pairs = 4 bytes, lone surrogates = 3). */
export function utf8Length(text: string): number {
  let bytes = 0;
  let i = 0;
  while (i < text.length) {
    const code = text.charCodeAt(i);
    let width = 3;
    let units = 1;
    if (code < 0x80) {
      width = 1;
    } else if (code < 0x800) {
      width = 2;
    } else if (code >= 0xD800 && code <= 0xDBFF && i + 1 < text.length) {
      const low = text.charCodeAt(i + 1);
      if (low >= 0xDC00 && low <= 0xDFFF) {
        width = 4;
        units = 2;
      }
    }
    bytes += width;
    i += units;
  }
  return bytes;
}

/** Truncates on a code-point boundary to at most 1 MiB of UTF-8. */
export function truncateClipboardText(text: string): string {
  let bytes = 0;
  let i = 0;
  while (i < text.length) {
    const code = text.charCodeAt(i);
    let width = 3;
    let units = 1;
    if (code < 0x80) {
      width = 1;
    } else if (code < 0x800) {
      width = 2;
    } else if (code >= 0xD800 && code <= 0xDBFF && i + 1 < text.length) {
      const low = text.charCodeAt(i + 1);
      if (low >= 0xDC00 && low <= 0xDFFF) {
        width = 4;
        units = 2;
      }
    }
    if (bytes + width > CLIPBOARD_MAX_BYTES) {
      return text.substring(0, i);
    }
    bytes += width;
    i += units;
  }
  return text;
}

/**
 * Coalescing gate for the asynchronous pasteboard read.
 *
 * begin() starts a read and returns false while one is already running; that
 * call is merged into a single follow-up read requested by end(). This is what
 * keeps an 'update' storm (or our own setData echo) from starting overlapping
 * getData() calls.
 */
export class LocalReadGate {
  private running: boolean = false;
  private rerun: boolean = false;

  isRunning(): boolean {
    return this.running;
  }

  begin(): boolean {
    if (this.running) {
      this.rerun = true;
      return false;
    }
    this.running = true;
    this.rerun = false;
    return true;
  }

  /** true when the caller must issue one more read (a call was merged). */
  end(): boolean {
    this.running = false;
    if (!this.rerun) {
      return false;
    }
    this.rerun = false;
    return true;
  }

  reset(): void {
    this.running = false;
    this.rerun = false;
  }
}

/** A remote cut taken (or about to be taken) for one session. */
export interface RemotePending {
  version: number;
  text: string;
  taken: boolean;
}

/**
 * Everything RemoteClipboardApplier needs from the surrounding module.
 * Production wires these to the real pasteboard, loop guard and serial queue;
 * the self-check injects fakes so take/write races are deterministic.
 */
export interface RemoteApplyHooks {
  /** Reads and clears the native pending ServerCutText; null = read failed. */
  take: (sessionId: string) => string | null;
  /** true when the remote text already equals the local pasteboard baseline. */
  sameAsLocal: (text: string) => boolean;
  /** false when the system pasteboard is unavailable (keep pending, retry). */
  writable: () => boolean;
  /** Records the imminent write BEFORE setData; returns a rollback token. */
  beginWrite: (payload: string) => number;
  /** Writes payload to the local system pasteboard. */
  write: (payload: string) => Promise<void>;
  /** Rolls back a failed write (marker removed, baseline restored). */
  abortWrite: (token: number) => void;
  /** Adopts a successful write as the local baseline. */
  adoptWrite: (payload: string) => void;
  /** Queues another apply (a newer remote cut replaced the pending entry). */
  schedule: (sessionId: string) => void;
  normalize: (text: string) => string;
  payload: (text: string) => string;
  warnTake: (sessionId: string, version: number, error: string) => void;
  warnWrite: (sessionId: string, version: number, error: string) => void;
  infoApplied: (sessionId: string, version: number, payload: string) => void;
}

/**
 * Remote -> local state machine, one pending entry per session.
 *
 * The pending entry is deleted CONDITIONALLY after a write: if a newer remote
 * cut replaced it while setData() was in flight, the newer entry must survive
 * and be applied immediately. Deleting unconditionally lost the newest text
 * for good, because the page had already recorded the newer version as
 * notified and would never call us for it again.
 *
 * applyPending() must run on the module's single serial queue, so two writes
 * for one session can never interleave.
 */
export class RemoteClipboardApplier {
  private pending: Map<string, RemotePending> = new Map<string, RemotePending>();
  private consumed: Map<string, number> = new Map<string, number>();
  private hooks: RemoteApplyHooks;

  constructor(hooks: RemoteApplyHooks) {
    this.hooks = hooks;
  }

  /**
   * Records the version reported by the status JSON. Returns true when the
   * caller should schedule an apply: a new entry was created, an entry was
   * replaced by a newer version, or one that is still pending should be
   * retried. False once the version has already been consumed.
   */
  noteVersion(sessionId: string, version: number): boolean {
    const pending = this.pending.get(sessionId);
    if (pending !== undefined) {
      if (version > pending.version) {
        const replacement: RemotePending = { version: version, text: '', taken: false };
        this.pending.set(sessionId, replacement);
      }
      return true;
    }
    const seen = this.consumed.get(sessionId);
    if (seen !== undefined && seen >= version) {
      return false;
    }
    const entry: RemotePending = { version: version, text: '', taken: false };
    this.pending.set(sessionId, entry);
    return true;
  }

  pendingSessions(): string[] {
    return Array.from(this.pending.keys());
  }

  pendingCount(): number {
    return this.pending.size;
  }

  pendingVersion(sessionId: string): number {
    const pending = this.pending.get(sessionId);
    return pending === undefined ? -1 : pending.version;
  }

  consumedVersion(sessionId: string): number {
    const seen = this.consumed.get(sessionId);
    return seen === undefined ? -1 : seen;
  }

  forget(sessionId: string): void {
    this.pending.delete(sessionId);
    this.consumed.delete(sessionId);
  }

  clear(): void {
    this.pending.clear();
    this.consumed.clear();
  }

  /** One apply step; the caller's queue must serialize calls per session. */
  async applyPending(sessionId: string): Promise<void> {
    const pending = this.pending.get(sessionId);
    if (pending === undefined) {
      return;
    }
    // Keep the version: a newer cut may replace the map entry below, and
    // settle() still has to know which version this task handled.
    const version = pending.version;
    if (!pending.taken) {
      let taken: string | null = null;
      try {
        taken = this.hooks.take(sessionId);
      } catch (error) {
        this.hooks.warnTake(sessionId, version, String(error));
        return;
      }
      if (taken === null) {
        // "Read failed" is NOT "nothing pending": consume nothing and retry
        // later, or the one-time content is lost for good.
        this.hooks.warnTake(sessionId, version, 'takeClipboard 返回 null（读取失败）');
        return;
      }
      pending.taken = true;
      pending.text = this.hooks.normalize(taken);
      if (pending.text.length === 0) {
        // Genuinely nothing pending on the native side: consume the version.
        this.settle(sessionId, version);
        return;
      }
    }
    const text = pending.text;
    if (this.hooks.sameAsLocal(text)) {
      this.settle(sessionId, version);
      return;
    }
    if (!this.hooks.writable()) {
      return;
    }
    const payload = this.hooks.payload(text);
    // Record BEFORE setData: the 'update' event may arrive before the promise
    // resolves, and it has to recognize this text as our own.
    const token = this.hooks.beginWrite(payload);
    try {
      await this.hooks.write(payload);
    } catch (error) {
      this.hooks.abortWrite(token);
      this.hooks.warnWrite(sessionId, version, String(error));
      // A newer pending entry (if any) is untouched and retried; otherwise this
      // one keeps taken=true and is retried by the demand-driven retry.
      return;
    }
    this.hooks.adoptWrite(payload);
    this.settle(sessionId, version);
    this.hooks.infoApplied(sessionId, version, payload);
  }

  /**
   * Marks `version` done, but only removes the pending entry if it still IS
   * that version. If a newer cut replaced it while the write was in flight, the
   * newer entry is kept and scheduled right away.
   */
  private settle(sessionId: string, version: number): void {
    const current = this.pending.get(sessionId);
    if (current === undefined) {
      this.markConsumed(sessionId, version);
      return;
    }
    if (current.version === version) {
      this.pending.delete(sessionId);
      this.markConsumed(sessionId, version);
      return;
    }
    this.markConsumed(sessionId, version);
    if (current.version > version) {
      // Newer cut replaced this entry during the await: apply it now, in the
      // same serial queue, instead of waiting for a retry timer (the page has
      // already recorded the newer version and will not notify again).
      this.hooks.schedule(sessionId);
    } else {
      // Stale entry (versions only grow per session); drop it.
      this.pending.delete(sessionId);
    }
  }

  private markConsumed(sessionId: string, version: number): void {
    const seen = this.consumed.get(sessionId);
    if (seen === undefined || version > seen) {
      this.consumed.set(sessionId, version);
    }
  }
}
// @selfcheck-end clipboard-core

const sessionIds: Set<string> = new Set<string>();
/** Local clipboard revision already accepted by setClipboard, per session. */
const deliveredRevs: Map<string, number> = new Map<string, number>();
/** Sessions with an apply task already in the serial queue. */
const applyScheduled: Set<string> = new Set<string>();
const loopGuard: ClipboardLoopGuard = new ClipboardLoopGuard();

let lastLocalText: string = '';
let localRev: number = 0;
/** 'none' | 'local' | 'remote' — remote content is not fanned out to sessions. */
let localOrigin: string = 'none';
let queue: Promise<void> = Promise.resolve();
/** Coalesces update events/fallback ticks into one async read at a time. */
const readGate: LocalReadGate = new LocalReadGate();
/** 'event' | 'fallback' | 'off' — which local->remote trigger is active. */
let watchMode: string = 'off';
/** 'unknown' | 'requesting' | 'granted' | 'denied' — READ_PASTEBOARD state. */
let readPermissionState: string = 'unknown';
/** Exactly one permission dialog per process, whatever its outcome. */
let readPermissionRequested: boolean = false;
/** The "perm=0, reads disabled" explanation is logged once, not per event. */
let readGateLogged: boolean = false;
/**
 * Sessions whose page is currently visible; reads only make sense then.
 * Remote pages run in sibling ability instances but share this module (same
 * process), so a Set per session is enough: showing/hiding one page never
 * duplicates the pasteboard listener, and only the last visible page hiding
 * turns it off.
 */
const visibleSessions: Set<string> = new Set<string>();
/** Count of pasteboard 'update' events observed (diagnostics). */
let updateEventCount: number = 0;
/** Update events merged into the next read pass (0 for baseline/fallback). */
let pendingUpdateReads: number = 0;
/** Duration/bytes of the getData() awaited in the current pass (diagnostics). */
let lastReadUsDiag: number = 0;
let lastReadBytesDiag: number = 0;
/** on('update') listener currently registered. */
let updateHookOn: boolean = false;
/** 500 ms async fallback, only when on('update') cannot be registered. */
let fallbackTimer: number = -1;
/** Demand-driven retry timer; only while a delivery/apply is waiting. */
let retryTimer: number = -1;
/** A local text still has sessions that did not accept it. */
let pendingDelivery: boolean = false;
/**
 * The first read after the module starts only records the clipboard's existing
 * content as the baseline (it is not a user copy and is not pushed out).
 */
let baselinePending: boolean = true;
let board: pasteboard.SystemPasteboard | undefined = undefined;
let lastReadFailure: string = '';
/** One-shot explanation for "the pasteboard read yields no plain text". */
let nonTextReadLogged: boolean = false;
let truncationCount: number = 0;
let lastTruncatedOriginalBytes: number = 0;
let lastTruncatedSentBytes: number = 0;
/** Local text replaced by the write in flight; restored when that write fails. */
let writeBaseline: string = '';

// --- Diagnostic timing (event-driven clipboard build) ---------------------
// One tick = one completed read pass, from the trigger (update event / fallback
// tick / baseline) to the end of the queued task, so the serial-queue wait is
// included. Segments: read = the awaited system pasteboard getData() (async, it
// does not block the UI thread), compare = classifyLocalRead, write =
// setClipboard fan-out. reads = pasteboard reads completed in the window, bytes
// = UTF-8 bytes read in the window. Values are microseconds until formatted.
let statWindowStartUs: number = 0;
let statTickN: number = 0;
let statTickSumUs: number = 0;
let statTickMaxUs: number = 0;
let statReadN: number = 0;
let statReadSumUs: number = 0;
let statReadMaxUs: number = 0;
let statCompareN: number = 0;
let statCompareSumUs: number = 0;
let statCompareMaxUs: number = 0;
let statWriteN: number = 0;
let statWriteSumUs: number = 0;
let statWriteMaxUs: number = 0;
let statReads: number = 0;
let statBytes: number = 0;

// Microseconds. getUptime(..., true) is NANOSECONDS: /1000 gives microseconds
// (the previous /1e6 silently yielded milliseconds, which made the 2 s window
// comparison never fire).
function clipNowUs(): number {
  try {
    return systemDateTime.getUptime(systemDateTime.TimeType.ACTIVE, true) / 1000;
  } catch (error) {
    return Date.now() * 1000;
  }
}

function noteClipRead(micros: number, bytes: number): void {
  statReadN += 1;
  statReadSumUs += micros;
  if (micros > statReadMaxUs) {
    statReadMaxUs = micros;
  }
  statReads += 1;
  statBytes += bytes;
}

function noteClipCompare(micros: number): void {
  statCompareN += 1;
  statCompareSumUs += micros;
  if (micros > statCompareMaxUs) {
    statCompareMaxUs = micros;
  }
}

function noteClipWrite(micros: number): void {
  statWriteN += 1;
  statWriteSumUs += micros;
  if (micros > statWriteMaxUs) {
    statWriteMaxUs = micros;
  }
}

function statSegment(n: number, sumUs: number, maxUs: number): string {
  if (n === 0) {
    return '0.00/0.00';
  }
  return (sumUs / n / 1000).toFixed(2) + '/' + (maxUs / 1000).toFixed(2);
}

/** Records one completed async read pass and emits the 2 s debug summary. */
function recordClipTick(totalUs: number): void {
  statTickN += 1;
  statTickSumUs += totalUs;
  if (totalUs > statTickMaxUs) {
    statTickMaxUs = totalUs;
  }
  const now = clipNowUs();
  if (statWindowStartUs <= 0) {
    statWindowStartUs = now;
    return;
  }
  if (now - statWindowStartUs < STAT_WINDOW_MS * 1000) {
    return;
  }
  const line = 'TianguanClip mode=' + watchMode +
    ' perm=' + readPermissionFlag() +
    ' n=' + statTickN +
    ' avg=' + (statTickSumUs / statTickN / 1000).toFixed(2) +
    'ms max=' + (statTickMaxUs / 1000).toFixed(2) +
    'ms reads=' + statReads +
    ' bytes=' + statBytes +
    ' read=' + statSegment(statReadN, statReadSumUs, statReadMaxUs) +
    ' compare=' + statSegment(statCompareN, statCompareSumUs, statCompareMaxUs) +
    ' write=' + statSegment(statWriteN, statWriteSumUs, statWriteMaxUs) +
    ' updateEvents=' + updateEventCount;
  // debug: periodic (2 s) timing summary; hilog.debug keeps it out of the
  // default log level while real clipboard errors below stay WARN.
  hilog.debug(DOMAIN, TAG, '%{public}s', line);
  statWindowStartUs = now;
  statTickN = 0;
  statTickSumUs = 0;
  statTickMaxUs = 0;
  statReadN = 0;
  statReadSumUs = 0;
  statReadMaxUs = 0;
  statCompareN = 0;
  statCompareSumUs = 0;
  statCompareMaxUs = 0;
  statWriteN = 0;
  statWriteSumUs = 0;
  statWriteMaxUs = 0;
  statReads = 0;
  statBytes = 0;
}

/** Per-session remote -> local queue with conditional settlement (defect A/B). */
const remoteHooks: RemoteApplyHooks = {
  take: (sessionId: string): string | null => takeClipboardOrNull(sessionId),
  sameAsLocal: (text: string): boolean => text === lastLocalText,
  writable: (): boolean => systemBoard() !== undefined,
  beginWrite: (payload: string): number => {
    writeBaseline = lastLocalText;
    const token = loopGuard.markOwnWrite(payload);
    lastLocalText = payload;
    return token;
  },
  write: async (payload: string): Promise<void> => {
    const active = systemBoard();
    if (active === undefined) {
      throw new Error('system pasteboard unavailable');
    }
    const data = pasteboard.createData(pasteboard.MIMETYPE_TEXT_PLAIN, payload);
    await active.setData(data);
  },
  abortWrite: (token: number): void => {
    loopGuard.discardOwnWrite(token);
    lastLocalText = writeBaseline;
  },
  adoptWrite: (payload: string): void => {
    lastLocalText = payload;
    localOrigin = 'remote';
    // Remote content is never fanned back out, so nothing is pending delivery.
    pendingDelivery = false;
  },
  schedule: (sessionId: string): void => {
    scheduleRemoteApply(sessionId);
  },
  normalize: (text: string): string => normalizeClipboardText(text),
  payload: (text: string): string => payloadForDirection(text, 'remote->local'),
  warnTake: (sessionId: string, version: number, error: string): void => {
    hilog.warn(DOMAIN, TAG,
      'clipboard takeClipboard 失败，保留版本等待重试：session=%{public}s version=%{public}d %{public}s',
      sessionId, version, error);
  },
  warnWrite: (sessionId: string, version: number, error: string): void => {
    hilog.warn(DOMAIN, TAG,
      'clipboard 写入本机剪贴板失败，内容保留到稍后重试：session=%{public}s version=%{public}d %{public}s',
      sessionId, version, error);
  },
  infoApplied: (sessionId: string, version: number, payload: string): void => {
    hilog.info(DOMAIN, TAG,
      'clipboard 远端->本地 同步 %{public}d 字节（session=%{public}s version=%{public}d）',
      utf8Length(payload), sessionId, version);
  }
};

const remoteApplier: RemoteClipboardApplier = new RemoteClipboardApplier(remoteHooks);

export interface ClipboardSyncInfo {
  knownSessions: number;
  /** Session pages currently visible (the local->remote direction needs one). */
  visibleSessions: number;
  /** 'event' | 'fallback' | 'off' — which local->remote trigger is active. */
  watchMode: string;
  /** Pasteboard 'update' events observed since start (0 on the fallback path). */
  updateEvents: number;
  deliveredSessions: number;
  pendingRemoteSessions: number;
  localRevision: number;
  localOrigin: string;
  lastPayloadBytes: number;
  truncationCount: number;
  lastTruncatedOriginalBytes: number;
  lastTruncatedSentBytes: number;
}

function systemBoard(): pasteboard.SystemPasteboard | undefined {
  if (board !== undefined) {
    return board;
  }
  try {
    board = pasteboard.getSystemPasteboard();
    return board;
  } catch (error) {
    hilog.warn(DOMAIN, TAG, 'clipboard 获取系统剪贴板失败：%{public}s', String(error));
    return undefined;
  }
}

/** \r\n and lone \r become \n (contract normalizes too; we do it again). */
export function normalizeClipboardText(text: string): string {
  return text.replace(/\r\n/g, '\n').replace(/\r/g, '\n');
}

/** One serial queue for both directions; a failed task never breaks the chain. */
function enqueue(task: () => Promise<void>): void {
  queue = queue.then((): Promise<void> => task()).catch((error: Error): void => {
    hilog.warn(DOMAIN, TAG, 'clipboard 串行队列任务失败：%{public}s', String(error));
  });
}

function payloadForDirection(text: string, direction: string): string {
  const payload = truncateClipboardText(text);
  if (payload.length === text.length) {
    return payload;
  }
  const originalBytes = utf8Length(text);
  const sentBytes = utf8Length(payload);
  if (originalBytes !== lastTruncatedOriginalBytes) {
    truncationCount += 1;
    hilog.warn(DOMAIN, TAG,
      'clipboard 内容超过 1 MiB，已截断：direction=%{public}s 原始 %{public}d 字节 -> 截断为 %{public}d 字节',
      direction, originalBytes, sentBytes);
  }
  lastTruncatedOriginalBytes = originalBytes;
  lastTruncatedSentBytes = sentBytes;
  return payload;
}

/**
 * Remote page registers its session when it appears. The page starts visible
 * (ArkUI fires onPageShow before any hide), so the module starts watching here.
 */
export function clipboardRegisterSession(sessionId: string): void {
  sessionIds.add(sessionId);
  visibleSessions.add(sessionId);
  updateWatchState();
}

export function clipboardUnregisterSession(sessionId: string): void {
  sessionIds.delete(sessionId);
  visibleSessions.delete(sessionId);
  deliveredRevs.delete(sessionId);
  remoteApplier.forget(sessionId);
  applyScheduled.delete(sessionId);
  updateWatchState();
}

/**
 * Page visibility edge (Remote onPageShow/onPageHide). A visible session page is
 * what makes the local pasteboard readable at all, so the 'update' listener is
 * attached only while at least one session page is visible and removed with the
 * same callback reference when the last one hides. Showing again starts a normal
 * change-detecting read, so a copy made while the window was hidden is still
 * delivered instead of being mistaken for a new baseline.
 */
export function clipboardSetSessionVisible(sessionId: string, visible: boolean): void {
  if (!sessionIds.has(sessionId)) {
    return;
  }
  if (visible) {
    visibleSessions.add(sessionId);
  } else {
    visibleSessions.delete(sessionId);
  }
  updateWatchState();
}

function watchEnabled(): boolean {
  return sessionIds.size > 0 && visibleSessions.size > 0;
}

function updateWatchState(): void {
  if (watchEnabled()) {
    startWatch();
  } else {
    stopWatch();
  }
}

/** 1 when the pasteboard read permission is usable, 0 otherwise (log field). */
function readPermissionFlag(): number {
  return readPermissionState === 'granted' ? 1 : 0;
}

/**
 * One-shot explanation that the local->remote direction is parked. It must stay
 * one line: without the permission the read path is never entered, so the usual
 * TianguanClip summary (which only runs after a read) would never appear.
 */
function logClipReadGateOnce(reason: string): void {
  if (readGateLogged) {
    return;
  }
  readGateLogged = true;
  hilog.warn(DOMAIN, TAG,
    'TianguanClip perm=0 mode=off reads=0 bytes=0 read=0.00/0.00 gate=%{public}s：' +
      '未授权 READ_PASTEBOARD，本机剪贴板读取已停用（不再按 update 事件或定时器调用 getData；' +
      '本机->远端自动同步不可用，远端->本地不受影响）',
    reason);
}

function readAccessTokenId(): number {
  try {
    const info = bundleManager.getBundleInfoForSelfSync(
      bundleManager.BundleFlag.GET_BUNDLE_INFO_WITH_APPLICATION);
    return info.appInfo.accessTokenId;
  } catch (error) {
    hilog.warn(DOMAIN, TAG, 'clipboard 获取自身 tokenId 失败：%{public}s', String(error));
    return -1;
  }
}

/**
 * Asks for ohos.permission.READ_PASTEBOARD once per process. The caller is the
 * Remote page (it owns the visible session window), which passes its host
 * context. checkAccessToken() first: when the permission is already granted no
 * dialog is shown. Otherwise requestPermissionsFromUser() shows the system
 * dialog; authResults[0] === 0 means granted. Both outcomes are logged with
 * authResults/dialogShownResults/errorReasons. A denial or a failed request
 * leaves the read path gated OFF (never one getData per update event), so a
 * rejected read costs nothing; only a later process start can ask again.
 */
export function clipboardRequestReadPermission(context: common.Context): void {
  if (readPermissionRequested || readPermissionState === 'granted') {
    return;
  }
  readPermissionRequested = true;
  readPermissionState = 'requesting';
  requestReadPermission(context).catch((error: Error): void => {
    readPermissionState = 'denied';
    logClipReadGateOnce('request-failed');
    hilog.warn(DOMAIN, TAG,
      'TianguanClip perm=0 READ_PASTEBOARD 授权流程失败，本机->远端自动同步停用：%{public}s',
      String(error));
    updateWatchState();
  });
}

async function requestReadPermission(context: common.Context): Promise<void> {
  const manager = abilityAccessCtrl.createAtManager();
  const tokenId = readAccessTokenId();
  let status: abilityAccessCtrl.GrantStatus = abilityAccessCtrl.GrantStatus.PERMISSION_DENIED;
  if (tokenId >= 0) {
    status = await manager.checkAccessToken(tokenId, READ_PASTEBOARD_PERMISSION);
  }
  if (status === abilityAccessCtrl.GrantStatus.PERMISSION_GRANTED) {
    readPermissionState = 'granted';
    hilog.info(DOMAIN, TAG,
      'TianguanClip perm=1 READ_PASTEBOARD 已授权（checkAccessToken=GRANTED，不弹窗）');
    onReadPermissionGranted();
    return;
  }
  hilog.info(DOMAIN, TAG,
    'TianguanClip perm=0 READ_PASTEBOARD 未授权（checkAccessToken=DENIED），弹窗申请一次');
  const permissionList: Array<Permissions> = [READ_PASTEBOARD_PERMISSION];
  const result: PermissionRequestResult =
    await manager.requestPermissionsFromUser(context, permissionList);
  const auth = result.authResults.length > 0 ? result.authResults[0] : -99;
  let shown = -1;
  if (result.dialogShownResults !== undefined && result.dialogShownResults.length > 0) {
    shown = result.dialogShownResults[0] ? 1 : 0;
  }
  let reason = -1;
  if (result.errorReasons !== undefined && result.errorReasons.length > 0) {
    reason = result.errorReasons[0];
  }
  hilog.info(DOMAIN, TAG,
    'TianguanClip perm=%{public}d READ_PASTEBOARD 申请结果：authResults[0]=%{public}d ' +
      'dialogShown=%{public}d errorReason=%{public}d（authResults 0=已授权 -1=拒绝 2=无效；' +
      'dialogShown 1=弹过窗 0=未弹 -1=未返回）',
    auth === 0 ? 1 : 0, auth, shown, reason);
  if (auth === 0) {
    readPermissionState = 'granted';
    onReadPermissionGranted();
    return;
  }
  readPermissionState = 'denied';
  logClipReadGateOnce('denied');
  updateWatchState();
}

/** Grant edge: start the normal event watch and take the first baseline read. */
function onReadPermissionGranted(): void {
  readGateLogged = false;
  // The module could not read before this grant, so the clipboard's current
  // content is still the baseline: the first read must not be pushed out.
  baselinePending = true;
  updateWatchState();
}

/**
 * Event-driven mode: the 'update' listener only marks a pending read and starts
 * one asynchronous getData() (see requestLocalScan). No setInterval polls the
 * pasteboard. If subscribing throws (API/ROM without the event), a low-frequency
 * poll that still uses the asynchronous getData() promise takes over.
 */
function startWatch(): void {
  if (readPermissionState !== 'granted') {
    // Without READ_PASTEBOARD every getData() would be a rejected ~100 ms IPC:
    // do not register the listener and do not start the fallback poll.
    logClipReadGateOnce('watch');
    return;
  }
  const active = systemBoard();
  if (active === undefined) {
    return;
  }
  if (watchMode === 'off') {
    if (!updateHookOn && fallbackTimer < 0) {
      try {
        active.on('update', onLocalUpdate);
        updateHookOn = true;
        watchMode = 'event';
        hilog.info(DOMAIN, TAG,
          'clipboard 事件驱动模式：on(update) 已注册（异步 getData，无同步读、无定时轮询）');
      } catch (error) {
        hilog.warn(DOMAIN, TAG,
          'clipboard on(update) 注册失败，退回 %{public}d ms 异步轮询（只用 getData Promise）：%{public}s',
          FALLBACK_POLL_INTERVAL_MS, String(error));
        startFallbackPoll();
      }
    }
  }
  // Baseline / change read. It is a promise: nothing here blocks the UI thread.
  requestLocalScan();
}

function startFallbackPoll(): void {
  watchMode = 'fallback';
  if (fallbackTimer >= 0) {
    return;
  }
  fallbackTimer = setInterval((): void => {
    try {
      requestLocalScan();
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'clipboard 兜底轮询异常：%{public}s', String(error));
    }
  }, FALLBACK_POLL_INTERVAL_MS);
}

function stopWatch(): void {
  if (fallbackTimer >= 0) {
    clearInterval(fallbackTimer);
    fallbackTimer = -1;
  }
  if (retryTimer >= 0) {
    clearTimeout(retryTimer);
    retryTimer = -1;
  }
  if (updateHookOn) {
    const active = systemBoard();
    if (active !== undefined) {
      try {
        active.off('update', onLocalUpdate);
      } catch (error) {
        hilog.warn(DOMAIN, TAG, 'clipboard 注销 update 事件失败：%{public}s', String(error));
      }
    }
    updateHookOn = false;
  }
  if (watchMode !== 'off') {
    hilog.info(DOMAIN, TAG, 'clipboard 本地->远端监听已停止（没有可见会话）');
  }
  watchMode = 'off';
}

/** 'update' listener: no work here, only a coalesced asynchronous read. */
function onLocalUpdate(): void {
  updateEventCount += 1;
  pendingUpdateReads += 1;
  if (updateEventCount === 1) {
    hilog.info(DOMAIN, TAG, 'clipboard update 事件已观测到（事件驱动读取生效）');
  }
  try {
    requestLocalScan();
  } catch (error) {
    hilog.warn(DOMAIN, TAG, 'clipboard update 回调异常：%{public}s', String(error));
  }
}

/**
 * Starts ONE asynchronous read pass. An update/fallback tick that arrives while
 * a read is running is merged into exactly one follow-up pass. This replaces the
 * fixed-interval poll: the pasteboard is only read when something happened, and
 * never with getDataSync().
 */
function requestLocalScan(): void {
  if (readPermissionState !== 'granted') {
    logClipReadGateOnce('scan');
    return;
  }
  if (!watchEnabled()) {
    return;
  }
  if (!readGate.begin()) {
    return;
  }
  const tickStart = clipNowUs();
  enqueue(async (): Promise<void> => {
    try {
      await readLocalPass(tickStart);
    } finally {
      if (readGate.end()) {
        requestLocalScan();
      }
    }
  });
}

/** One read pass on the serial queue: async getData, compare, fan out. */
async function readLocalPass(tickStart: number): Promise<void> {
  if (readPermissionState !== 'granted') {
    return;
  }
  const updateDriven = pendingUpdateReads;
  pendingUpdateReads = 0;
  if (!watchEnabled()) {
    return;
  }
  lastReadUsDiag = 0;
  lastReadBytesDiag = 0;
  retryPendingRemote();
  await syncLocal();
  if (updateDriven > 0) {
    // Verification line: an update event triggered the async read, and this is
    // how long the awaited getData() took (it does not block the UI thread).
    hilog.info(DOMAIN, TAG,
      'clipboard update 驱动异步读取：events=%{public}d read=%{public}sms bytes=%{public}d',
      updateDriven, (lastReadUsDiag / 1000).toFixed(2), lastReadBytesDiag);
  }
  maybeScheduleRetry();
  recordClipTick(clipNowUs() - tickStart);
}

/** Queues a retry for every remote text whose local write has not landed. */
function retryPendingRemote(): void {
  const ids: string[] = remoteApplier.pendingSessions();
  for (let i = 0; i < ids.length; i++) {
    scheduleRemoteApply(ids[i]);
  }
}

/**
 * Demand-driven retry, only while a broadcast or a remote->local write is
 * waiting. It never reads the pasteboard; it replaces the old "the next poll
 * will retry" behaviour without any fixed-interval clipboard read.
 */
function maybeScheduleRetry(): void {
  if (retryTimer >= 0 || !watchEnabled() || !hasPendingClipboardWork()) {
    return;
  }
  retryTimer = setTimeout((): void => {
    retryTimer = -1;
    if (!watchEnabled()) {
      return;
    }
    enqueue(async (): Promise<void> => {
      retryPendingRemote();
      if (pendingDelivery && localOrigin === 'local' && lastLocalText.length > 0) {
        const writeStart = clipNowUs();
        deliverLocalText(lastLocalText, localRev);
        noteClipWrite(clipNowUs() - writeStart);
      }
      maybeScheduleRetry();
    });
  }, RETRY_INTERVAL_MS);
}

function hasPendingClipboardWork(): boolean {
  if (pendingDelivery) {
    return true;
  }
  return remoteApplier.pendingCount() > 0;
}

/** Local -> remote: async read of the system pasteboard, fan a new text out. */
async function syncLocal(): Promise<void> {
  if (readPermissionState !== 'granted') {
    return;
  }
  if (!watchEnabled()) {
    return;
  }
  const active = systemBoard();
  if (active === undefined) {
    return;
  }
  let text = '';
  let data: pasteboard.PasteData | undefined = undefined;
  const readStart = clipNowUs();
  try {
    // Asynchronous: the getDataSync() form blocked the UI thread for ~104 ms on
    // the diagnostic device; the promise does the same work off that thread.
    data = await active.getData();
  } catch (error) {
    lastReadUsDiag = clipNowUs() - readStart;
    lastReadBytesDiag = 0;
    noteClipRead(lastReadUsDiag, 0);
    // Expected while the app is backgrounded or without READ_PASTEBOARD. Log
    // the code, throttled to changes of the code.
    const token = String(error);
    if (token !== lastReadFailure) {
      lastReadFailure = token;
      hilog.info(DOMAIN, TAG,
        'clipboard 读取本机剪贴板失败（后台或无 READ_PASTEBOARD 时暂停本地->远端）：%{public}s', token);
    }
    baselinePending = false;
    return;
  }
  const readUs = clipNowUs() - readStart;
  lastReadUsDiag = readUs;
  lastReadBytesDiag = 0;
  if (data === undefined || !watchEnabled()) {
    // No data, or the page went away while the read was in flight: drop it.
    noteClipRead(readUs, 0);
    return;
  }
  if (data.getRecordCount() !== 1) {
    // Not a single-record pasteboard: not plain text, or the read was refused.
    // Without ohos.permission.READ_PASTEBOARD (system_basic on this profile) the
    // pasteboard service returns an empty PasteData, so explain it once instead
    // of silently doing nothing.
    if (!nonTextReadLogged) {
      nonTextReadLogged = true;
      hilog.info(DOMAIN, TAG,
        'clipboard 本机剪贴板无可同步的纯文本（记录数 %{public}d；无 READ_PASTEBOARD 时系统返回空数据）',
        data.getRecordCount());
    }
    noteClipRead(readUs, 0);
    return;
  }
  const record = data.getRecordAt(0);
  if (record.mimeType !== pasteboard.MIMETYPE_TEXT_PLAIN) {
    noteClipRead(readUs, 0);
    return;
  }
  text = normalizeClipboardText(record.plainText);
  noteClipRead(readUs, utf8Length(text));
  lastReadBytesDiag = utf8Length(text);
  lastReadFailure = '';
  nonTextReadLogged = false;
  if (baselinePending) {
    // First read after the module starts: this is the clipboard as it already
    // was, not a user copy, so it must not be pushed to the sessions.
    baselinePending = false;
    lastLocalText = text;
    localOrigin = 'none';
    pendingDelivery = false;
    hilog.info(DOMAIN, TAG, 'clipboard 基线已建立：%{public}d 字节（历史内容不推送）', utf8Length(text));
    return;
  }
  if (text.length === 0) {
    if (lastLocalText.length !== 0) {
      lastLocalText = '';
      localOrigin = 'none';
      pendingDelivery = false;
    }
    return;
  }
  const compareStart = clipNowUs();
  const kind = classifyLocalRead(loopGuard, lastLocalText, text);
  noteClipCompare(clipNowUs() - compareStart);
  if (kind === CLIPBOARD_READ_OWN) {
    // Our own remote -> local write showed up: never echo it to sessions.
    lastLocalText = text;
    return;
  }
  if (kind === CLIPBOARD_READ_UNCHANGED) {
    if (localOrigin === 'local') {
      // Retry sessions that missed the broadcast (disconnected at the time).
      const writeStart = clipNowUs();
      deliverLocalText(lastLocalText, localRev);
      noteClipWrite(clipNowUs() - writeStart);
    }
    return;
  }
  // A genuine user copy supersedes any stale own-write marker.
  loopGuard.clearOwnWrites();
  lastLocalText = text;
  localRev += 1;
  localOrigin = 'local';
  const writeStart = clipNowUs();
  deliverLocalText(text, localRev);
  noteClipWrite(clipNowUs() - writeStart);
}

/**
 * Sends the current local text to every session that has not accepted this
 * revision yet. Only QUEUED/COALESCED marks a session delivered; a rejected or
 * failed session stays pending and is retried by the demand-driven retry.
 */
function deliverLocalText(text: string, rev: number): void {
  const payload = payloadForDirection(text, 'local->remote');
  let accepted = 0;
  let waiting = 0;
  sessionIds.forEach((sessionId: string): void => {
    const delivered = deliveredRevs.get(sessionId);
    if (delivered !== undefined && delivered === rev) {
      return;
    }
    const reply = setClipboard(sessionId, payload);
    if (reply === 'QUEUED' || reply === 'COALESCED') {
      deliveredRevs.set(sessionId, rev);
      accepted += 1;
    } else {
      waiting += 1;
    }
  });
  pendingDelivery = waiting > 0;
  if (accepted > 0) {
    hilog.info(DOMAIN, TAG,
      'clipboard 本地->远端 同步 %{public}d 字节 -> %{public}d 会话（待重试 %{public}d，rev=%{public}d）',
      utf8Length(payload), accepted, waiting, rev);
  } else if (waiting > 0) {
    hilog.info(DOMAIN, TAG,
      'clipboard 本地->远端 暂未投递（%{public}d 会话），稍后重试 rev=%{public}d', waiting, rev);
  }
}

/**
 * Remote -> local edge: the status JSON's per-session clipboard.version moved.
 * Called by the remote page on every status poll; the version guard makes the
 * take happen exactly once per server-side cut. A take or write failure leaves
 * the version unconsumed and the pending entry for the demand-driven retry. If
 * this version is newer than one whose write is still in flight, noteVersion()
 * replaces the pending entry and the in-flight settle() schedules it.
 */
export function onRemoteClipboardVersion(sessionId: string, version: number): void {
  if (remoteApplier.noteVersion(sessionId, version)) {
    scheduleRemoteApply(sessionId);
  }
}

function scheduleRemoteApply(sessionId: string): void {
  if (applyScheduled.has(sessionId)) {
    return;
  }
  // The task itself runs on the one serial queue, so a newer version scheduled
  // from settle() can never interleave with the write that is still running.
  applyScheduled.add(sessionId);
  enqueue(async (): Promise<void> => {
    applyScheduled.delete(sessionId);
    await remoteApplier.applyPending(sessionId);
    if (remoteApplier.pendingCount() > 0) {
      // A take/write failed (or the pasteboard is unavailable): retry later
      // without polling the clipboard.
      maybeScheduleRetry();
    }
  });
}

/**
 * Diagnostics: sessions delivered at the current local revision, pending remote
 * writes and truncation counters (also logged with hilog.warn on truncation).
 */
export function clipboardSyncInfo(): ClipboardSyncInfo {
  let delivered = 0;
  sessionIds.forEach((sessionId: string): void => {
    const rev = deliveredRevs.get(sessionId);
    if (rev !== undefined && rev === localRev) {
      delivered += 1;
    }
  });
  const info: ClipboardSyncInfo = {
    knownSessions: sessionIds.size,
    visibleSessions: visibleSessions.size,
    watchMode: watchMode,
    updateEvents: updateEventCount,
    deliveredSessions: delivered,
    pendingRemoteSessions: remoteApplier.pendingCount(),
    localRevision: localRev,
    localOrigin: localOrigin,
    lastPayloadBytes: utf8Length(lastLocalText),
    truncationCount: truncationCount,
    lastTruncatedOriginalBytes: lastTruncatedOriginalBytes,
    lastTruncatedSentBytes: lastTruncatedSentBytes
  };
  return info;
}

/** Full reset (app quit). */
export function clipboardReset(): void {
  stopWatch();
  sessionIds.clear();
  visibleSessions.clear();
  deliveredRevs.clear();
  remoteApplier.clear();
  applyScheduled.clear();
  loopGuard.clearOwnWrites();
  lastLocalText = '';
  localRev = 0;
  localOrigin = 'none';
  writeBaseline = '';
  pendingDelivery = false;
  baselinePending = true;
  readGate.reset();
  updateEventCount = 0;
  pendingUpdateReads = 0;
  lastReadUsDiag = 0;
  lastReadBytesDiag = 0;
  queue = Promise.resolve();
  truncationCount = 0;
  lastTruncatedOriginalBytes = 0;
  lastTruncatedSentBytes = 0;
  lastReadFailure = '';
  nonTextReadLogged = false;
}
