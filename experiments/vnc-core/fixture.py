# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded RFB 3.8 loopback fixture, Python standard library only."""
import socket
import struct
import subprocess
import sys
import threading

binary, case = sys.argv[1:]
errors = []

def recv(s, n):
    data = b""
    while len(data) < n:
        chunk = s.recv(n - len(data))
        if not chunk:
            raise AssertionError("unexpected client EOF")
        data += chunk
    return data

listener = socket.socket()
listener.bind(("127.0.0.1", 0))
listener.listen(1)
listener.settimeout(5)
port = listener.getsockname()[1]

def serve():
    try:
        with listener.accept()[0] as s:
            s.settimeout(5)
            if case == "timeout":
                # Wait for client's own deadline/EOF; no unbounded sleeping.
                assert s.recv(1) == b""
                return
            s.sendall(b"RFB 003.008\n")
            assert recv(s, 12) == b"RFB 003.008\n"
            s.sendall(b"\x01\x7f" if case == "unsupported" else b"\x01\x01")
            if case in ("unsupported", "none_rejected"):
                assert s.recv(1) == b""
                return
            assert recv(s, 1) == b"\x01"
            s.sendall(struct.pack(">I", 0))
            assert recv(s, 1) == b"\x01"
            pf = struct.pack(">BBBBHHHBBB3x", 32, 24, 0, 1, 255, 255, 255, 16, 8, 0)
            init = struct.pack(">HH", 2, 1) + pf + struct.pack(">I", 7) + b"fixture"
            if case == "truncated":
                s.sendall(init[:10])
                return
            s.sendall(init)
            sent = False
            keys = []
            pointer = None
            while pointer is None:
                kind = recv(s, 1)[0]
                if kind == 0:
                    requested = recv(s, 19)
                    assert requested[3:] == pf
                elif kind == 2:
                    count = struct.unpack(">H", recv(s, 3)[1:])[0]
                    encodings = struct.unpack(">" + "i" * count, recv(s, count * 4))
                    assert 0 in encodings
                elif kind == 3:
                    recv(s, 9)
                    if not sent:
                        s.sendall(b"\x00\x00\x00\x01" + struct.pack(">HHHHi", 0, 0, 2, 1, 0)
                                  + bytes.fromhex("3322110066554400"))
                        sent = True
                elif kind == 4:
                    keys.append(struct.unpack(">B2xI", recv(s, 7)))
                elif kind == 5:
                    pointer = struct.unpack(">BHH", recv(s, 5))
                else:
                    raise AssertionError(f"unexpected client message {kind}")
            assert keys == [(1, 0x61), (0, 0x61)], keys
            assert pointer == (1, 1, 0), pointer
    except BaseException as e:
        errors.append(e)
    finally:
        listener.close()

thread = threading.Thread(target=serve, daemon=True)
thread.start()
mode = "--fixture-loopback-strict" if case == "none_rejected" else "--fixture-loopback-none"
result = subprocess.run([binary, mode, str(port)], capture_output=True, text=True, timeout=6)
thread.join(5)
assert not thread.is_alive(), "fixture did not terminate"
assert not errors, errors
assert result.returncode == (0 if case == "raw" else 1), (result.returncode, result.stdout, result.stderr)
if case == "raw":
    assert "Raw pixels=112233,445566" in result.stdout
elif case == "truncated":
    assert "End of stream" in result.stderr
elif case == "timeout":
    assert "deadline" in result.stderr
elif case in ("unsupported", "none_rejected"):
    assert "security" in result.stderr.lower()
print(f"{case}: PASS; client exit={result.returncode}")
print(result.stdout + result.stderr, end="")
