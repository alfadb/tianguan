import diag from 'libtianguan_diag.so';
import { common } from '@kit.AbilityKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import asset from "@ohos.security.asset";
import { util } from '@kit.ArkTS';

interface Connection {
  name: string;
  host: string;
  port: string;
  savePassword: boolean;
}

@Entry
@Component
struct Index {
  @State private connections: Connection[] = [];
  @State private name: string = '';
  @State private host: string = '192.168.50.62';
  @State private port: string = '5901';
  @State private password: string = '';
  @State private savePassword: boolean = false;
  @State private status: string = '';
  @State private pendingPin: string = '';
  @State private busy: boolean = false;
  private started: boolean = false;

  aboutToAppear(): void {
    if (!this.started) {
      this.started = true;
      this.connections = this.loadConnections();
    }
  }

  private filesDir(): string {
    const context = this.getUIContext().getHostContext() as common.UIAbilityContext;
    return context.filesDir;
  }

  private readText(name: string): string {
    try {
      const path = this.filesDir() + '/' + name;
      if (!fs.accessSync(path)) {
        return '';
      }
      return fs.readTextSync(path);
    } catch (error) {
      return '';
    }
  }

  private writeText(name: string, text: string): void {
    const file = fs.openSync(this.filesDir() + '/' + name,
      fs.OpenMode.CREATE | fs.OpenMode.READ_WRITE | fs.OpenMode.TRUNC);
    try {
      fs.writeSync(file.fd, text);
    } finally {
      fs.closeSync(file);
    }
  }

  private removeFile(name: string): void {
    try {
      const path = this.filesDir() + '/' + name;
      if (fs.accessSync(path)) {
        fs.unlinkSync(path);
      }
    } catch (error) {
      hilog.error(0x0000, 'TianguanDiag', '删除文件失败：%{public}s', String(error));
    }
  }

  private loadConnections(): Connection[] {
    const text = this.readText('connections.json');
    if (text.trim().length === 0) {
      return [];
    }
    try {
      return JSON.parse(text) as Connection[];
    } catch (error) {
      hilog.error(0x0000, 'TianguanDiag', '连接列表解析失败：%{public}s', String(error));
      return [];
    }
  }

  private persistConnections(): void {
    this.writeText('connections.json', JSON.stringify(this.connections));
  }

  private aliasFor(item: Connection): string {
    return 'tianguan:' + item.host + ':' + item.port;
  }

  private toBytes(text: string): Uint8Array {
    return new util.TextEncoder().encodeInto(text);
  }

  private fromBytes(bytes: Uint8Array | undefined): string {
    if (bytes === undefined) {
      return '';
    }
    return util.TextDecoder.create('utf-8').decodeWithStream(bytes);
  }

  private aliasQuery(item: Connection): asset.AssetMap {
    const query: asset.AssetMap = new Map();
    query.set(asset.Tag.ALIAS, this.toBytes(this.aliasFor(item)));
    return query;
  }

  private loadQuery(item: Connection): asset.AssetMap {
    const query = this.aliasQuery(item);
    // Without RETURN_TYPE=ALL a query returns only the queried attributes, so
    // the secret would come back missing.
    query.set(asset.Tag.RETURN_TYPE, asset.ReturnType.ALL);
    return query;
  }

  private async saveSecret(item: Connection, secret: string): Promise<boolean> {
    try {
      try {
        await asset.remove(this.aliasQuery(item));
      } catch (error) {
        // No previous entry is the normal first-save case.
      }
      const map: asset.AssetMap = new Map();
      map.set(asset.Tag.ALIAS, this.toBytes(this.aliasFor(item)));
      map.set(asset.Tag.SECRET, this.toBytes(secret));
      map.set(asset.Tag.ACCESSIBILITY, asset.Accessibility.DEVICE_FIRST_UNLOCKED);
      await asset.add(map);
      return true;
    } catch (error) {
      hilog.error(0x0000, 'TianguanDiag', '保存密码失败：%{public}s', String(error));
      return false;
    }
  }

  private async loadSecret(item: Connection): Promise<string> {
    try {
      const rows: asset.AssetMap[] = await asset.query(this.loadQuery(item));
      if (rows.length === 0) {
        return '';
      }
      return this.fromBytes(rows[0].get(asset.Tag.SECRET) as Uint8Array | undefined);
    } catch (error) {
      hilog.error(0x0000, 'TianguanDiag', '读取密码失败：%{public}s', String(error));
      return '';
    }
  }

  private async removeSecret(item: Connection): Promise<void> {
    try {
      await asset.remove(this.aliasQuery(item));
    } catch (error) {
      hilog.error(0x0000, 'TianguanDiag', '删除密码失败：%{public}s', String(error));
    }
  }

  private saveCurrent(): void {
    if (this.name.trim().length === 0 || this.host.trim().length === 0) {
      this.status = '请填写名称和地址';
      return;
    }
    const item: Connection = {
      name: this.name.trim(),
      host: this.host.trim(),
      port: this.port.trim().length > 0 ? this.port.trim() : '5901',
      savePassword: this.savePassword
    };
    const kept = this.connections.filter((entry: Connection) =>
      entry.host !== item.host || entry.port !== item.port);
    kept.push(item);
    this.connections = kept;
    this.persistConnections();
    if (this.savePassword) {
      this.saveSecret(item, this.password).then((ok: boolean) => {
        this.status = ok ? '已保存连接与密码（系统密钥库）' : '已保存连接，但密码保存失败';
      });
    } else {
      this.removeSecret(item);
      this.status = '已保存连接（未保存密码）';
    }
  }

  private deleteCurrent(index: number): void {
    const item = this.connections[index];
    this.connections = this.connections.filter((entry: Connection, at: number) => at !== index);
    this.persistConnections();
    this.removeSecret(item);
    this.status = '已删除连接';
  }

  private pick(item: Connection): void {
    this.name = item.name;
    this.host = item.host;
    this.port = item.port;
    this.savePassword = item.savePassword;
    this.password = '';
    if (item.savePassword) {
      this.loadSecret(item).then((secret: string) => {
        if (secret.length > 0) {
          this.password = secret;
        }
      });
    }
  }

  private async openRemoteWindow(): Promise<void> {
    const stage = AppStorage.get<window.WindowStage>('windowStage');
    if (stage === undefined) {
      this.status = '无法获取窗口，远程画面未打开';
      return;
    }
    // TEMP: sub-windows never received keyboard events (focus ok, zero keys),
    // so the remote page runs in the main window until that is solved.
    const useSubWindow = false;
    if (!useSubWindow) {
      try {
        this.getUIContext().getRouter().pushUrl({ url: 'pages/Remote' });
        this.status = '远程画面已在当前窗口打开';
      } catch (routeError) {
        this.status = '打开远程画面失败：' + String(routeError);
      }
      return;
    }
    try {
      const existing = await window.findWindow('remote');
      await existing.destroyWindow();
    } catch (error) {
      // No previous remote window.
    }
    try {
      // Size the sub-window relative to the main window: a fixed 1280x860 is
      // larger than a phone screen and would land off-screen.
      const main = stage.getMainWindowSync();
      const rect = main.getWindowProperties().windowRect;
      const width = Math.max(320, Math.round(rect.width * 0.92));
      const height = Math.max(240, Math.round(rect.height * 0.9));
      const remote = await stage.createSubWindow('remote');
      await remote.setUIContent('pages/Remote');
      await remote.resize(width, height);
      try {
        // Dragging inside the remote desktop must never move this window.
        remote.setWindowTitleMoveEnabled(false);
      } catch (moveError) {
        hilog.error(0x0000, 'TianguanDiag', '禁用窗口拖动失败：%{public}s', String(moveError));
      }
      await remote.show();
      try {
        // Keyboard input follows the focused *window*; a sub-window created like
        // this keeps focus on the main window, so key events never arrive.
        await remote.setWindowFocusable(true);
        const mainId = main.getWindowProperties().id;
        const remoteId = remote.getWindowProperties().id;
        await window.shiftAppWindowFocus(mainId, remoteId);
      } catch (focusError) {
        hilog.error(0x0000, 'TianguanDiag', '转移窗口焦点失败：%{public}s', String(focusError));
      }
    } catch (error) {
      hilog.error(0x0000, 'TianguanDiag', '打开远程窗口失败：%{public}s', String(error));
      // Fallback: show the same page inside the main window.
      try {
        this.getUIContext().getRouter().pushUrl({ url: 'pages/Remote' });
        this.status = '子窗口不可用，已在主窗口打开远程画面';
      } catch (fallbackError) {
        this.status = '打开远程画面失败：' + String(fallbackError);
      }
    }
  }

  private async connect(): Promise<void> {
    if (this.busy) {
      return;
    }
    if (this.host.trim().length === 0) {
      this.status = '请填写服务器地址';
      return;
    }
    if (this.password.length === 0) {
      this.status = '请先输入密码';
      return;
    }
    this.busy = true;
    this.status = '连接中…';
    this.pendingPin = '';
    try {
      const item: Connection = {
        name: this.name.trim().length > 0 ? this.name.trim() : this.host.trim(),
        host: this.host.trim(),
        port: this.port.trim().length > 0 ? this.port.trim() : '5901',
        savePassword: this.savePassword
      };
      const pins = this.readPins();
      const key = item.host + ':' + item.port;
      let pin = pins[key] === undefined ? '' : pins[key];
      // Without a trusted fingerprint the session aborts before authentication,
      // so probe first and let the user confirm the server identity.
      if (pin.length === 0) {
        this.writeText('vnc-test.json',
          '{"host":"' + item.host + '","port":' + item.port + '}\n');
        this.writeText('vnc-test.pass', this.password + '\n');
        const probe = await diag.runNetworkTest(this.filesDir());
        this.removeFile('vnc-test.pass');
        const marker = 'NEEDS_CONFIRMATION: ';
        const at = probe.indexOf(marker);
        if (at >= 0) {
          const end = probe.indexOf('\n', at);
          this.pendingPin = probe.substring(at + marker.length,
            end < 0 ? probe.length : end).trim();
          this.status = '该服务器尚未信任，请核对指纹后确认';
          this.busy = false;
          return;
        }
        if (probe.indexOf('FAIL ') >= 0) {
          this.status = probe.trim();
          this.busy = false;
          return;
        }
        pin = '';
      }
      const config = pin.length > 0
        ? '{"host":"' + item.host + '","port":' + item.port + ',"expectedFingerprint":"' + pin + '"}'
        : '{"host":"' + item.host + '","port":' + item.port + '}';
      this.writeText('vnc-test.json', config + '\n');
      this.writeText('vnc-test.pass', this.password + '\n');
      const reply = diag.startSession(this.filesDir());
      this.removeFile('vnc-test.pass');
      if (this.savePassword) {
        this.saveSecret(item, this.password);
      }
      if (reply !== 'STARTED' && reply !== 'ALREADY_RUNNING') {
        this.status = reply;
        this.busy = false;
        return;
      }
      await this.openRemoteWindow();
      this.status = '连接已启动，远程画面在独立窗口';
    } catch (error) {
      this.status = '连接失败：' + String(error);
      this.removeFile('vnc-test.pass');
    }
    this.busy = false;
  }

  private trustAndConnect(): void {
    if (this.pendingPin.length === 0) {
      return;
    }
    const pins = this.readPins();
    pins[this.host.trim() + ':' + (this.port.trim().length > 0 ? this.port.trim() : '5901')] =
      this.pendingPin;
    this.writeText('vnc-pins.json', JSON.stringify(pins));
    this.pendingPin = '';
    this.connect();
  }

  private readPins(): Record<string, string> {
    try {
      const text = this.readText('vnc-pins.json');
      if (text.trim().length === 0) {
        return {};
      }
      return JSON.parse(text) as Record<string, string>;
    } catch (error) {
      return {};
    }
  }

  build() {
    Column({ space: 10 }) {
      Text('天关').fontSize(26).fontWeight(FontWeight.Bold)
      Text('保存的连接').fontSize(16).fontWeight(FontWeight.Medium)
      if (this.connections.length === 0) {
        Text('还没有保存的连接').fontSize(14)
      } else {
        List() {
          ForEach(this.connections, (item: Connection, index: number) => {
            ListItem() {
              Row({ space: 10 }) {
                Column({ space: 2 }) {
                  Text(item.name).fontSize(16)
                  Text(item.host + ':' + item.port).fontSize(13)
                }.alignItems(HorizontalAlign.Start).layoutWeight(1)
                Button('使用').onClick(() => { this.pick(item); })
                Button('删除').onClick(() => { this.deleteCurrent(index); })
              }.width('100%')
            }
          }, (item: Connection) => item.host + ':' + item.port)
        }.height(140).width('100%')
      }
      TextInput({ text: this.name, placeholder: '连接名称' })
        .onChange((value: string) => { this.name = value; })
        .width('100%')
      Row({ space: 8 }) {
        TextInput({ text: this.host, placeholder: '服务器地址' })
          .onChange((value: string) => { this.host = value; })
          .layoutWeight(2)
        TextInput({ text: this.port, placeholder: '端口' })
          .type(InputType.Number)
          .onChange((value: string) => { this.port = value; })
          .layoutWeight(1)
      }.width('100%')
      TextInput({ placeholder: '密码（不回显）' })
        .type(InputType.Password)
        .onChange((value: string) => { this.password = value; })
        .width('100%')
      Row({ space: 8 }) {
        Checkbox()
          .select(this.savePassword)
          .onChange((checked: boolean) => { this.savePassword = checked; })
        Text('保存密码（系统密钥库）').fontSize(14)
      }
      Row({ space: 8 }) {
        Button(this.busy ? '连接中…' : '连接').enabled(!this.busy)
          .onClick(() => { this.connect(); })
        Button('保存连接').onClick(() => { this.saveCurrent(); })
      }
      Text(this.status).fontSize(14)
      if (this.pendingPin.length > 0) {
        Text('服务端指纹：' + this.pendingPin).fontSize(14).fontWeight(FontWeight.Medium)
        Text('请与服务端核对。确认后记住该指纹，之后不一致将拒绝连接。').fontSize(12)
        Button('确认并信任').onClick(() => { this.trustAndConnect(); })
      }
      Scroll() {
        Text('提示：连接成功后远程画面会在独立窗口打开；密码只在系统密钥库保存，不写入连接列表。')
          .fontSize(12).width('100%')
      }.layoutWeight(1).width('100%')
    }.padding(20).width('100%').height('100%').alignItems(HorizontalAlign.Start)
  }
}
