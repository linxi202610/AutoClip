# 用 Windows 官方 UpdateResource API 给 exe 注入图标（系统自行处理节与对齐）
import ctypes, struct, sys, shutil, os
from ctypes import wintypes

src = sys.argv[1]
ico_path = sys.argv[2]
dst = sys.argv[3]

ico = open(ico_path, "rb").read()
icon_off = struct.unpack_from("<I", ico, 6 + 12)[0]
icon_w, icon_h = ico[6], ico[7]
icon_size = struct.unpack_from("<I", ico, 6 + 8)[0]
image = ico[icon_off:icon_off + icon_size]

grp = struct.pack("<HHH", 0, 1, 1) + struct.pack(
    "<BBBBHHIH", icon_w, icon_h, 0, 0, 1, 32, len(image), 1)

shutil.copyfile(src, dst)

k = ctypes.windll.kernel32
k.BeginUpdateResourceW.restype = wintypes.HANDLE
k.BeginUpdateResourceW.argtypes = [wintypes.LPCWSTR, wintypes.BOOL]
k.UpdateResourceW.restype = wintypes.BOOL
k.UpdateResourceW.argtypes = [wintypes.HANDLE, wintypes.LPCWSTR, wintypes.LPCWSTR,
                              wintypes.WORD, ctypes.c_void_p, wintypes.DWORD]
k.EndUpdateResourceW.restype = wintypes.BOOL
k.EndUpdateResourceW.argtypes = [wintypes.HANDLE, wintypes.BOOL]

h = k.BeginUpdateResourceW(dst, False)
if not h:
    print("FAIL BeginUpdateResource err=%d" % k.GetLastError())
    sys.exit(1)

def ptr(n):
    return ctypes.cast(ctypes.c_void_p(n), wintypes.LPCWSTR)

buf_icon = ctypes.create_string_buffer(image, len(image))
ok1 = k.UpdateResourceW(h, ptr(3), ptr(1), 0, buf_icon, len(image))
err1 = k.GetLastError()

buf_grp = ctypes.create_string_buffer(grp, len(grp))
ok2 = k.UpdateResourceW(h, ptr(14), ptr(1), 0, buf_grp, len(grp))
err2 = k.GetLastError()

ok3 = k.EndUpdateResourceW(h, False)
print("RT_ICON=%s(err=%d)  RT_GROUP_ICON=%s(err=%d)  commit=%s" % (ok1, err1, ok2, err2, ok3))
print("size: %d -> %d" % (os.path.getsize(src), os.path.getsize(dst)))
