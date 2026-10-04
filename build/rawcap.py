import socket, threading, time
def serve():
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", 8978)); s.listen(1)
    s.settimeout(20)
    c, _ = s.accept()
    c.settimeout(20)
    data = b""
    try:
        data = c.recv(65536)
        # read a bit more in case headers split
        try:
            while b"\r\n\r\n" not in data and len(data) < 65536:
                chunk = c.recv(65536)
                if not chunk: break
                data += chunk
        except Exception:
            pass
    except Exception as e:
        print("recv err", e)
    open("/tmp/rawcap.bin","wb").write(data)
    print("RAW FIRST 500 BYTES:")
    print(repr(data[:500]))
    try:
        c.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}")
    except Exception:
        pass
    c.close(); s.close()
threading.Thread(target=serve, daemon=True).start()
time.sleep(25)
