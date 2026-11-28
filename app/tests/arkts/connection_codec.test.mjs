// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host-side unit test for the pure ArkTS logic that needs no device:
//   - app/entry/src/main/ets/model/ConnectionCodec.ets   (connections.json codec)
//   - app/entry/src/main/ets/model/SunshineResolutions.ets (Sunshine mode picker)
//
// Runs under plain Node (no device, no network, no oh_modules):
//   1. copies the .ets sources byte for byte into a temp dir as .ts,
//   2. compiles them with the TypeScript bundled in the HarmonyOS SDK,
//   3. imports the resulting .mjs and asserts the codec and mode-selection
//      behaviour.
//
// The source files are never modified; ArkTS-only syntax is not used there
// exactly so this compile step stays trivial. Run directly with:
//   node app/tests/arkts/connection_codec.test.mjs

import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = path.resolve(HERE, '../../..');
const SOURCE = path.join(REPO_ROOT, 'app/entry/src/main/ets/model/ConnectionCodec.ets');
const RESOLUTION_SOURCE =
  path.join(REPO_ROOT, 'app/entry/src/main/ets/model/SunshineResolutions.ets');
const PICTURE_SOURCE =
  path.join(REPO_ROOT, 'app/entry/src/main/ets/model/PictureGeometry.ets');
const SDK_ROOT = process.env.TIANGUAN_SDK_ROOT ||
  '/home/worker/harmonyos/command-line-tools/26.0.0.821';
const TSC = process.env.TIANGUAN_TSC ||
  path.join(SDK_ROOT, 'sdk/default/openharmony/ets/build-tools/ets-loader/node_modules/typescript/bin/tsc');

if (!fs.existsSync(TSC)) {
  console.error(`FAIL: TypeScript compiler not found: ${TSC}`);
  console.error('Set TIANGUAN_TSC=/path/to/tsc or TIANGUAN_SDK_ROOT=/path/to/sdk.');
  process.exit(1);
}
if (!fs.existsSync(SOURCE)) {
  console.error(`FAIL: source not found: ${SOURCE}`);
  process.exit(1);
}
if (!fs.existsSync(RESOLUTION_SOURCE)) {
  console.error(`FAIL: source not found: ${RESOLUTION_SOURCE}`);
  process.exit(1);
}
if (!fs.existsSync(PICTURE_SOURCE)) {
  console.error(`FAIL: source not found: ${PICTURE_SOURCE}`);
  process.exit(1);
}

const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), 'tianguan-codec-test-'));
let codec;
let resolutions;
let picture;

try {
  // Stage the real production sources as .ts (tsc does not accept .ets) without
  // editing a single byte of them.
  const staged = path.join(tempDir, 'ConnectionCodec.ts');
  fs.copyFileSync(SOURCE, staged);
  const stagedResolutions = path.join(tempDir, 'SunshineResolutions.ts');
  fs.copyFileSync(RESOLUTION_SOURCE, stagedResolutions);
  const stagedPicture = path.join(tempDir, 'PictureGeometry.ts');
  fs.copyFileSync(PICTURE_SOURCE, stagedPicture);
  const outDir = path.join(tempDir, 'out');
  fs.mkdirSync(outDir);

  execFileSync(process.execPath, [
    TSC,
    '--target', 'es2020',
    '--module', 'es2020',
    '--skipLibCheck',
    '--outDir', outDir,
    staged,
    stagedResolutions,
    stagedPicture
  ], { stdio: 'inherit' });

  // Give the emitted files an .mjs extension so Node loads them as ES modules
  // regardless of any package.json above the temp directory.
  const modulePath = path.join(outDir, 'ConnectionCodec.mjs');
  fs.renameSync(path.join(outDir, 'ConnectionCodec.js'), modulePath);
  const resolutionModulePath = path.join(outDir, 'SunshineResolutions.mjs');
  fs.renameSync(path.join(outDir, 'SunshineResolutions.js'), resolutionModulePath);
  const pictureModulePath = path.join(outDir, 'PictureGeometry.mjs');
  fs.renameSync(path.join(outDir, 'PictureGeometry.js'), pictureModulePath);
  codec = await import(pathToFileURL(modulePath).href);
  resolutions = await import(pathToFileURL(resolutionModulePath).href);
  picture = await import(pathToFileURL(pictureModulePath).href);
} catch (error) {
  console.error(`FAIL: could not compile/load the pure ArkTS sources: ${error && error.message}`);
  fs.rmSync(tempDir, { recursive: true, force: true });
  process.exit(1);
}

const {
  DEFAULT_PORT, DEFAULT_RDP_PORT, DEFAULT_SUNSHINE_PORT, UNGROUPED,
  defaultPortFor, newConnectionId, connectionIdOf,
  parseConnectionRecords, serializeConnectionRecords,
  pinKeyFor, passwordAliasFor,
  SUNSHINE_PRESETS, SUNSHINE_PRESET_BALANCED, SUNSHINE_PRESET_CUSTOM,
  SUNSHINE_PRESET_SMOOTH, SUNSHINE_PRESET_HD, SUNSHINE_PRESET_UHD,
  SUNSHINE_CODEC_AUTO, SUNSHINE_CODEC_H264, SUNSHINE_CODEC_HEVC,
  sunshinePresetFor, sunshineFormatsFor, normalizeSunshineQuality,
  isSunshinePreset, isSunshineCodec, isValidSunshineSize,
  isValidSunshineFps, isValidSunshineBitrateMbps
} = codec;

let passed = 0;
let failed = 0;

function test(name, fn) {
  try {
    fn();
    passed += 1;
    console.log(`ok ${passed + failed} - ${name}`);
  } catch (error) {
    failed += 1;
    console.error(`NOT OK ${passed + failed} - ${name}`);
    console.error(String(error && error.stack ? error.stack : error).split('\n')
      .map((line) => `    ${line}`).join('\n'));
  }
}

/**
 * A fully populated entry; override only the fields a test cares about. The
 * Sunshine quality fields carry the 均衡 defaults because every loaded record
 * has them (old files without them are filled in by the parser).
 */
function entry(overrides) {
  return Object.assign({
    id: 'cbase', name: 'base', host: 'example.test', port: '5901',
    group: '', savePassword: false, noPassword: false, lastConnectedAt: 0,
    protocol: 'vnc', username: '', domain: '', sunshineApp: '',
    sunshinePreset: 'balanced', sunshineWidth: 1920, sunshineHeight: 1080,
    sunshineFps: 60, sunshineBitrate: 20, sunshineCodec: 'auto',
    sunshineLocalCursor: false, sunshineHostCursorHidden: false,
    sunshineFollowWindow: true, sunshineVsync: true
  }, overrides || {});
}

// ---------------------------------------------------------------------------
// Parsing and migration of connections.json
// ---------------------------------------------------------------------------

test('legacy record (no id, no protocol) migrates to id=host:port, protocol=vnc', () => {
  const records = parseConnectionRecords(JSON.stringify([
    { name: '旧连接', host: '192.168.1.5', port: '5901', savePassword: true }
  ]));
  assert.equal(records.length, 1);
  const record = records[0];
  assert.equal(record.id, '192.168.1.5:5901');
  assert.equal(record.protocol, 'vnc');
  assert.equal(record.name, '旧连接');
  assert.equal(record.host, '192.168.1.5');
  assert.equal(record.port, '5901');
  assert.equal(record.group, '');
  assert.equal(record.lastConnectedAt, 0);
  assert.equal(record.username, '');
  assert.equal(record.domain, '');
  assert.equal(record.savePassword, true);
  assert.equal(record.noPassword, false);
});

test('legacy record with missing or empty port defaults to 5901 in both port and id', () => {
  const missing = parseConnectionRecords(JSON.stringify([{ host: '10.0.0.1' }]));
  assert.equal(missing[0].port, '5901');
  assert.equal(missing[0].id, '10.0.0.1:5901');
  const empty = parseConnectionRecords(JSON.stringify([{ host: '10.0.0.2', port: '' }]));
  assert.equal(empty[0].port, '5901');
  assert.equal(empty[0].id, '10.0.0.2:5901');
});

test('an existing id is never rewritten (opaque ids and legacy host:port ids)', () => {
  const records = parseConnectionRecords(JSON.stringify([
    { id: 'cmyopaqueid', host: '192.168.1.5', port: '5901', name: 'renamed' },
    { id: '192.168.1.6:5901', host: '192.168.1.7', port: '5902' }
  ]));
  assert.equal(records[0].id, 'cmyopaqueid');
  assert.equal(records[1].id, '192.168.1.6:5901');
  // Even though host:port changed, the stored id stays verbatim.
  assert.equal(records[1].host, '192.168.1.7');
  assert.equal(records[1].port, '5902');
});

test('new-format fields (group/lastConnectedAt/protocol/username/domain) survive parsing', () => {
  const records = parseConnectionRecords(JSON.stringify([{
    id: 'crdp1', name: 'RDP box', host: '10.0.0.9', port: '3389', group: '工作',
    savePassword: true, noPassword: true, lastConnectedAt: 1729000000000,
    protocol: 'rdp', username: 'Administrator', domain: 'CORP'
  }]));
  assert.deepEqual(records[0], entry({
    id: 'crdp1', name: 'RDP box', host: '10.0.0.9', port: '3389', group: '工作',
    savePassword: true, noPassword: true, lastConnectedAt: 1729000000000,
    protocol: 'rdp', username: 'Administrator', domain: 'CORP'
  }));
});

test('missing or unknown protocol normalises to vnc; only exact "rdp" is rdp', () => {
  const parsed = parseConnectionRecords(JSON.stringify([
    { id: 'c1', host: 'a' },
    { id: 'c2', host: 'b', protocol: 'ssh' },
    { id: 'c3', host: 'c', protocol: 'RDP' },
    { id: 'c4', host: 'd', protocol: 'rdp' },
    { id: 'c5', host: 'e', protocol: '' }
  ]));
  assert.deepEqual(parsed.map((record) => record.protocol), ['vnc', 'vnc', 'vnc', 'rdp', 'vnc']);
});

test('Sunshine normalisation: only exact "sunshine" selects it, case variants are vnc', () => {
  const parsed = parseConnectionRecords(JSON.stringify([
    { id: 'c1', host: 'a', protocol: 'sunshine' },
    { id: 'c2', host: 'b', protocol: 'Sunshine' },
    { id: 'c3', host: 'c', protocol: 'SUNSHINE' },
    { id: 'c4', host: 'd', protocol: 'moonlight' },
    { id: 'c5', host: 'e', protocol: 'rdp' }
  ]));
  assert.deepEqual(parsed.map((record) => record.protocol),
    ['sunshine', 'vnc', 'vnc', 'vnc', 'rdp']);
});

test('sunshineApp is a Sunshine-only string field: defaults to "", rejects non-strings', () => {
  const parsed = parseConnectionRecords(JSON.stringify([
    { id: 'c1', host: 'a', protocol: 'sunshine' },
    { id: 'c2', host: 'b', protocol: 'sunshine', sunshineApp: 'Steam Big Picture' },
    { id: 'c3', host: 'c', protocol: 'sunshine', sunshineApp: 42 },
    { id: 'c4', host: 'd', protocol: 'vnc', sunshineApp: 'Desktop' }
  ]));
  assert.equal(parsed[0].sunshineApp, '');
  assert.equal(parsed[1].sunshineApp, 'Steam Big Picture');
  assert.equal(parsed[2].sunshineApp, '');
  // The field survives parsing for any protocol (it is only read/sent for
  // Sunshine); a legacy record simply has the default.
  assert.equal(parsed[3].sunshineApp, 'Desktop');
  assert.equal(parsed[0].protocol, 'sunshine');
  assert.equal(parsed[0].username, '');
  assert.equal(parsed[0].domain, '');
});

test('Sunshine ConnectionEntry round-trips through serialize -> parse', () => {
  const expected = [entry({
    id: 'csun1', name: 'Sunshine', host: '192.168.50.62', port: '47989',
    group: '游戏', savePassword: false, noPassword: false,
    lastConnectedAt: 1729000000002, protocol: 'sunshine',
    sunshineApp: 'Desktop'
  })];
  const text = serializeConnectionRecords(expected);
  assert.deepEqual(parseConnectionRecords(text), expected);
  // Stable on a second round trip and never carries a password.
  assert.deepEqual(parseConnectionRecords(serializeConnectionRecords(parseConnectionRecords(text))),
    expected);
  assert.equal(Object.prototype.hasOwnProperty.call(JSON.parse(text)[0], 'password'), false);
  assert.equal(JSON.parse(text)[0].protocol, 'sunshine');
  assert.equal(JSON.parse(text)[0].sunshineApp, 'Desktop');
});

// ---------------------------------------------------------------------------
// Sunshine quality: the single tier table, option mapping and persistence
// ---------------------------------------------------------------------------

test('Sunshine quality tiers match the agreed numbers (流畅/均衡/高清/超清)', () => {
  assert.deepEqual(SUNSHINE_PRESETS.map((tier) => [tier.id, tier.label]),
    [['smooth', '流畅'], ['balanced', '均衡'], ['hd', '高清'], ['uhd', '超清']]);
  assert.deepEqual(SUNSHINE_PRESETS.map((tier) => [tier.width, tier.height, tier.fps,
    tier.bitrateMbps]),
  [[1280, 720, 60, 15], [1920, 1080, 60, 20], [2560, 1440, 60, 40], [3840, 2160, 60, 80]]);
});

test('sunshinePresetFor: known id wins, missing/unknown falls back to 均衡', () => {
  assert.equal(sunshinePresetFor('uhd').width, 3840);
  assert.equal(sunshinePresetFor(SUNSHINE_PRESET_SMOOTH).bitrateMbps, 15);
  assert.equal(sunshinePresetFor(undefined).id, SUNSHINE_PRESET_BALANCED);
  assert.equal(sunshinePresetFor('').id, SUNSHINE_PRESET_BALANCED);
  assert.equal(sunshinePresetFor('4k').id, SUNSHINE_PRESET_BALANCED);
  assert.equal(sunshinePresetFor('custom').id, SUNSHINE_PRESET_BALANCED);
  assert.equal(isSunshinePreset(SUNSHINE_PRESET_CUSTOM), true);
  assert.equal(isSunshinePreset('4k'), false);
  assert.equal(isSunshinePreset(undefined), false);
});

test('sunshineFormatsFor: 自动 omits, H.264 sends h264, HEVC sends h264,hevc', () => {
  assert.equal(sunshineFormatsFor(SUNSHINE_CODEC_AUTO), '');
  assert.equal(sunshineFormatsFor(undefined), '');
  assert.equal(sunshineFormatsFor('unknown'), '');
  assert.equal(sunshineFormatsFor(SUNSHINE_CODEC_H264), 'h264');
  assert.equal(sunshineFormatsFor(SUNSHINE_CODEC_HEVC), 'h264,hevc');
  assert.equal(isSunshineCodec(SUNSHINE_CODEC_HEVC), true);
  assert.equal(isSunshineCodec('vp9'), false);
});

test('normalizeSunshineQuality: tier numbers for presets, stored values for custom', () => {
  const preset = normalizeSunshineQuality('hd', undefined, undefined, undefined, undefined,
    undefined);
  assert.deepEqual(preset, {
    preset: 'hd', width: 2560, height: 1440, fps: 60, bitrateMbps: 40, codec: 'auto'
  });
  const custom = normalizeSunshineQuality('custom', 1600, 900, 30, 12, 'hevc');
  assert.deepEqual(custom, {
    preset: 'custom', width: 1600, height: 900, fps: 30, bitrateMbps: 12, codec: 'hevc'
  });
  // Invalid/out-of-range values fall back to the record's own tier (or 均衡),
  // never to something createSession would reject.
  const broken = normalizeSunshineQuality('4k', 0, 99999, -1, 500, 'vp9');
  assert.deepEqual(broken, {
    preset: 'balanced', width: 1920, height: 1080, fps: 60, bitrateMbps: 20, codec: 'auto'
  });
});

test('native quality limits are exposed as validators (form rejects, never clamps)', () => {
  assert.equal(isValidSunshineSize(1), true);
  assert.equal(isValidSunshineSize(4096), true);
  assert.equal(isValidSunshineSize(0), false);
  assert.equal(isValidSunshineSize(4097), false);
  assert.equal(isValidSunshineSize(1920.5), false);
  assert.equal(isValidSunshineFps(60), true);
  assert.equal(isValidSunshineFps(481), false);
  assert.equal(isValidSunshineBitrateMbps(1), true);
  assert.equal(isValidSunshineBitrateMbps(200), true);
  assert.equal(isValidSunshineBitrateMbps(201), false);
});

test('custom quality fields round-trip; presets keep their stored numbers', () => {
  const expected = [entry({
    id: 'csun2', name: 'Sunshine', host: '192.168.50.62', port: '47989',
    protocol: 'sunshine', sunshineApp: 'Desktop',
    sunshinePreset: 'custom', sunshineWidth: 1600, sunshineHeight: 900,
    sunshineFps: 30, sunshineBitrate: 12, sunshineCodec: 'hevc'
  })];
  const text = serializeConnectionRecords(expected);
  assert.deepEqual(parseConnectionRecords(text), expected);
  const uhd = [entry({
    id: 'csun3', host: '10.0.0.3', port: '47989', protocol: 'sunshine',
    sunshinePreset: 'uhd', sunshineWidth: 3840, sunshineHeight: 2160,
    sunshineFps: 60, sunshineBitrate: 80, sunshineCodec: 'h264'
  })];
  assert.deepEqual(parseConnectionRecords(serializeConnectionRecords(uhd)), uhd);
});

test('legacy record without quality fields loads as 均衡 and keeps its id', () => {
  const parsed = parseConnectionRecords(JSON.stringify([
    { id: 'cold', host: '10.0.0.4', port: '47989', protocol: 'sunshine',
      sunshineApp: 'Desktop' }
  ]));
  assert.equal(parsed.length, 1);
  const record = parsed[0];
  assert.equal(record.id, 'cold');
  assert.equal(record.sunshinePreset, SUNSHINE_PRESET_BALANCED);
  assert.equal(record.sunshineWidth, 1920);
  assert.equal(record.sunshineHeight, 1080);
  assert.equal(record.sunshineFps, 60);
  assert.equal(record.sunshineBitrate, 20);
  assert.equal(record.sunshineCodec, SUNSHINE_CODEC_AUTO);
  // Local cursor defaults OFF for every record (the remote cursor stays
  // visible); the recorded host state is "shown" until a session reports
  // otherwise.
  assert.equal(record.sunshineLocalCursor, false);
  assert.equal(record.sunshineHostCursorHidden, false);
});

test('local cursor is enabled only by an explicit true; host state only literal true', () => {
  const parsed = parseConnectionRecords(JSON.stringify([
    { id: 'c1', host: 'a', protocol: 'sunshine', sunshineLocalCursor: 'true' },
    { id: 'c2', host: 'b', protocol: 'sunshine', sunshineLocalCursor: 1 },
    { id: 'c3', host: 'c', protocol: 'sunshine', sunshineLocalCursor: true },
    { id: 'c4', host: 'd', protocol: 'sunshine', sunshineLocalCursor: false },
    { id: 'c5', host: 'e', protocol: 'sunshine', sunshineHostCursorHidden: 'true' },
    { id: 'c6', host: 'f', protocol: 'sunshine', sunshineHostCursorHidden: 1 },
    { id: 'c7', host: 'g', protocol: 'sunshine', sunshineHostCursorHidden: true }
  ]));
  assert.deepEqual(parsed.map((record) => record.sunshineLocalCursor),
    [false, false, true, false, false, false, false]);
  assert.deepEqual(parsed.map((record) => record.sunshineHostCursorHidden),
    [false, false, false, false, false, false, true]);
});

test('local cursor preference and recorded host state round-trip', () => {
  const expected = [entry({
    id: 'csun-cursor', host: '10.0.0.5', port: '47989', protocol: 'sunshine',
    sunshineLocalCursor: true, sunshineHostCursorHidden: true
  })];
  const text = serializeConnectionRecords(expected);
  assert.deepEqual(parseConnectionRecords(text), expected);
  // A second round trip is stable, and both fields are really on disk.
  assert.deepEqual(parseConnectionRecords(serializeConnectionRecords(parseConnectionRecords(text))),
    expected);
  const raw = JSON.parse(text)[0];
  assert.equal(raw.sunshineLocalCursor, true);
  assert.equal(raw.sunshineHostCursorHidden, true);
});

test('follow-window defaults ON for old records; only a literal false disables it', () => {
  const parsed = parseConnectionRecords(JSON.stringify([
    { id: 'c1', host: 'a', protocol: 'sunshine' },
    { id: 'c2', host: 'b', protocol: 'sunshine', sunshineFollowWindow: 'false' },
    { id: 'c3', host: 'c', protocol: 'sunshine', sunshineFollowWindow: false },
    { id: 'c4', host: 'd', protocol: 'sunshine', sunshineFollowWindow: true }
  ]));
  assert.deepEqual(parsed.map((record) => record.sunshineFollowWindow),
    [true, true, false, true]);
  // Round trip keeps the explicit false and the default true.
  const expected = [
    entry({ id: 'csun-follow-off', host: '10.0.0.6', port: '47989',
      protocol: 'sunshine', sunshineFollowWindow: false }),
    entry({ id: 'csun-follow-on', host: '10.0.0.7', port: '47989',
      protocol: 'sunshine' })
  ];
  const text = serializeConnectionRecords(expected);
  assert.deepEqual(parseConnectionRecords(text), expected);
  assert.deepEqual(parseConnectionRecords(serializeConnectionRecords(parseConnectionRecords(text))),
    expected);
});

test('vsync defaults ON for old records; only a literal false disables it', () => {
  const parsed = parseConnectionRecords(JSON.stringify([
    { id: 'c1', host: 'a', protocol: 'sunshine' },
    { id: 'c2', host: 'b', protocol: 'sunshine', sunshineVsync: 'false' },
    { id: 'c3', host: 'c', protocol: 'sunshine', sunshineVsync: false },
    { id: 'c4', host: 'd', protocol: 'sunshine', sunshineVsync: true }
  ]));
  assert.deepEqual(parsed.map((record) => record.sunshineVsync),
    [true, true, false, true]);
  const expected = [entry({
    id: 'csun-vsync-off', host: '10.0.0.8', port: '47989',
    protocol: 'sunshine', sunshineVsync: false
  })];
  const text = serializeConnectionRecords(expected);
  assert.deepEqual(parseConnectionRecords(text), expected);
  assert.deepEqual(parseConnectionRecords(serializeConnectionRecords(parseConnectionRecords(text))),
    expected);
  assert.equal(JSON.parse(text)[0].sunshineVsync, false);
});

test('username/domain default to empty string and reject non-strings', () => {
  const parsed = parseConnectionRecords(JSON.stringify([
    { id: 'c1', host: 'a', protocol: 'rdp' },
    { id: 'c2', host: 'b', protocol: 'rdp', username: 42, domain: null },
    { id: 'c3', host: 'c', protocol: 'rdp', username: '', domain: '' }
  ]));
  assert.equal(parsed[0].username, '');
  assert.equal(parsed[0].domain, '');
  assert.equal(parsed[1].username, '');
  assert.equal(parsed[1].domain, '');
  assert.equal(parsed[2].username, '');
});

test('group/lastConnectedAt are new-format only (legacy records drop them)', () => {
  const parsed = parseConnectionRecords(JSON.stringify([
    { host: 'legacy', port: '5901', group: 'old-group', lastConnectedAt: 12345 },
    { id: 'cnew', host: 'new', port: '5901', group: 7, lastConnectedAt: -1 },
    { id: 'cnew2', host: 'new2', port: '5901', group: 'kept', lastConnectedAt: 0 }
  ]));
  assert.equal(parsed[0].group, '');
  assert.equal(parsed[0].lastConnectedAt, 0);
  assert.equal(parsed[1].group, '');
  assert.equal(parsed[1].lastConnectedAt, 0);
  assert.equal(parsed[2].group, 'kept');
  assert.equal(parsed[2].lastConnectedAt, 0);
});

test('missing name falls back to the host', () => {
  const parsed = parseConnectionRecords(JSON.stringify([
    { id: 'c1', host: 'name-fallback', port: '5901' },
    { id: 'c2', host: 'kept-host', port: '5901', name: '' }
  ]));
  assert.equal(parsed[0].name, 'name-fallback');
  assert.equal(parsed[1].name, 'kept-host');
});

test('records without a usable host are skipped; invalid/empty input yields []', () => {
  const parsed = parseConnectionRecords(JSON.stringify([
    { host: '', port: '5901' },
    { name: 'no host' },
    { host: 42, port: '5901' },
    { host: 'valid.test', port: '5901' }
  ]));
  assert.equal(parsed.length, 1);
  assert.equal(parsed[0].host, 'valid.test');

  assert.deepEqual(parseConnectionRecords(''), []);
  assert.deepEqual(parseConnectionRecords('   \n\t '), []);
  assert.deepEqual(parseConnectionRecords('{"host":"single-object"}'), []);
});

test('non-object entries (null/number/string) are skipped, not fatal (whole list survives)', () => {
  const parsed = parseConnectionRecords(JSON.stringify([
    null,
    42,
    'not-a-record',
    true,
    [],
    {},
    { name: 'no host' },
    { id: 'cok', name: 'kept', host: 'ok.test', port: '5901' }
  ]));
  assert.equal(parsed.length, 1);
  assert.equal(parsed[0].id, 'cok');
  assert.equal(parsed[0].name, 'kept');
  assert.equal(parsed[0].host, 'ok.test');

  // All-bad input returns [] instead of throwing: a single bad entry used to
  // make parseConnectionRecords throw and loadConnections wipe the whole list.
  assert.deepEqual(parseConnectionRecords(JSON.stringify([null, 1, 'x', false, {}, []])), []);
});

test('savePassword/noPassword are booleans, only literal true is true', () => {
  const parsed = parseConnectionRecords(JSON.stringify([
    { id: 'c1', host: 'a', savePassword: 'yes', noPassword: 1 },
    { id: 'c2', host: 'b', savePassword: true, noPassword: false }
  ]));
  assert.equal(parsed[0].savePassword, false);
  assert.equal(parsed[0].noPassword, false);
  assert.equal(parsed[1].savePassword, true);
  assert.equal(parsed[1].noPassword, false);
});

// ---------------------------------------------------------------------------
// Serialization (the exact connections.json write path)
// ---------------------------------------------------------------------------

test('serialize -> parse round-trips every persisted field (F8 regression)', () => {
  const expected = [
    entry({ id: 'c1', name: 'VNC', host: '10.0.0.1', port: '5901', group: '组A',
      savePassword: true, lastConnectedAt: 1729000000000, protocol: 'vnc' }),
    entry({ id: 'c2', name: 'RDP', host: '10.0.0.2', port: '3389', group: '组B',
      noPassword: true, lastConnectedAt: 1729000000001, protocol: 'rdp',
      username: 'user', domain: 'DOMAIN' })
  ];
  const text = serializeConnectionRecords(expected);
  assert.ok(text.startsWith('['));
  assert.deepEqual(parseConnectionRecords(text), expected);
  // A second round trip is stable (no field is dropped on save).
  assert.deepEqual(parseConnectionRecords(serializeConnectionRecords(parseConnectionRecords(text))), expected);
});

test('serialized connection data never contains a password field', () => {
  const text = serializeConnectionRecords([entry({ id: 'c1', savePassword: true })]);
  const raw = JSON.parse(text)[0];
  assert.equal(Object.prototype.hasOwnProperty.call(raw, 'password'), false);
  assert.equal(raw.savePassword, true);
});

// ---------------------------------------------------------------------------
// Protocol key namespaces: fingerprint pin and password alias
// ---------------------------------------------------------------------------

test('pinKeyFor: VNC keeps host:port, RDP is namespaced rdp:host:port', () => {
  assert.equal(pinKeyFor(entry({ protocol: 'vnc', host: '192.168.50.62', port: '5901' })),
    '192.168.50.62:5901');
  assert.equal(pinKeyFor(entry({ protocol: 'rdp', host: '192.168.50.62', port: '5901' })),
    'rdp:192.168.50.62:5901');
});

test('VNC and RDP on the same host:port get different pin keys (no trust clash)', () => {
  const vnc = entry({ protocol: 'vnc', host: 'host.test', port: '5901' });
  const rdp = entry({ protocol: 'rdp', host: 'host.test', port: '5901' });
  assert.notEqual(pinKeyFor(vnc), pinKeyFor(rdp));
});

test('passwordAliasFor: VNC keeps legacy tianguan:host:port, RDP is tianguan:rdp:host:port', () => {
  const vnc = entry({ protocol: 'vnc', host: '192.168.50.62', port: '5901' });
  const rdp = entry({ protocol: 'rdp', host: '192.168.50.62', port: '5901' });
  assert.equal(passwordAliasFor(vnc), 'tianguan:192.168.50.62:5901');
  assert.equal(passwordAliasFor(rdp), 'tianguan:rdp:192.168.50.62:5901');
  assert.notEqual(passwordAliasFor(vnc), passwordAliasFor(rdp));
});

test('password alias and pin key are namespaced independently per protocol', () => {
  const rdp = entry({ protocol: 'rdp', host: 'h', port: '3389' });
  assert.equal(pinKeyFor(rdp), 'rdp:h:3389');
  assert.equal(passwordAliasFor(rdp), 'tianguan:rdp:h:3389');
  const vnc = entry({ protocol: 'vnc', host: 'h', port: '5901' });
  assert.equal(pinKeyFor(vnc), 'h:5901');
  assert.equal(passwordAliasFor(vnc), 'tianguan:h:5901');
});

test('Sunshine pins/passwords never share a VNC/RDP key on the same host:port', () => {
  const host = '192.168.50.62';
  const port = '47989';
  const sunshine = entry({ protocol: 'sunshine', host: host, port: port });
  // Sunshine is pinned natively inside sunshineStateDir: its keys are a guard
  // namespace, never a VNC/RDP trust anchor or password alias.
  assert.equal(pinKeyFor(sunshine), 'sunshine:' + host + ':' + port);
  assert.equal(passwordAliasFor(sunshine), 'tianguan:sunshine:' + host + ':' + port);
  const vnc = entry({ protocol: 'vnc', host: host, port: port });
  const rdp = entry({ protocol: 'rdp', host: host, port: port });
  assert.notEqual(pinKeyFor(sunshine), pinKeyFor(vnc));
  assert.notEqual(pinKeyFor(sunshine), pinKeyFor(rdp));
  assert.notEqual(passwordAliasFor(sunshine), passwordAliasFor(vnc));
  assert.notEqual(passwordAliasFor(sunshine), passwordAliasFor(rdp));
});

// ---------------------------------------------------------------------------
// Constants and id helpers
// ---------------------------------------------------------------------------

test('defaultPortFor: RDP 3389, Sunshine 47989, VNC/unknown 5901', () => {
  assert.equal(defaultPortFor('rdp'), '3389');
  assert.equal(defaultPortFor('sunshine'), '47989');
  assert.equal(defaultPortFor('vnc'), '5901');
  assert.equal(defaultPortFor(''), '5901');
  assert.equal(defaultPortFor('ssh'), '5901');
  assert.equal(DEFAULT_PORT, '5901');
  assert.equal(DEFAULT_RDP_PORT, '3389');
  assert.equal(DEFAULT_SUNSHINE_PORT, '47989');
  assert.equal(UNGROUPED, '未分组');
});

test('connectionIdOf builds the legacy host:port identity', () => {
  assert.equal(connectionIdOf('10.0.0.1', '5901'), '10.0.0.1:5901');
  assert.equal(connectionIdOf('h', ''), 'h:');
});

test('newConnectionId returns opaque unique ids, not host:port', () => {
  const ids = new Set();
  for (let i = 0; i < 100; i++) {
    const id = newConnectionId();
    assert.ok(id.length > 0);
    assert.equal(id.charAt(0), 'c');
    assert.equal(id.includes(':'), false);
    ids.add(id);
  }
  assert.equal(ids.size, 100);
});

// ---------------------------------------------------------------------------
// Sunshine virtual-display mode selection (model/SunshineResolutions.ets)
// ---------------------------------------------------------------------------

const {
  SUNSHINE_MODE_GROUPS, SUNSHINE_MODE_PIXEL_CAP, selectSunshineMode
} = resolutions;

/** '1920x1080' for readable assertions. */
function modeText(mode) {
  return `${mode.width}x${mode.height}`;
}

test('Sunshine mode table covers every host mode (74 entries, 14 ratio families)', () => {
  assert.equal(SUNSHINE_MODE_GROUPS.length, 14);
  let count = 0;
  for (const group of SUNSHINE_MODE_GROUPS) {
    count += group.modes.length;
  }
  assert.equal(count, 74);
  // The pixel budget is 3840x2160.
  assert.equal(SUNSHINE_MODE_PIXEL_CAP, 3840 * 2160);
});

test('PC window 1620x1024 snaps to the 16:10 mode closest in height', () => {
  assert.equal(modeText(selectSunshineMode(1620, 1024)), '1680x1050');
});

test('2-in-1 fullscreen 3000x2000 maps to the 3:2 mode of the same size', () => {
  assert.equal(modeText(selectSunshineMode(3000, 2000)), '3000x2000');
});

test('1920x1080 stays on the 16:9 mode of the same size', () => {
  assert.equal(modeText(selectSunshineMode(1920, 1080)), '1920x1080');
});

test('ultrawide window 3840x1640 snaps to the 21:9 mode 3440x1440', () => {
  assert.equal(modeText(selectSunshineMode(3840, 1640)), '3440x1440');
});

test('portrait 1600x2560 keeps the portrait 10:16 mode 1600x2560', () => {
  assert.equal(modeText(selectSunshineMode(1600, 2560)), '1600x2560');
});

test('phone fullscreen 1316x2832 snaps to the closest portrait phone mode', () => {
  // 1260x2720 leaves only 0.3% black bars and is scaled 1.04x; 1080x2340 would
  // leave 0.7% bars but be blown up 1.21x, so the score picks 1260x2720.
  assert.equal(modeText(selectSunshineMode(1316, 2832)), '1260x2720');
  // Same orientation is enforced: a phone window never gets a landscape mode.
  const portrait = selectSunshineMode(1316, 2832);
  assert.ok(portrait.height > portrait.width);
});

test('over-budget modes are excluded before scoring (cap 3840x2160 px)', () => {
  // 16:9 window above the budget: 5120x2880 is excluded up front, so the best
  // remaining candidate is the 3840x2160 16:9 mode (zero bars, scale 1.56).
  assert.equal(modeText(selectSunshineMode(6000, 3375)), '3840x2160');
  assert.ok(3840 * 2160 <= SUNSHINE_MODE_PIXEL_CAP);
  // A huge 4:3 window: the 4:3 4096x3072 is over budget, and among the modes
  // that remain the 2732x2048 tablet mode fits 4:3 almost exactly with the
  // lowest scale penalty, so it beats 2048x1536 (which would be a 2.4x blow-up).
  assert.equal(modeText(selectSunshineMode(5000, 3750)), '2732x2048');
  // Whatever the window, the pick never exceeds the pixel budget.
  for (const [w, h] of [[8000, 4500], [7000, 7000], [9000, 4000]]) {
    const mode = selectSunshineMode(w, h);
    assert.ok(mode.width * mode.height <= SUNSHINE_MODE_PIXEL_CAP,
      `${modeText(mode)} is over the pixel cap`);
  }
});

test('invalid window sizes fall back to 1920x1080', () => {
  assert.equal(modeText(selectSunshineMode(0, 0)), '1920x1080');
  assert.equal(modeText(selectSunshineMode(-10, 100)), '1920x1080');
  assert.equal(modeText(selectSunshineMode(Number.NaN, 1080)), '1920x1080');
});

// ---------------------------------------------------------------------------
// Presented-picture geometry (model/PictureGeometry.ets): the system pointer is
// hidden exactly when the point is on the picture, so the black bars keep it.
// ---------------------------------------------------------------------------

const { containGeometry, pointInPicture, remotePointInPicture, hidesSystemPointer } = picture;

test('contain geometry: 1920x1080 frame in a 2000x1000 area is pillarboxed', () => {
  const geometry = containGeometry(1920, 1080, 2000, 1000);
  assert.ok(geometry !== undefined);
  assert.equal(geometry.height, 1000);
  assert.ok(Math.abs(geometry.width - 1777.7778) < 0.001);
  assert.ok(Math.abs(geometry.left - 111.1111) < 0.001);
  assert.equal(geometry.top, 0);
});

test('pointer on a black bar does NOT hide the cursor; on the picture it does', () => {
  const geometry = containGeometry(1920, 1080, 2000, 1000);
  // Left / right pillarbox: the system pointer must stay visible.
  assert.equal(pointInPicture(0, 500, geometry), false);
  assert.equal(pointInPicture(111, 500, geometry), false);
  assert.equal(pointInPicture(1999, 500, geometry), false);
  // Just inside the picture: the remote cursor takes over.
  assert.equal(pointInPicture(112, 500, geometry), true);
  assert.equal(pointInPicture(1000, 500, geometry), true);
  // 16:9 frame in a 3:2 window is letterboxed instead (top / bottom bars).
  const letterbox = containGeometry(1920, 1080, 3000, 2000);
  assert.equal(pointInPicture(1500, 10, letterbox), false);
  assert.equal(pointInPicture(1500, 1000, letterbox), true);
  assert.equal(pointInPicture(1500, 1999, letterbox), false);
});

test('remotePointInPicture maps only inside the picture (sendPointer geometry)', () => {
  const geometry = containGeometry(1920, 1080, 2000, 1000);
  assert.deepEqual(remotePointInPicture(1000, 500, geometry), [960, 540]);
  assert.deepEqual(remotePointInPicture(0, 500, geometry), [-1, -1]);
  assert.deepEqual(remotePointInPicture(1999, 500, geometry), [-1, -1]);
});

test('degenerate geometry (not connected / no frame) never reports on-picture', () => {
  assert.equal(containGeometry(0, 1080, 2000, 1000), undefined);
  assert.equal(containGeometry(1920, 1080, 0, 0), undefined);
  assert.equal(containGeometry(Number.NaN, 1080, 2000, 1000), undefined);
  // No picture => the caller keeps the system pointer visible.
  assert.equal(pointInPicture(10, 10, undefined), false);
  const geometry = containGeometry(1920, 1080, 2000, 1000);
  assert.equal(pointInPicture(Number.NaN, 500, geometry), false);
  assert.equal(pointInPicture(1000, Number.NaN, geometry), false);
});

test('local-cursor mode keeps the system pointer visible even on the picture', () => {
  // Normal mode: hidden on the picture, visible on the black bars.
  assert.equal(hidesSystemPointer(true, false), true);
  assert.equal(hidesSystemPointer(false, false), false);
  // Local-cursor mode (Sunshine): the host cursor is hidden, so the system
  // pointer IS the cursor and must stay visible everywhere.
  assert.equal(hidesSystemPointer(true, true), false);
  assert.equal(hidesSystemPointer(false, true), false);
});

// ---------------------------------------------------------------------------

fs.rmSync(tempDir, { recursive: true, force: true });

console.log(`# connection codec + sunshine modes: ${passed} passed, ${failed} failed`);
process.exit(failed === 0 ? 0 : 1);
