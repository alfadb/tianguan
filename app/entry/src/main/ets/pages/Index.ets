/**
 * Main window: connection manager ONLY (the remote desktop itself lives in one
 * independent RemoteAbility window per session).
 *
 *  - left: single-level group column (全部 / groups / 未分组);
 *  - right: connection cards (name, host:port, status, last-connected time,
 *    primary action + more menu 重连/新建会话/编辑/复制/移组/删除);
 *  - top: search (name + host:port) and 新建连接;
 *  - new/edit form overlay: 名称/协议(VNC|RDP|Sunshine)/地址/端口/[RDP 用户名/域]/
 *    [Sunshine 应用名 + 画质档位/自定义宽高帧率码率编码]/分组/
 *    无需密码(VNC)/密码/保存密码 + 保存 / 保存并连接;
 *  - per-connection live status from the SessionRegistry (未连接/连接中/已连接·N个/
 *    重连中/已断开/需要确认身份/需要配对/失败), fingerprint confirm inline.
 *
 * Cards are keyed by entry.id only. Their live data (status, action label,
 * edited fields) lives in an @Observed ConnectionView that the card component
 * observes, so a session-status tick refreshes just the affected cards instead
 * of re-creating every card (the previous key embedded registryVersion).
 */
import { hilog } from '@kit.PerformanceAnalysisKit';
import { display } from '@kit.ArkUI';
import { fileIo as fs } from '@kit.CoreFileKit';
import type { common } from '@kit.AbilityKit';
import {
  createSession, diagMultiSessionAvailable, forgetTrustedFingerprint,
  SESSION_ERROR_SESSION_TAKEN_OVER, SESSION_STATE_PAIRING_REQUIRED
} from '../diag/SessionApi';
import type { SessionOptions } from '../diag/SessionApi';
import {
  findConnection, forgetFingerprint, loadPassword, newConnectionId, passwordAliasFor, pinKeyFor,
  readTrustedFingerprint, removeConnection, removePassword, savePassword, savedConnections,
  trustFingerprint, upsertConnection, DEFAULT_PORT, defaultPortFor, UNGROUPED,
  normalizeSunshineQuality, sunshineFormatsFor, sunshinePresetFor, isSunshineCodec,
  isSunshinePreset, isValidSunshineBitrateMbps, isValidSunshineFps, isValidSunshineSize,
  SUNSHINE_CODEC_AUTO, SUNSHINE_CODEC_H264, SUNSHINE_CODEC_HEVC,
  SUNSHINE_PRESET_BALANCED, SUNSHINE_PRESET_CUSTOM, SUNSHINE_PRESETS
} from '../model/ConnectionStore';
import type { ConnectionEntry, SunshinePreset } from '../model/ConnectionStore';
import {
  addSessionsListener, removeSessionsListener, sessionsForConnection, allSessions, findSession,
  registerSession, closeSessionById, reconnectById
} from '../model/SessionRegistry';
import type { RuntimeSession } from '../model/SessionRegistry';
import { closeRemoteWindow, openRemoteWindow } from '../windows/RemoteWindows';
import {
  isPrivacyAccepted, loadPrivacyPolicy, readAppVersion, setPrivacyAccepted
} from '../privacy/PrivacyStore';

const DOMAIN: number = 0x0000;
const TAG: string = 'TianguanDiag';
const HINT_TIMEOUT_MS: number = 5000;
/**
 * Below this window width (vp) the two-column layout collapses: the group
 * column is replaced by one horizontally scrollable chip row and the cards get
 * their stacked narrow layout. 600 vp is a phone in portrait (376 vp on the
 * PLA-AL10) or a narrow phone-like window on 2in1; a normal PC window is
 * wider.
 */
const NARROW_BREAKPOINT_VP: number = 600;

/** Aggregated per-connection status for one list row. */
interface RowStatus {
  kind: string; // idle | connecting | connected | reconnecting | disconnected | trust | failed
  label: string;
  count: number;
  fingerprint: string;
  /** Native failure code of the failed session ('' otherwise). */
  errorCode: string;
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
  /** 'vnc' | 'rdp' | 'sunshine' — shown as the label in front of the endpoint. */
  protocol: string;
  endpoint: string;
  lastConnectedText: string;
  statusKind: string;
  statusLabel: string;
  statusFingerprint: string;
  /** Failed-session error code; only 'fingerprint-mismatch' gets a panel. */
  statusErrorCode: string;
  /** Pin the trust store held when the mismatch panel appeared ('' = none). */
  mismatchTrusted: string;
  hasSessions: boolean;

  constructor(entry: ConnectionEntry) {
    this.id = entry.id;
    this.entry = entry;
    this.name = entry.name;
    this.protocol = entry.protocol === 'rdp' ? 'rdp' :
      (entry.protocol === 'sunshine' ? 'sunshine' : 'vnc');
    this.endpoint = entry.host + ':' + entry.port;
    this.lastConnectedText = '';
    this.statusKind = 'idle';
    this.statusLabel = '未连接';
    this.statusFingerprint = '';
    this.statusErrorCode = '';
    this.mismatchTrusted = '';
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
  /** True below NARROW_BREAKPOINT_VP: stacked layout, actions on their own row. */
  @Prop narrow: boolean = false;
  onPrimary: (id: string) => void = (): void => {};
  onReconnect: (id: string) => void = (): void => {};
  onNewSession: (id: string) => void = (): void => {};
  onDisconnect: (id: string) => void = (): void => {};
  onEdit: (id: string) => void = (): void => {};
  onCopy: (id: string) => void = (): void => {};
  onMove: (id: string) => void = (): void => {};
  onDelete: (id: string) => void = (): void => {};
  onConfirmTrust: (id: string, fingerprint: string) => void = (): void => {};
  onResetTrust: (id: string) => void = (): void => {};

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

  /** "Sunshine  192.168.50.62:47989" — protocol label before the endpoint. */
  private protocolEndpoint(): string {
    const label = this.view.protocol === 'rdp' ? 'RDP' :
      (this.view.protocol === 'sunshine' ? 'Sunshine' : 'VNC');
    return label + '  ' + this.view.endpoint;
  }

  private statusColor(): Resource {
    switch (this.view.statusKind) {
      case 'connected':
        return $r('app.color.success');
      case 'connecting':
        return $r('app.color.brand');
      case 'reconnecting':
      case 'trust':
      case 'pairing':
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
      // RDP: one Windows account owns one session window at most. Offering
      // 「新建会话」 here would be a direct way around that rule, so the entry
      // is VNC-only; 「连接」 on an RDP row switches to the existing window.
      if (this.view.protocol !== 'rdp') {
        items.push({
          value: '新建会话',
          action: (): void => {
            this.onNewSession(this.view.id);
          }
        });
      }
      // Ending the session from the list matters most on a phone: the session
      // window has no title bar there, so its own close affordance is all the
      // user has besides this menu row.
      items.push({
        value: '断开',
        action: (): void => {
          this.onDisconnect(this.view.id);
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
      if (this.narrow) {
        // Narrow: identity on its own full-width block (otherwise the fixed
        // status + button + menu eat the whole row and layoutWeight(1) squeezes
        // name/endpoint to 0 width), actions on a second row.
        Column({ space: 6 }) {
          Column({ space: 2 }) {
            Text(this.view.name)
              .fontSize(16)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('app.color.text_primary'))
              .maxLines(1)
              .textOverflow({ overflow: TextOverflow.Ellipsis })
              .width('100%')
            Text(this.protocolEndpoint())
              .fontSize(13)
              .fontColor($r('app.color.text_secondary'))
              .maxLines(1)
              .textOverflow({ overflow: TextOverflow.Ellipsis })
              .width('100%')
            Text(this.view.lastConnectedText)
              .fontSize(12)
              .fontColor($r('app.color.text_secondary'))
              .maxLines(1)
              .textOverflow({ overflow: TextOverflow.Ellipsis })
              .width('100%')
          }
          .alignItems(HorizontalAlign.Start)
          .width('100%')

          Row({ space: 8 }) {
            Circle({ width: 8, height: 8 })
              .fill(this.statusColor())
            Text(this.view.statusLabel)
              .fontSize(13)
              .fontColor(this.statusColor())
              .maxLines(1)
              .textOverflow({ overflow: TextOverflow.Ellipsis })
              .layoutWeight(1)
            Button(this.primaryLabel())
              .fontSize(14)
              .fontColor(Color.White)
              .backgroundColor($r('app.color.brand'))
              .height(36)
              .borderRadius(12)
              .onClick(() => {
                this.onPrimary(this.view.id);
              })
            Text('⋯')
              .fontSize(18)
              .fontColor($r('app.color.text_secondary'))
              .width(36)
              .height(36)
              .textAlign(TextAlign.Center)
              .borderRadius(12)
              .bindMenu(this.menuItems())
          }
          .width('100%')
        }
        .alignItems(HorizontalAlign.Start)
        .width('100%')
      } else {
        Row({ space: 12 }) {
          Column({ space: 4 }) {
            Text(this.view.name)
              .fontSize(16)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('app.color.text_primary'))
              .maxLines(1)
              .textOverflow({ overflow: TextOverflow.Ellipsis })
            Text(this.protocolEndpoint())
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
                .maxLines(1)
                .textOverflow({ overflow: TextOverflow.Ellipsis })
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
      }

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

      // Fingerprint changed since it was trusted: tell the user both values and
      // offer a re-confirmation. Resetting only makes the server show its
      // fingerprint again; nothing is trusted until 确认并信任 is pressed.
      // Sunshine is different: its server certificate is pinned natively during
      // pairing, so 重新确认指纹 does not apply (and this version has no NAPI to
      // clear that pin) — only the native reason is shown.
      if (this.view.statusKind === 'failed' &&
          this.view.statusErrorCode === 'fingerprint-mismatch') {
        Column({ space: 8 }) {
          Text(this.view.protocol === 'sunshine'
            ? 'Sunshine 的配对证书与已配对记录不一致，已拒绝连接'
            : '服务端指纹与已信任值不一致，已拒绝连接')
            .fontSize(13)
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.danger'))
          if (this.view.protocol === 'sunshine') {
            Text('Sunshine 会在配对时固定服务端证书，本机无法在应用内重置该记录。请在主机端删除本客户端的配对后重试。')
              .fontSize(12)
              .fontColor($r('app.color.text_secondary'))
          } else {
            Text('已信任：' + (this.view.mismatchTrusted.length > 0
              ? this.view.mismatchTrusted : '（未取到）'))
              .fontSize(12)
            Text('当前看到：' + (this.view.statusFingerprint.length > 0
              ? this.view.statusFingerprint : '（未取到）'))
              .fontSize(12)
            Text('指纹变化通常是因为服务器重装或更新了证书，也可能是连接被冒充。请先与服务端核对指纹。')
              .fontSize(12)
              .fontColor($r('app.color.text_secondary'))
            Button('重新确认指纹')
              .fontSize(14)
              .fontColor(Color.White)
              .backgroundColor($r('app.color.brand'))
              .height(40)
              .borderRadius(12)
              .onClick(() => {
                this.onResetTrust(this.view.id);
              })
          }
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
  /**
   * Width of this page's own window in vp (not the screen's): on 2in1 the
   * window can be resized narrower than the display. Seeded from the display
   * in aboutToAppear so the first frame already uses the right layout, then
   * kept current by the root's onAreaChange.
   */
  @State private viewportWidth: number = 0;
  // Form overlay state.
  @State private formVisible: boolean = false;
  @State private editingId: string = '';
  @State private formName: string = '';
  @State private formHost: string = '';
  @State private formProtocol: string = 'vnc';
  @State private formUsername: string = '';
  @State private formDomain: string = '';
  /** Sunshine only: /applist title to launch; '' = the native default Desktop. */
  @State private formSunshineApp: string = '';
  // Sunshine quality. The tier table itself lives in ConnectionCodec.ets
  // (SUNSHINE_PRESETS) — this form only holds the selection and, for 'custom',
  // the four edited numbers plus the codec.
  @State private formSunshinePreset: string = SUNSHINE_PRESET_BALANCED;
  @State private formSunshineWidth: string = '1920';
  @State private formSunshineHeight: string = '1080';
  @State private formSunshineFps: string = '60';
  /** Form-facing bitrate unit is Mbps; connect() converts it to native kbps. */
  @State private formSunshineBitrate: string = '20';
  @State private formSunshineCodec: string = SUNSHINE_CODEC_AUTO;
  /** Sunshine local cursor (Moonlight style): ON unless the user turns it off. */
  @State private formSunshineLocalCursor: boolean = true;
  @State private formPort: string = DEFAULT_PORT;
  @State private formGroup: string = '';
  @State private formPassword: string = '';
  @State private formSavePassword: boolean = false;
  @State private formNoPassword: boolean = false;
  // Move-group overlay state.
  @State private moveTargetId: string = '';
  @State private moveGroupText: string = '';
  // Password re-entry overlay (shown for auth-failed).
  @State private pwdVisible: boolean = false;
  @State private pwdConnectionId: string = '';
  @State private pwdValue: string = '';
  @State private pwdReason: string = '';
  // Privacy: first-launch consent, the "关于" panel and the full policy text.
  // privacyReady stays false until the stored flag has been read, so the
  // consent overlay also covers the very first frames (the manager must not be
  // usable before the user has seen the dialog).
  @State private privacyReady: boolean = false;
  @State private privacyAccepted: boolean = false;
  @State private consentError: string = '';
  @State private aboutVisible: boolean = false;
  @State private policyVisible: boolean = false;
  @State private policyLoading: boolean = false;
  @State private policyText: string = '';
  @State private appVersion: string = '';
  private started: boolean = false;
  // In-flight connect per connection id: a second click while createSession /
  // openRemoteWindow is still running would create a second session AND a
  // second window for the same connection. A Set (not one string) so parallel
  // connects for different entries cannot clear each other's guard.
  private connectingEntryIds: Set<string> = new Set<string>();
  // Sessions whose window is being opened right now: a double click must not
  // start two multiton instances for the same session.
  private openingSessions: Set<string> = new Set<string>();
  // RDP account keys (see rdpAccountKey) with a connect attempt in flight: the
  // registry has no session yet while the first attempt waits for its password,
  // so two entries of the same Windows account must not both get through.
  private connectingRdpKeys: Set<string> = new Set<string>();
  private listener: () => void = (): void => {};
  private hintTimer: number = -1;
  private viewMap: Map<string, ConnectionView> = new Map<string, ConnectionView>();
  // Failed sessions already notified, so the 2 s poll does not pop the same
  // hint/dialog again.
  private notifiedFailures: Set<string> = new Set<string>();
  // connectionId -> latest Chinese failure reason, kept after the failed session
  // is closed so the row can still explain why.
  private failureText: Map<string, string> = new Map<string, string>();
  // Sessions whose reconnect was started by this list's 重连/primary action. When
  // one of them reaches connected, its window is brought forward once (P2).
  // Populated only by reconnectEntry(), so other paths never steal focus.
  private pendingForegroundSessions: Set<string> = new Set<string>();

  aboutToAppear(): void {
    this.viewportWidth = this.initialViewportWidth();
    this.appVersion = readAppVersion();
    this.loadPrivacyState().catch((error: Error): void => {
      hilog.warn(DOMAIN, TAG, '初始化隐私同意状态失败：%{public}s', String(error));
      this.privacyReady = true;
    });
    this.loadPolicyText().catch((error: Error): void => {
      hilog.warn(DOMAIN, TAG, '预读取隐私政策失败：%{public}s', String(error));
    });
    if (!this.started) {
      this.started = true;
      this.connections = savedConnections();
      this.syncViews();
      // Status changes refresh the affected cards through their ConnectionView
      // (@ObjectLink), NOT through the ForEach key: keying on a registry
      // version rebuilt every card on every session tick.
      this.listener = (): void => {
        this.trackFailures();
        this.promoteReconnectedSessions();
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
  // Layout breakpoint.
  // -------------------------------------------------------------------------

  /** Screen width in vp, used only before the first onAreaChange lands. */
  private initialViewportWidth(): number {
    try {
      const area = display.getDefaultDisplaySync();
      if (area.width > 0 && area.densityPixels > 0) {
        return area.width / area.densityPixels;
      }
    } catch (error) {
      hilog.warn(DOMAIN, TAG, '读取屏幕宽度失败，按宽屏布局：%{public}s', String(error));
    }
    return NARROW_BREAKPOINT_VP;
  }

  private isNarrow(): boolean {
    return this.viewportWidth > 0 && this.viewportWidth < NARROW_BREAKPOINT_VP;
  }

  // -------------------------------------------------------------------------
  // Data plumbing.
  // -------------------------------------------------------------------------

  private endpointOf(entry: ConnectionEntry): string {
    return entry.host + ':' + entry.port;
  }

  /**
   * Identity of the Windows account an RDP connection logs on as. One account
   * owns at most one session window at any moment, whatever state it is in.
   * The key deliberately spans connection entries (a 复制 copy has its own id
   * but the same account) and is lower-cased because Windows host names and
   * logon names are case-insensitive. VNC and RDP entries without a user name
   * yield '' (unknown account, never blocked): an account that cannot be
   * confirmed is not treated as the same one.
   */
  private rdpAccountKey(entry: ConnectionEntry): string {
    if (entry.protocol !== 'rdp') {
      return '';
    }
    const username = entry.username.trim();
    if (username.length === 0) {
      return '';
    }
    return entry.host.trim().toLowerCase() + ':' + entry.port + '|' + username.toLowerCase();
  }

  private statusOf(entry: ConnectionEntry): RowStatus {
    const sessions: RuntimeSession[] = sessionsForConnection(entry.id);
    if (sessions.length === 0) {
      const failure = this.failureText.get(entry.id);
      if (failure !== undefined) {
        const label = failure.length > 0 ? '失败：' + failure : '失败';
        return { kind: 'failed', label: label, count: 0, fingerprint: '', errorCode: '' };
      }
      return { kind: 'idle', label: '未连接', count: 0, fingerprint: '', errorCode: '' };
    }
    let connected = 0;
    for (let i = 0; i < sessions.length; i++) {
      const state = sessions[i].state;
      if (state === 'trust-required') {
        return {
          kind: 'trust', label: '需要确认身份', count: sessions.length,
          fingerprint: sessions[i].fingerprint, errorCode: ''
        };
      }
      if (state === SESSION_STATE_PAIRING_REQUIRED) {
        // Sunshine is waiting for the PIN from the server's web page. Not a
        // failure: the session window shows the PIN panel and connecting
        // finishes without a reconnect.
        return {
          kind: 'pairing', label: '需要配对', count: sessions.length,
          fingerprint: '', errorCode: ''
        };
      }
      if (state === 'failed') {
        // errorCode and the fingerprint the server presented travel with the
        // failed status so the card can explain a mismatch and offer a reset.
        return {
          kind: 'failed', label: '失败', count: sessions.length,
          fingerprint: sessions[i].fingerprint, errorCode: sessions[i].errorCode
        };
      }
      if (state === 'reconnecting') {
        return {
          kind: 'reconnecting', label: '重连中', count: sessions.length,
          fingerprint: '', errorCode: ''
        };
      }
      if (state === 'connecting') {
        return {
          kind: 'connecting', label: '连接中', count: sessions.length,
          fingerprint: '', errorCode: ''
        };
      }
      if (state === 'disconnected') {
        return {
          kind: 'disconnected', label: '已断开', count: sessions.length,
          fingerprint: '', errorCode: ''
        };
      }
      if (state === 'connected') {
        connected += 1;
      }
    }
    if (connected > 0) {
      return {
        kind: 'connected', label: '已连接·' + connected + '个', count: connected,
        fingerprint: '', errorCode: ''
      };
    }
    return { kind: 'idle', label: '未连接', count: 0, fingerprint: '', errorCode: '' };
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
    const protocol = entry.protocol === 'rdp' ? 'rdp' :
      (entry.protocol === 'sunshine' ? 'sunshine' : 'vnc');
    const lastConnectedText = this.formatTime(entry.lastConnectedAt);
    const status = this.statusOf(entry);
    const hasSessions = sessionsForConnection(entry.id).length > 0;
    if (view.entry !== entry) {
      view.entry = entry;
    }
    if (view.name !== entry.name) {
      view.name = entry.name;
    }
    if (view.protocol !== protocol) {
      view.protocol = protocol;
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
    // The trusted value is only needed while the mismatch panel is up. Read it
    // once when that state is entered (the store cannot change underneath the
    // panel unless the user resets the trust), not on every 2 s status tick.
    // This must run BEFORE statusErrorCode is updated: the comparison is what
    // detects the transition.
    const mismatch = status.kind === 'failed' && status.errorCode === 'fingerprint-mismatch';
    if (mismatch && view.statusErrorCode !== 'fingerprint-mismatch') {
      view.mismatchTrusted = readTrustedFingerprint(pinKeyFor(entry));
    } else if (!mismatch && view.mismatchTrusted.length > 0) {
      view.mismatchTrusted = '';
    }
    if (view.statusErrorCode !== status.errorCode) {
      view.statusErrorCode = status.errorCode;
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
    this.formProtocol = 'vnc';
    this.formUsername = '';
    this.formDomain = '';
    this.formSunshineApp = '';
    this.formSunshineLocalCursor = true;
    this.resetSunshineQuality(SUNSHINE_PRESET_BALANCED, SUNSHINE_CODEC_AUTO);
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
    this.formProtocol = entry.protocol === 'rdp' ? 'rdp' :
      (entry.protocol === 'sunshine' ? 'sunshine' : 'vnc');
    this.formUsername = entry.username;
    this.formDomain = entry.domain;
    this.formSunshineApp = entry.sunshineApp;
    // Quality: old records loaded by parseConnectionRecords already carry the
    // resolved values (balanced tier when the field was absent); the helpers
    // below only guard against a hand-written / partial record.
    const preset = typeof entry.sunshinePreset === 'string' &&
      isSunshinePreset(entry.sunshinePreset) ? entry.sunshinePreset : SUNSHINE_PRESET_BALANCED;
    const tier = sunshinePresetFor(preset);
    this.formSunshinePreset = preset;
    this.formSunshineWidth = String(typeof entry.sunshineWidth === 'number'
      ? entry.sunshineWidth : tier.width);
    this.formSunshineHeight = String(typeof entry.sunshineHeight === 'number'
      ? entry.sunshineHeight : tier.height);
    this.formSunshineFps = String(typeof entry.sunshineFps === 'number'
      ? entry.sunshineFps : tier.fps);
    this.formSunshineBitrate = String(typeof entry.sunshineBitrate === 'number'
      ? entry.sunshineBitrate : tier.bitrateMbps);
    this.formSunshineCodec = typeof entry.sunshineCodec === 'string' &&
      isSunshineCodec(entry.sunshineCodec) ? entry.sunshineCodec : SUNSHINE_CODEC_AUTO;
    this.formSunshineLocalCursor = entry.sunshineLocalCursor !== false;
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

  /**
   * Puts the Sunshine quality fields back onto one tier (used for a new
   * connection). The numbers always come from the shared SUNSHINE_PRESETS
   * table, so the form can never drift from what connect() sends.
   */
  private resetSunshineQuality(preset: string, codec: string): void {
    const tier = sunshinePresetFor(preset);
    this.formSunshinePreset = preset;
    this.formSunshineWidth = String(tier.width);
    this.formSunshineHeight = String(tier.height);
    this.formSunshineFps = String(tier.fps);
    this.formSunshineBitrate = String(tier.bitrateMbps);
    this.formSunshineCodec = codec;
  }

  /**
   * Selects a Sunshine quality tier. A named tier overwrites the four numbers
   * with the tier table; 自定义 keeps whatever is in the fields so the user can
   * start editing from the previous tier.
   */
  private selectSunshinePreset(preset: string): void {
    if (preset === SUNSHINE_PRESET_CUSTOM) {
      this.formSunshinePreset = preset;
      return;
    }
    const tier = sunshinePresetFor(preset);
    this.formSunshinePreset = preset;
    this.formSunshineWidth = String(tier.width);
    this.formSunshineHeight = String(tier.height);
    this.formSunshineFps = String(tier.fps);
    this.formSunshineBitrate = String(tier.bitrateMbps);
  }

  /**
   * Switches the form protocol. The port follows the new default only while it
   * is empty or still the other protocol's default; a custom port the user
   * typed is never overwritten.
   */
  private selectProtocol(protocol: string): void {
    if (protocol === this.formProtocol) {
      return;
    }
    const previous = this.formProtocol;
    const port = this.formPort.trim();
    if (port.length === 0 || port === defaultPortFor(previous)) {
      this.formPort = defaultPortFor(protocol);
    }
    this.formProtocol = protocol;
    if (protocol !== 'vnc') {
      // RDP and Sunshine have no "server needs no password" semantics.
      this.formNoPassword = false;
    }
    if (protocol === 'sunshine') {
      // Sunshine never uses a password: a value typed under VNC/RDP must not
      // survive into a Sunshine connection (the form hides these fields).
      this.formPassword = '';
      this.formSavePassword = false;
    }
  }

  private formEntry(): ConnectionEntry | undefined {
    if (this.formHost.trim().length === 0) {
      this.showHint('请填写服务器地址', true);
      return undefined;
    }
    const protocol = this.formProtocol === 'rdp' ? 'rdp' :
      (this.formProtocol === 'sunshine' ? 'sunshine' : 'vnc');
    const username = protocol === 'rdp' ? this.formUsername.trim() : '';
    const domain = protocol === 'rdp' ? this.formDomain.trim() : '';
    // Sunshine launches an app by /applist title; '' means the native default.
    const sunshineApp = protocol === 'sunshine' ? this.formSunshineApp.trim() : '';
    if (protocol === 'rdp' && username.length === 0) {
      this.showHint('请输入用户名', true);
      return undefined;
    }
    const port = this.formPort.trim().length > 0 ? this.formPort.trim() : defaultPortFor(protocol);
    const portNumber = Number(port);
    if (!Number.isInteger(portNumber) || portNumber <= 0 || portNumber > 65535) {
      this.showHint('端口无效（1-65535）', true);
      return undefined;
    }
    // Editing keeps the original opaque id, so changing host/port updates that
    // record instead of leaving the old one behind and adding a second one.
    const previous = this.editingId.length > 0 ? findConnection(this.editingId) : undefined;
    // Sunshine quality: a named tier always stores the tier table's numbers;
    // 'custom' stores the four edited values after validating them against the
    // native limits (an out-of-range value would make createSession fail with
    // "invalid sunshine …"). Every other protocol stores the balanced defaults
    // (the fields are simply ignored for it).
    let sunshinePreset = SUNSHINE_PRESET_BALANCED;
    let sunshineTier = sunshinePresetFor(SUNSHINE_PRESET_BALANCED);
    let sunshineWidth = sunshineTier.width;
    let sunshineHeight = sunshineTier.height;
    let sunshineFps = sunshineTier.fps;
    let sunshineBitrate = sunshineTier.bitrateMbps;
    let sunshineCodec = SUNSHINE_CODEC_AUTO;
    if (protocol === 'sunshine') {
      sunshinePreset = isSunshinePreset(this.formSunshinePreset) ?
        this.formSunshinePreset : SUNSHINE_PRESET_BALANCED;
      sunshineTier = sunshinePresetFor(sunshinePreset);
      sunshineWidth = sunshineTier.width;
      sunshineHeight = sunshineTier.height;
      sunshineFps = sunshineTier.fps;
      sunshineBitrate = sunshineTier.bitrateMbps;
      if (sunshinePreset === SUNSHINE_PRESET_CUSTOM) {
        sunshineWidth = Number(this.formSunshineWidth.trim());
        sunshineHeight = Number(this.formSunshineHeight.trim());
        if (!isValidSunshineSize(sunshineWidth) || !isValidSunshineSize(sunshineHeight)) {
          this.showHint('分辨率无效（1-4096）', true);
          return undefined;
        }
        sunshineFps = Number(this.formSunshineFps.trim());
        if (!isValidSunshineFps(sunshineFps)) {
          this.showHint('帧率无效（1-480）', true);
          return undefined;
        }
        sunshineBitrate = Number(this.formSunshineBitrate.trim());
        if (!isValidSunshineBitrateMbps(sunshineBitrate)) {
          this.showHint('码率无效（1-200 Mbps，整数）', true);
          return undefined;
        }
      }
      sunshineCodec = isSunshineCodec(this.formSunshineCodec) ?
        this.formSunshineCodec : SUNSHINE_CODEC_AUTO;
    }
    // Sunshine has no password at all, so noPassword/savePassword are forced off
    // and the password fields never reach the record.
    const sunshine = protocol === 'sunshine';
    const entry: ConnectionEntry = {
      id: previous === undefined ? newConnectionId() : previous.id,
      name: this.formName.trim().length > 0 ? this.formName.trim() : this.formHost.trim(),
      host: this.formHost.trim(),
      port: port,
      group: this.formGroup.trim(),
      savePassword: sunshine ? false : (this.formNoPassword ? false : this.formSavePassword),
      noPassword: sunshine ? false : this.formNoPassword,
      lastConnectedAt: previous === undefined ? 0 : previous.lastConnectedAt,
      protocol: protocol,
      username: username,
      domain: domain,
      sunshineApp: sunshineApp,
      sunshinePreset: sunshinePreset,
      sunshineWidth: sunshineWidth,
      sunshineHeight: sunshineHeight,
      sunshineFps: sunshineFps,
      sunshineBitrate: sunshineBitrate,
      sunshineCodec: sunshineCodec,
      sunshineLocalCursor: sunshine ? this.formSunshineLocalCursor : true,
      // The host cursor state is not edited in the form: keep whatever the
      // session page last recorded, so editing a connection cannot reset it.
      sunshineHostCursorHidden: previous === undefined ? false :
        previous.sunshineHostCursorHidden === true
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
      this.showHint(entry.protocol === 'sunshine' ? '已保存连接' :
        (entry.noPassword ? '已保存连接（该服务器无需密码）' : '已保存连接（未保存密码）'));
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
      lastConnectedAt: 0,
      protocol: entry.protocol,
      username: entry.username,
      domain: entry.domain,
      sunshineApp: entry.sunshineApp,
      sunshinePreset: entry.sunshinePreset,
      sunshineWidth: entry.sunshineWidth,
      sunshineHeight: entry.sunshineHeight,
      sunshineFps: entry.sunshineFps,
      sunshineBitrate: entry.sunshineBitrate,
      sunshineCodec: entry.sunshineCodec,
      sunshineLocalCursor: entry.sunshineLocalCursor,
      sunshineHostCursorHidden: entry.sunshineHostCursorHidden
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
    // Drop the endpoint's pin only when no other saved connection still uses
    // the same trust key (protocol + host + port): a 复制 copy shares it and
    // must keep the trust anchor.
    this.forgetTrustIfUnshared(entry);
    // The saved password lives under a shared alias too, so deleting a 复制 copy
    // must not take the original's password with it.
    this.forgetPasswordIfUnshared(entry);
    this.connections = savedConnections();
    this.syncViews();
    this.showHint('已删除连接');
  }

  /**
   * Removes the deleted entry's fingerprint from the trust store unless another
   * saved connection maps to the same pin key. Runs after removeConnection, so
   * only the surviving list decides.
   */
  private forgetTrustIfUnshared(entry: ConnectionEntry): void {
    // Sunshine never has an entry in the ArkTS trust store (its server
    // certificate is pinned by the native backend inside sunshineStateDir), so
    // there is nothing to forget and the VNC/RDP store must not be touched.
    if (entry.protocol === 'sunshine') {
      return;
    }
    const key = pinKeyFor(entry);
    const remaining = savedConnections();
    for (let i = 0; i < remaining.length; i++) {
      if (remaining[i].id !== entry.id && pinKeyFor(remaining[i]) === key) {
        return;
      }
    }
    forgetFingerprint(key);
  }

  /**
   * Removes the deleted entry's saved password unless another saved connection
   * maps to the same key-store alias (protocol + host + port). A 复制 copy
   * shares that alias without carrying the secret, so deleting the copy used to
   * wipe the original's password. Runs after removeConnection, like the pin
   * check above.
   */
  private forgetPasswordIfUnshared(entry: ConnectionEntry): void {
    // Sunshine has no password: the form never saves one and deleting the
    // record must not run an asset-store removal for it.
    if (entry.protocol === 'sunshine') {
      return;
    }
    const alias = passwordAliasFor(entry);
    const remaining = savedConnections();
    for (let i = 0; i < remaining.length; i++) {
      if (remaining[i].id !== entry.id && passwordAliasFor(remaining[i]) === alias) {
        return;
      }
    }
    removePassword(entry);
  }

  /**
   * Ends every live session of this connection but keeps the connection
   * itself (same close path as deleteEntry, minus the deletion). The card drops
   * back to 未连接 through the registry notification.
   */
  private disconnectEntry(entry: ConnectionEntry): void {
    const sessions = sessionsForConnection(entry.id);
    if (sessions.length === 0) {
      this.showHint('该连接没有活跃会话');
      return;
    }
    for (let i = 0; i < sessions.length; i++) {
      const sessionId = sessions[i].sessionId;
      closeSessionById(sessionId);
      closeRemoteWindow(sessionId).then((): void => {}).catch((error: Error): void => {
        hilog.warn(DOMAIN, TAG, '关闭会话远程窗口失败：%{public}s', String(error));
      });
    }
    this.failureText.delete(entry.id);
    this.syncViews();
    this.showHint('已断开：' + entry.name);
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

  /**
   * First live session of the same RDP account, resolved across connection
   * entries. A session whose owner entry is missing (orphan: connectionId '')
   * or whose entry is not confirmably RDP has no usable key and is skipped:
   * only a proven same-account match blocks a new session.
   */
  private existingRdpAccountSession(entry: ConnectionEntry): RuntimeSession | undefined {
    const key = this.rdpAccountKey(entry);
    if (key.length === 0) {
      return undefined;
    }
    const sessions = allSessions();
    for (let i = 0; i < sessions.length; i++) {
      const owner = findConnection(sessions[i].connectionId);
      if (owner === undefined) {
        continue;
      }
      if (this.rdpAccountKey(owner) === key) {
        return sessions[i];
      }
    }
    return undefined;
  }

  /**
   * Hard guard for every path that would create a session. Returns true when
   * the new session was replaced by "switch to the existing window": a
   * failed/disconnected/reconnecting session goes through the existing
   * reconnect logic, every other state just gets its window activated.
   */
  private activateExistingRdpSession(entry: ConnectionEntry): boolean {
    const existing = this.existingRdpAccountSession(entry);
    if (existing === undefined) {
      return false;
    }
    if (this.isReconnectKind(existing.state)) {
      reconnectById(existing.sessionId);
      this.showHint('同一个 Windows 账号只允许一个远程会话，已切换到现有会话并重连');
    } else {
      this.activateSession(existing.sessionId, existing.name);
      this.showHint('同一个 Windows 账号只允许一个远程会话，已切换到现有会话');
    }
    return true;
  }

  /** Primary action: reconnect when the row is in a recoverable state. */
  private primaryAction(entry: ConnectionEntry): void {
    const sessions = sessionsForConnection(entry.id);
    if (sessions.length === 0) {
      // A row whose failed session was already closed (e.g. auth-failed) has
      // nothing to reconnect; it must start a fresh attempt.
      this.connect(entry, '');
      return;
    }
    if (this.isReconnectKind(this.statusOf(entry).kind)) {
      this.reconnectEntry(entry);
      return;
    }
    // Same session clicked again: activate its window instead of a new one.
    this.activateSession(sessions[0].sessionId, sessions[0].name);
  }

  /** Reconnect every session of this connection that is failed/disconnected. */
  private reconnectEntry(entry: ConnectionEntry): void {
    const sessions = sessionsForConnection(entry.id);
    let count = 0;
    for (let i = 0; i < sessions.length; i++) {
      const state = sessions[i].state;
      if (state === 'failed' || state === 'disconnected' || state === 'reconnecting') {
        reconnectById(sessions[i].sessionId);
        this.pendingForegroundSessions.add(sessions[i].sessionId);
        count += 1;
      }
    }
    if (count > 0) {
      this.showHint('正在重连：' + entry.name);
    } else {
      this.showHint('没有可重连的会话', true);
    }
  }

  /**
   * P2: bring the window of a session reconnected from this list forward once it
   * reaches connected. Reuses activateSession() (the same path as 打开), and only
   * runs for sessions reconnectEntry() queued: a reconnect that ends in
   * failed/disconnected, or a session that no longer exists, is dropped so a
   * later unrelated state change cannot steal focus.
   */
  private promoteReconnectedSessions(): void {
    if (this.pendingForegroundSessions.size === 0) {
      return;
    }
    const done: string[] = [];
    const activate: RuntimeSession[] = [];
    this.pendingForegroundSessions.forEach((sessionId: string) => {
      const session = findSession(sessionId);
      if (session === undefined || session.state === 'failed' ||
          session.state === 'disconnected') {
        done.push(sessionId);
      } else if (session.state === 'connected') {
        done.push(sessionId);
        activate.push(session);
      }
    });
    // Clear before activating: openRemoteWindow() notifies the registry on
    // windowOpen, which re-enters this listener and must not queue the same
    // session twice.
    for (let i = 0; i < done.length; i++) {
      this.pendingForegroundSessions.delete(done[i]);
    }
    for (let i = 0; i < activate.length; i++) {
      this.activateSession(activate[i].sessionId, activate[i].name);
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

  private disconnectEntryById(id: string): void {
    const entry = findConnection(id);
    if (entry !== undefined) {
      this.disconnectEntry(entry);
    }
  }

  private confirmTrustById(id: string, fingerprint: string): void {
    const entry = findConnection(id);
    if (entry !== undefined) {
      this.confirmTrust(entry, fingerprint);
    }
  }

  private resetTrustById(id: string): void {
    const entry = findConnection(id);
    if (entry !== undefined) {
      this.resetTrust(entry);
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

  /**
   * App-sandbox absolute path of the Sunshine client state directory
   * (client certificate/private key, uniqueid and the pinned server
   * certificate). The native backend requires the directory to exist; it is
   * created here on first use. '' means the path could not be prepared and the
   * connection must be refused instead of starting a session that would fail
   * with "missing state directory".
   */
  private sunshineStateDir(): string {
    const context = this.abilityContext();
    if (context === undefined) {
      return '';
    }
    const stateDir = context.filesDir + '/sunshine';
    try {
      if (!fs.accessSync(stateDir)) {
        fs.mkdirSync(stateDir, true);
      }
    } catch (error) {
      // A concurrent create can fail with "file exists" while the directory is
      // already usable: only report the failure when the directory is missing.
      hilog.warn(DOMAIN, TAG, '创建 Sunshine 状态目录失败：%{public}s', String(error));
      try {
        if (!fs.accessSync(stateDir)) {
          return '';
        }
      } catch (inner) {
        return '';
      }
    }
    return stateDir;
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
    // RDP: one Windows account owns at most one session window at any moment.
    // Every new-session path ends here, so the hard guard runs before anything
    // else: a blocked attempt leaves the row untouched and switches to (or
    // reconnects) the existing window instead.
    if (this.activateExistingRdpSession(entry)) {
      return;
    }
    // A new attempt clears the previous failure reason (the row must leave the
    // "失败：…" state even when this attempt stops early).
    this.failureText.delete(entry.id);
    this.syncViews();
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
    // Same RDP account reached from two different entries: while the first
    // attempt waits for its password there is no registered session yet, so
    // the registry check above cannot see it. One in-flight attempt per account
    // closes that hole too.
    const rdpKey = this.rdpAccountKey(entry);
    if (rdpKey.length > 0 && this.connectingRdpKeys.has(rdpKey)) {
      this.showHint('同一个 Windows 账号只允许一个远程会话，该账号已有连接正在建立', true);
      return;
    }
    this.connectingEntryIds.add(entry.id);
    if (rdpKey.length > 0) {
      this.connectingRdpKeys.add(rdpKey);
    }
    try {
      // Sunshine never asks for a password (the PIN pairing replaces it), so it
      // skips the whole password gate — including the key-store read: it must
      // never touch the VNC/RDP password store.
      const sunshine = entry.protocol === 'sunshine';
      let password = explicitPassword;
      if (!sunshine) {
        if (password.length === 0 && entry.savePassword) {
          password = await loadPassword(entry);
        }
        if (password.length === 0 && entry.noPassword !== true) {
          // No password available: open the form focused on the password field.
          this.openFormEdit(entry);
          this.showHint('请输入密码后再连接', true);
          return;
        }
      }
      const endpoint = this.endpointOf(entry);
      const options: SessionOptions = {
        host: entry.host,
        port: Number(entry.port),
        name: entry.name
      };
      // password is optional in the contract: omit it entirely for servers that
      // need none instead of sending an empty credential.
      if (!sunshine && password.length > 0) {
        options.password = password;
      }
      if (sunshine) {
        // Sunshine/Moonlight. The client state directory is mandatory: the
        // native backend keeps the client identity and the pinned server
        // certificate there. security / password / expectedFingerprint are
        // deliberately NOT set — Sunshine has no RFB security types and pins
        // its server certificate natively inside that directory, so it must
        // never read the VNC/RDP trust store.
        const stateDir = this.sunshineStateDir();
        if (stateDir.length === 0) {
          this.showHint('无法准备 Sunshine 状态目录，连接已取消', true);
          return;
        }
        options.protocol = 'sunshine';
        options.sunshineStateDir = stateDir;
        if (entry.sunshineApp.length > 0) {
          // '' keeps the native default ("Desktop").
          options.sunshineApp = entry.sunshineApp;
        }
        // Quality: a named tier resolves through the single SUNSHINE_PRESETS
        // table (so tuning a tier is one edit); 'custom' uses the stored
        // numbers. sunshineBitrate is kbps natively but Mbps in the store/form.
        const quality = normalizeSunshineQuality(entry.sunshinePreset,
          entry.sunshineWidth, entry.sunshineHeight, entry.sunshineFps,
          entry.sunshineBitrate, entry.sunshineCodec);
        const tier = sunshinePresetFor(quality.preset);
        const custom = quality.preset === SUNSHINE_PRESET_CUSTOM;
        options.sunshineWidth = custom ? quality.width : tier.width;
        options.sunshineHeight = custom ? quality.height : tier.height;
        options.sunshineFps = custom ? quality.fps : tier.fps;
        options.sunshineBitrate = (custom ? quality.bitrateMbps : tier.bitrateMbps) * 1000;
        // auto: omit -> native picks by hardware decoder; h264 -> "h264";
        // hevc -> "h264,hevc" (server prefers HEVC, falls back to H.264).
        const formats = sunshineFormatsFor(quality.codec);
        if (formats.length > 0) {
          options.sunshineFormats = formats;
        }
        // Host cursor state recorded by the last session (host-wide, survives
        // this app). The session page re-asserts it once connected.
        if (entry.sunshineHostCursorHidden === true) {
          options.sunshineHostCursorHidden = true;
        }
      } else if (entry.protocol === 'rdp') {
        // RDP shares the session state machine; only the endpoint options
        // differ. security stays unset (RFB security types only), but
        // expectedFingerprint is the RDP server certificate pin: the pin is
        // keyed per protocol so a VNC pin on the same host:port never applies.
        options.protocol = 'rdp';
        if (entry.username.length > 0) {
          options.username = entry.username;
        }
        if (entry.domain.length > 0) {
          options.domain = entry.domain;
        }
        const pin = readTrustedFingerprint(pinKeyFor(entry));
        if (pin.length > 0) {
          options.expectedFingerprint = pin;
        }
      } else {
        // Servers that publish only None (type 1) reject the encrypted-only
        // default, so a "no password" connection offers None explicitly.
        if (entry.noPassword === true) {
          options.security = 'None';
        }
        const pin = readTrustedFingerprint(pinKeyFor(entry));
        if (pin.length > 0) {
          options.expectedFingerprint = pin;
        }
      }
      const sessionId = createSession(options);
      if (sessionId.length === 0) {
        this.showHint('创建会话失败（' + endpoint + '）', true);
        return;
      }
      // The native side reports startup/argument failures as "ERROR:<原因>",
      // not as an empty string: registering that as a session id would open a
      // window for a session that never existed.
      if (sessionId.indexOf('ERROR:') === 0) {
        const reason = sessionId.substring(6).trim();
        this.showHint('创建会话失败：' + (reason.length > 0 ? reason : '原生层未给出原因'), true);
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
      this.connectingRdpKeys.delete(rdpKey);
    }
  }

  /** Chinese short reason for a failed session. */
  private reasonOf(session: RuntimeSession): string {
    switch (session.errorCode) {
      case 'auth-failed':
        return '密码错误';
      case 'fingerprint-mismatch':
        return '服务端指纹与已信任值不一致，已拒绝连接';
      case 'network':
        return '网络连接中断';
      case 'protocol':
        return '协议不匹配或服务器拒绝';
      case SESSION_ERROR_SESSION_TAKEN_OVER:
        return '该账号的会话已被别的连接接管。在远端注销被接管的会话，或直接点「重连」即可重新连接。';
      case 'pair-failed':
        // Sunshine only; normal retry happens inside the PIN panel without a
        // reconnect, this text is the fallback when the session really failed.
        return '配对失败：PIN 不正确或已过期，请重试';
      default:
        return '连接失败';
    }
  }

  /** Short inline reason for the row label; '' means no specific reason. */
  private shortReasonOf(session: RuntimeSession): string {
    switch (session.errorCode) {
      case 'auth-failed':
        return '密码错误';
      case 'fingerprint-mismatch':
        return '指纹不一致';
      case 'network':
        return '网络中断';
      case 'protocol':
        return '协议不匹配';
      case SESSION_ERROR_SESSION_TAKEN_OVER:
        return '会话被接管';
      case 'pair-failed':
        return '配对失败';
      default:
        return '';
    }
  }

  /**
   * Called on every registry notification BEFORE syncViews: reacts once per
   * failed session (not every 2 s poll), and clears the remembered reason as
   * soon as a new attempt leaves the failed state.
   */
  private trackFailures(): void {
    for (let i = 0; i < this.connections.length; i++) {
      const entry = this.connections[i];
      const sessions = sessionsForConnection(entry.id);
      for (let j = 0; j < sessions.length; j++) {
        const session = sessions[j];
        if (session.state === 'failed') {
          if (!this.notifiedFailures.has(session.sessionId)) {
            this.notifiedFailures.add(session.sessionId);
            this.onSessionFailed(entry, session);
          }
        } else {
          this.notifiedFailures.delete(session.sessionId);
          this.failureText.delete(entry.id);
        }
      }
    }
  }

  private onSessionFailed(entry: ConnectionEntry, session: RuntimeSession): void {
    const reason = this.reasonOf(session);
    this.failureText.set(entry.id, this.shortReasonOf(session));
    if (session.errorCode === 'auth-failed' && entry.noPassword !== true) {
      // The auth failure happens before the connection exists, so the dead
      // session and its (never filled) window can be closed safely first.
      closeSessionById(session.sessionId);
      closeRemoteWindow(session.sessionId).then((): void => {}).catch((error: Error): void => {
        hilog.warn(DOMAIN, TAG, '关闭密码错误会话的远程窗口失败：%{public}s', String(error));
      });
      if (!this.pwdVisible || this.pwdConnectionId !== entry.id) {
        const detail = session.errorText.length > 0 ? '（' + session.errorText + '）' : '';
        this.pwdConnectionId = entry.id;
        this.pwdReason = '密码错误，请重新输入密码' + detail;
        this.pwdValue = '';
        this.pwdVisible = true;
        this.showHint('密码错误，请重新输入密码', true);
      }
    } else {
      const detail = session.errorText.length > 0 ? '（' + session.errorText + '）' : '';
      const hint = reason === '连接失败' ? '连接失败' + detail : '连接失败：' + reason + detail;
      this.showHint(hint, true);
    }
    this.syncViews();
  }

  private cancelPassword(): void {
    this.pwdVisible = false;
    this.pwdValue = '';
    this.pwdReason = '';
    this.pwdConnectionId = '';
  }

  private submitPassword(): void {
    const password = this.pwdValue;
    if (password.length === 0) {
      this.showHint('请输入密码后再连接', true);
      return;
    }
    const entry = findConnection(this.pwdConnectionId);
    if (entry === undefined) {
      this.cancelPassword();
      return;
    }
    const sessions = sessionsForConnection(entry.id);
    for (let i = 0; i < sessions.length; i++) {
      const sessionId = sessions[i].sessionId;
      closeSessionById(sessionId);
      closeRemoteWindow(sessionId).then((): void => {}).catch((error: Error): void => {
        hilog.warn(DOMAIN, TAG, '关闭旧会话窗口失败：%{public}s', String(error));
      });
    }
    this.failureText.delete(entry.id);
    this.pwdVisible = false;
    this.pwdValue = '';
    this.pwdReason = '';
    this.pwdConnectionId = '';
    if (entry.savePassword === true) {
      savePassword(entry, password).then((ok: boolean): void => {
        if (!ok) {
          hilog.warn(DOMAIN, TAG, '新密码写入密钥库失败');
        }
      }).catch((error: Error): void => {
        hilog.warn(DOMAIN, TAG, '新密码保存异常：%{public}s', String(error));
      });
    }
    // A fresh attempt with the new password, not reconnectById: the old
    // session is gone and its old credential must not be reused.
    this.connect(entry, password);
  }

  /** 确认并信任 (inline on a trust-required row). */
  private confirmTrust(entry: ConnectionEntry, fingerprint: string): void {
    if (fingerprint.length === 0) {
      return;
    }
    trustFingerprint(pinKeyFor(entry), fingerprint);
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

  /**
   * 重新确认指纹 (inline on a fingerprint-mismatch row). Only the sessions that
   * are actually in the mismatch state are asked to forget the trusted pin and
   * reconnect with no expected fingerprint: a session that is still connected
   * must keep running untouched. Nothing is trusted here.
   */
  private resetTrust(entry: ConnectionEntry): void {
    const sessions = sessionsForConnection(entry.id);
    const mismatchSessions: RuntimeSession[] = [];
    for (let i = 0; i < sessions.length; i++) {
      if (sessions[i].state === 'failed' &&
          sessions[i].errorCode === 'fingerprint-mismatch') {
        mismatchSessions.push(sessions[i]);
      }
    }
    if (mismatchSessions.length === 0) {
      this.showHint('没有需要重新确认指纹的会话', true);
      return;
    }
    let accepted = 0;
    let error = '';
    for (let i = 0; i < mismatchSessions.length; i++) {
      const result = forgetTrustedFingerprint(mismatchSessions[i].sessionId);
      if (result === 'OK') {
        accepted += 1;
      } else if (error.length === 0) {
        error = result.length > 0 ? result : '未知错误';
      }
    }
    if (accepted === 0) {
      this.showHint('重新确认指纹失败：' + error, true);
      return;
    }
    if (error.length > 0) {
      this.showHint('部分会话未能重新确认指纹：' + error, true);
    } else {
      this.showHint('已清除本机信任，等待重新确认指纹');
    }
    this.syncViews();
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

  /** Narrow-screen replacement for the group column: one chip per group. */
  @Builder
  private groupChip(label: string, key: string, count: number) {
    Text(label + ' · ' + String(count))
      .fontSize(13)
      .fontColor(this.selectedGroup === key
        ? Color.White : $r('app.color.text_primary'))
      .height(32)
      .padding({ left: 14, right: 14 })
      .borderRadius(16)
      .maxLines(1)
      .backgroundColor(this.selectedGroup === key
        ? $r('app.color.brand') : $r('app.color.chip_background'))
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
        Button('VNC')
          .fontSize(14)
          .height(40)
          .layoutWeight(1)
          .borderRadius(12)
          .backgroundColor(this.formProtocol === 'vnc'
            ? $r('app.color.brand') : $r('app.color.chip_background'))
          .fontColor(this.formProtocol === 'vnc'
            ? Color.White : $r('app.color.text_primary'))
          .onClick(() => { this.selectProtocol('vnc'); })
        Button('RDP')
          .fontSize(14)
          .height(40)
          .layoutWeight(1)
          .borderRadius(12)
          .backgroundColor(this.formProtocol === 'rdp'
            ? $r('app.color.brand') : $r('app.color.chip_background'))
          .fontColor(this.formProtocol === 'rdp'
            ? Color.White : $r('app.color.text_primary'))
          .onClick(() => { this.selectProtocol('rdp'); })
        Button('Sunshine')
          .fontSize(14)
          .height(40)
          .layoutWeight(1)
          .borderRadius(12)
          .backgroundColor(this.formProtocol === 'sunshine'
            ? $r('app.color.brand') : $r('app.color.chip_background'))
          .fontColor(this.formProtocol === 'sunshine'
            ? Color.White : $r('app.color.text_primary'))
          .onClick(() => { this.selectProtocol('sunshine'); })
      }.width('100%')
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
      if (this.formProtocol === 'rdp') {
        TextInput({ text: this.formUsername, placeholder: '用户名' })
          .height(40)
          .onChange((value: string) => { this.formUsername = value; })
        TextInput({ text: this.formDomain, placeholder: '域（可留空）' })
          .height(40)
          .onChange((value: string) => { this.formDomain = value; })
      }
      if (this.formProtocol === 'sunshine') {
        // Sunshine launches an app by title from the host's /applist; empty
        // keeps the native default ("Desktop").
        TextInput({ text: this.formSunshineApp, placeholder: '应用名（留空为 Desktop）' })
          .height(40)
          .onChange((value: string) => { this.formSunshineApp = value; })
        // ---- 画质: one tier + (for 自定义) the four numbers and the codec ----
        Text('画质').fontSize(13).fontColor($r('app.color.text_secondary'))
        Row({ space: 6 }) {
          ForEach(SUNSHINE_PRESETS, (preset: SunshinePreset) => {
            Button(preset.label)
              .fontSize(12)
              .height(36)
              .layoutWeight(1)
              .borderRadius(10)
              .backgroundColor(this.formSunshinePreset === preset.id
                ? $r('app.color.brand') : $r('app.color.chip_background'))
              .fontColor(this.formSunshinePreset === preset.id
                ? Color.White : $r('app.color.text_primary'))
              .onClick(() => { this.selectSunshinePreset(preset.id); })
          }, (preset: SunshinePreset) => preset.id)
          Button('自定义')
            .fontSize(12)
            .height(36)
            .layoutWeight(1)
            .borderRadius(10)
            .backgroundColor(this.formSunshinePreset === SUNSHINE_PRESET_CUSTOM
              ? $r('app.color.brand') : $r('app.color.chip_background'))
            .fontColor(this.formSunshinePreset === SUNSHINE_PRESET_CUSTOM
              ? Color.White : $r('app.color.text_primary'))
            .onClick(() => { this.selectSunshinePreset(SUNSHINE_PRESET_CUSTOM); })
        }.width('100%')
        if (this.formSunshinePreset === SUNSHINE_PRESET_CUSTOM) {
          Row({ space: 8 }) {
            TextInput({ text: this.formSunshineWidth, placeholder: '宽' })
              .height(40)
              .type(InputType.Number)
              .layoutWeight(1)
              .onChange((value: string) => { this.formSunshineWidth = value; })
            TextInput({ text: this.formSunshineHeight, placeholder: '高' })
              .height(40)
              .type(InputType.Number)
              .layoutWeight(1)
              .onChange((value: string) => { this.formSunshineHeight = value; })
          }.width('100%')
          Row({ space: 8 }) {
            TextInput({ text: this.formSunshineFps, placeholder: '帧率' })
              .height(40)
              .type(InputType.Number)
              .layoutWeight(1)
              .onChange((value: string) => { this.formSunshineFps = value; })
            TextInput({ text: this.formSunshineBitrate, placeholder: '码率（Mbps）' })
              .height(40)
              .type(InputType.Number)
              .layoutWeight(1)
              .onChange((value: string) => { this.formSunshineBitrate = value; })
          }.width('100%')
          // 视频编码: auto leaves sunshineFormats unset; H.264 -> "h264";
          // HEVC -> "h264,hevc" so a server without HEVC still connects.
          Row({ space: 6 }) {
            Button('自动')
              .fontSize(13)
              .height(36)
              .layoutWeight(1)
              .borderRadius(10)
              .backgroundColor(this.formSunshineCodec === SUNSHINE_CODEC_AUTO
                ? $r('app.color.brand') : $r('app.color.chip_background'))
              .fontColor(this.formSunshineCodec === SUNSHINE_CODEC_AUTO
                ? Color.White : $r('app.color.text_primary'))
              .onClick(() => { this.formSunshineCodec = SUNSHINE_CODEC_AUTO; })
            Button('H.264')
              .fontSize(13)
              .height(36)
              .layoutWeight(1)
              .borderRadius(10)
              .backgroundColor(this.formSunshineCodec === SUNSHINE_CODEC_H264
                ? $r('app.color.brand') : $r('app.color.chip_background'))
              .fontColor(this.formSunshineCodec === SUNSHINE_CODEC_H264
                ? Color.White : $r('app.color.text_primary'))
              .onClick(() => { this.formSunshineCodec = SUNSHINE_CODEC_H264; })
            Button('HEVC')
              .fontSize(13)
              .height(36)
              .layoutWeight(1)
              .borderRadius(10)
              .backgroundColor(this.formSunshineCodec === SUNSHINE_CODEC_HEVC
                ? $r('app.color.brand') : $r('app.color.chip_background'))
              .fontColor(this.formSunshineCodec === SUNSHINE_CODEC_HEVC
                ? Color.White : $r('app.color.text_primary'))
              .onClick(() => { this.formSunshineCodec = SUNSHINE_CODEC_HEVC; })
          }.width('100%')
        }
        Text('分辨率、帧率与码率在重新连接后生效')
          .fontSize(11)
          .fontColor($r('app.color.text_secondary'))
          .width('100%')
        Text('码率低于 15 Mbps 时音频会降为普通音质')
          .fontSize(11)
          .fontColor($r('app.color.text_secondary'))
          .width('100%')
        // Moonlight-style local cursor: the host hides its cursor and this
        // client shows the system pointer, so it follows the hand with no lag.
        Row({ space: 8 }) {
          Checkbox()
            .select(this.formSunshineLocalCursor)
            .onChange((checked: boolean) => { this.formSunshineLocalCursor = checked; })
          Text('本地光标（更跟手，但光标形状固定为箭头）').fontSize(14)
        }
        Text('主机光标状态是主机级的，会保留到下次会话；Sunshine 重启后恢复显示')
          .fontSize(11)
          .fontColor($r('app.color.text_secondary'))
          .width('100%')
      }
      TextInput({ text: this.formGroup, placeholder: '分组（可留空）' })
        .height(40)
        .onChange((value: string) => { this.formGroup = value; })
      if (this.formProtocol === 'vnc') {
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
      }
      // Sunshine has no password at all: the whole password block is hidden
      // (a stale value can never be saved for it, see selectProtocol/formEntry).
      if (this.formProtocol !== 'sunshine') {
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
    // Width comes from the enclosing Scroll (86% of the window); the panel only
    // adds its own background/padding so scrolling never cuts the card edges.
    .width('100%')
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

  @Builder
  private passwordOverlay() {
    Column({ space: 12 }) {
      Text('密码错误').fontSize(18).fontWeight(FontWeight.Bold)
      Text(this.pwdReason)
        .fontSize(13)
        .fontColor($r('app.color.danger'))
        .width('100%')
      TextInput({ text: this.pwdValue, placeholder: '密码（不回显）' })
        .height(40)
        .type(InputType.Password)
        .defaultFocus(true)
        .onChange((value: string) => { this.pwdValue = value; })
        .onSubmit((enterKey: EnterKeyType) => { this.submitPassword(); })
      Row({ space: 12 }) {
        Button('取消')
          .fontSize(14)
          .height(40)
          .layoutWeight(1)
          .borderRadius(12)
          .backgroundColor($r('app.color.chip_background'))
          .fontColor($r('app.color.text_primary'))
          .onClick(() => { this.cancelPassword(); })
        Button('连接')
          .fontSize(14)
          .height(40)
          .layoutWeight(1)
          .borderRadius(12)
          .backgroundColor($r('app.color.brand'))
          .fontColor(Color.White)
          .onClick(() => { this.submitPassword(); })
      }.width('100%')
    }
    .width('86%')
    .padding(24)
    .borderRadius(16)
    .backgroundColor($r('app.color.card_background'))
  }

  // -------------------------------------------------------------------------
  // Privacy: first-launch consent, "关于" panel, full policy text.
  // -------------------------------------------------------------------------

  private async loadPrivacyState(): Promise<void> {
    const context = this.abilityContext();
    if (context !== undefined) {
      try {
        this.privacyAccepted = await isPrivacyAccepted(context);
      } catch (error) {
        // Fail closed: an unreadable flag shows the consent dialog again.
        hilog.warn(DOMAIN, TAG, '读取隐私同意标记失败，按未同意处理：%{public}s', String(error));
        this.privacyAccepted = false;
      }
    }
    this.privacyReady = true;
  }

  private async loadPolicyText(): Promise<void> {
    if (this.policyLoading || this.policyText.length > 0) {
      return;
    }
    this.policyLoading = true;
    const context = this.abilityContext();
    if (context === undefined) {
      this.policyText = '隐私政策读取失败，请关闭后重试。';
    } else {
      try {
        this.policyText = await loadPrivacyPolicy(context);
      } catch (error) {
        hilog.error(DOMAIN, TAG, '读取隐私政策失败：%{public}s', String(error));
        this.policyText = '隐私政策读取失败，请关闭后重试。';
      }
    }
    this.policyLoading = false;
  }

  private openPolicy(): void {
    this.aboutVisible = false;
    this.policyVisible = true;
    if (this.policyText.length === 0 && !this.policyLoading) {
      this.loadPolicyText().catch((error: Error): void => {
        hilog.warn(DOMAIN, TAG, '打开隐私政策失败：%{public}s', String(error));
      });
    }
  }

  /** 同意并继续: only enters the manager after the flag is really persisted. */
  private acceptPrivacy(): void {
    const context = this.abilityContext();
    if (context === undefined) {
      this.consentError = '保存隐私设置失败，请重试。';
      return;
    }
    this.consentError = '';
    setPrivacyAccepted(context).then((): void => {
      this.privacyAccepted = true;
      hilog.info(DOMAIN, TAG, 'privacy consent accepted');
    }).catch((error: Error): void => {
      this.consentError = '保存隐私设置失败，请重试。';
      hilog.error(DOMAIN, TAG, '保存隐私同意标记失败：%{public}s', String(error));
    });
  }

  /** 不同意: the app must not stay usable, so end this ability. */
  private declinePrivacy(): void {
    hilog.info(DOMAIN, TAG, 'privacy consent declined, terminating');
    const context = this.abilityContext();
    if (context === undefined) {
      this.consentError = '退出失败，请手动关闭应用。';
      return;
    }
    context.terminateSelf().catch((error: Error): void => {
      this.consentError = '退出失败，请手动关闭应用。';
      hilog.error(DOMAIN, TAG, '拒绝隐私政策后退出失败：%{public}s', String(error));
    });
  }

  @Builder
  private aboutEntryButton() {
    Button('关于')
      .fontSize(12)
      .fontColor($r('app.color.text_secondary'))
      .height(28)
      .padding({ left: 10, right: 10 })
      .borderRadius(8)
      .backgroundColor($r('app.color.chip_background'))
      .onClick(() => { this.aboutVisible = true; })
  }

  @Builder
  private consentOverlay() {
    Column({ space: 16 }) {
      if (!this.privacyReady) {
        Column({ space: 12 }) {
          LoadingProgress()
            .width(32)
            .height(32)
            .color($r('app.color.brand'))
          Text('正在读取隐私设置…')
            .fontSize(13)
            .fontColor($r('app.color.text_secondary'))
        }
        .width('100%')
        .justifyContent(FlexAlign.Center)
        .padding({ top: 12, bottom: 12 })
      } else {
        Text('欢迎使用天关')
          .fontSize(20)
          .fontWeight(FontWeight.Bold)
          .fontColor($r('app.color.text_primary'))
          .width('100%')
        Text('使用前请阅读并同意《隐私政策》')
          .fontSize(15)
          .fontColor($r('app.color.text_primary'))
          .width('100%')
        Column({ space: 6 }) {
          Text('天关不收集、不上传您的任何个人信息。')
            .fontSize(13)
            .fontColor($r('app.color.text_primary'))
            .width('100%')
          Text('连接配置与证书指纹只保存在本机，密码保存在系统密钥库；远程桌面连接直接连到您指定的服务器，不经过任何中转。')
            .fontSize(12)
            .lineHeight(18)
            .fontColor($r('app.color.text_secondary'))
            .width('100%')
        }
        .width('100%')
        .alignItems(HorizontalAlign.Start)
        .padding(12)
        .borderRadius(10)
        .backgroundColor($r('app.color.page_background'))

        if (this.consentError.length > 0) {
          Text(this.consentError)
            .fontSize(12)
            .fontColor($r('app.color.danger'))
            .width('100%')
        }

        Button('查看隐私政策')
          .fontSize(14)
          .fontColor($r('app.color.brand'))
          .backgroundColor($r('app.color.chip_background'))
          .height(40)
          .width('100%')
          .borderRadius(12)
          .onClick(() => { this.openPolicy(); })

        Row({ space: 12 }) {
          Button('不同意')
            .fontSize(14)
            .height(40)
            .layoutWeight(1)
            .borderRadius(12)
            .backgroundColor($r('app.color.chip_background'))
            .fontColor($r('app.color.text_primary'))
            .onClick(() => { this.declinePrivacy(); })
          Button('同意并继续')
            .fontSize(14)
            .height(40)
            .layoutWeight(1.4)
            .borderRadius(12)
            .backgroundColor($r('app.color.brand'))
            .fontColor(Color.White)
            .onClick(() => { this.acceptPrivacy(); })
        }
        .width('100%')
      }
    }
    .width('86%')
    .padding(24)
    .borderRadius(16)
    .backgroundColor($r('app.color.card_background'))
  }

  @Builder
  private aboutInfoRow(label: string, value: string) {
    Row({ space: 12 }) {
      Text(label)
        .fontSize(13)
        .fontColor($r('app.color.text_secondary'))
        .width(72)
      Text(value)
        .fontSize(13)
        .fontColor($r('app.color.text_primary'))
        .layoutWeight(1)
    }
    .width('100%')
    .alignItems(VerticalAlign.Top)
  }

  @Builder
  private aboutOverlay() {
    Column({ space: 14 }) {
      Row() {
        Text('关于天关')
          .fontSize(20)
          .fontWeight(FontWeight.Bold)
          .fontColor($r('app.color.text_primary'))
          .layoutWeight(1)
        Text('✕')
          .fontSize(18)
          .fontColor($r('app.color.text_secondary'))
          .width(32)
          .height(32)
          .textAlign(TextAlign.Center)
          .borderRadius(8)
          .onClick(() => { this.aboutVisible = false; })
      }
      .width('100%')

      Scroll() {
        Column({ space: 14 }) {
          Column({ space: 4 }) {
            Text('天关')
              .fontSize(24)
              .fontWeight(FontWeight.Bold)
              .fontColor($r('app.color.text_primary'))
            Text('版本 ' + (this.appVersion.length > 0 ? this.appVersion : '未知'))
              .fontSize(13)
              .fontColor($r('app.color.text_secondary'))
            Text('HarmonyOS 远程桌面客户端（VNC / RDP / Sunshine）')
              .fontSize(13)
              .fontColor($r('app.color.text_secondary'))
          }
          .width('100%')
          .alignItems(HorizontalAlign.Start)

          Divider().color($r('app.color.chip_background'))

          Column({ space: 8 }) {
            this.aboutInfoRow('开发者', '天关开源项目')
            this.aboutInfoRow('联系方式', 'support@alfadb.cn')
            this.aboutInfoRow('应用包名', 'cn.alfadb.tianguan')
            this.aboutInfoRow('开源许可', 'GPL-3.0-or-later')
            this.aboutInfoRow('源代码', 'https://github.com/alfadb/tianguan')
          }
          .width('100%')

          Text('天关是独立社区项目，不收集、不上传任何用户数据。远程桌面连接直接连接您指定的服务器，不经过任何中转。')
            .fontSize(12)
            .lineHeight(18)
            .fontColor($r('app.color.text_secondary'))
            .width('100%')

          Row({ space: 8 }) {
            Text('隐私政策')
              .fontSize(14)
              .fontColor($r('app.color.brand'))
              .layoutWeight(1)
            Text('查看全文 ›')
              .fontSize(12)
              .fontColor($r('app.color.text_secondary'))
          }
          .width('100%')
          .padding(12)
          .borderRadius(10)
          .backgroundColor($r('app.color.chip_background'))
          .onClick(() => { this.openPolicy(); })
        }
        .width('100%')
        .alignItems(HorizontalAlign.Start)
      }
      .layoutWeight(1)
      .width('100%')
      .align(Alignment.Top)
      .scrollBar(BarState.Auto)

      Row({ space: 12 }) {
        Button('隐私政策')
          .fontSize(14)
          .height(40)
          .layoutWeight(1)
          .borderRadius(12)
          .backgroundColor($r('app.color.chip_background'))
          .fontColor($r('app.color.brand'))
          .onClick(() => { this.openPolicy(); })
        Button('关闭')
          .fontSize(14)
          .height(40)
          .layoutWeight(1)
          .borderRadius(12)
          .backgroundColor($r('app.color.brand'))
          .fontColor(Color.White)
          .onClick(() => { this.aboutVisible = false; })
      }
      .width('100%')
    }
    .width(this.isNarrow() ? '92%' : 520)
    .height('78%')
    .padding(20)
    .borderRadius(16)
    .backgroundColor($r('app.color.card_background'))
  }

  @Builder
  private policyOverlay() {
    Column({ space: 12 }) {
      Row() {
        Text('隐私政策')
          .fontSize(20)
          .fontWeight(FontWeight.Bold)
          .fontColor($r('app.color.text_primary'))
          .layoutWeight(1)
        Text('✕')
          .fontSize(18)
          .fontColor($r('app.color.text_secondary'))
          .width(32)
          .height(32)
          .textAlign(TextAlign.Center)
          .borderRadius(8)
          .onClick(() => { this.policyVisible = false; })
      }
      .width('100%')

      if (this.policyLoading) {
        Column({ space: 10 }) {
          LoadingProgress()
            .width(32)
            .height(32)
            .color($r('app.color.brand'))
          Text('正在读取隐私政策…')
            .fontSize(13)
            .fontColor($r('app.color.text_secondary'))
        }
        .layoutWeight(1)
        .width('100%')
        .justifyContent(FlexAlign.Center)
      } else {
        Scroll() {
          Text(this.policyText.length > 0 ? this.policyText : '隐私政策内容暂不可用。')
            .fontSize(13)
            .lineHeight(20)
            .fontColor($r('app.color.text_primary'))
            .width('100%')
        }
        .layoutWeight(1)
        .width('100%')
        .align(Alignment.Top)
        .scrollBar(BarState.Auto)
      }

      Button('关闭')
        .fontSize(14)
        .height(40)
        .width('100%')
        .borderRadius(12)
        .backgroundColor($r('app.color.brand'))
        .fontColor(Color.White)
        .onClick(() => { this.policyVisible = false; })
    }
    .width(this.isNarrow() ? '94%' : 640)
    .height('88%')
    .padding(20)
    .borderRadius(16)
    .backgroundColor($r('app.color.card_background'))
  }

  build() {
    Stack() {
      Column() {
        // Top bar: search + new. Narrow screens stack the two into two rows so
        // neither the search field nor the button is squeezed off the edge.
        if (this.isNarrow()) {
          Column({ space: 8 }) {
            Row({ space: 12 }) {
              Text('天关').fontSize(20).fontWeight(FontWeight.Bold)
                .fontColor($r('app.color.text_primary'))
                .layoutWeight(1)
              this.aboutEntryButton()
              Button('新建连接')
                .fontSize(14)
                .height(40)
                .borderRadius(12)
                .backgroundColor($r('app.color.brand'))
                .fontColor(Color.White)
                .onClick(() => { this.openFormNew(); })
            }
            .width('100%')
            TextInput({ text: this.searchText, placeholder: '搜索名称或 host:port' })
              .height(40)
              .width('100%')
              .backgroundColor($r('app.color.chip_background'))
              .onChange((value: string) => { this.searchText = value; })
          }
          .width('100%')
          .padding({ left: 16, right: 16, top: 12, bottom: 12 })
        } else {
          Row({ space: 12 }) {
            Text('天关').fontSize(20).fontWeight(FontWeight.Bold)
              .fontColor($r('app.color.text_primary'))
            this.aboutEntryButton()
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
        }

        // Narrow: the group column becomes one horizontally scrollable chip row.
        if (this.isNarrow()) {
          Scroll() {
            Row({ space: 8 }) {
              ForEach(this.groupsWithCounts(), (group: string) => {
                if (group === '__all__') {
                  this.groupChip('全部', '__all__', this.groupCount(group))
                } else if (group === UNGROUPED) {
                  this.groupChip(UNGROUPED, UNGROUPED, this.groupCount(group))
                } else {
                  this.groupChip(group, group, this.groupCount(group))
                }
              }, (group: string) => group + '#' + String(this.groupCount(group)) +
                (this.selectedGroup === group ? '#selected' : ''))
            }
            .padding({ left: 16, right: 16 })
          }
          .scrollable(ScrollDirection.Horizontal)
          .scrollBar(BarState.Off)
          .width('100%')
          .padding({ top: 6, bottom: 6 })
          .backgroundColor($r('app.color.side_background'))
        }

        Row({ space: 0 }) {
          // Left group column (wide layout only).
          if (!this.isNarrow()) {
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
          }

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
                      narrow: this.isNarrow(),
                      onPrimary: (id: string): void => {
                        this.primaryActionById(id);
                      },
                      onReconnect: (id: string): void => {
                        this.reconnectEntryById(id);
                      },
                      onNewSession: (id: string): void => {
                        this.newSessionById(id);
                      },
                      onDisconnect: (id: string): void => {
                        this.disconnectEntryById(id);
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
                      },
                      onResetTrust: (id: string): void => {
                        this.resetTrustById(id);
                      }
                    })
                  }, (view: ConnectionView) => view.id)
                }
                .width('100%')
                .padding({ left: 16, right: 16, top: 8, bottom: 16 })
              }
              .scrollBar(BarState.Auto)
              .layoutWeight(1)
              // Top-align the list content: Alignment.Start is left-center and
              // would vertically center a short list inside the viewport.
              .align(Alignment.TopStart)
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
        // The form grew with the Sunshine quality group: scroll it when it is
        // taller than the window instead of letting the buttons fall off screen.
        Scroll() {
          this.formOverlay()
        }
        .width('86%')
        .constraintSize({ maxHeight: '92%' })
        .scrollBar(BarState.Auto)
      }
      if (this.moveTargetId.length > 0) {
        Column()
          .width('100%')
          .height('100%')
          .backgroundColor('rgba(0,0,0,0.45)')
          .onClick(() => { this.moveTargetId = ''; })
        this.moveOverlay()
      }
      if (this.pwdVisible) {
        Column()
          .width('100%')
          .height('100%')
          .backgroundColor('rgba(0,0,0,0.45)')
          .onClick(() => { this.cancelPassword(); })
        this.passwordOverlay()
      }

      // Privacy overlays render last: consent (bottom), about, policy (top).
      // While the flag is still loading the consent overlay also covers the
      // manager, so nothing is usable before the dialog has been shown.
      if (!this.privacyReady || !this.privacyAccepted) {
        Column()
          .width('100%')
          .height('100%')
          .backgroundColor('rgba(0,0,0,0.55)')
          .onClick(() => {})
        this.consentOverlay()
      }
      if (this.aboutVisible) {
        Column()
          .width('100%')
          .height('100%')
          .backgroundColor('rgba(0,0,0,0.45)')
          .onClick(() => { this.aboutVisible = false; })
        this.aboutOverlay()
      }
      if (this.policyVisible) {
        Column()
          .width('100%')
          .height('100%')
          .backgroundColor('rgba(0,0,0,0.45)')
          .onClick(() => { this.policyVisible = false; })
        this.policyOverlay()
      }
    }
    .width('100%')
    .height('100%')
    // This window's own width, not the display's: a narrowed 2in1 window uses
    // the phone layout too. The assignment only happens on a real width change,
    // so this cannot re-trigger itself in a loop.
    .onAreaChange((oldValue: Area, newValue: Area) => {
      const width = Number(newValue.width);
      if (width > 0 && width !== this.viewportWidth) {
        this.viewportWidth = width;
      }
    })
  }
}
