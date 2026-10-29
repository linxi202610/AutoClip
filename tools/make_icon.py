# 生成 AutoClip 的 ico + res（供 TCC 链接进 exe）
import struct, math, os

S = 32

def rounded_rect(x, y, w, h, r):
    # 返回该点是否在圆角矩形内
    cx = min(max(x, r), w - 1 - r)
    cy = min(max(y, r), h - 1 - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r or (r <= x < w - r) or (r <= y < h - r)

def dist_seg(px, py, x1, y1, x2, y2):
    dx, dy = x2 - x1, y2 - y1
    L2 = dx * dx + dy * dy
    if L2 == 0:
        return math.hypot(px - x1, py - y1)
    t = max(0, min(1, ((px - x1) * dx + (py - y1) * dy) / L2))
    return math.hypot(px - (x1 + t * dx), py - (y1 + t * dy))

GREEN = (0x22, 0xC5, 0x5E)
DARK = (0x16, 0x9E, 0x4A)

px = [[(0, 0, 0, 0) for _ in range(S)] for _ in range(S)]

for y in range(S):
    for x in range(S):
        inside = rounded_rect(x, y, S, S, 8)
        if not inside:
            continue
        # 竖向渐变绿
        t = y / (S - 1)
        r = int(GREEN[0] * (1 - t) + DARK[0] * t)
        g = int(GREEN[1] * (1 - t) + DARK[1] * t)
        b = int(GREEN[2] * (1 - t) + DARK[2] * t)
        px[y][x] = (r, g, b, 255)

# 白色对勾
for y in range(S):
    for x in range(S):
        if px[y][x][3] == 0:
            continue
        d = min(dist_seg(x, y, 9, 17, 14, 22), dist_seg(x, y, 14, 22, 23, 10))
        if d <= 2.4:
            px[y][x] = (255, 255, 255, 255)

# 组装 ICO（32bpp BGRA, bottom-up）
xor = b""
for y in range(S - 1, -1, -1):
    for x in range(S):
        r, g, b, a = px[y][x]
        xor += bytes((b, g, r, a))

and_mask = bytes(4 * S)  # 每行 4 字节对齐，全 0

bih = struct.pack("<IiiHHIIiiII", 40, S, S * 2, 1, 32, 0, len(xor) + len(and_mask), 0, 0, 0, 0)
image = bih + xor + and_mask

icondir = struct.pack("<HHH", 0, 1, 1)
entry = struct.pack("<BBBBHHII", S, S, 0, 0, 1, 32, len(image), 6 + 16)
ico = icondir + entry + image
open("app.ico", "wb").write(ico)

# 组装 RES
def res_entry(rtype, name, data):
    return (struct.pack("<II", len(data), 0x20)
            + struct.pack("<HH", 0xFFFF, rtype)
            + struct.pack("<HH", 0xFFFF, name)
            + struct.pack("<IHHII", 0, 0x0030, 0x0409, 0, 0)
            + data)

# GRPICONDIR: reserved, type=1, count=1, (w,h,color,res,planes,bitcount,bytes,id)
grp = struct.pack("<HHH", 0, 1, 1) + struct.pack("<BBBBHHIH", S, S, 0, 0, 1, 32, len(image), 1)

res = res_entry(3, 1, image) + res_entry(14, 1, grp) + struct.pack("<I", 0)
open("app.res", "wb").write(res)

print("ico:", len(ico), "bytes  res:", len(res), "bytes")
