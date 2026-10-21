/**
 * Process-wide runtime session table. Owns the ONE poll against the native
 * layer (listSessions + per-session sessionStatus, 2 s), so the state shown by
 * the connection list, the tray menu and the quit flow all comes from a single
 * source of truth that keeps running while the main window is hidden.
 */
import { hilog } from '@kit.PerformanceAnalysisKit';
import {
  closeSession, listSessions, reconnectSession, sessionStatus,
  SESSION_STATE_CONNECTED
} from '../diag/SessionApi';
import type { ListedSession } from '../diag/SessionApi';
import { markConnected } from './ConnectionStore';

const DOMAIN: number = 0x0000;
const TAG: string = 'TianguanDiag';
const POLL_INTERVAL_MS: number = 2000;

/** What the UI needs to know about one live session. */
export interface RuntimeSession {
  sessionId: string;
  connectionId: string;
  name: string;
  endpoint: string;
  state: string;
  errorCode: string;
  errorText: string;
  fingerprint: string;
  retryInMs: number;
  windowOpen: boolean;
}

export type SessionsChangeListener = () => void;

const sessions: Map<string, RuntimeSession> = new Map<string, RuntimeSession>();
/**
 * Sessions the UI closed on purpose. Native close is asynchronous, so
 * listSessions can still report them for a poll or two; without this set the
 * reconcile loop would adopt them back as orphans.
 */
const closedIds: Set<string> = new Set<string>();
let listeners: SessionsChangeListener[] = [];
let pollTimer: number = -1;

export function addSessionsListener(listener: SessionsChangeListener): void {
  listeners.push(listener);
}

export function removeSessionsListener(listener: SessionsChangeListener): void {
  listeners = listeners.filter((item: SessionsChangeListener) => item !== listener);
}

function notifyChanged(): void {
  for (let i = 0; i < listeners.length; i++) {
    try {
      listeners[i]();
    } catch (error) {
      hilog.warn(DOMAIN, TAG, '会话监听器异常：%{public}s', String(error));
    }
  }
}

export function registerSession(sessionId: string, connectionId: string, name: string,
  endpoint: string): void {
  closedIds.delete(sessionId);
  sessions.set(sessionId, {
    sessionId: sessionId,
    connectionId: connectionId,
    name: name,
    endpoint: endpoint,
    state: 'connecting',
    errorCode: 'none',
    errorText: '',
    fingerprint: '',
    retryInMs: 0,
    windowOpen: false
  });
  notifyChanged();
}

export function removeSession(sessionId: string): void {
  sessions.delete(sessionId);
  notifyChanged();
}

export function markWindowOpen(sessionId: string, open: boolean): void {
  const item = sessions.get(sessionId);
  if (item !== undefined && item.windowOpen !== open) {
    item.windowOpen = open;
    notifyChanged();
  }
}

export function findSession(sessionId: string): RuntimeSession | undefined {
  return sessions.get(sessionId);
}

export function allSessions(): RuntimeSession[] {
  return Array.from(sessions.values());
}

export function sessionsForConnection(connectionId: string): RuntimeSession[] {
  const result: RuntimeSession[] = [];
  sessions.forEach((item: RuntimeSession) => {
    if (item.connectionId === connectionId) {
      result.push(item);
    }
  });
  return result;
}

/** Session rows the tray shows: every session that is not closed. */
export function activeSessions(): RuntimeSession[] {
  return allSessions().filter((item: RuntimeSession) =>
    item.state !== 'disconnected' && item.state !== 'failed');
}

export function reconnectById(sessionId: string): void {
  reconnectSession(sessionId);
  const item = sessions.get(sessionId);
  if (item !== undefined) {
    item.state = 'connecting';
    notifyChanged();
  }
}

/** User-facing close: native close (idempotent) + table removal. */
export function closeSessionById(sessionId: string): void {
  closeSession(sessionId);
  closedIds.add(sessionId);
  sessions.delete(sessionId);
  notifyChanged();
}

export function closeAllSessions(): void {
  let touched = false;
  sessions.forEach((item: RuntimeSession) => {
    closeSession(item.sessionId);
    closedIds.add(item.sessionId);
    touched = true;
  });
  sessions.clear();
  if (touched) {
    notifyChanged();
  }
}

/** One poll cycle: reconcile with the native table, refresh every state. */
function refreshOnce(): void {
  const listed: ListedSession[] | undefined = listSessions();
  if (listed === undefined) {
    // A failed read says NOTHING about which sessions still exist. Treating it
    // as an empty table would delete every session (and its tombstone), so skip
    // this round and keep the last known table until the next poll (F31).
    hilog.warn(DOMAIN, TAG, '会话列表读取失败，跳过本轮对账（保留现有会话）');
    return;
  }
  const nativeIds = new Set<string>();
  let changed = false;
  for (let i = 0; i < listed.length; i++) {
    nativeIds.add(listed[i].id);
  }
  // Sessions that vanished inside the native layer (crash, external close).
  const dropped: string[] = [];
  sessions.forEach((item: RuntimeSession, key: string) => {
    if (!nativeIds.has(key)) {
      dropped.push(key);
    }
  });
  for (let i = 0; i < dropped.length; i++) {
    sessions.delete(dropped[i]);
    changed = true;
  }
  // Tombstones only matter while the native side can still report the session;
  // once it is gone from listSessions they can be forgotten.
  closedIds.forEach((id: string) => {
    if (!nativeIds.has(id)) {
      closedIds.delete(id);
    }
  });
  // Native sessions we do not know yet (e.g. ability restarted while the
  // process lived): adopt them so the tray can still reach them — unless the
  // UI just closed them on purpose (the native side may still list them).
  for (let i = 0; i < listed.length; i++) {
    if (!sessions.has(listed[i].id) && !closedIds.has(listed[i].id)) {
      // The trust store is keyed by "host:port", so an orphan adopted here
      // must carry the address its native session is using. With an empty
      // endpoint the trust confirmation cannot persist the confirmed
      // fingerprint and the user is asked again forever. An address the native
      // side has not resolved yet stays '' rather than being invented.
      const endpoint: string = listed[i].host.length > 0 && listed[i].port > 0
        ? listed[i].host + ':' + listed[i].port : '';
      sessions.set(listed[i].id, {
        sessionId: listed[i].id,
        connectionId: '',
        name: listed[i].profileName.length > 0 ? listed[i].profileName : listed[i].id,
        endpoint: endpoint,
        state: listed[i].state,
        errorCode: 'none',
        errorText: '',
        fingerprint: '',
        retryInMs: 0,
        windowOpen: false
      });
      changed = true;
    }
  }
  sessions.forEach((item: RuntimeSession) => {
    const status = sessionStatus(item.sessionId);
    if (status === undefined) {
      return;
    }
    if (item.state !== status.state) {
      item.state = status.state;
      changed = true;
      if (status.state === SESSION_STATE_CONNECTED && item.connectionId.length > 0) {
        markConnected(item.connectionId);
      }
    }
    const nextFingerprint = status.fingerprint === undefined ? '' : status.fingerprint;
    if (item.fingerprint !== nextFingerprint) {
      item.fingerprint = nextFingerprint;
      changed = true;
    }
    const nextError = status.error === undefined ? '' : status.error;
    if (item.errorText !== nextError) {
      item.errorText = nextError;
      changed = true;
    }
    const nextCode = status.errorCode === undefined ? 'none' : status.errorCode;
    if (item.errorCode !== nextCode) {
      item.errorCode = nextCode;
      changed = true;
    }
    const nextRetry = status.retryInMs === undefined ? 0 : status.retryInMs;
    if (item.retryInMs !== nextRetry) {
      item.retryInMs = nextRetry;
      changed = true;
    }
  });
  if (changed) {
    notifyChanged();
  }
}

export function startRegistryPolling(): void {
  if (pollTimer >= 0) {
    return;
  }
  pollTimer = setInterval((): void => {
    try {
      refreshOnce();
    } catch (error) {
      hilog.warn(DOMAIN, TAG, '会话轮询异常：%{public}s', String(error));
    }
  }, POLL_INTERVAL_MS);
}

export function stopRegistryPolling(): void {
  if (pollTimer >= 0) {
    clearInterval(pollTimer);
    pollTimer = -1;
  }
}
