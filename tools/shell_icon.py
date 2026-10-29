import ctypes, struct, zlib, sys, os
from ctypes import wintypes

path = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else r"dist\AutoClip_icon.exe")
out = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else r"tools\shell_icon.png")

shell32 = ctypes.windll.shell32
user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32


class SHFILEINFOW(ctypes.Structure):
    _fields_ = [("hIcon", wintypes.HICON), ("iIcon", ctypes.c_int),
                ("dwAttributes", wintypes.DWORD),
                ("szDisplayName", wintypes.WCHAR * 260),
                ("szTypeName", wintypes.WCHAR * 80)]


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


shell32.SHGetFileInfoW.restype = ctypes.c_void_p
shell32.SHGetFileInfoW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD,
                                   ctypes.POINTER(SHFILEINFOW), wintypes.UINT, wintypes.UINT]

shfi = SHFILEINFOW()
SHGFI_ICON, SHGFI_LARGEICON = 0x000000100, 0x000000000
r = shell32.SHGetFileInfoW(path, 0, ctypes.byref(shfi), ctypes.sizeof(shfi),
                           SHGFI_ICON | SHGFI_LARGEICON)
print("SHGetFileInfo ->", r, "hIcon =", shfi.hIcon, "type =", shfi.szTypeName)
if not r or not shfi.hIcon:
    print("FAIL: shell could not produce an icon")
    sys.exit(1)

ii = ICONINFO()
user32.GetIconInfo.argtypes = [wintypes.HICON, ctypes.POINTER(ICONINFO)]
user32.GetIconInfo.restype = wintypes.BOOL
user32.GetDC.restype = wintypes.HDC
user32.GetDC.argtypes = [wintypes.HWND]
user32.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
gdi32.GetObjectW.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p]
gdi32.GetDIBits.argtypes = [wintypes.HDC, wintypes.HBITMAP, wintypes.UINT, wintypes.UINT,
                            ctypes.c_void_p, ctypes.c_void_p, wintypes.UINT]

hicon = ctypes.c_void_p(shfi.hIcon & 0xFFFFFFFFFFFFFFFF)
if not user32.GetIconInfo(hicon, ctypes.byref(ii)):
    print("FAIL: GetIconInfo")
    sys.exit(1)

bm = BITMAP()
gdi32.GetObjectW(ii.hbmColor, ctypes.sizeof(bm), ctypes.byref(bm))
w, h = bm.bmWidth, bm.bmHeight
print("icon bitmap %dx%d %dbpp" % (w, h, bm.bmBitsPixel))

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

S = 8
rows = []
for y in range(h):
    row = bytearray(b"\x00")
    for x in range(w):
        b, g, r_, a = buf.raw[(y * w + x) * 4:(y * w + x) * 4 + 4]
        al = a / 255.0
        row += bytes((int(r_ * al + 255 * (1 - al)),
                      int(g * al + 255 * (1 - al)),
                      int(b * al + 255 * (1 - al))))
    for _ in range(S):
        wide = bytearray(b"\x00")
        for x in range(w):
            wide += bytes(row[1 + x * 3:1 + x * 3 + 3]) * S
        rows.append(bytes(wide))


def chunk(t, d):
    c = t + d
    return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)


png = (b"\x89PNG\r\n\x1a\n"
       + chunk(b"IHDR", struct.pack(">IIBBBBB", w * S, h * S, 8, 2, 0, 0, 0))
       + chunk(b"IDAT", zlib.compress(b"".join(rows), 9))
       + chunk(b"IEND", b""))
open(out, "wb").write(png)
print("saved", out)
