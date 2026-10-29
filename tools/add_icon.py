# 给 TCC 生成的 PE32+ 追加 .rsrc 节，使 exe 自带图标
# 若节表后没有空位，则把节数据整体后移一个 FileAlignment 块来腾出节表项空间
import struct, sys

EXE = sys.argv[1] if len(sys.argv) > 1 else "dist/AutoClip.exe"
ICO = sys.argv[2] if len(sys.argv) > 2 else "tools/app.ico"
OUT = sys.argv[3] if len(sys.argv) > 3 else "dist/AutoClip_icon.exe"


def align(v, a):
    return (v + a - 1) // a * a


orig = open(EXE, "rb").read()
data = bytearray(orig)
ico = open(ICO, "rb").read()

icon_off = struct.unpack_from("<I", ico, 6 + 12)[0]
icon_w, icon_h = ico[6], ico[7]
icon_size = struct.unpack_from("<I", ico, 6 + 8)[0]
image = ico[icon_off:icon_off + icon_size]

e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
assert data[e_lfanew:e_lfanew + 4] == b"PE\x00\x00"

coff = e_lfanew + 4
num_sections = struct.unpack_from("<H", data, coff + 2)[0]
opt_size = struct.unpack_from("<H", data, coff + 16)[0]
opt = coff + 20
assert struct.unpack_from("<H", data, opt)[0] == 0x20B

sec_align = struct.unpack_from("<I", data, opt + 32)[0]
file_align = struct.unpack_from("<I", data, opt + 36)[0]
sec_table = opt + opt_size


def read_sections():
    out = []
    for i in range(num_sections):
        o = sec_table + i * 40
        nm = bytes(data[o:o + 8]).rstrip(b"\x00").decode(errors="replace")
        vs, va, rs, rp = struct.unpack_from("<IIII", data, o + 8)
        out.append([nm, vs, va, rs, rp])
    return out


secs = read_sections()
first_raw = min(s[4] for s in secs if s[4] > 0)
space_after_table = first_raw - (sec_table + num_sections * 40)

moved = 0
if space_after_table < 40:
    shift = file_align
    data = data[:first_raw] + bytearray(shift) + data[first_raw:]
    for i in range(num_sections):
        o = sec_table + i * 40
        rp = struct.unpack_from("<I", data, o + 20)[0]
        struct.pack_into("<I", data, o + 20, rp + shift)
    moved = shift
    secs = read_sections()

last = secs[-1]
new_rva = align(last[2] + max(last[1], last[3]), sec_align)

# ---------- 构造 .rsrc 目录树 ----------
grp = struct.pack("<HHH", 0, 1, 1) + struct.pack(
    "<BBBBHHIH", icon_w, icon_h, 0, 0, 1, 32, len(image), 1)

DIR, ENT, DAT = 16, 8, 16
root_off = 0
d_icon = root_off + DIR + 2 * ENT
d_grp = d_icon + DIR + ENT
n_icon = d_grp + DIR + ENT
n_grp = n_icon + DIR + ENT
l_icon = n_grp + DIR + ENT
l_grp = l_icon + DIR + ENT
e_icon = l_grp + DIR + ENT
e_grp = e_icon + DAT
b_icon = e_grp + DAT
b_grp = b_icon + align(len(image), 4)

rsrc = bytearray(b_grp + len(grp))


def put_dir(off, items):
    struct.pack_into("<IIHHHH", rsrc, off, 0, 0, 0, 0, 0, len(items))
    for i, (rid, target) in enumerate(items):
        struct.pack_into("<II", rsrc, off + DIR + i * ENT, rid, target)


put_dir(root_off, [(3, 0x80000000 | d_icon), (14, 0x80000000 | d_grp)])
put_dir(d_icon, [(1, 0x80000000 | n_icon)])
put_dir(d_grp, [(1, 0x80000000 | n_grp)])
put_dir(n_icon, [(0x409, 0x80000000 | l_icon)])
put_dir(n_grp, [(0x409, 0x80000000 | l_grp)])
put_dir(l_icon, [(0x409, e_icon)])
put_dir(l_grp, [(0x409, e_grp)])
struct.pack_into("<IIII", rsrc, e_icon, new_rva + b_icon, len(image), 0, 0)
struct.pack_into("<IIII", rsrc, e_grp, new_rva + b_grp, len(grp), 0, 0)
rsrc[b_icon:b_icon + len(image)] = image
rsrc[b_grp:b_grp + len(grp)] = grp

# ---------- 追加节数据 ----------
raw_ptr = align(len(data), file_align)
data.extend(bytearray(raw_ptr - len(data)))
new_raw_size = align(len(rsrc), file_align)
data.extend(rsrc)
data.extend(bytearray(new_raw_size - len(rsrc)))

# ---------- 新节表项 ----------
o = sec_table + num_sections * 40
data[o:o + 40] = bytearray(40)
data[o:o + 8] = b".rsrc\x00\x00\x00"
struct.pack_into("<I", data, o + 8, len(rsrc))
struct.pack_into("<I", data, o + 12, new_rva)
struct.pack_into("<I", data, o + 16, new_raw_size)
struct.pack_into("<I", data, o + 20, raw_ptr)
struct.pack_into("<I", data, o + 36, 0x40000040)

struct.pack_into("<H", data, coff + 2, num_sections + 1)
new_soi = align(new_rva + len(rsrc), sec_align)
struct.pack_into("<I", data, opt + 56, new_soi)

num_dirs = struct.unpack_from("<I", data, opt + 108)[0]
assert num_dirs > 2, "resource data directory slot missing"
struct.pack_into("<II", data, opt + 112 + 2 * 8, new_rva, len(rsrc))

open(OUT, "wb").write(data)
print("OK   %s -> %s" % (EXE, OUT))
print("     size %d -> %d (delta %+d, moved %d)" % (len(orig), len(data), len(data) - len(orig), moved))
print("     .rsrc rva=0x%X raw=0x%X size=%d  SizeOfImage=0x%X" % (new_rva, raw_ptr, len(rsrc), new_soi))
