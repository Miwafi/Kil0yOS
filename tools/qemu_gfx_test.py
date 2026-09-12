import socket, time, sys, struct, zlib

MON = "/tmp/kmon.sock"
PPM = "/tmp/gfx.ppm"
PNG = "/tmp/gfx.png"

def mon(cmd, wait=0.3):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(MON)
    s.settimeout(1.0)
    try:
        s.recv(65536)
    except Exception:
        pass
    s.sendall((cmd + "\n").encode())
    time.sleep(wait)
    try:
        s.recv(65536)
    except Exception:
        pass
    s.close()

def type_cmd(cmd):
    for k in cmd:
        mon("sendkey " + k)
        time.sleep(0.1)
    mon("sendkey ret")

type_cmd("gfx")
time.sleep(4)
mon("screendump /tmp/gfx.ppm")
time.sleep(1)

mon("sendkey esc")          # exit gfx back to text
time.sleep(2)
type_cmd("gui")             # launch the VGA desktop
time.sleep(6)
mon("screendump /tmp/gui.ppm")
time.sleep(1)
PPMS = [(PPM, PNG), ("/tmp/gui.ppm", "/tmp/gui.png")]

# ppm -> png
def chunk(tag, payload):
    c = struct.pack(">I", len(payload)) + tag + payload
    return c + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

for PPM, PNG in PPMS:
    with open(PPM, "rb") as f:
        data = f.read()
    # parse header P6 w h max
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    raw = parts[3]

    rows = b""
    for y in range(h):
        rows += b"\x00" + raw[y*w*3:(y+1)*w*3]
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(rows, 6))
           + chunk(b"IEND", b""))
    with open(PNG, "wb") as f:
        f.write(png)
    print("saved", PNG, w, "x", h)

# sample the middle row of each color bar
with open(PPM, "rb") as f:
    data = f.read()
parts = data.split(b"\n", 3)
w, h = map(int, parts[1].split())
raw = parts[3]
for x in (40, 120, 200, 280, 360, 440, 520, 600):
    off = 240 * w * 3 + x * 3
    print(x, tuple(raw[off:off + 3]))
