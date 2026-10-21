import diag from 'libtianguan_diag.so';
import { common } from '@kit.AbilityKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { image } from '@kit.ImageKit';
import { KeyCode } from '@ohos.multimodalInput.keyCode';
import pointer from '@ohos.multimodalInput.pointer';
import { window } from '@kit.ArkUI';

@Entry
@Component
struct Remote {
  @State private status: string = '连接中…';
  @State private lastEvent: string = '无输入事件';
  @State private canvasShown: boolean = false;
  @State private cursorPm: image.PixelMap | undefined = undefined;
  @State private cursorLeft: number = -1;
  @State private cursorTop: number = -1;
  @State private cursorDrawW: number = 0;
  @State private cursorDrawH: number = 0;
  private settings: RenderingContextSettings = new RenderingContextSettings(true);
  private ctx: CanvasRenderingContext2D = new CanvasRenderingContext2D(this.settings);
  private canvasReady: boolean = false;
  private framePixels: image.PixelMap | undefined = undefined;
  private frameReady: boolean = false;
  private regionBuffer: ArrayBuffer = new ArrayBuffer(0);
  @State private frameWidth: number = 0;
  @State private frameHeight: number = 0;
  private viewWidth: number = 0;
  private viewHeight: number = 0;
  private lastVersion: number = -1;
  private lastCursorVersion: number = -1;
  private timer: number = -1;
  private hasMouse: boolean = false;
  // Buttons currently held, in RFB mask form (0 = none).
  private pressedMask: number = 0;
  private lastTapMs: number = 0;
  private lastTapX: number = -1;
  private lastTapY: number = -1;
  private shiftDown: boolean = false;
  private pressedKeysym: Map<number, number> = new Map<number, number>();
  private statFrames: number = 0;
  private statDecodeMs: number = 0;
  private statLastAt: number = 0;
  private statLastVersion: number = -1;
  private lastStatusText: string = '';
  private closed: boolean = false;
  private lastTouchX: number = -1;
  private lastTouchY: number = -1;
  private viewPointerX: number = -1;
  private viewPointerY: number = -1;

  aboutToAppear(): void {
    this.timer = setInterval(() => { this.poll(); }, 30);
  }

  aboutToDisappear(): void {
    this.stopPolling();
    diag.stopSession();
    this.setHostCursorVisible(true);
  }

  private stopPolling(): void {
    if (this.timer >= 0) {
      clearInterval(this.timer);
      this.timer = -1;
    }
  }

  private filesDir(): string {
    const context = this.getUIContext().getHostContext() as common.UIAbilityContext;
    return context.filesDir;
  }

  private recordEvent(text: string): void {
    this.lastEvent = text;
    // Append instead of overwrite: the order (focus result, then key events) is
    // the evidence. The file is capped so it cannot grow without bound.
    try {
      const path = this.filesDir() + '/last-input.txt';
      if (fs.accessSync(path)) {
        const size = fs.statSync(path).size;
        if (size > 8192) {
          fs.unlinkSync(path);
        }
      }
      const file = fs.openSync(path,
        fs.OpenMode.CREATE | fs.OpenMode.READ_WRITE | fs.OpenMode.APPEND);
      try {
        fs.writeSync(file.fd, text + '\n');
      } finally {
        fs.closeSync(file);
      }
    } catch (error) {
      hilog.error(0x0000, 'TianguanDiag', '记录输入事件失败：%{public}s', String(error));
    }
  }

  private setHostCursorVisible(visible: boolean): void {
    try {
      pointer.setPointerVisible(visible).catch((error: Error) => {
        hilog.error(0x0000, 'TianguanDiag', '光标可见性设置失败：%{public}s', String(error));
      });
    } catch (error) {
      hilog.error(0x0000, 'TianguanDiag', '光标可见性异常：%{public}s', String(error));
    }
  }

  private async ensureFrameBuffer(): Promise<void> {
    if (this.frameReady || this.frameWidth <= 0 || this.frameHeight <= 0) {
      return;
    }
    try {
      const empty = new ArrayBuffer(this.frameWidth * this.frameHeight * 4);
      const options: image.InitializationOptions = {
        size: { width: this.frameWidth, height: this.frameHeight },
        pixelFormat: image.PixelMapFormat.BGRA_8888,
        editable: true,
        alphaType: image.AlphaType.OPAQUE
      };
      this.framePixels = await image.createPixelMap(empty, options);
      this.frameReady = true;
      // The mirror starts empty, so ask the server for one complete frame.
      diag.requestFullFrame();
    } catch (error) {
      hilog.error(0x0000, 'TianguanDiag', '创建帧缓冲失败：%{public}s', String(error));
    }
  }

  // Applies the changed rectangle to the local framebuffer mirror and blits the
  // whole picture once — the same model vncviewer uses.
  private async applyPending(): Promise<void> {
    const started = Date.now();
    if (!this.canvasReady || this.viewWidth <= 0 || this.frameWidth <= 0) {
      return;
    }
    await this.ensureFrameBuffer();
    if (this.framePixels === undefined) {
      return;
    }
    try {
      if (this.regionBuffer.byteLength < this.frameWidth * this.frameHeight * 4) {
        this.regionBuffer = new ArrayBuffer(this.frameWidth * this.frameHeight * 4);
      }
      // One call returns both pixels and rectangle, so a frame arriving in
      // between cannot mix sizes and tear the picture.
      const rect = diag.takeRegion(this.regionBuffer);
      if (rect === 'NONE') {
        return;
      }
      const parts = rect.split(',');
      if (parts.length !== 4) {
        return;
      }
      const x = Number(parts[0]);
      const y = Number(parts[1]);
      const w = Number(parts[2]);
      const h = Number(parts[3]);
      if (w <= 0 || h <= 0) {
        return;
      }
      const area: image.PositionArea = {
        pixels: this.regionBuffer,
        offset: 0,
        stride: w * 4,
        region: { size: { width: w, height: h }, x: x, y: y }
      };
      await this.framePixels.writePixels(area);
      const scale = Math.min(this.viewWidth / this.frameWidth, this.viewHeight / this.frameHeight);
      const offsetX = (this.viewWidth - this.frameWidth * scale) / 2;
      const offsetY = (this.viewHeight - this.frameHeight * scale) / 2;
      const dw = Math.round(this.frameWidth * scale);
      const dh = Math.round(this.frameHeight * scale);
      this.ctx.drawImage(this.framePixels, Math.round(offsetX), Math.round(offsetY), dw, dh);
      this.statFrames += 1;
      this.statDecodeMs += Date.now() - started;
    } catch (error) {
      hilog.error(0x0000, 'TianguanDiag', '应用画面失败：%{public}s', String(error));
    }
  }

  // Cursor is drawn locally, so moving the mouse needs no framebuffer update.
  private async updateCursorShape(cw: number, ch: number): Promise<void> {
    if (cw <= 0 || ch <= 0) {
      return;
    }
    try {
      const buffer = new ArrayBuffer(cw * ch * 4);
      diag.copyCursor(buffer);
      const options: image.InitializationOptions = {
        size: { width: cw, height: ch },
        pixelFormat: image.PixelMapFormat.RGBA_8888,
        editable: false,
        alphaType: image.AlphaType.UNPREMUL
      };
      const previous = this.cursorPm;
      this.cursorPm = await image.createPixelMap(buffer, options);
      if (previous !== undefined) {
        previous.release();
      }
    } catch (error) {
      hilog.error(0x0000, 'TianguanDiag', '更新光标失败：%{public}s', String(error));
    }
  }

  // Repositioning the local cursor is pure layout work: no image rebuild, no
  // server round trip.
  // The local cursor follows the physical pointer, like a normal viewer: its
  // position never needs a server round trip.
  private placeCursor(cw: number, ch: number, hotX: number, hotY: number): void {
    if (this.viewWidth <= 0 || this.frameWidth <= 0 || cw <= 0 || ch <= 0) {
      return;
    }
    if (this.viewPointerX < 0 || this.viewPointerY < 0) {
      return;
    }
    const scale = Math.min(this.viewWidth / this.frameWidth, this.viewHeight / this.frameHeight);
    this.cursorDrawW = Math.max(1, Math.round(cw * scale));
    this.cursorDrawH = Math.max(1, Math.round(ch * scale));
    this.cursorLeft = Math.round(this.viewPointerX - hotX * scale);
    this.cursorTop = Math.round(this.viewPointerY - hotY * scale);
  }

  private poll(): void {
    let text = '';
    try {
      text = diag.sessionStatus();
    } catch (error) {
      this.lastStatusText = 'THROW ' + String(error);
      return;
    }
    this.lastStatusText = text;
    const running = text.indexOf('running=1') >= 0;
    const versionAt = text.indexOf('version=');
    const sizeAt = text.indexOf('size=');
    const errorAt = text.indexOf('error=');
    if (sizeAt >= 0) {
      const raw = text.substring(sizeAt + 5, text.indexOf(' ', sizeAt) >= 0
        ? text.indexOf(' ', sizeAt) : text.length);
      const parts = raw.split('x');
      if (parts.length === 2) {
        this.frameWidth = Number(parts[0]);
        this.frameHeight = Number(parts[1]);
      }
    }
    if (versionAt >= 0) {
      const version = Number(text.substring(versionAt + 8, text.indexOf(' ', versionAt)));
      if (!Number.isNaN(version) && version > 0) {
        this.canvasShown = true;
        this.applyPending();
      }
    }
    // Cursor shape and position come from the server but are drawn locally.
    let cw = 0;
    let ch = 0;
    let hotX = 0;
    let hotY = 0;
    let cursorVersion = 0;
    const cursorAt = text.indexOf('cursor=');
    if (cursorAt >= 0) {
      const end = text.indexOf(' ', cursorAt);
      const raw = text.substring(cursorAt + 7, end < 0 ? text.length : end);
      const parts = raw.split(',');
      if (parts.length === 4) {
        const dims = parts[0].split('x');
        if (dims.length === 2) {
          cw = Number(dims[0]);
          ch = Number(dims[1]);
        }
        hotX = Number(parts[1]);
        hotY = Number(parts[2]);
        cursorVersion = Number(parts[3]);
      }
    }
    let posX = 0;
    let posY = 0;
    const posAt = text.indexOf('cpos=');
    if (posAt >= 0) {
      const end = text.indexOf(' ', posAt);
      const parts = text.substring(posAt + 5, end < 0 ? text.length : end).split(',');
      if (parts.length === 2) {
        posX = Number(parts[0]);
        posY = Number(parts[1]);
      }
    }
    let cursorBlank = false;
    const blankAt = text.indexOf('blank=');
    if (blankAt >= 0) {
      cursorBlank = text.substring(blankAt + 6, blankAt + 7) === '1';
    }
    if (cursorBlank) {
      // The server told us it drew the pointer into the picture itself.
      if (this.cursorPm !== undefined) {
        const previous = this.cursorPm;
        this.cursorPm = undefined;
        previous.release();
      }
      this.cursorLeft = -1;
    } else if (cw > 0 && ch > 0 && cursorVersion !== this.lastCursorVersion &&
        this.canvasShown) {
      this.lastCursorVersion = cursorVersion;
      this.updateCursorShape(cw, ch);
    }
    if (this.canvasShown && !cursorBlank) {
      this.placeCursor(cw, ch, hotX, hotY);
    }
    if (errorAt >= 0) {
      // The session thread keeps retrying in the background, so report the
      // state but keep polling; stopping here made a dropped connection look
      // like a frozen window.
      this.status = '连接中断，正在重连：' + text.substring(errorAt + 6);
      return;
    }
    this.status = running ? '已连接（实时刷新）' : '连接中…';
    this.writeStats(versionAt >= 0 ? this.lastVersion : -1);
  }

  private writeStats(version: number): void {
    const now = Date.now();
    if (this.statLastAt === 0) {
      this.statLastAt = now;
      this.statLastVersion = version;
      return;
    }
    const elapsed = now - this.statLastAt;
    if (elapsed < 3000) {
      return;
    }
    const fps = elapsed > 0 ? (this.statFrames * 1000 / elapsed) : 0;
    const decodeAvg = this.statFrames > 0 ? (this.statDecodeMs / this.statFrames) : 0;
    const versions = version - this.statLastVersion;
    const line = 'fps=' + fps.toFixed(1) + ' frames=' + this.statFrames +
      ' decodeMs=' + decodeAvg.toFixed(1) + ' versions=' + versions +
      ' windowMs=' + elapsed + ' status=[' + this.lastStatusText + ']';
    try {
      const file = fs.openSync(this.filesDir() + '/perf.txt',
        fs.OpenMode.CREATE | fs.OpenMode.READ_WRITE | fs.OpenMode.APPEND);
      try {
        fs.writeSync(file.fd, line + '\n');
      } finally {
        fs.closeSync(file);
      }
    } catch (error) {
      hilog.error(0x0000, 'TianguanDiag', '写性能日志失败：%{public}s', String(error));
    }
    this.statFrames = 0;
    this.statDecodeMs = 0;
    this.statLastAt = now;
    this.statLastVersion = version;
  }

  // RFB button bits: 1 left, 2 middle, 4 right (8 and 16 are the wheel notches).
  private static buttonMask(button: MouseButton): number {
    if (button === MouseButton.Right) {
      return 4;
    }
    if (button === MouseButton.Middle) {
      return 2;
    }
    return 1;
  }

  private pointerDown(x: number, y: number, mask: number = 1): void {
    this.pressedMask = mask;
    // Move first: the working desktop path sends move, press, release; sending
    // the press without moving the pointer can be ignored by the server.
    this.sendPointerReport(x, y, 0);
    this.sendPointerReport(x, y, mask);
  }

  private pointerUp(x: number, y: number, fromTouch: boolean): void {
    if (this.pressedMask === 0) {
      return;
    }
    this.pressedMask = 0;
    const released = this.sendPointerReport(x, y, 0);
    // Touch screens have no double-click gesture, so two quick taps at the same
    // spot are turned into a real double click. A mouse already sends its own
    // double click, so that path must not add a third one.
    const now = Date.now();
    const near = Math.abs(x - this.lastTapX) < 24 && Math.abs(y - this.lastTapY) < 24;
    if (fromTouch && now - this.lastTapMs < 400 && near) {
      const secondMove = this.sendPointerReport(x, y, 0);
      const secondPress = this.sendPointerReport(x, y, 1);
      const secondRelease = this.sendPointerReport(x, y, 0);
      this.lastTapMs = 0;
      this.recordEvent('double x=' + Math.round(x) + ' y=' + Math.round(y) +
        ' ' + secondMove + '/' + secondPress + '/' + secondRelease + ' first=' + released);
      return;
    }
    this.lastTapMs = now;
    this.lastTapX = x;
    this.lastTapY = y;
    this.recordEvent('release x=' + Math.round(x) + ' y=' + Math.round(y) +
      ' release=' + released);
  }

  private testClick(): void {
    // Probe target: double-click the desktop Home icon at remote (63, 68).
    // Opening the file manager is unmistakable, and this bypasses touch
    // coordinate mapping so it isolates the send path from the mapping.
    let report = 'test';
    for (let round = 0; round < 2; round++) {
      report += ' ' + diag.sendPointer(63, 68, 0);
      report += '/' + diag.sendPointer(63, 68, 1);
      report += '/' + diag.sendPointer(63, 68, 0);
    }
    this.recordEvent(report);
  }

  // Maps a view point to remote pixels; returns -1,-1 when outside the picture.
  private mapToRemote(x: number, y: number): number[] {
    if (this.frameWidth <= 0 || this.frameHeight <= 0 ||
        this.viewWidth <= 0 || this.viewHeight <= 0) {
      return [-1, -1];
    }
    const scale = Math.min(this.viewWidth / this.frameWidth, this.viewHeight / this.frameHeight);
    const contentW = this.frameWidth * scale;
    const contentH = this.frameHeight * scale;
    const offsetX = (this.viewWidth - contentW) / 2;
    const offsetY = (this.viewHeight - contentH) / 2;
    if (x < offsetX || y < offsetY || x > offsetX + contentW || y > offsetY + contentH) {
      return [-1, -1];
    }
    return [
      Math.max(0, Math.min(this.frameWidth - 1, Math.round((x - offsetX) / scale))),
      Math.max(0, Math.min(this.frameHeight - 1, Math.round((y - offsetY) / scale)))
    ];
  }

  // RFB has no wheel message: a wheel notch is button 4 (up) or 5 (down),
  // sent as press + release with the pointer mask carrying that bit.
  // The global focusControl.requestFocus only asks for a transfer on the next
  // frame and returned false here; the UIContext FocusController transfers
  // synchronously, which is what a click handler needs.
  private requestViewFocus(): void {
    try {
      this.getUIContext().getFocusController().activate(true);
      this.getUIContext().getFocusController().requestFocus('remote_area');
      this.getUIContext().getFocusController().requestFocus('page_root');
    } catch (error) {
      // Focus transfer is best effort: if it fails, keys simply may not arrive.
    }
  }

  private forwardWheel(event: AxisEvent): void {
    if (event.action !== AxisAction.UPDATE && event.action !== AxisAction.END) {
      return;
    }
    const vertical = event.getVerticalAxisValue();
    if (vertical === 0 || !this.canvasShown) {
      return;
    }
    const point = this.mapToRemote(this.viewPointerX, this.viewPointerY);
    if (point[0] < 0 || point[1] < 0) {
      return;
    }
    const mask = vertical > 0 ? 16 : 8;
    diag.sendPointer(point[0], point[1], mask);
    diag.sendPointer(point[0], point[1], 0);
  }

  private sendPointerReport(x: number, y: number, mask: number): string {
    if (this.frameWidth <= 0 || this.frameHeight <= 0 || this.viewWidth <= 0 || this.viewHeight <= 0) {
      return 'NODIM';
    }
    const scale = Math.min(this.viewWidth / this.frameWidth, this.viewHeight / this.frameHeight);
    const contentW = this.frameWidth * scale;
    const contentH = this.frameHeight * scale;
    const offsetX = (this.viewWidth - contentW) / 2;
    const offsetY = (this.viewHeight - contentH) / 2;
    if (x < offsetX || y < offsetY || x > offsetX + contentW || y > offsetY + contentH) {
      return 'OUTSIDE';
    }
    const fx = Math.max(0, Math.min(this.frameWidth - 1, Math.round((x - offsetX) / scale)));
    const fy = Math.max(0, Math.min(this.frameHeight - 1, Math.round((y - offsetY) / scale)));
    return diag.sendPointer(fx, fy, mask);
  }

  private sendPointerAt(x: number, y: number, mask: number): void {
    if (this.frameWidth <= 0 || this.frameHeight <= 0 || this.viewWidth <= 0 || this.viewHeight <= 0) {
      return;
    }
    const scale = Math.min(this.viewWidth / this.frameWidth, this.viewHeight / this.frameHeight);
    const contentW = this.frameWidth * scale;
    const contentH = this.frameHeight * scale;
    const offsetX = (this.viewWidth - contentW) / 2;
    const offsetY = (this.viewHeight - contentH) / 2;
    if (x < offsetX || y < offsetY || x > offsetX + contentW || y > offsetY + contentH) {
      return;
    }
    const fx = Math.round((x - offsetX) / scale);
    const fy = Math.round((y - offsetY) / scale);
    diag.sendPointer(Math.max(0, Math.min(this.frameWidth - 1, fx)),
      Math.max(0, Math.min(this.frameHeight - 1, fy)), mask);
  }

  private keysymFor(event: KeyEvent): number {
    switch (event.keyCode) {
      case KeyCode.KEYCODE_ENTER:
      case KeyCode.KEYCODE_NUMPAD_ENTER:
        return 0xff0d;
      case KeyCode.KEYCODE_DEL:
        return 0xff08;
      case KeyCode.KEYCODE_FORWARD_DEL:
        return 0xffff;
      case KeyCode.KEYCODE_TAB:
        return 0xff09;
      case KeyCode.KEYCODE_ESCAPE:
        return 0xff1b;
      case KeyCode.KEYCODE_DPAD_UP:
        return 0xff52;
      case KeyCode.KEYCODE_DPAD_DOWN:
        return 0xff54;
      case KeyCode.KEYCODE_DPAD_LEFT:
        return 0xff51;
      case KeyCode.KEYCODE_DPAD_RIGHT:
        return 0xff53;
      case KeyCode.KEYCODE_MOVE_HOME:
        return 0xff50;
      case KeyCode.KEYCODE_MOVE_END:
        return 0xff57;
      case KeyCode.KEYCODE_PAGE_UP:
        return 0xff55;
      case KeyCode.KEYCODE_PAGE_DOWN:
        return 0xff56;
      case KeyCode.KEYCODE_SHIFT_LEFT:
        return 0xffe1;
      case KeyCode.KEYCODE_SHIFT_RIGHT:
        return 0xffe2;
      case KeyCode.KEYCODE_CTRL_LEFT:
        return 0xffe3;
      case KeyCode.KEYCODE_CTRL_RIGHT:
        return 0xffe4;
      case KeyCode.KEYCODE_ALT_LEFT:
        return 0xffe9;
      case KeyCode.KEYCODE_ALT_RIGHT:
        return 0xffea;
      default:
        break;
    }
    // HarmonyOS reports letters/digits as ranges, and keyText holds the constant
    // name ("KEYCODE_A") rather than the character, so map by key code.
    const kc = event.keyCode;
    if (kc >= KeyCode.KEYCODE_A && kc <= KeyCode.KEYCODE_Z) {
      return 0x61 + (kc - KeyCode.KEYCODE_A);
    }
    if (kc >= KeyCode.KEYCODE_0 && kc <= KeyCode.KEYCODE_9) {
      return 0x30 + (kc - KeyCode.KEYCODE_0);
    }
    const punctuation: Map<number, number> = new Map<number, number>([
      [KeyCode.KEYCODE_SPACE, 0x20],
      [KeyCode.KEYCODE_MINUS, 0x2d],
      [KeyCode.KEYCODE_EQUALS, 0x3d],
      [KeyCode.KEYCODE_LEFT_BRACKET, 0x5b],
      [KeyCode.KEYCODE_RIGHT_BRACKET, 0x5d],
      [KeyCode.KEYCODE_BACKSLASH, 0x5c],
      [KeyCode.KEYCODE_SEMICOLON, 0x3b],
      [KeyCode.KEYCODE_APOSTROPHE, 0x27],
      [KeyCode.KEYCODE_COMMA, 0x2c],
      [KeyCode.KEYCODE_PERIOD, 0x2e],
      [KeyCode.KEYCODE_SLASH, 0x2f],
      [KeyCode.KEYCODE_GRAVE, 0x60]
    ]);
    const punctuationSym = punctuation.get(kc);
    if (punctuationSym !== undefined) {
      return punctuationSym;
    }
    const text = event.keyText;
    if (text !== undefined && text.length === 1) {
      const code = text.charCodeAt(0);
      if (code >= 0x20) {
        return code;
      }
    }
    return 0;
  }

  private forwardKey(event: KeyEvent): void {
    // Track modifiers locally: the server did not apply the Shift keysym we
    // sent, so shifted characters are translated to their own keysym instead.
    if (event.keyCode === KeyCode.KEYCODE_SHIFT_LEFT ||
        event.keyCode === KeyCode.KEYCODE_SHIFT_RIGHT) {
      this.shiftDown = event.type === KeyType.Down;
    }
    let keysym = this.keysymFor(event);
    const down = event.type === KeyType.Down;
    if (this.shiftDown && keysym > 0) {
      const shifted = Remote.shiftedKeysym(keysym);
      if (shifted !== 0) {
        keysym = shifted;
      }
    }
    if (keysym === 0) {
      return;
    }
    if (down) {
      // Remember what was pressed per physical key so the release always uses
      // the same keysym even if the modifiers changed in between.
      this.pressedKeysym.set(event.keyCode, keysym);
      diag.sendKey(keysym, 1);
    } else if (event.type === KeyType.Up) {
      const pressedSym = this.pressedKeysym.get(event.keyCode);
      if (pressedSym !== undefined) {
        keysym = pressedSym;
        this.pressedKeysym.delete(event.keyCode);
      }
      diag.sendKey(keysym, 0);
    }
  }

  // US layout: the shifted symbol for the unshifted ASCII keysym.
  private static shiftedKeysym(sym: number): number {
    const pairs: Map<number, number> = new Map<number, number>([
      [0x61, 0x41], [0x62, 0x42], [0x63, 0x43], [0x64, 0x44], [0x65, 0x45],
      [0x66, 0x46], [0x67, 0x47], [0x68, 0x48], [0x69, 0x49], [0x6a, 0x4a],
      [0x6b, 0x4b], [0x6c, 0x4c], [0x6d, 0x4d], [0x6e, 0x4e], [0x6f, 0x4f],
      [0x70, 0x50], [0x71, 0x51], [0x72, 0x52], [0x73, 0x53], [0x74, 0x54],
      [0x75, 0x55], [0x76, 0x56], [0x77, 0x57], [0x78, 0x58], [0x79, 0x59],
      [0x7a, 0x5a],
      [0x31, 0x21], [0x32, 0x40], [0x33, 0x23], [0x34, 0x24], [0x35, 0x25],
      [0x36, 0x5e], [0x37, 0x26], [0x38, 0x2a], [0x39, 0x28], [0x30, 0x29],
      [0x2d, 0x5f], [0x3d, 0x2b], [0x5b, 0x7b], [0x5d, 0x7d],
      [0x5c, 0x7c], [0x3b, 0x3a], [0x27, 0x22], [0x2c, 0x3c],
      [0x2e, 0x3e], [0x2f, 0x3f], [0x60, 0x7e]
    ]);
    const shifted = pairs.get(sym);
    return shifted === undefined ? 0 : shifted;
  }

  private async closeWindow(): Promise<void> {
    if (this.closed) {
      return;
    }
    this.closed = true;
    this.stopPolling();
    diag.stopSession();
    this.setHostCursorVisible(true);
    try {
      const remote = await window.findWindow('remote');
      await remote.destroyWindow();
    } catch (error) {
      hilog.error(0x0000, 'TianguanDiag', '关闭远程窗口失败：%{public}s', String(error));
    }
  }

  build() {
    Column({ space: 8 }) {
      Text(this.status).fontSize(14).maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
      if (this.canvasShown) {
        Stack({ alignContent: Alignment.BottomEnd }) {
          Column() {
            Stack({ alignContent: Alignment.TopStart }) {
              Canvas(this.ctx)
                .width('100%')
                .height('100%')
                .draggable(false)
                .onReady(() => {
                  this.canvasReady = true;
                })
              if (this.cursorPm !== undefined && this.cursorLeft >= -64) {
                Image(this.cursorPm)
                  .width(this.cursorDrawW)
                  .height(this.cursorDrawH)
                  .position({ x: this.cursorLeft, y: this.cursorTop })
                  .hitTestBehavior(HitTestMode.None)
                  .draggable(false)
              }
            }
            .width('100%')
            .height('100%')
          }
          .width('100%')
          .height('100%')
          .borderWidth(1)
          .id('remote_area')
          .key('remote_area')
          .draggable(false)
          .focusable(true)
          .focusOnTouch(true)
          .defaultFocus(true)
          .tabIndex(0)
          .onAreaChange((oldValue: Area, newValue: Area) => {
            this.viewWidth = Number(newValue.width);
            this.viewHeight = Number(newValue.height);
            // Canvas content is lost on resize and the first paint may happen
            // before sizes are known, so ask for one complete frame.
            diag.requestFullFrame();
          })
          // Keys are handled on the page root only: key events bubble, so two
          // handlers forwarded every key twice and broke Shift combinations.
          .onAxisEvent((event: AxisEvent) => {
            this.forwardWheel(event);
          })
          .onHover((isHover: boolean) => {
            this.setHostCursorVisible(!isHover);
          })
          .onTouch((event: TouchEvent) => {
            // HarmonyOS may synthesise mouse events from touch, so both handlers
            // run; the pressed flag is what deduplicates them.
            if (event.touches.length > 0) {
              this.lastTouchX = event.touches[0].x;
              this.lastTouchY = event.touches[0].y;
              this.viewPointerX = this.lastTouchX;
              this.viewPointerY = this.lastTouchY;
            }
            if (event.type === TouchType.Down) {
              this.requestViewFocus();
              this.pointerDown(this.lastTouchX, this.lastTouchY);
            } else if (event.type === TouchType.Move) {
              this.sendPointerAt(this.lastTouchX, this.lastTouchY, this.pressedMask);
            } else if (event.type === TouchType.Up || event.type === TouchType.Cancel) {
              this.pointerUp(this.lastTouchX, this.lastTouchY, true);
            }
          })
          .onMouse((event: MouseEvent) => {
            this.viewPointerX = event.x;
            this.viewPointerY = event.y;
            if (event.action === MouseAction.Press) {
              // Focus normally follows touch, not mouse clicks, so grab it here;
              // otherwise key events never reach onKeyEvent.
              this.requestViewFocus();
              this.pointerDown(event.x, event.y, Remote.buttonMask(event.button));
            } else if (event.action === MouseAction.Release) {
              this.pointerUp(event.x, event.y, false);
            } else {
              this.sendPointerAt(event.x, event.y, this.pressedMask);
            }
          })
          // Consume drags so the system does not treat a drag on the remote
          // desktop as moving the application window.
          .parallelGesture(
            PanGesture({ fingers: 1, distance: 1 })
              .onActionStart(() => {})
              .onActionUpdate(() => {})
              .onActionEnd(() => {})
          )
          Row({ space: 6 }) {
            // Kept out of the focus chain on purpose: the remote view must be
            // the only focusable target so keys reach its onKeyEvent.
            Button('测试点击').fontSize(12).height(30).focusable(false)
              .onClick(() => { this.testClick(); })
            Button('断开').fontSize(12).height(30).focusable(false)
              .onClick(() => { this.closeWindow(); })
          }
          .padding(6)
        }
        .width('100%')
        .layoutWeight(1)
      } else {
        Text('等待首帧…').fontSize(16).layoutWeight(1)
      }
    }
      .padding(10)
      .width('100%')
      .height('100%')
      .alignItems(HorizontalAlign.Start)
      // The page root also watches keys: key events bubble up from whichever
      // child holds focus, so this path does not depend on the view itself.
      .id('page_root')
      .key('page_root')
      .focusable(true)
      .defaultFocus(true)
      .onKeyEvent((event: KeyEvent) => {
        this.forwardKey(event);
      })
  }
}
