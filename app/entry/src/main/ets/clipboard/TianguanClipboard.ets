/**
 * Bidirectional automatic clipboard sync, per docs/napi-contract.md §剪贴板:
 *   remote ServerCutText  -> local system pasteboard (per-session
 *   clipboardVersion from the status JSON; text taken once per version);
 *   local pasteboard copy -> every session via setClipboard (ClientCutText).
 *
 * Loop prevention: every write we make to the system pasteboard is recorded in
 * ClipboardLoopGuard BEFORE setData is called. The 'update' event and the 500 ms
 * poll classify what they read against that record, so a write that is still
 * in flight (or whose promise resolves after the event) can never be mistaken
 * for a user copy and echoed to the sessions.
 *
 * Ordering: every pasteboard write (remote -> local setData) and every session
 * broadcast (local -> remote setClipboard) runs through one promise-chain queue,
 * so the two directions cannot interleave.
 *
 * Delivery: success is tracked per session (deliveredRevs), never as one global
 * "lastSyncedText". A session that was disconnected while a local text was
 * broadcast gets it on a later poll; only QUEUED/COALESCED counts as delivered.
 *
 * Remote -> local failures (including a failed takeClipboard read) keep the
 * pending entry and retry on the next poll; a version is marked consumed and
 * its entry removed only conditionally, so a newer cut that arrived while a
 * write was in flight is kept and applied immediately instead of being lost.
 * Text above 1 MiB is truncated (UTF-8 safe) and logged plus exposed in
 * clipboardSyncInfo().
 *
 * Text only; \r\n normalized to \n both ways. Failures are logged (a background
 * app cannot read the pasteboard without a system-level permission, so a hidden
 * window simply pauses the local->remote direction until it is visible again —
 * the remote->local writes keep working).
 */
import { pasteboard } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { setClipboard, takeClipboardOrNull } from '../diag/SessionApi';

const DOMAIN: number = 0x0000;
const TAG: string = 'TianguanDiag';
const POLL_INTERVAL_MS: number = 500;

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
        // "Read failed" is NOT "nothing pending": consume nothing and retry on
        // the next poll, or the one-time content is lost for good.
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
      // one keeps taken=true and is retried by the next poll.
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
      // same serial queue, instead of waiting for the next poll (the page has
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
let scanQueued: boolean = false;
let pollTimer: number = -1;
let updateHookOn: boolean = false;
let board: pasteboard.SystemPasteboard | undefined = undefined;
let lastReadFailure: string = '';
let truncationCount: number = 0;
let lastTruncatedOriginalBytes: number = 0;
let lastTruncatedSentBytes: number = 0;
/** Local text replaced by the write in flight; restored when that write fails. */
let writeBaseline: string = '';

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
      'clipboard 写入本机剪贴板失败，内容保留到下一轮重试：session=%{public}s version=%{public}d %{public}s',
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

/** Remote page registers its session when it starts polling. */
export function clipboardRegisterSession(sessionId: string): void {
  sessionIds.add(sessionId);
  ensurePolling();
  requestLocalScan();
}

export function clipboardUnregisterSession(sessionId: string): void {
  sessionIds.delete(sessionId);
  deliveredRevs.delete(sessionId);
  remoteApplier.forget(sessionId);
  applyScheduled.delete(sessionId);
  if (sessionIds.size === 0) {
    stopPolling();
  }
}

function ensurePolling(): void {
  const active = systemBoard();
  if (active === undefined) {
    return;
  }
  if (!updateHookOn) {
    try {
      active.on('update', onLocalUpdate);
      updateHookOn = true;
    } catch (error) {
      // The 500 ms poll below still works without the event.
      hilog.warn(DOMAIN, TAG, 'clipboard 注册 update 事件失败：%{public}s', String(error));
    }
  }
  if (pollTimer < 0) {
    pollTimer = setInterval((): void => {
      try {
        requestLocalScan();
      } catch (error) {
        hilog.warn(DOMAIN, TAG, 'clipboard 轮询异常：%{public}s', String(error));
      }
    }, POLL_INTERVAL_MS);
  }
}

function stopPolling(): void {
  if (pollTimer >= 0) {
    clearInterval(pollTimer);
    pollTimer = -1;
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
}

function onLocalUpdate(): void {
  try {
    requestLocalScan();
  } catch (error) {
    hilog.warn(DOMAIN, TAG, 'clipboard update 回调异常：%{public}s', String(error));
  }
}

/** Coalesces poll/event reads into one queued scan. */
function requestLocalScan(): void {
  if (sessionIds.size === 0 || scanQueued) {
    return;
  }
  scanQueued = true;
  enqueue(async (): Promise<void> => {
    scanQueued = false;
    retryPendingRemote();
    syncLocal();
  });
}

/** Queues a retry for every remote text whose local write has not landed. */
function retryPendingRemote(): void {
  const ids: string[] = remoteApplier.pendingSessions();
  for (let i = 0; i < ids.length; i++) {
    scheduleRemoteApply(ids[i]);
  }
}

/** Local -> remote: read the system pasteboard, fan a genuinely new text out. */
function syncLocal(): void {
  if (sessionIds.size === 0) {
    return;
  }
  const active = systemBoard();
  if (active === undefined) {
    return;
  }
  let text = '';
  try {
    const data = active.getDataSync();
    if (data.getRecordCount() !== 1) {
      // Not a single-record pasteboard: not plain text, leave it alone.
      return;
    }
    const record = data.getRecordAt(0);
    if (record.mimeType !== pasteboard.MIMETYPE_TEXT_PLAIN) {
      return;
    }
    text = normalizeClipboardText(record.plainText);
  } catch (error) {
    // Expected while the app is backgrounded (read needs a system-level
    // permission there). Log the code, throttled to changes of the code.
    const token = String(error);
    if (token !== lastReadFailure) {
      lastReadFailure = token;
      hilog.info(DOMAIN, TAG, 'clipboard 读取本机剪贴板失败（后台受限时暂停本地->远端）：%{public}s', token);
    }
    return;
  }
  lastReadFailure = '';
  if (text.length === 0) {
    if (lastLocalText.length !== 0) {
      lastLocalText = '';
      localOrigin = 'none';
    }
    return;
  }
  const kind = classifyLocalRead(loopGuard, lastLocalText, text);
  if (kind === CLIPBOARD_READ_OWN) {
    // Our own remote -> local write showed up: never echo it to sessions.
    lastLocalText = text;
    return;
  }
  if (kind === CLIPBOARD_READ_UNCHANGED) {
    if (localOrigin === 'local') {
      // Retry sessions that missed the broadcast (disconnected at the time).
      deliverLocalText(lastLocalText, localRev);
    }
    return;
  }
  // A genuine user copy supersedes any stale own-write marker.
  loopGuard.clearOwnWrites();
  lastLocalText = text;
  localRev += 1;
  localOrigin = 'local';
  deliverLocalText(text, localRev);
}

/**
 * Sends the current local text to every session that has not accepted this
 * revision yet. Only QUEUED/COALESCED marks a session delivered; a rejected or
 * failed session is retried by the next poll.
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
  if (accepted > 0) {
    hilog.info(DOMAIN, TAG,
      'clipboard 本地->远端 同步 %{public}d 字节 -> %{public}d 会话（待重试 %{public}d，rev=%{public}d）',
      utf8Length(payload), accepted, waiting, rev);
  } else if (waiting > 0) {
    hilog.info(DOMAIN, TAG,
      'clipboard 本地->远端 暂未投递（%{public}d 会话），下一轮重试 rev=%{public}d', waiting, rev);
  }
}

/**
 * Remote -> local edge: the status JSON's per-session clipboard.version moved.
 * Called by the remote page on every poll; the version guard makes the take
 * happen exactly once per server-side cut. A take or write failure leaves the
 * version unconsumed and the pending entry for the poll to retry. If this
 * version is newer than one whose write is still in flight, noteVersion()
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
  sessionIds.clear();
  deliveredRevs.clear();
  remoteApplier.clear();
  applyScheduled.clear();
  loopGuard.clearOwnWrites();
  lastLocalText = '';
  localRev = 0;
  localOrigin = 'none';
  writeBaseline = '';
  scanQueued = false;
  queue = Promise.resolve();
  truncationCount = 0;
  lastTruncatedOriginalBytes = 0;
  lastTruncatedSentBytes = 0;
  lastReadFailure = '';
  stopPolling();
}
