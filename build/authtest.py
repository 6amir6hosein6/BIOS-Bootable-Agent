#!/usr/bin/env python3
"""Decisive auth test: read key from .env, run the real agent binary against a
local capture socket, compare the Authorization header byte-for-byte.
Prints only lengths/hashes/verdicts — never the key itself."""
import socket, hashlib, re, subprocess, threading, os, sys

FA = os.path.expanduser("~/bootable-agent/flashagent")

# 1. read key from .env (same source that works with curl)
key = None
for line in open(os.path.expanduser("~/.hermes/.env")):
    if "HERMES_CUSTOM_157_66_255_8_4000_API_KEY" in line:
        key = line.split("=", 1)[1].strip()
        break
assert key, "key not found in .env"
print(f"expected key: len={len(key)} sha16={hashlib.sha256(key.encode()).hexdigest()[:16]}")

# 2. capture server on :8980
srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", 8980))
srv.listen(1)

result = {}
def serve():
    c, _ = srv.accept()
    data = c.recv(65536)
    c.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}")
    c.close()
    hdr = data.split(b"\r\n\r\n")[0].decode(errors="replace")
    v = None
    for l in hdr.split("\r\n"):
        if l.lower().startswith("authorization"):
            v = l.split(":", 1)[1].strip()
            break
    result["v"] = v

t = threading.Thread(target=serve, daemon=True)
t.start()

# 3. run the real agent binary with the key straight from the file
env = dict(os.environ)
env["LLM_KEY"] = key
env["LLM_BASE"] = "http://127.0.0.1:8980/v1"
env["LLM_MODEL"] = "qwen3.8"
subprocess.run([f"{FA}/build/flashagent", "--once", "hi"], env=env,
               timeout=30, capture_output=True)
t.join(timeout=5)

v = result.get("v")
if v is None:
    print("NO Authorization header received")
    sys.exit(1)
tok = v[7:] if v.startswith("Bearer ") else v
print(f"received token: len={len(tok)} sha16={hashlib.sha256(tok.encode()).hexdigest()[:16]}")
print("VERDICT:", "MATCH — agent sends the key byte-for-byte" if tok == key
      else "MISMATCH — agent is NOT sending the file key")
if tok != key:
    # where do they differ (char classes only, no secret material)
    import string
    def cls(ch):
        if ch in string.ascii_letters + string.digits: return "alnum"
        if ch == "-": return "dash"
        if ch == "_": return "under"
        return repr(ch)
    print("expected class seq:", "".join(cls(c) for c in key))
    print("received class seq:", "".join(cls(c) for c in tok))
