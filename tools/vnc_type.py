import socket, time, struct
HOST, PORT = '127.0.0.1', 5901
s = socket.create_connection((HOST, PORT), timeout=5)
s.settimeout(5.0)
def recv_exact(n):
    data = b''
    while len(data) < n:
        chunk = s.recv(n - len(data))
        if not chunk:
            raise EOFError
        data += chunk
    return data
banner = recv_exact(12)
s.sendall(b'RFB 003.008\n')
ntypes = recv_exact(1)[0]
recv_exact(ntypes)
s.sendall(b'\x01')
recv_exact(4)
s.sendall(b'\x01')
recv_exact(24)          # ServerInit (keep default pixel format)
def key(sym, down):
    s.sendall(struct.pack('>BBxx I', 4, 1 if down else 0, sym))
def type_str(text):
    for ch in text:
        key(ord(ch), True); time.sleep(0.08)
        key(ord(ch), False); time.sleep(0.08)
time.sleep(1.0)
type_str('ver')
key(0xFF0D, True); time.sleep(0.08); key(0xFF0D, False)
time.sleep(3.0)
s.close()
print('typed ver + Enter')
