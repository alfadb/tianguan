#!/usr/bin/env python3
"""Real isolated TigerVNC RA2_256 integration fixture (standard library only).

Run: python3 ra2-fixture.py --client /absolute/path/to/vnc-core
Probe server/X observer without a client: python3 ra2-fixture.py --probe
Never contacts an existing display. Each case creates and retains a private run
with private secrets (0600), logs, compiled observer and structured result.json.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import selectors
import shutil
import signal
import socket
import stat
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
BASE = ROOT / '.work' / 'ra2-runs'
AUTH = 'RA2_256 authentication complete'
FRAME = 'RA2_256 first frame RGB=112233; key+pointer sent'
CALLBACK = 'RA2_256 credential callback'


def run(argv, env, timeout=8, data=None):
    p = subprocess.run(argv, input=data, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, env=env, timeout=timeout)
    if p.returncode:
        # Commands consuming secrets must never echo captured output on failure.
        raise RuntimeError(f'{Path(argv[0]).name} exit={p.returncode}')
    return p.stdout


def private(path, data):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'wb') as f:
        f.write(data)


def tlv(data, offset=0):
    tag = data[offset]
    n = data[offset + 1]
    offset += 2
    if n & 128:
        k = n & 127
        if not 1 <= k <= 4:
            raise ValueError('invalid DER length')
        n = int.from_bytes(data[offset:offset+k], 'big')
        offset += k
    end = offset + n
    if end > len(data):
        raise ValueError('truncated DER')
    return tag, data[offset:end], end


def fingerprint(der):
    # openssl rsa -RSAPublicKey_out: PKCS#1 SEQUENCE(INTEGER N, INTEGER E).
    tag, seq, end = tlv(der)
    if tag != 0x30 or end != len(der):
        raise ValueError('invalid public RSA sequence')
    nt, n, off = tlv(seq)
    et, e, end = tlv(seq, off)
    if nt != 2 or et != 2 or end != len(seq):
        raise ValueError('invalid public RSA integers')
    ni, ei = int.from_bytes(n, 'big'), int.from_bytes(e, 'big')
    if ni.bit_length() != 2048 or not 1 < ei < ni:
        raise ValueError('unexpected RSA public key')
    raw = struct.pack('!I', 2048) + ni.to_bytes(256, 'big') + ei.to_bytes(256, 'big')
    return '-'.join(f'{v:02x}' for v in hashlib.sha1(raw).digest()[:8])


def wait_line(proc, prefix, deadline, log):
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)
    try:
        while time.monotonic() < deadline:
            events = sel.select(min(0.25, max(0, deadline-time.monotonic())))
            if events:
                line = proc.stdout.readline()
                if not line:
                    raise RuntimeError(f'helper exited={proc.poll()} before {prefix}')
                log.write(line)
                log.flush()
                if line.startswith(prefix):
                    return
            elif proc.poll() is not None:
                raise RuntimeError(f'helper exited={proc.returncode} before {prefix}')
        raise TimeoutError(f'expected helper {prefix}')
    finally:
        sel.close()


def listener_check(pid, port):
    # Match sockets held by this exact Popen child, not an unrelated listener.
    inodes = set()
    for fd in Path(f'/proc/{pid}/fd').iterdir():
        try:
            link = os.readlink(fd)
        except FileNotFoundError:
            continue
        if link.startswith('socket:['):
            inodes.add(link[8:-1])
    found = []
    for proto in ('tcp', 'tcp6'):
        for line in Path(f'/proc/{pid}/net/{proto}').read_text().splitlines()[1:]:
            cols = line.split()
            if cols[3] != '0A' or cols[9] not in inodes:
                continue
            address, p = cols[1].split(':')
            if int(p, 16) != port or proto != 'tcp' or address != '0100007F':
                raise RuntimeError('server has an unexpected TCP listener')
            found.append({'protocol': proto, 'address': '127.0.0.1', 'port': port})
    if len(found) != 1:
        raise RuntimeError('expected one owned loopback RFB listener')
    return found


def inject_probe(env):
    # XTEST sends real server input solely to this freshly created display.
    old = {k: os.environ.get(k) for k in ('DISPLAY', 'XAUTHORITY')}
    os.environ.update({k: env[k] for k in old})
    d = None
    try:
        x = ctypes.CDLL('libX11.so.6')
        t = ctypes.CDLL('libXtst.so.6')
        x.XOpenDisplay.argtypes = [ctypes.c_char_p]
        x.XOpenDisplay.restype = ctypes.c_void_p
        x.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        x.XKeysymToKeycode.restype = ctypes.c_ubyte
        x.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
        x.XCloseDisplay.argtypes = [ctypes.c_void_p]
        t.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
        t.XTestFakeMotionEvent.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_ulong]
        t.XTestFakeButtonEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
        d = x.XOpenDisplay(env['DISPLAY'].encode())
        if not d:
            raise RuntimeError('probe cannot open owned display')
        code = x.XKeysymToKeycode(d, ord('a'))
        if not code:
            raise RuntimeError('probe cannot map a')
        for pressed in (1, 0):
            if not t.XTestFakeKeyEvent(d, code, pressed, 0):
                raise RuntimeError('XTEST key failed')
        if not t.XTestFakeMotionEvent(d, 0, 16, 12, 0):
            raise RuntimeError('XTEST motion failed')
        for pressed in (1, 0):
            if not t.XTestFakeButtonEvent(d, 1, pressed, 0):
                raise RuntimeError('XTEST button failed')
        x.XSync(d, 0)
    finally:
        if d:
            x.XCloseDisplay(d)
        for k, v in old.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v


def one_case(name, client, observer_source):
    start = time.monotonic()
    deadline = start + 55
    directory = Path(tempfile.mkdtemp(prefix=name+'-', dir=BASE)).resolve()
    os.chmod(directory, 0o700)
    if directory.parent != BASE.resolve() or directory.stat().st_uid != os.getuid():
        raise RuntimeError('unexpected run directory ownership/path')
    env = dict(os.environ)
    # Do not inherit the user's desktop or X configuration paths.
    for k in ('DISPLAY', 'XAUTHORITY', 'XENVIRONMENT', 'XINITRC', 'XSERVERRC'):
        env.pop(k, None)
    for k, leaf in (('HOME','home'), ('XDG_CONFIG_HOME','config'),
                    ('XDG_CACHE_HOME','cache'), ('XDG_DATA_HOME','data'),
                    ('XDG_RUNTIME_DIR','runtime')):
        p = directory / leaf
        p.mkdir(mode=0o700)
        env[k] = str(p)
    env['XAUTHORITY'] = str(directory / 'Xauthority')
    result = {'case': name, 'run_dir': str(directory), 'status': 'FAIL'}
    procs, handles = [], []
    password = ''.join(secrets.choice('abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789') for _ in range(8))
    wrong = ('A' if password[0] != 'A' else 'B') + password[1:]
    secret_paths = [directory / x for x in ('password', 'credential', 'rsa.pem', 'Xauthority')]
    def case_timeout(signum, frame):
        raise TimeoutError('whole case exceeded 55s')
    previous_alarm = signal.signal(signal.SIGALRM, case_timeout)
    signal.alarm(55)
    try:
        private(directory/'password', run(['tigervncpasswd','-f'], env, data=(password+'\n').encode()))
        private(directory/'credential', ((wrong if name=='wrong-password' else password)+'\n').encode())
        private(directory/'rsa.pem', b'')
        run(['openssl','genrsa','-out',str(directory/'rsa.pem'),'2048'], env)
        der = run(['openssl','rsa','-in',str(directory/'rsa.pem'),'-RSAPublicKey_out','-outform','DER'], env)
        pin = fingerprint(der)
        result['server_fingerprint'] = pin
        private(directory/'Xauthority', b'')
        cookie = secrets.token_hex(16)
        # Server reads this cookie independent of the eventual display number.
        run(['xauth','-f',env['XAUTHORITY']], env,
            data=f'add :65530 MIT-MAGIC-COOKIE-1 {cookie}\n'.encode())
        observer = directory/'ra2-observer'
        run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(observer_source),'-o',str(observer),'-lX11'], env)
        # Reserve a high port until immediately before exec. Bind races fail in
        # our server startup; never connect to a port before verifying ownership.
        with socket.socket() as reserve:
            reserve.bind(('127.0.0.1', 0))
            port = reserve.getsockname()[1]
        if port <= 5901:
            raise RuntimeError('allocator returned a non-high RFB port')
        rfd, wfd = os.pipe()
        serverlog = open(directory/'server.log','wb'); handles.append(serverlog)
        # A high starting display with displayfd retains Xorg atomic lock choice.
        candidate = 1000 + secrets.randbelow(20000)
        argv = ['/usr/bin/Xtigervnc',f':{candidate}','-displayfd',str(wfd),
                '-interface','127.0.0.1','-localhost','-UseIPv6=0',
                '-rfbport',str(port),'-nolisten','tcp','-SecurityTypes','RA2_256',
                '-RequireUsername=0','-PasswordFile',str(directory/'password'),
                '-RSAKey',str(directory/'rsa.pem'),'-geometry','64x48','-depth','24',
                '-auth',env['XAUTHORITY'],'-desktop','tg-ra2-test','-noreset','-nocursor']
        try:
            server = subprocess.Popen(argv, env=env, stdout=serverlog, stderr=serverlog, pass_fds=(wfd,))
            procs.append(server)
        finally:
            os.close(wfd)
        try:
            sel = selectors.DefaultSelector(); sel.register(rfd, selectors.EVENT_READ)
            if not sel.select(8):
                raise TimeoutError('server did not publish displayfd within 8s')
            display_bytes = os.read(rfd, 64)
            sel.close()
        finally:
            os.close(rfd)
        if not display_bytes.strip().isdigit():
            raise RuntimeError('server exited without valid displayfd')
        display = int(display_bytes.strip())
        if display == 1:
            raise RuntimeError('refusing display :1')
        env['DISPLAY'] = f':{display}'
        result.update(display=env['DISPLAY'], port=port, server_pid=server.pid)
        if server.poll() is not None:
            raise RuntimeError('owned server already exited')
        result['listeners'] = listener_check(server.pid, port)
        run(['xauth','-f',env['XAUTHORITY']], env,
            data=f'add {env["DISPLAY"]} MIT-MAGIC-COOKIE-1 {cookie}\n'.encode())
        del cookie
        info = run(['xdpyinfo'], env)
        (directory/'xdpyinfo.log').write_bytes(info)
        observererr = open(directory/'observer.stderr','wb'); handles.append(observererr)
        obslog = open(directory/'observer.log','wb'); handles.append(obslog)
        obs = subprocess.Popen([str(observer)],env=env,stdout=subprocess.PIPE,stderr=observererr,bufsize=0)
        procs.append(obs)
        wait_line(obs,b'READY',min(deadline,time.monotonic()+5),obslog)
        result['observer_ready'] = True
        if name == 'probe':
            with socket.create_connection(('127.0.0.1',port),timeout=2) as rfb:
                def receive(n):
                    data = b''
                    while len(data) < n:
                        part = rfb.recv(n-len(data))
                        if not part:
                            raise RuntimeError('truncated owned server handshake')
                        data += part
                    return data
                version = receive(12)
                if version != b'RFB 003.008\n':
                    raise RuntimeError('unexpected owned server RFB version')
                rfb.sendall(version)
                count = receive(1)[0]
                types = list(receive(count))
                if types != [129]:
                    raise RuntimeError('owned server did not offer only RA2_256')
                result['rfb_version'] = version.decode().strip()
                result['security_types'] = types
            inject_probe(env)
            wait_line(obs,b'PASS',min(deadline,time.monotonic()+5),obslog)
            result['probe'] = 'XTEST input independently observed; RA2 authentication not exercised'
        else:
            used_pin = pin if name != 'wrong-pin' else ('00' if pin[:2]!='00' else '01')+pin[2:]
            cp = subprocess.Popen([str(client),'--ra2-test',str(port),'--credential-file',str(directory/'credential'),
                                   '--server-fingerprint',used_pin], env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            procs.append(cp)
            out, err = cp.communicate(timeout=min(23,max(0.1,deadline-time.monotonic())))
            # Passwords must never be persisted even if a faulty client echoes one.
            if any(p.encode() in out+err for p in (password,wrong)):
                raise RuntimeError('client leaked a credential; output withheld')
            (directory/'client.stdout').write_bytes(out)
            (directory/'client.stderr').write_bytes(err)
            text = (out+err).decode(errors='replace')
            result['client_exit'] = cp.returncode
            result['credential_callback'] = CALLBACK in text
            if name == 'success':
                if cp.returncode != 0 or AUTH not in text or FRAME not in text or text.index(AUTH)>text.index(FRAME):
                    raise RuntimeError('expected successful authentication followed by first frame')
                if CALLBACK not in text:
                    raise RuntimeError('missing explicit credential callback marker')
                wait_line(obs,b'PASS',min(deadline,time.monotonic()+5),obslog)
            else:
                if cp.returncode != 1 or AUTH in text or FRAME in text:
                    raise RuntimeError('negative case did not return a normal authentication failure')
                if name == 'wrong-pin' and ('identity rejected' not in text or CALLBACK in text):
                    raise RuntimeError('wrong pin was not rejected before credential callback')
                if name == 'wrong-password' and CALLBACK not in text:
                    raise RuntimeError('wrong password did not reach credential callback')
            result['observer_pass'] = name == 'success'
        if server.poll() is not None or obs.poll() is not None:
            raise RuntimeError('server/helper exited unexpectedly')
        listener_check(server.pid,port)
        result['status'] = 'PASS'
    except Exception as exc:
        result['error'] = f'{type(exc).__name__}: {exc}'
    finally:
        signal.alarm(0)
        signal.signal(signal.SIGALRM, previous_alarm)
        cleanup_errors = []
        for p in reversed(procs):
            try:
                if p.poll() is None:
                    p.terminate()
                    try:
                        p.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        p.kill(); p.wait(timeout=2)
                        cleanup_errors.append(f'pid {p.pid} required kill')
                p.wait(timeout=2)
                if p.stdout:
                    p.stdout.close()
                if p.stderr:
                    p.stderr.close()
            except Exception as exc:
                cleanup_errors.append(f'pid {p.pid}: {type(exc).__name__}')
        for h in handles:
            h.close()
        if name == 'wrong-password':
            # TigerVNC 1.15: inspect the completed log of this case's owned
            # server. A callback/exit=1 or a generic disconnect is insufficient.
            try:
                lines = (directory/'server.log').read_text(errors='replace').splitlines()
                lines = [line.strip() for line in lines]
                accepted = [(i, re.fullmatch(r'Connections: Accepted: (127\.0\.0\.1::[0-9]+)', line))
                            for i, line in enumerate(lines)]
                accepted = [(i, match.group(1)) for i, match in accepted if match]
                request = 'SConnection: Client requests security type RA2_256(129)'
                rejection = 'SConnection: Authentication error: Authentication failed'
                requests = [i for i, line in enumerate(lines) if line == request]
                rejections = [i for i, line in enumerate(lines) if line == rejection]
                closing = ([i for i, line in enumerate(lines)
                            if line == f'VNCSConnST:  Closing {accepted[0][1]}: Authentication failed']
                           if len(accepted) == 1 else [])
                rejected = (len(accepted) == len(requests) == len(rejections) == len(closing) == 1
                            and accepted[0][0] < requests[0] < rejections[0] < closing[0])
                result['server_authentication_rejected'] = rejected
                result['server_rejection_evidence'] = {
                    'log': 'server.log', 'checked_after_process_cleanup': True,
                    'accepted_connections': len(accepted),
                    'ra2_requests': len(requests), 'authentication_errors': len(rejections),
                    'matching_authentication_closures': len(closing),
                    'category': 'Authentication failed' if rejected else None,
                }
                if not rejected:
                    result['status'] = 'FAIL'
                    result.setdefault('error', 'missing owned server RA2_256 authentication rejection evidence')
            except Exception as exc:
                result['status'] = 'FAIL'
                result['server_authentication_rejected'] = False
                result.setdefault('error', f'server rejection evidence: {type(exc).__name__}')
        # Private runs are retained intentionally; no deletion of locks, sockets
        # or secrets, and no cleanup can target a path outside this owned run.
        result['secrets_retained_mode'] = {}
        for p in secret_paths:
            if p.exists():
                mode = stat.S_IMODE(p.stat().st_mode)
                result['secrets_retained_mode'][p.name] = oct(mode)
                if mode != 0o600 or p.stat().st_uid != os.getuid():
                    cleanup_errors.append('secret ownership/mode mismatch')
        result['live_owned_pids'] = [p.pid for p in procs if p.poll() is None]
        if result['live_owned_pids']:
            cleanup_errors.append('owned processes remain alive')
        if time.monotonic() > deadline:
            cleanup_errors.append('case exceeded 55s deadline')
        if cleanup_errors:
            result['status']='FAIL'; result['cleanup_errors']=cleanup_errors
        result['elapsed_seconds'] = round(time.monotonic()-start,3)
        (directory/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result),flush=True)
    return result['status']=='PASS'


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--client',type=Path)
    parser.add_argument('--probe',action='store_true')
    args=parser.parse_args()
    if not args.probe and not args.client:
        parser.error('--client is required unless --probe')
    os.umask(0o077)
    BASE.mkdir(parents=True,exist_ok=True,mode=0o700)
    if BASE.is_symlink() or BASE.stat().st_uid != os.getuid() or stat.S_IMODE(BASE.stat().st_mode)!=0o700:
        parser.error('run base must be owned, real directory with mode 0700')
    for tool in ('tigervncpasswd','openssl','xauth','cc','xdpyinfo'):
        if not shutil.which(tool):
            parser.error(f'missing tool: {tool}')
    cases=['probe'] if args.probe else ['success','wrong-pin','wrong-password']
    client=args.client.resolve() if args.client else None
    results=[one_case(c,client,Path(__file__).with_name('ra2-observer.c')) for c in cases]
    return 0 if all(results) else 1

if __name__=='__main__':
    raise SystemExit(main())
