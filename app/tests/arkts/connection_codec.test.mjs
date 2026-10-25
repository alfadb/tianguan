// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host-side unit test for the pure connection logic in
// app/entry/src/main/ets/model/ConnectionCodec.ets.
//
// Runs under plain Node (no device, no network, no oh_modules):
//   1. copies the .ets source byte for byte into a temp dir as ConnectionCodec.ts,
//   2. compiles it with the TypeScript bundled in the HarmonyOS SDK,
//   3. imports the resulting .mjs and asserts the parse/migrate/serialize and
//      fingerprint/password key behaviour.
//
// The source file is never modified; ArkTS-only syntax is not used there
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

const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), 'tianguan-codec-test-'));
let codec;

try {
  // Stage the real production source as .ts (tsc does not accept .ets) without
  // editing a single byte of it.
  const staged = path.join(tempDir, 'ConnectionCodec.ts');
  fs.copyFileSync(SOURCE, staged);
  const outDir = path.join(tempDir, 'out');
  fs.mkdirSync(outDir);

  execFileSync(process.execPath, [
    TSC,
    '--target', 'es2020',
    '--module', 'es2020',
    '--skipLibCheck',
    '--outDir', outDir,
    staged
  ], { stdio: 'inherit' });

  // Give the emitted file an .mjs extension so Node loads it as an ES module
  // regardless of any package.json above the temp directory.
  const emitted = path.join(outDir, 'ConnectionCodec.js');
  const modulePath = path.join(outDir, 'ConnectionCodec.mjs');
  fs.renameSync(emitted, modulePath);
  codec = await import(pathToFileURL(modulePath).href);
} catch (error) {
  console.error(`FAIL: could not compile/load ConnectionCodec.ets: ${error && error.message}`);
  fs.rmSync(tempDir, { recursive: true, force: true });
  process.exit(1);
}

const {
  DEFAULT_PORT, DEFAULT_RDP_PORT, UNGROUPED,
  defaultPortFor, newConnectionId, connectionIdOf,
  parseConnectionRecords, serializeConnectionRecords,
  pinKeyFor, passwordAliasFor
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

/** A fully populated entry; override only the fields a test cares about. */
function entry(overrides) {
  return Object.assign({
    id: 'cbase', name: 'base', host: 'example.test', port: '5901',
    group: '', savePassword: false, noPassword: false, lastConnectedAt: 0,
    protocol: 'vnc', username: '', domain: ''
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

// ---------------------------------------------------------------------------
// Constants and id helpers
// ---------------------------------------------------------------------------

test('defaultPortFor: RDP 3389, VNC/unknown 5901; constants match the UI defaults', () => {
  assert.equal(defaultPortFor('rdp'), '3389');
  assert.equal(defaultPortFor('vnc'), '5901');
  assert.equal(defaultPortFor(''), '5901');
  assert.equal(defaultPortFor('ssh'), '5901');
  assert.equal(DEFAULT_PORT, '5901');
  assert.equal(DEFAULT_RDP_PORT, '3389');
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

fs.rmSync(tempDir, { recursive: true, force: true });

console.log(`# connection codec: ${passed} passed, ${failed} failed`);
process.exit(failed === 0 ? 0 : 1);
