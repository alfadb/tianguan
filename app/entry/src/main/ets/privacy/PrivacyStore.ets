/**
 * Privacy compliance state: the first-launch consent flag (preferences), the
 * packaged privacy policy text (rawfile) and the displayed app version.
 *
 * The consent flag is intentionally a very small standalone preferences file
 * ("privacy" namespace), separate from the connection data: clearing it must
 * never touch connections.json or the asset key store.
 */
import { bundleManager, common } from '@kit.AbilityKit';
import { preferences } from '@kit.ArkData';
import { util } from '@kit.ArkTS';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN: number = 0x0000;
const TAG: string = 'TianguanDiag';
const PREFERENCES_NAME: string = 'privacy';
const KEY_ACCEPTED: string = 'privacy_accepted';
const POLICY_RAWFILE: string = 'privacy_policy.md';

/** True when the user has accepted the privacy policy on this device. */
export async function isPrivacyAccepted(context: common.UIAbilityContext): Promise<boolean> {
  const options: preferences.Options = { name: PREFERENCES_NAME };
  const store: preferences.Preferences = await preferences.getPreferences(context, options);
  const value: preferences.ValueType = await store.get(KEY_ACCEPTED, false);
  return value === true;
}

/** Persists the consent flag; resolves only after it is on disk. */
export async function setPrivacyAccepted(context: common.UIAbilityContext): Promise<void> {
  const options: preferences.Options = { name: PREFERENCES_NAME };
  const store: preferences.Preferences = await preferences.getPreferences(context, options);
  await store.put(KEY_ACCEPTED, true);
  await store.flush();
}

/** Reads and UTF-8-decodes the privacy policy shipped in the HAP rawfile. */
export async function loadPrivacyPolicy(context: common.UIAbilityContext): Promise<string> {
  const bytes: Uint8Array = await context.resourceManager.getRawFileContent(POLICY_RAWFILE);
  return util.TextDecoder.create('utf-8').decodeToString(bytes);
}

/** versionName from this HAP (empty string when it cannot be read). */
export function readAppVersion(): string {
  try {
    const info: bundleManager.BundleInfo = bundleManager.getBundleInfoForSelfSync(
      bundleManager.BundleFlag.GET_BUNDLE_INFO_DEFAULT);
    return info.versionName;
  } catch (error) {
    hilog.warn(DOMAIN, TAG, '读取应用版本号失败：%{public}s', String(error));
    return '';
  }
}
