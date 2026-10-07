"""Switch homebrew packaging in plain Python: control.nacp and ELF -> NRO.

Byte-for-byte the output of switchbrew's nacptool --create and elf2nro (switch-tools, ISC license,
copyright 2017 plutoo and contributors), so the PC builder needs no devkitPro tools.

  python3 tools/switch/nro.py <elf> <nro> [--icon=<jpg>] [--name=N] [--author=A] [--version=V]
"""
import struct
import sys

PT_LOAD = 1
SHT_NOTE = 7
NT_GNU_BUILD_ID = 3


def make_nacp(name, author, version, titleid=0):
    """control.nacp (0x4000 bytes) as nacptool --create writes it."""
    nacp = bytearray(0x4000)

    def put_str(offset, size, text):
        raw = text.encode("utf-8")[: size - 1]  # strncpy(dst, src, size - 1) into a zeroed field
        nacp[offset:offset + len(raw)] = raw

    for lang in range(12):
        put_str(lang * 0x300, 0x200, name)
        put_str(lang * 0x300 + 0x200, 0x100, author)
    put_str(0x3060, 0x10, version)
    if titleid:
        struct.pack_into("<Q", nacp, 0x3038, titleid)
        struct.pack_into("<Q", nacp, 0x3070, titleid + 0x1000)
        struct.pack_into("<Q", nacp, 0x3078, titleid)
        struct.pack_into("<Q", nacp, 0x30B0, titleid)
    struct.pack_into("<I", nacp, 0x3024, 0x100)
    struct.pack_into("<I", nacp, 0x302C, 0xBFF)
    struct.pack_into("<I", nacp, 0x3034, 0x10000)
    nacp[0x3040:0x3060] = bytes([0x0C, 0xFF, 0xFF, 0x0A, 0xFF, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0D, 0x0D]) + b"\xff" * 20
    struct.pack_into("<I", nacp, 0x3080, 0x3E00000)
    struct.pack_into("<I", nacp, 0x3088, 0x180000)
    struct.pack_into("<I", nacp, 0x30F0, 0x102)
    return bytes(nacp)


def _page(n):
    return (n + 0xFFF) & ~0xFFF


def elf_to_nro(elf, icon=None, nacp=None):
    """The NRO elf2nro makes of a linked homebrew ELF (three PT_LOAD segments: code, rodata, data)."""
    if elf[:4] != b"\x7fELF" or struct.unpack_from("<H", elf, 18)[0] != 183:  # EM_AARCH64
        raise ValueError("not an AArch64 ELF")
    e_phoff, e_shoff = struct.unpack_from("<QQ", elf, 32)
    e_phentsize, e_phnum, e_shentsize, e_shnum = struct.unpack_from("<HHHH", elf, 54)
    loads = []
    for i in range(e_phnum):
        p_type, _flags, p_offset, p_vaddr, _paddr, p_filesz, p_memsz, _align = struct.unpack_from(
            "<IIQQQQQQ", elf, e_phoff + i * e_phentsize)
        if p_type == PT_LOAD:
            loads.append((p_offset, p_vaddr, p_filesz, p_memsz))
    if len(loads) < 3:
        raise ValueError("expected 3 loadable segments (code, rodata, data)")
    segments, file_off = [], 0
    for p_offset, p_vaddr, p_filesz, _memsz in loads[:3]:
        size = _page(p_filesz)
        segments.append((p_vaddr, size, elf[p_offset:p_offset + p_filesz]))
        file_off = _page(file_off + size)
    data_filesz, data_memsz = loads[2][2], loads[2][3]
    bss = _page(data_memsz - _page(data_filesz)) if data_memsz > _page(data_filesz) else 0
    build_id = b""
    for i in range(e_shnum):
        sh = e_shoff + i * e_shentsize
        sh_type = struct.unpack_from("<I", elf, sh + 4)[0]
        if sh_type != SHT_NOTE:
            continue
        sh_offset = struct.unpack_from("<Q", elf, sh + 24)[0]
        namesz, descsz, ntype = struct.unpack_from("<III", elf, sh_offset)
        name = elf[sh_offset + 12:sh_offset + 12 + namesz]
        if ntype == NT_GNU_BUILD_ID and namesz == 4 and name == b"GNU\x00":
            desc = sh_offset + 12 + namesz
            build_id = elf[desc:desc + min(descsz, 0x20)]

    # segments at their virtual addresses, then the header over bytes 0x10-0x80 of the code segment
    out = bytearray(max(off + size for off, size, _ in segments))
    for off, _size, data in segments:
        out[off:off + len(data)] = data
    header = bytearray(0x70)
    header[0:4] = b"NRO0"
    struct.pack_into("<III", header, 4, 0, file_off, 0)  # version, size, flags
    for i, (off, size, _data) in enumerate(segments):
        struct.pack_into("<II", header, 0x10 + i * 8, off, size)
    struct.pack_into("<I", header, 0x28, bss)
    header[0x30:0x30 + len(build_id)] = build_id
    out[0x10:0x80] = header
    if icon is None and nacp is None:
        return bytes(out)

    # asset section after the image: header, icon, nacp (no RomFS)
    out.extend(b"\0" * (file_off - len(out)))
    assets = bytearray(0x38)
    assets[0:4] = b"ASET"
    offset = 0x38
    if icon is not None:
        struct.pack_into("<QQ", assets, 8, offset, len(icon))
        offset += len(icon)
    if nacp is not None:
        struct.pack_into("<QQ", assets, 0x18, offset, len(nacp))
    out += assets + (icon or b"") + (nacp or b"")
    return bytes(out)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    opts = dict(a[2:].split("=", 1) for a in argv[2:] if a.startswith("--") and "=" in a)
    with open(argv[0], "rb") as f:
        elf = f.read()
    icon = open(opts["icon"], "rb").read() if "icon" in opts else None
    nacp = make_nacp(opts.get("name", "Wind Waker HD"), opts.get("author", "WindWakerHDNX"), opts.get("version", "0.1"))
    with open(argv[1], "wb") as f:
        f.write(elf_to_nro(elf, icon, nacp))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
