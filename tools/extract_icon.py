import ctypes, struct, zlib, sys
from ctypes import wintypes

path = sys.argv[1] if len(sys.argv) > 1 else r"dist\AutoClip_icon.exe"
out = sys.argv[2] if len(sys.argv) > 2 else r"tools\extracted.png"

shell32 = ctypes.windll.shell32
user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32


class ICONINFO(ctypes.Structure):
    _fields_ = [("fIcon", wintypes.BOOL), ("xHotspot", wintypes.DWORD),
                ("yHotspot", wintypes.DWORD), ("hbmMask", wintypes.HBITMAP),
                ("hbmColor", wintypes.HBITMAP)]


class BITMAP(ctypes.Structure):
    _fields_ = [("bmType", wintypes.LONG), ("bmWidth", wintypes.LONG),
                ("bmHeight", wintypes.LONG), ("bmWidthBytes", wintypes.LONG),
                ("bmPlanes", wintypes.WORD), ("bmBitsPixel", wintypes.WORD),
                ("bmBits", ctypes.c_void_p)]


class BIH(ctypes.Structure):
    _fields_ = [("biSize", wintypes.DWORD), ("biWidth", wintypes.LONG),
                ("biHeight", wintypes.LONG), ("biPlanes", wintypes.WORD),
                ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
                ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", wintypes.LONG),
                ("biYPelsPerMeter", wintypes.LONG), ("biClrUsed", wintypes.DWORD),
                ("biClrImportant", wintypes.DWORD)]


n = shell32.ExtractIconExW(wintypes.LPCWSTR(path), -1, None, None, 0)
print("icons in exe:", n)
if n <= 0:
    print("FAIL: no icon resource found in exe")
    sys.exit(1)

large = wintypes.HICON()
small = wintypes.HICON()
got = shell32.ExtractIconExW(wintypes.LPCWSTR(path), 0,
                             ctypes.byref(large), ctypes.byref(small), 1)
print("extracted:", got, "large=", large, "small=", small)

ii = ICONINFO()
if not user32.GetIconInfo(large, ctypes.byref(ii)):
    print("FAIL: GetIconInfo")
    sys.exit(1)

bm = BITMAP()
gdi32.GetObjectW(ii.hbmColor, ctypes.sizeof(bm), ctypes.byref(bm))
w, h = bm.bmWidth, bm.bmHeight
print("bitmap: %dx%d  %d bpp" % (w, h, bm.bmBitsPixel))

bih = BIH()
bih.biSize = ctypes.sizeof(BIH)
bih.biWidth = w
bih.biHeight = -h
bih.biPlanes = 1
bih.biBitCount = 32
bih.biCompression = 0

buf = ctypes.create_string_buffer(w * h * 4)
hdc = user32.GetDC(None)
lines = gdi32.GetDIBits(hdc, ii.hbmColor, 0, h, buf, ctypes.byref(bih), 0)
user32.ReleaseDC(None, hdc)
print("GetDIBits lines:", lines)

# 合成到白色背景，放大 8 倍
S = 8
rows = []
for y in range(h):
    row = bytearray(b"\x00")
    for x in range(w):
        b, g, r, a = buf.raw[(y * w + x) * 4:(y * w + x) * 4 + 4]
        al = a / 255.0
        row += bytes((int(r * al + 255 * (1 - al)),
                      int(g * al + 255 * (1 - al)),
                      int(b * al + 255 * (1 - al))))
    for _ in range(S):
        wide = bytearray(b"\x00")
        for x in range(w):
            wide += bytes(row[1 + x * 3:1 + x * 3 + 3]) * S
        rows.append(bytes(wide))

raw = b"".join(rows)


def chunk(t, d):
    c = t + d
    return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)


png = (b"\x89PNG\r\n\x1a\n"
       + chunk(b"IHDR", struct.pack(">IIBBBBB", w * S, h * S, 8, 2, 0, 0, 0))
       + chunk(b"IDAT", zlib.compress(raw, 9))
       + chunk(b"IEND", b""))
open(out, "wb").write(png)
print("saved", out)
