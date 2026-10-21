import { UIAbility } from '@kit.AbilityKit';
import { window } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
export default class EntryAbility extends UIAbility {
  onWindowStageCreate(windowStage: window.WindowStage): void {
    // The remote desktop is shown in a sub-window, so keep the stage reachable.
    AppStorage.setOrCreate('windowStage', windowStage);
    windowStage.loadContent('pages/Index', (error) => {
      if (error.code) {
        hilog.error(0x0000, 'TianguanDiag', '页面加载失败：%{public}s', error.message);
      }
    });
  }
}
