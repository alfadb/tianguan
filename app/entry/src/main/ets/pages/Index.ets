/**
 * Main window: connection manager ONLY (the remote desktop itself lives in one
 * independent RemoteAbility window per session).
 *
 *  - left: single-level group column (全部 / groups / 未分组);
 *  - right: connection cards (name, host:port, status, last-connected time,
 *    primary action + more menu 重连/新建会话/编辑/复制/移组/删除);
 *  - top: search (name + host:port) and 新建连接;
 *  - new/edit form overlay: 名称/地址/端口/分组/无需密码/密码/保存密码 + 保存 / 保存并连接;
 *  - per-connection live status from the SessionRegistry (未连接/连接中/已连接·N个/
 *    重连中/已断开/需要确认身份/失败), fingerprint confirm inline.
 *
 * Cards are keyed by entry.id only. Their live data (status, action label,
 * edited fields) lives in an @Observed ConnectionView that the card component
 * observes, so a session-status tick refreshes just the affected cards instead
 * of re-creating every card (the previous key embedded registryVersion).
 */
import { hilog } from '@kit.PerformanceAnalysisKit';
import type { common } from '@kit.AbilityKit';
import {
  createSession, diagMultiSessionAvailable
} from '../diag/SessionApi';
import type { SessionOptions } from '../diag/SessionApi';
import {
  findConnection, loadPassword, newConnectionId, readTrustedFingerprint,
  removeConnection, removePassword, savePassword, savedConnections,
  trustFingerprint, upsertConnection, DEFAULT_PORT, UNGROUPED
} from '../model/ConnectionStore';
import type { ConnectionEntry } from '../model/ConnectionStore';
import {
  addSessionsListener, removeSessionsListener, sessionsForConnection,
  registerSession, closeSessionById, reconnectById
} from '../model/SessionRegistry';
import type { RuntimeSession } from '../model/SessionRegistry';
import { closeRemoteWindow, openRemoteWindow } from '../windows/RemoteWindows';

const DOMAIN: number = 0x0000;
const TAG: string = 'TianguanDiag';
const HINT_TIMEOUT_MS: number = 5000;

/** Aggregated per-connection status for one list row. */
interface RowStatus {
  kind: string; // idle | connecting | connected | reconnecting | disconnected | trust | failed
  label: string;
  count: number;
  fingerprint: string;
}

/**
 * Mutable per-row view model. The list is keyed by entry.id only, so ArkUI
 * reuses (and would never refresh) cards built from a plain ConnectionEntry;
 * this @Observed object is what the card component observes, so status, the
 * action label and edited fields update without rebuilding every card.
 */
@Observed
class ConnectionView {
  id: string;
  entry: ConnectionEntry;
  name: string;
  endpoint: string;
  lastConnectedText: string;
  statusKind: string;
  statusLabel: string;
  statusFingerprint: string;
  hasSessions: boolean;

  constructor(entry: ConnectionEntry) {
    this.id = entry.id;
    this.entry = entry;
    this.name = entry.name;
    this.endpoint = entry.host + ':' + entry.port;
    this.lastConnectedText = '';
    this.statusKind = 'idle';
    this.statusLabel = '未连接';
    this.statusFingerprint = '';
    this.hasSessions = false;
  }
}

/**
 * One connection card. @ObjectLink makes the card re-render when its
 * ConnectionView changes; callbacks only receive the id so the parent always
 * resolves the latest stored entry.
 */
@Component
struct ConnectionCard {
  @ObjectLink view: ConnectionView;
  onPrimary: (id: string) => void = (): void => {};
  onReconnect: (id: string) => void = (): void => {};
  onNewSession: (id: string) => void = (): void => {};
  onEdit: (id: string) => void = (): void => {};
  onCopy: (id: string) => void = (): void => {};
  onMove: (id: string) => void = (): void => {};
  onDelete: (id: string) => void = (): void => {};
  onConfirmTrust: (id: string, fingerprint: string) => void = (): void => {};

  private reconnectable(): boolean {
    return this.view.statusKind === 'failed' || this.view.statusKind === 'disconnected' ||
      this.view.statusKind === 'reconnecting';
  }

  private primaryLabel(): string {
    if (this.reconnectable()) {
      return '重连';
    }
    return this.view.hasSessions ? '打开' : '连接';
  }

  private statusColor(): Resource {
    switch (this.view.statusKind) {
      case 'connected':
        return $r('app.color.success');
      case 'connecting':
        return $r('app.color.brand');
      case 'reconnecting':
      case 'trust':
        return $r('app.color.warning');
      case 'failed':
        return $r('app.color.danger');
      default:
        return $r('app.color.text_secondary');
    }
  }

  private menuItems(): MenuElement[] {
    const items: MenuElement[] = [];
    if (this.reconnectable()) {
      items.push({
        value: '重连',
        action: (): void => {
          this.onReconnect(this.view.id);
        }
      });
    }
    if (this.view.hasSessions) {
      items.push({
        value: '新建会话',
        action: (): void => {
          this.onNewSession(this.view.id);
        }
      });
    }
    items.push({
      value: '编辑',
      action: (): void => {
        this.onEdit(this.view.id);
      }
    });
    items.push({
      value: '复制',
      action: (): void => {
        this.onCopy(this.view.id);
      }
    });
    items.push({
      value: '移组',
      action: (): void => {
        this.onMove(this.view.id);
      }
    });
    items.push({
      value: '删除',
      action: (): void => {
        this.onDelete(this.view.id);
      }
    });
    return items;
  }

  build() {
    Column({ space: 8 }) {
      Row({ space: 12 }) {
        Column({ space: 4 }) {
          Text(this.view.name)
            .fontSize(16)
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.text_primary'))
            .maxLines(1)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
          Text(this.view.endpoint)
            .fontSize(13)
            .fontColor($r('app.color.text_secondary'))
            .maxLines(1)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
          Row({ space: 6 }) {
            Text(this.view.lastConnectedText)
              .fontSize(12)
              .fontColor($r('app.color.text_secondary'))
          }
        }
        .alignItems(HorizontalAlign.Start)
        .layoutWeight(1)

        Column({ space: 2 }) {
          Row({ space: 6 }) {
            Circle({ width: 8, height: 8 })
              .fill(this.statusColor())
            Text(this.view.statusLabel)
              .fontSize(13)
              .fontColor(this.statusColor())
          }
        }
        .justifyContent(FlexAlign.End)

        Button(this.primaryLabel())
          .fontSize(14)
          .fontColor(Color.White)
          .backgroundColor($r('app.color.brand'))
          .height(40)
          .borderRadius(12)
          .onClick(() => {
            this.onPrimary(this.view.id);
          })
        Text('⋯')
          .fontSize(18)
          .fontColor($r('app.color.text_secondary'))
          .width(40)
          .height(40)
          .textAlign(TextAlign.Center)
          .borderRadius(12)
          .bindMenu(this.menuItems())
      }
      .width('100%')

      if (this.view.statusKind === 'trust') {
        Column({ space: 8 }) {
          Text('服务端指纹：' + this.view.statusFingerprint)
            .fontSize(13)
            .fontWeight(FontWeight.Medium)
          Text('请与服务端核对。确认后记住该指纹，之后不一致将拒绝连接。')
            .fontSize(12)
            .fontColor($r('app.color.text_secondary'))
          Button('确认并信任')
            .fontSize(14)
            .fontColor(Color.White)
            .backgroundColor($r('app.color.brand'))
            .height(40)
            .borderRadius(12)
            .onClick(() => {
              this.onConfirmTrust(this.view.id, this.view.statusFingerprint);
            })
        }
        .width('100%')
        .padding(12)
        .borderRadius(12)
        .backgroundColor($r('app.color.warning_surface'))
        .alignItems(HorizontalAlign.Start)
      }
    }
    .width('100%')
    .padding(12)
    .borderRadius(14)
    .backgroundColor($r('app.color.card_background'))
  }
}

@Entry
@Component
struct Index {
  @State private connections: ConnectionEntry[] = [];
  @State private selectedGroup: string = '__all__';
  @State private searchText: string = '';
  @State private hint: string = '';
  @State private hintIsError: boolean = false;
  // Form overlay state.
  @State private formVisible: boolean = false;
  @State private editingId: string = '';
  @State private formName: string = '';
  @State private formHost: string = '';
  @State private formPort: string = DEFAULT_PORT;
  @State private formGroup: string = '';
  @State private formPassword: string = '';
  @State private formSavePassword: boolean = false;
  @State private formNoPassword: boolean = false;
  // Move-group overlay state.
  @State private moveTargetId: string = '';
  @State private moveGroupText: string = '';
  private started: boolean = false;
  // In-flight connect per connection id: a second click while createSession /
  // openRemoteWindow is still running would create a second session AND a
  // second window for the same connection. A Set (not one string) so parallel
  // connects for different entries cannot clear each other's guard.
  private connectingEntryIds: Set<string> = new Set<string>();
  // Sessions whose window is being opened right now: a double click must not
  // start two multiton instances for the same session.
  private openingSessions: Set<string> = new Set<string>();
  private listener: () => void = (): void => {};
  private hintTimer: number = -1;
  private viewMap: Map<string, ConnectionView> = new Map<string, ConnectionView>();

  aboutToAppear(): void {
    if (!this.started) {
      this.started = true;
      this.connections = savedConnections();
      this.syncViews();
      // Status changes refresh the affected cards through their ConnectionView
      // (@ObjectLink), NOT through the ForEach key: keying on a registry
      // version rebuilt every card on every session tick.
      this.listener = (): void => {
        this.syncViews();
      };
      addSessionsListener(this.listener);
      diagMultiSessionAvailable();
    }
  }

  aboutToDisappear(): void {
    removeSessionsListener(this.listener);
    this.clearHintTimer();
  }

  // -------------------------------------------------------------------------
  // Data plumbing.
  // -------------------------------------------------------------------------

  private endpointOf(entry: ConnectionEntry): string {
    return entry.host + ':' + entry.port;
  }

  private statusOf(entry: ConnectionEntry): RowStatus {
    const sessions: RuntimeSession[] = sessionsForConnection(entry.id);
    if (sessions.length === 0) {
      return { kind: 'idle', label: '未连接', count: 0, fingerprint: '' };
    }
    let connected = 0;
    for (let i = 0; i < sessions.length; i++) {
      const state = sessions[i].state;
      if (state === 'trust-required') {
        return {
          kind: 'trust', label: '需要确认身份', count: sessions.length,
          fingerprint: sessions[i].fingerprint
        };
      }
      if (state === 'failed') {
        return { kind: 'failed', label: '失败', count: sessions.length, fingerprint: '' };
      }
      if (state === 'reconnecting') {
        return { kind: 'reconnecting', label: '重连中', count: sessions.length, fingerprint: '' };
      }
      if (state === 'connecting') {
        return { kind: 'connecting', label: '连接中', count: sessions.length, fingerprint: '' };
      }
      if (state === 'disconnected') {
        return { kind: 'disconnected', label: '已断开', count: sessions.length, fingerprint: '' };
      }
      if (state === 'connected') {
        connected += 1;
      }
    }
    if (connected > 0) {
      return { kind: 'connected', label: '已连接·' + connected + '个', count: connected, fingerprint: '' };
    }
    return { kind: 'idle', label: '未连接', count: 0, fingerprint: '' };
  }

  private groupOf(entry: ConnectionEntry): string {
    return entry.group.length > 0 ? entry.group : UNGROUPED;
  }

  private groupsWithCounts(): string[] {
    const names: string[] = ['__all__'];
    const seen = new Set<string>();
    for (let i = 0; i < this.connections.length; i++) {
      const group = this.groupOf(this.connections[i]);
      if (group !== UNGROUPED && !seen.has(group)) {
        seen.add(group);
        names.push(group);
      }
    }
    // 未分组 last, only when something is ungrouped.
    for (let i = 0; i < this.connections.length; i++) {
      if (this.connections[i].group.length === 0) {
        names.push(UNGROUPED);
        break;
      }
    }
    return names;
  }

  private filteredConnections(): ConnectionEntry[] {
    const query = this.searchText.trim().toLowerCase();
    return this.connections.filter((entry: ConnectionEntry): boolean => {
      if (this.selectedGroup !== '__all__' && this.groupOf(entry) !== this.selectedGroup) {
        return false;
      }
      if (query.length === 0) {
        return true;
      }
      return entry.name.toLowerCase().indexOf(query) >= 0 ||
        this.endpointOf(entry).toLowerCase().indexOf(query) >= 0;
    });
  }

  /** Rows for the list: the filtered connections' persistent view objects. */
  private filteredViews(): ConnectionView[] {
    const entries = this.filteredConnections();
    const views: ConnectionView[] = [];
    for (let i = 0; i < entries.length; i++) {
      const view = this.viewMap.get(entries[i].id);
      if (view !== undefined) {
        views.push(view);
      }
    }
    return views;
  }

  private groupCount(group: string): number {
    if (group === '__all__') {
      return this.connections.length;
    }
    if (group === UNGROUPED) {
      return this.connections.filter((entry: ConnectionEntry): boolean =>
        entry.group.length === 0).length;
    }
    return this.connections.filter((entry: ConnectionEntry): boolean =>
      this.groupOf(entry) === group).length;
  }

  /**
   * Pushes the latest stored entry + live session status into the persistent
   * ConnectionView, creating it on first sight and mutating it afterwards so
   * @ObjectLink cards refresh in place.
   */
  private viewFor(entry: ConnectionEntry): ConnectionView {
    let view = this.viewMap.get(entry.id);
    if (view === undefined) {
      view = new ConnectionView(entry);
      this.viewMap.set(entry.id, view);
    }
    // Assign only real changes: @ObjectLink cards are notified per assignment,
    // so a plain 2 s registry tick must not dirty every card.
    const endpoint = this.endpointOf(entry);
    const lastConnectedText = this.formatTime(entry.lastConnectedAt);
    const status = this.statusOf(entry);
    const hasSessions = sessionsForConnection(entry.id).length > 0;
    if (view.entry !== entry) {
      view.entry = entry;
    }
    if (view.name !== entry.name) {
      view.name = entry.name;
    }
    if (view.endpoint !== endpoint) {
      view.endpoint = endpoint;
    }
    if (view.lastConnectedText !== lastConnectedText) {
      view.lastConnectedText = lastConnectedText;
    }
    if (view.statusKind !== status.kind) {
      view.statusKind = status.kind;
    }
    if (view.statusLabel !== status.label) {
      view.statusLabel = status.label;
    }
    if (view.statusFingerprint !== status.fingerprint) {
      view.statusFingerprint = status.fingerprint;
    }
    if (view.hasSessions !== hasSessions) {
      view.hasSessions = hasSessions;
    }
    return view;
  }

  /** Refreshes every row's view model against the connection/session tables. */
  private syncViews(): void {
    const alive = new Set<string>();
    for (let i = 0; i < this.connections.length; i++) {
      const entry = this.connections[i];
      alive.add(entry.id);
      this.viewFor(entry);
    }
    const stale: string[] = [];
    this.viewMap.forEach((view: ConnectionView, key: string) => {
      if (!alive.has(key)) {
        stale.push(key);
      }
    });
    for (let i = 0; i < stale.length; i++) {
      this.viewMap.delete(stale[i]);
    }
  }

  private formatTime(ms: number): string {
    if (ms <= 0) {
      return '未连接过';
    }
    const date = new Date(ms);
    const pad = (value: number): string => value < 10 ? '0' + value : String(value);
    return pad(date.getMonth() + 1) + '-' + pad(date.getDate()) + ' ' +
      pad(date.getHours()) + ':' + pad(date.getMinutes());
  }

  // -------------------------------------------------------------------------
  // Actions.
  // -------------------------------------------------------------------------

  private showHint(text: string, isError: boolean = false): void {
    this.hint = text;
    this.hintIsError = isError;
    if (this.hintTimer >= 0) {
      clearTimeout(this.hintTimer);
    }
    this.hintTimer = setTimeout((): void => {
      this.hint = '';
      this.hintIsError = false;
      this.hintTimer = -1;
    }, HINT_TIMEOUT_MS);
  }

  private clearHintTimer(): void {
    if (this.hintTimer >= 0) {
      clearTimeout(this.hintTimer);
      this.hintTimer = -1;
    }
  }

  private openFormNew(): void {
    this.editingId = '';
    this.formName = '';
    this.formHost = '';
    this.formPort = DEFAULT_PORT;
    this.formGroup = this.selectedGroup === '__all__' || this.selectedGroup === UNGROUPED
      ? '' : this.selectedGroup;
    this.formPassword = '';
    this.formSavePassword = false;
    this.formNoPassword = false;
    this.formVisible = true;
  }

  private openFormEdit(entry: ConnectionEntry): void {
    this.editingId = entry.id;
    this.formName = entry.name;
    this.formHost = entry.host;
    this.formPort = entry.port;
    this.formGroup = entry.group;
    this.formSavePassword = entry.savePassword;
    this.formNoPassword = entry.noPassword === true;
    this.formPassword = '';
    this.formVisible = true;
    if (entry.savePassword) {
      loadPassword(entry).then((secret: string): void => {
        // The user may have opened another connection's form (or closed this
        // one) while the keystore read was pending: a late result must not
        // write this connection's password into someone else's form.
        if (!this.formVisible || this.editingId !== entry.id) {
          return;
        }
        if (secret.length > 0 && !this.formNoPassword) {
          this.formPassword = secret;
        }
      });
    }
  }

  private formEntry(): ConnectionEntry | undefined {
    if (this.formHost.trim().length === 0) {
      this.showHint('请填写服务器地址', true);
      return undefined;
    }
    const port = this.formPort.trim().length > 0 ? this.formPort.trim() : DEFAULT_PORT;
    const portNumber = Number(port);
    if (!Number.isInteger(portNumber) || portNumber <= 0 || portNumber > 65535) {
      this.showHint('端口无效（1-65535）', true);
      return undefined;
    }
    // Editing keeps the original opaque id, so changing host/port updates that
    // record instead of leaving the old one behind and adding a second one.
    const previous = this.editingId.length > 0 ? findConnection(this.editingId) : undefined;
    const entry: ConnectionEntry = {
      id: previous === undefined ? newConnectionId() : previous.id,
      name: this.formName.trim().length > 0 ? this.formName.trim() : this.formHost.trim(),
      host: this.formHost.trim(),
      port: port,
      group: this.formGroup.trim(),
      savePassword: this.formNoPassword ? false : this.formSavePassword,
      noPassword: this.formNoPassword,
      lastConnectedAt: previous === undefined ? 0 : previous.lastConnectedAt
    };
    return entry;
  }

  private saveForm(andConnect: boolean): void {
    const entry = this.formEntry();
    if (entry === undefined) {
      return;
    }
    const stored = upsertConnection(entry);
    this.connections = savedConnections();
    this.syncViews();
    const password = this.formNoPassword ? '' : this.formPassword;
    if (!entry.noPassword && entry.savePassword && password.length > 0) {
      savePassword(stored, password).then((ok: boolean): void => {
        this.showHint(ok ? '已保存连接与密码（系统密钥库）' : '已保存连接，但密码保存失败', !ok);
      });
    } else {
      if (!entry.savePassword) {
        removePassword(stored);
      }
      this.showHint(entry.noPassword ? '已保存连接（该服务器无需密码）' : '已保存连接（未保存密码）');
    }
    this.formVisible = false;
    if (andConnect) {
      this.connect(stored, password);
    }
  }

  private duplicateEntry(entry: ConnectionEntry): void {
    const copy: ConnectionEntry = {
      // Copy always gets a NEW opaque id; host:port alone must never decide it.
      id: newConnectionId(),
      name: entry.name + ' 副本',
      host: entry.host,
      port: entry.port,
      group: entry.group,
      savePassword: false, // 复制配置默认不带密码
      noPassword: entry.noPassword,
      lastConnectedAt: 0
    };
    upsertConnection(copy);
    this.connections = savedConnections();
    this.syncViews();
    this.showHint('已复制连接（不含密码）');
  }

  private deleteEntry(entry: ConnectionEntry): void {
    // Ending live sessions of this connection first keeps the window set tidy.
    const sessions = sessionsForConnection(entry.id);
    for (let i = 0; i < sessions.length; i++) {
      const sessionId = sessions[i].sessionId;
      closeSessionById(sessionId);
      // The window of a dead session must not be left behind.
      closeRemoteWindow(sessionId).then((): void => {});
    }
    removeConnection(entry.id);
    removePassword(entry);
    this.connections = savedConnections();
    this.syncViews();
    this.showHint('已删除连接');
  }

  private openMove(entry: ConnectionEntry): void {
    this.moveTargetId = entry.id;
    this.moveGroupText = entry.group;
  }

  private applyMove(): void {
    const entry = findConnection(this.moveTargetId);
    if (entry !== undefined) {
      entry.group = this.moveGroupText.trim();
      upsertConnection(entry);
      this.connections = savedConnections();
      this.syncViews();
      this.showHint('已移动分组');
    }
    this.moveTargetId = '';
  }

  private isReconnectKind(kind: string): boolean {
    return kind === 'failed' || kind === 'disconnected' || kind === 'reconnecting';
  }

  /** Primary action: reconnect when the row is in a recoverable state. */
  private primaryAction(entry: ConnectionEntry): void {
    if (this.isReconnectKind(this.statusOf(entry).kind)) {
      this.reconnectEntry(entry);
      return;
    }
    const sessions = sessionsForConnection(entry.id);
    if (sessions.length > 0) {
      // Same session clicked again: activate its window instead of a new one.
      this.activateSession(sessions[0].sessionId, sessions[0].name);
      return;
    }
    this.connect(entry, '');
  }

  /** Reconnect every session of this connection that is failed/disconnected. */
  private reconnectEntry(entry: ConnectionEntry): void {
    const sessions = sessionsForConnection(entry.id);
    let count = 0;
    for (let i = 0; i < sessions.length; i++) {
      const state = sessions[i].state;
      if (state === 'failed' || state === 'disconnected' || state === 'reconnecting') {
        reconnectById(sessions[i].sessionId);
        count += 1;
      }
    }
    if (count > 0) {
      this.showHint('正在重连：' + entry.name);
    } else {
      this.showHint('没有可重连的会话', true);
    }
  }

  // -------------------------------------------------------------------------
  // Card callbacks: the card only knows the id, the parent resolves the entry
  // so an edited/renamed connection is always acted on with fresh data.
  // -------------------------------------------------------------------------

  private primaryActionById(id: string): void {
    const entry = findConnection(id);
    if (entry !== undefined) {
      this.primaryAction(entry);
    }
  }

  private reconnectEntryById(id: string): void {
    const entry = findConnection(id);
    if (entry !== undefined) {
      this.reconnectEntry(entry);
    }
  }

  private newSessionById(id: string): void {
    const entry = findConnection(id);
    if (entry !== undefined) {
      this.connect(entry, '');
    }
  }

  private editEntryById(id: string): void {
    const entry = findConnection(id);
    if (entry !== undefined) {
      this.openFormEdit(entry);
    }
  }

  private copyEntryById(id: string): void {
    const entry = findConnection(id);
    if (entry !== undefined) {
      this.duplicateEntry(entry);
    }
  }

  private moveEntryById(id: string): void {
    const entry = findConnection(id);
    if (entry !== undefined) {
      this.openMove(entry);
    }
  }

  private deleteEntryById(id: string): void {
    const entry = findConnection(id);
    if (entry !== undefined) {
      this.deleteEntry(entry);
    }
  }

  private confirmTrustById(id: string, fingerprint: string): void {
    const entry = findConnection(id);
    if (entry !== undefined) {
      this.confirmTrust(entry, fingerprint);
    }
  }

  private abilityContext(): common.UIAbilityContext | undefined {
    try {
      return this.getUIContext().getHostContext() as common.UIAbilityContext;
    } catch (error) {
      hilog.warn(DOMAIN, TAG, '获取主窗口 ability context 失败：%{public}s', String(error));
      return undefined;
    }
  }

  private activateSession(sessionId: string, displayName: string): void {
    // Opening is async; a double click used to start two opens, and multiton
    // does not deduplicate, so two instances (double polling, two input
    // sources) would exist for one session. Guard per session id and release
    // once the open finishes.
    if (this.openingSessions.has(sessionId)) {
      return;
    }
    this.openingSessions.add(sessionId);
    // displayName only titles the window; the session itself is always the id.
    openRemoteWindow(sessionId, displayName, this.abilityContext()).then((result: string): void => {
      if (result === 'failed') {
        // No window means no session (the opener already closed it). Report it
        // instead of quietly showing the session somewhere else.
        this.showHint('无法打开远程窗口，会话已结束，请重试', true);
      } else if (result === 'no-stage') {
        this.showHint('窗口环境不可用，无法打开远程窗口', true);
      }
    }).catch((error: Error): void => {
      // A rejected open (window APIs can throw on an abnormal window state)
      // must surface as a hint, never as an unhandled rejection: that aborts
      // the whole app.
      hilog.warn(DOMAIN, TAG, '激活/打开远程窗口异常：%{public}s', String(error));
      this.showHint('无法打开远程窗口，请重试', true);
    }).finally((): void => {
      this.openingSessions.delete(sessionId);
    });
  }

  private async connect(entry: ConnectionEntry, explicitPassword: string): Promise<void> {
    if (!diagMultiSessionAvailable()) {
      this.showHint('原生多会话接口尚未就绪（等待原生层升级后重试）', true);
      return;
    }
    // One in-flight connect per connection: the button stays live, a second
    // click while this one runs is ignored instead of creating a second
    // session and a second window. A Set keeps per-entry state so starting B
    // while A waits on its password cannot clear A's guard.
    if (this.connectingEntryIds.has(entry.id)) {
      return;
    }
    this.connectingEntryIds.add(entry.id);
    try {
      let password = explicitPassword;
      if (password.length === 0 && entry.savePassword) {
        password = await loadPassword(entry);
      }
      if (password.length === 0 && entry.noPassword !== true) {
        // No password available: open the form focused on the password field.
        this.openFormEdit(entry);
        this.showHint('请输入密码后再连接', true);
        return;
      }
      const endpoint = this.endpointOf(entry);
      const options: SessionOptions = {
        host: entry.host,
        port: Number(entry.port),
        name: entry.name
      };
      // Servers that publish only None (type 1) reject the encrypted-only
      // default, so a "no password" connection offers None explicitly.
      if (entry.noPassword === true) {
        options.security = 'None';
      }
      // password is optional in the contract: omit it entirely for servers that
      // need none instead of sending an empty credential.
      if (password.length > 0) {
        options.password = password;
      }
      const pin = readTrustedFingerprint(endpoint);
      if (pin.length > 0) {
        options.expectedFingerprint = pin;
      }
      const sessionId = createSession(options);
      if (sessionId.length === 0) {
        this.showHint('创建会话失败（' + endpoint + '）', true);
        return;
      }
      // Register before opening the window: the page resolves its id through the
      // registry when the AppStorage keys cannot answer. lastConnectedAt is
      // stamped by the registry poll when the session reaches connected.
      registerSession(sessionId, entry.id, entry.name, endpoint);
      this.showHint('连接中：' + entry.name);
      const result = await openRemoteWindow(sessionId, entry.name, this.abilityContext());
      if (result === 'failed') {
        // The opener closed the session because no window could be created.
        this.showHint('无法打开远程窗口，会话已结束，请重试', true);
      } else if (result === 'no-stage') {
        this.showHint('窗口环境不可用，无法打开远程窗口', true);
      }
    } catch (error) {
      // createSession/openRemoteWindow throwing must not escape as an
      // unhandled rejection (that aborts the app); report and let the button
      // act again.
      hilog.warn(DOMAIN, TAG, '连接流程异常：%{public}s', String(error));
      this.showHint('连接失败：' + String(error), true);
    } finally {
      // Result received (success or failure): let the button act again. Only
      // this entry's guard is cleared, so another entry's connect keeps its own.
      this.connectingEntryIds.delete(entry.id);
    }
  }

  /** 确认并信任 (inline on a trust-required row). */
  private confirmTrust(entry: ConnectionEntry, fingerprint: string): void {
    if (fingerprint.length === 0) {
      return;
    }
    trustFingerprint(this.endpointOf(entry), fingerprint);
    const sessions = sessionsForConnection(entry.id);
    for (let i = 0; i < sessions.length; i++) {
      const state = sessions[i].state;
      // Only the sessions that actually need a new attempt: reconnecting a
      // live session bumps its generation and drops frames for nothing.
      if (state === 'trust-required' || state === 'failed' || state === 'disconnected') {
        reconnectById(sessions[i].sessionId);
      }
    }
    this.showHint('已信任指纹，正在重连');
  }

  // -------------------------------------------------------------------------
  // Build.
  // -------------------------------------------------------------------------

  @Builder
  private groupItem(label: string, key: string, count: number) {
    Row({ space: 8 }) {
      Text(label)
        .fontSize(14)
        .fontColor(this.selectedGroup === key
          ? $r('app.color.brand') : $r('app.color.text_primary'))
        .maxLines(1)
        .layoutWeight(1)
      Text(String(count))
        .fontSize(12)
        .fontColor($r('app.color.text_secondary'))
    }
    .height(40)
    .width('100%')
    .padding({ left: 12, right: 12 })
    .borderRadius(12)
    .backgroundColor(this.selectedGroup === key
      ? $r('app.color.selected_surface') : Color.Transparent)
    .onClick(() => {
      this.selectedGroup = key;
    })
  }

  @Builder
  private formOverlay() {
    Column({ space: 12 }) {
      Text(this.editingId.length > 0 ? '编辑连接' : '新建连接')
        .fontSize(18)
        .fontWeight(FontWeight.Bold)
      TextInput({ text: this.formName, placeholder: '连接名称' })
        .height(40)
        .onChange((value: string) => { this.formName = value; })
      Row({ space: 8 }) {
        TextInput({ text: this.formHost, placeholder: '服务器地址' })
          .height(40)
          .layoutWeight(2)
          .onChange((value: string) => { this.formHost = value; })
        TextInput({ text: this.formPort, placeholder: '端口' })
          .height(40)
          .type(InputType.Number)
          .layoutWeight(1)
          .onChange((value: string) => { this.formPort = value; })
      }.width('100%')
      TextInput({ text: this.formGroup, placeholder: '分组（可留空）' })
        .height(40)
        .onChange((value: string) => { this.formGroup = value; })
      Row({ space: 8 }) {
        Checkbox()
          .select(this.formNoPassword)
          .onChange((checked: boolean) => {
            this.formNoPassword = checked;
            if (checked) {
              this.formPassword = '';
              this.formSavePassword = false;
            }
          })
        Text('该服务器无需密码').fontSize(14)
      }
      TextInput({ text: this.formPassword, placeholder: '密码（不回显）' })
        .height(40)
        .type(InputType.Password)
        .enabled(!this.formNoPassword)
        .onChange((value: string) => {
          this.formPassword = value;
        })
      Row({ space: 8 }) {
        Checkbox()
          .select(this.formSavePassword)
          .enabled(!this.formNoPassword)
          .onChange((checked: boolean) => { this.formSavePassword = checked; })
        Text('保存密码（系统密钥库）')
          .fontSize(14)
          .fontColor(this.formNoPassword
            ? $r('app.color.text_secondary') : $r('app.color.text_primary'))
      }
      Row({ space: 12 }) {
        Button('取消')
          .fontSize(14)
          .height(40)
          .layoutWeight(1)
          .borderRadius(12)
          .backgroundColor($r('app.color.chip_background'))
          .fontColor($r('app.color.text_primary'))
          .onClick(() => { this.formVisible = false; })
        Button('保存')
          .fontSize(14)
          .height(40)
          .layoutWeight(1)
          .borderRadius(12)
          .backgroundColor($r('app.color.brand'))
          .fontColor(Color.White)
          .onClick(() => { this.saveForm(false); })
        Button('保存并连接')
          .fontSize(14)
          .height(40)
          .layoutWeight(1.4)
          .borderRadius(12)
          .backgroundColor($r('app.color.brand'))
          .fontColor(Color.White)
          .onClick(() => { this.saveForm(true); })
      }.width('100%')
    }
    .width('86%')
    .padding(24)
    .borderRadius(16)
    .backgroundColor($r('app.color.card_background'))
  }

  @Builder
  private moveOverlay() {
    Column({ space: 12 }) {
      Text('移动到分组').fontSize(18).fontWeight(FontWeight.Bold)
      TextInput({ text: this.moveGroupText, placeholder: '分组名称（留空 = 未分组）' })
        .height(40)
        .onChange((value: string) => { this.moveGroupText = value; })
      Row({ space: 12 }) {
        Button('取消')
          .fontSize(14)
          .height(40)
          .layoutWeight(1)
          .borderRadius(12)
          .backgroundColor($r('app.color.chip_background'))
          .fontColor($r('app.color.text_primary'))
          .onClick(() => { this.moveTargetId = ''; })
        Button('确定')
          .fontSize(14)
          .height(40)
          .layoutWeight(1)
          .borderRadius(12)
          .backgroundColor($r('app.color.brand'))
          .fontColor(Color.White)
          .onClick(() => { this.applyMove(); })
      }.width('100%')
    }
    .width('70%')
    .padding(24)
    .borderRadius(16)
    .backgroundColor($r('app.color.card_background'))
  }

  build() {
    Stack() {
      Column() {
        // Top bar: search + new.
        Row({ space: 12 }) {
          Text('天关').fontSize(20).fontWeight(FontWeight.Bold)
            .fontColor($r('app.color.text_primary'))
          Row({ space: 8 }) {
            TextInput({ text: this.searchText, placeholder: '搜索名称或 host:port' })
              .height(40)
              .layoutWeight(1)
              .backgroundColor($r('app.color.chip_background'))
              .onChange((value: string) => { this.searchText = value; })
            Button('新建连接')
              .fontSize(14)
              .height(40)
              .borderRadius(12)
              .backgroundColor($r('app.color.brand'))
              .fontColor(Color.White)
              .onClick(() => { this.openFormNew(); })
          }
          .layoutWeight(1)
        }
        .width('100%')
        .padding({ left: 16, right: 16, top: 12, bottom: 12 })

        Row({ space: 0 }) {
          // Left group column.
          Column({ space: 4 }) {
            Scroll() {
              Column({ space: 4 }) {
                ForEach(this.groupsWithCounts(), (group: string) => {
                  if (group === '__all__') {
                    this.groupItem('全部', '__all__', this.groupCount(group))
                  } else if (group === UNGROUPED) {
                    this.groupItem(UNGROUPED, UNGROUPED, this.groupCount(group))
                  } else {
                    this.groupItem(group, group, this.groupCount(group))
                  }
                }, (group: string) => group + '#' + String(this.groupCount(group)) +
                  (this.selectedGroup === group ? '#selected' : ''))
              }
              .width('100%')
            }
            .scrollBar(BarState.Off)
            .layoutWeight(1)
          }
          .width(148)
          .height('100%')
          .padding({ left: 8, right: 8, top: 4, bottom: 8 })
          .backgroundColor($r('app.color.side_background'))
          .alignItems(HorizontalAlign.Start)

          // Right connection list.
          Column() {
            if (this.connections.length === 0) {
              Column({ space: 12 }) {
                Text('还没有连接').fontSize(18).fontWeight(FontWeight.Medium)
                Text('新建一个连接，连接成功后远程桌面会在独立窗口打开。')
                  .fontSize(13)
                  .fontColor($r('app.color.text_secondary'))
                Button('新建连接')
                  .fontSize(14)
                  .height(40)
                  .borderRadius(12)
                  .backgroundColor($r('app.color.brand'))
                  .fontColor(Color.White)
                  .onClick(() => { this.openFormNew(); })
              }
              .justifyContent(FlexAlign.Center)
              .layoutWeight(1)
            } else {
              Scroll() {
                Column({ space: 8 }) {
                  if (this.filteredConnections().length === 0) {
                    Text('没有匹配的连接')
                      .fontSize(14)
                      .fontColor($r('app.color.text_secondary'))
                      .padding(24)
                  }
                  ForEach(this.filteredViews(), (view: ConnectionView) => {
                    ConnectionCard({
                      view: view,
                      onPrimary: (id: string): void => {
                        this.primaryActionById(id);
                      },
                      onReconnect: (id: string): void => {
                        this.reconnectEntryById(id);
                      },
                      onNewSession: (id: string): void => {
                        this.newSessionById(id);
                      },
                      onEdit: (id: string): void => {
                        this.editEntryById(id);
                      },
                      onCopy: (id: string): void => {
                        this.copyEntryById(id);
                      },
                      onMove: (id: string): void => {
                        this.moveEntryById(id);
                      },
                      onDelete: (id: string): void => {
                        this.deleteEntryById(id);
                      },
                      onConfirmTrust: (id: string, fingerprint: string): void => {
                        this.confirmTrustById(id, fingerprint);
                      }
                    })
                  }, (view: ConnectionView) => view.id)
                }
                .width('100%')
                .padding({ left: 16, right: 16, top: 8, bottom: 16 })
              }
              .scrollBar(BarState.Auto)
              .layoutWeight(1)
              .align(Alignment.Start)
            }
          }
          .layoutWeight(1)
          .height('100%')
        }
        .layoutWeight(1)

        if (this.hint.length > 0) {
          Text(this.hint)
            .fontSize(13)
            .fontColor(this.hintIsError
              ? $r('app.color.danger') : $r('app.color.text_secondary'))
            .width('100%')
            .padding({ left: 16, right: 16, top: 8, bottom: 8 })
            .maxLines(1)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
        }
      }
      .width('100%')
      .height('100%')
      .backgroundColor($r('app.color.page_background'))

      // Overlays. The dimming layer and the panel are SIBLINGS on the Stack:
      // only the dimming layer carries the close click, so a click inside the
      // panel (title, blank area, input fields) can no longer bubble into it
      // and close the panel under the pointer.
      if (this.formVisible) {
        Column()
          .width('100%')
          .height('100%')
          .backgroundColor('rgba(0,0,0,0.45)')
          .onClick(() => { this.formVisible = false; })
        this.formOverlay()
      }
      if (this.moveTargetId.length > 0) {
        Column()
          .width('100%')
          .height('100%')
          .backgroundColor('rgba(0,0,0,0.45)')
          .onClick(() => { this.moveTargetId = ''; })
        this.moveOverlay()
      }
    }
    .width('100%')
    .height('100%')
  }
}
