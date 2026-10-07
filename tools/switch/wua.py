"""Reads Cemu's .wua game archives (ZArchive, by Exzap; runtime/third_party/zarchive is the C++ reader).

Python 3.14+ (compression.zstd). Used by the Switch builder to take code/cking.rpx and meta/meta.xml out
of a player's archive; the Switch runtime reads the game data from the same .wua itself.

  python3 tools/switch/wua.py <archive.wua>            lists the titles
  python3 tools/switch/wua.py <archive.wua> <path> <out>  extracts one file (e.g. 0005000010143600_v0/code/cking.rpx)
"""
import struct
import sys

try:
    from compression import zstd
except ImportError:  # Python < 3.14
    zstd = None

BLOCK = 64 * 1024
RECORD_ENTRIES = 16
FOOTER_SIZE = 16 * 6 + 32 + 8 + 4 + 4
MAGIC, VERSION1 = 0x169F52D6, 0x61BF3A01


class WuaError(Exception):
    pass


class Wua:
    def __init__(self, path):
        if zstd is None:
            raise WuaError("reading .wua archives needs Python 3.14 or newer (compression.zstd)")
        self.f = open(path, "rb")
        self.f.seek(0, 2)
        size = self.f.tell()
        if size < FOOTER_SIZE:
            raise WuaError("not a Cemu .wua archive (too small)")
        self.f.seek(size - FOOTER_SIZE)
        footer = self.f.read(FOOTER_SIZE)
        sections = struct.unpack_from(">12Q", footer, 0)
        total, version, magic = struct.unpack_from(">QII", footer, 96 + 32)
        if magic != MAGIC or version != VERSION1 or total != size:
            raise WuaError("not a Cemu .wua archive (or damaged)")
        (self.data_off, self.data_size, rec_off, rec_size, names_off, names_size,
         tree_off, tree_size) = sections[:8]
        self.records = []
        raw = self._read(rec_off, rec_size)
        for i in range(len(raw) // 40):
            base = struct.unpack_from(">Q", raw, i * 40)[0]
            sizes = struct.unpack_from(">16H", raw, i * 40 + 8)
            self.records.append((base, sizes))
        self.names = self._read(names_off, names_size)
        raw = self._read(tree_off, tree_size)
        self.tree = [struct.unpack_from(">IIII", raw, i * 16) for i in range(len(raw) // 16)]
        self.cache = {}

    def _read(self, offset, size):
        self.f.seek(offset)
        data = self.f.read(size)
        if len(data) != size:
            raise WuaError("archive truncated")
        return data

    def _name(self, offset):
        if offset == 0x7FFFFFFF or offset >= len(self.names):
            return ""
        length = self.names[offset] & 0x7F
        if self.names[offset] & 0x80:  # two-byte length (as the C++ reader reads it)
            length |= self.names[offset] << 7
            offset += 2
        else:
            offset += 1
        return self.names[offset:offset + length].decode("utf-8", "replace")

    def _children(self, index):
        flag_name, start, count, _ = self.tree[index]
        if flag_name & 0x80000000:
            return []
        return [(self._name(self.tree[i][0] & 0x7FFFFFFF), i) for i in range(start, start + count)]

    def lookup(self, path):
        """Tree index of a path ('/'-separated, case-insensitive), or None."""
        index = 0
        for part in [p for p in path.replace("\\", "/").split("/") if p]:
            for name, child in self._children(index):
                if name.lower() == part.lower():
                    index = child
                    break
            else:
                return None
        return index

    def listdir(self, path=""):
        index = self.lookup(path)
        return [] if index is None else [name for name, _ in self._children(index)]

    def _block(self, n):
        if n in self.cache:
            return self.cache[n]
        base, sizes = self.records[n // RECORD_ENTRIES]
        sub = n % RECORD_ENTRIES
        offset = base + sum(s + 1 for s in sizes[:sub])
        csize = sizes[sub] + 1
        raw = self._read(self.data_off + offset, csize)
        data = raw if csize == BLOCK else zstd.decompress(raw)
        if len(data) != BLOCK:
            raise WuaError("archive block %d is damaged" % n)
        if len(self.cache) > 64:
            self.cache.clear()
        self.cache[n] = data
        return data

    def read(self, path):
        index = self.lookup(path)
        if index is None:
            raise WuaError("%s is not in the archive" % path)
        flag_name, off_low, size_low, high = self.tree[index]
        if not flag_name & 0x80000000:
            raise WuaError("%s is a folder" % path)
        offset = off_low | ((high & 0xFFFF) << 32)
        size = size_low | ((high >> 16) << 32)
        out = bytearray()
        pos = offset
        while len(out) < size:
            block = self._block(pos // BLOCK)
            start = pos % BLOCK
            take = min(BLOCK - start, size - len(out))
            out += block[start:start + take]
            pos += take
        return bytes(out)

    def titles(self):
        """[(folder, title id, version)]: the archive's top folders are <title id>_v<version>."""
        found = []
        for name in self.listdir(""):
            tid, _, ver = name.partition("_v")
            if len(tid) == 16 and ver.isdigit():
                found.append((name, tid.lower(), int(ver)))
        return found


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    w = Wua(argv[0])
    if len(argv) == 1:
        for folder, tid, ver in w.titles():
            print(folder, tid, ver)
        return 0
    with open(argv[2], "wb") as f:
        f.write(w.read(argv[1]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
