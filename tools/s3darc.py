#!/usr/bin/env python3
"""s3darc.py - list and extract TimeShift (Saber3D engine, 2007) .s3darc archives.

Usage (Python 3, standard library only):
    python -I s3darc.py list    <archive> [--blocks] [--names FILE]
    python -I s3darc.py extract <archive> <outdir> [--filter SUBSTR] [--raw]
                                [--resolve] [--no-blocks] [--names FILE]
    python -I s3darc.py names   <archive> [<archive> ...] -o FILE

    list     prints the directory (type, sizes, offset or base[index], name);
             --blocks also prints cache block members.
    extract  writes <outdir>/<Type>/<name> and <outdir>/CacheBlock/<block>/<member>,
             plus _index.csv.  Patch archives: only the entries stored in the
             patch, unless --resolve (then also unchanged ones from the base).
    names    harvests candidate names from the archives (directory, preload
             lists, scene text, cache block contents) and writes the names of
             cache block members it can identify, for use with --names.  Run
             it on all archives together (out, patch, out_<lang>); other
             files given (e.g. the game executable) are scanned for words.

Archive format (all integers little-endian u32 unless noted)
-------------------------------------------------------------
A "record" is ``[u32 usize][u32 csize][csize bytes of zlib]``; the zlib
stream inflates to exactly ``usize`` bytes.

    record  version      -> u32 1
    record  base name    -> u32 len + ASCII name ("" for a base archive,
                            "out.s3darc" etc. for a patch archive)
    u32     dir_offset   absolute file offset of the directory record
    ...     entry data   one raw zlib stream per stored entry, back to back
    record  directory    (at dir_offset, runs to end of file)

Directory (inflated):
    u32 count
    count x { u32 name_len; char name[name_len];      (flat name, no path)
              u32 type;                                (resource class, below)
              i32 offset;    absolute offset of the zlib stream, or -1
              u32 crc;       zlib.crc32 of the inflated data; when offset
                             is -1 this field is instead the entry's index
                             in the base archive's directory
              u32 usize;     inflated size
              u32 csize; }   stored (compressed) size

Patch archives: a patch (patch*.s3darc) names its base archive in the
header and carries a complete directory.  Entries with offset -1 are
unchanged and are read from the base archive (directory index in the crc
field); entries with a real offset replace the base copy or are new.  The
game opens the patch named in bin/steamwars.ini ([Preload] arcName /
arcNameLoc), which pulls in its base; the vendor archives (out_ati / out_nv,
patch_ati / patch_nv) hold GPU-specific texture encodings and precompiled
shader pairs, and out_<lang> holds localised voice/sound data.

Resource types (type id = index into the engine's preload category table;
extract writes each type to a folder of that name):
     0 Preload       per-level preload list, text "Key = [ "name", ... ]" with
                     a u32 length prefix (stripped on extract -> .txt); one per
                     level (menu, ts2_*, multiplayerN) plus initial_*
     1 SceneData     <level>.data
     2 CacheBlock    hash-indexed sub-archive, see below
     3 Shader        .fx/.psh/.vsh HLSL source (packed)
     4 ShaderCache   (not present)
     5 ShaderPairs   compiled shader permutations, vendor archives -> .pairs
     6 Textures      Saber texture ("TCIP" chunk header) -> .tex
     7 Cubemaps      same container -> .tex
     8 SoundData     FMOD Designer .fev projects, .psbin voice tables
     9 Sounds        (names only, listed in preload lists)
    10 WaveBanks_mem FMOD sample banks "FSB3" -> .fsb
    11 WaveBanks_strm_file (streamed banks, loose files on disk)
    12 Templates     binary entity templates and animations ("LPTA") -> .tpl
    13 VoiceSplines  (inside the vo_splines cache block)
    14 Strings       UTF-16LE localisation tables -> .txt
    15 Ragdolls      Havok .hkx (packed)
    16 Scene         <level>.lg: level objects, with their property text and
                     embedded scripts
    17-24            <level>.grs .scr .trc .rain .cdt .sm .slo .vis
                     (SceneGrs, SceneScr (packed), SceneTrc, SceneRain,
                     SceneCDT, SceneSM, SceneSLO, SceneVis)

"Packed" entries (types 3, 15, 18) carry a second compression layer inside
the stored data: ``u32 usize; u32 csize; u16 0; zlib[csize]``.  extract
removes it unless --raw is given.

CacheBlock (type 2: "ps", "ini", "mp_stuff" in out/patch, "vo_splines" in
out_<lang>).  The block name acts as a virtual directory: the engine looks a
file up in the block whose name equals the last directory component of the
requested path (e.g. pathPS "../PS" -> block "ps"), before the archive
directory and before loose files.
    u32 n_groups; u32 n_files; u32 data_size
    n_groups x { u32 group_csize; u32 group_nfiles;
                 group_nfiles x { u64 key; u32 usize } }
    then n_groups zlib streams of group_csize bytes each; group g inflates
    to the concatenation of its files.  Keys are sorted ascending.
    key = CRC-64 (ISO polynomial 0x1B, reflected form 0xD800000000000000,
    init 0, no final xor) of the lower-cased bare file name, for example
    "rebel_1_1.cls", "lght_search2.sfx", "materials.ps", "<texture>.td".
File names are not stored.  extract labels members from --names FILE and by
hashing words found in the block and the archive directory with the known
extensions (.cls .sfx .ui .td .ps .ini .spl .str .clsdb); the rest are
written as <key as 16 hex digits>.
"""
import argparse
import csv
import os
import re
import struct
import sys
import zlib

# type id -> (engine category name, extension added when the name has none).
# Names 0-16 come from the executable's table; 17-24 from the preload lists.
TYPE_INFO = {
    0: ("Preload", ".txt"),
    1: ("SceneData", ""),
    2: ("CacheBlock", ""),
    3: ("Shader", ""),
    4: ("ShaderCache", ""),
    5: ("ShaderPairs", ".pairs"),
    6: ("Textures", ".tex"),
    7: ("Cubemaps", ".tex"),
    8: ("SoundData", ""),
    9: ("Sounds", ""),
    10: ("WaveBanks_mem", ".fsb"),
    11: ("WaveBanks_strm_file", ".fsb"),
    12: ("Templates", ".tpl"),
    13: ("VoiceSplines", ""),
    14: ("Strings", ".txt"),
    15: ("Ragdolls", ""),
    16: ("Scene", ""),
    17: ("SceneGrs", ""),
    18: ("SceneScr", ""),
    19: ("SceneTrc", ""),
    20: ("SceneRain", ""),
    21: ("SceneCDT", ""),
    22: ("SceneSM", ""),
    23: ("SceneSLO", ""),
    24: ("SceneVis", ""),
}
PACKED_TYPES = {3, 15, 18}
CHUNK = 1 << 20


class Entry:
    __slots__ = ("index", "name", "type", "offset", "crc", "usize", "csize")

    def __init__(self, index, name, type_, offset, crc, usize, csize):
        self.index, self.name, self.type = index, name, type_
        self.offset, self.crc, self.usize, self.csize = offset, crc, usize, csize

    @property
    def local(self):
        return self.offset != -1


def _read_record(f):
    usize, csize = struct.unpack("<II", f.read(8))
    data = zlib.decompress(f.read(csize))
    if len(data) != usize:
        raise ValueError("record size mismatch")
    return data


class Archive:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        ver = _read_record(self.f)
        self.version = struct.unpack("<I", ver[:4])[0]
        base = _read_record(self.f)
        n = struct.unpack("<I", base[:4])[0]
        self.base_name = base[4:4 + n].decode("latin1")
        self.dir_offset = struct.unpack("<I", self.f.read(4))[0]
        self.data_offset = self.f.tell()
        self.f.seek(self.dir_offset)
        d = _read_record(self.f)
        count = struct.unpack_from("<I", d, 0)[0]
        o = 4
        self.entries = []
        for i in range(count):
            ln = struct.unpack_from("<I", d, o)[0]
            o += 4
            name = d[o:o + ln].decode("latin1")
            o += ln
            t, off, crc, us, cs = struct.unpack_from("<IiIII", d, o)
            o += 20
            self.entries.append(Entry(i, name, t, off, crc, us, cs))
        self._base = None

    @property
    def base(self):
        if self._base is None and self.base_name:
            self._base = Archive(os.path.join(os.path.dirname(self.path), self.base_name))
        return self._base

    def source(self, e):
        """Return (archive, entry) holding the data for entry e."""
        if e.local:
            return self, e
        if not self.base:
            raise ValueError("entry %s lives in a base archive" % e.name)
        return self.base.source(self.base.entries[e.crc])

    def stream(self, e):
        """Yield inflated chunks of entry e (must be local), checking size and crc."""
        self.f.seek(e.offset)
        z = zlib.decompressobj()
        left, crc, total = e.csize, 0, 0
        while left:
            raw = self.f.read(min(CHUNK, left))
            if not raw:
                raise ValueError("truncated entry %s" % e.name)
            left -= len(raw)
            out = z.decompress(raw)
            if out:
                crc = zlib.crc32(out, crc)
                total += len(out)
                yield out
        out = z.flush()
        if out:
            crc = zlib.crc32(out, crc)
            total += len(out)
            yield out
        if total != e.usize or crc != e.crc:
            raise ValueError("bad size/crc for %s" % e.name)

    def read(self, e):
        a, e = self.source(e)
        return b"".join(a.stream(e))


def _crc64_table():
    table = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0xD800000000000000 if c & 1 else c >> 1
        table.append(c)
    return table


CRC64_TABLE = _crc64_table()


def s3d_hash64(name, h=0):
    """Cache block member key: CRC-64 (ISO polynomial, reflected, init 0,
    no final xor) of the lower-cased file name, e.g. "rebel_1_1.cls"."""
    t = CRC64_TABLE
    for c in name.lower().encode("latin1"):
        h = (h >> 8) ^ t[(h ^ c) & 0xFF]
    return h


def parse_block(data):
    """Parse a CacheBlock; yield (hash, bytes) for every member."""
    ng, nf, total = struct.unpack_from("<III", data, 0)
    o = 12
    groups = []
    for _ in range(ng):
        cs, n = struct.unpack_from("<II", data, o)
        o += 8
        files = []
        for _ in range(n):
            h, us = struct.unpack_from("<QI", data, o)
            o += 12
            files.append((h, us))
        groups.append((cs, files))
    if o + total != len(data):
        raise ValueError("bad cache block layout")
    for cs, files in groups:
        blob = zlib.decompress(data[o:o + cs])
        o += cs
        p = 0
        for h, us in files:
            yield h, blob[p:p + us]
            p += us
        if p != len(blob):
            raise ValueError("cache block group size mismatch")


def unpack(e, data):
    """Strip the format-specific wrappers (length prefix, inner zlib)."""
    if e.type == 0 and len(data) >= 4 and struct.unpack_from("<I", data)[0] == len(data) - 4:
        return data[4:]
    if e.type in PACKED_TYPES and len(data) >= 10:
        us, cs = struct.unpack_from("<II", data)
        if data[8:10] == b"\0\0" and cs + 10 == len(data):
            out = zlib.decompress(data[10:])
            if len(out) == us:
                return out
    return data


def out_path(outdir, e):
    folder, ext = TYPE_INFO.get(e.type, ("type%02d" % e.type, ""))
    name = e.name.replace("\\", "/").lstrip("/")
    if "." not in os.path.basename(name):
        name += ext
    return os.path.join(outdir, folder, *name.split("/"))


def safe_join(root, path):
    full = os.path.abspath(path)
    if not full.startswith(os.path.abspath(root) + os.sep):
        raise ValueError("unsafe path %r" % path)
    return full


GUESS_EXTS = ("", ".cls", ".sfx", ".ui", ".td", ".ps", ".ini", ".spl", ".str", ".clsdb")
TOKEN_RE = re.compile(rb"[A-Za-z0-9_.\-]{2,80}")


def load_names(paths):
    names = {}
    for path in paths:
        if path and os.path.isfile(path):
            with open(path, encoding="latin1") as f:
                for line in f:
                    n = line.strip()
                    if n:
                        names.setdefault(s3d_hash64(n), n.lower())
    return names


def match_tokens(tokens, want):
    """Hash every token with every known extension; return {hash: name} for
    the hashes in want."""
    found = {}
    for tok in tokens:
        tok = tok.lower()
        h0 = s3d_hash64(tok)
        for ext in GUESS_EXTS:
            h = s3d_hash64(ext, h0) if ext else h0
            if h in want and h not in found:
                found[h] = tok + ext
    return found


def guess_names(members, words, names):
    """Label members by hashing words (from the block itself and the archive
    directory) with the extensions the engine uses for cache block files."""
    want = {h for h, _ in members if h not in names}
    if not want:
        return
    tokens = set(words)
    for _, b in members:
        tokens.update(m.decode("latin1") for m in TOKEN_RE.findall(b))
    names.update(match_tokens(tokens, want))


def cmd_names(args):
    """Harvest candidate names from the given archives (directory names,
    preload lists, scene text, cache block contents) and write every name
    that matches a cache block member, one per line."""
    want, tokens = set(), set()
    for path in args.archives:
        try:
            a = Archive(path)
        except (ValueError, struct.error, zlib.error):
            # not an archive (e.g. the game executable): harvest its strings
            with open(path, "rb") as f:
                tail = b""
                for chunk in iter(lambda: f.read(CHUNK), b""):
                    buf = tail + chunk
                    tokens.update(m.decode("latin1") for m in TOKEN_RE.findall(buf))
                    tail = buf[-80:]
            continue
        tokens.update(archive_words(a))
        for e in a.entries:
            if not e.local:
                continue
            if e.type == 2:
                for h, b in parse_block(a.read(e)):
                    want.add(h)
                    tokens.update(m.decode("latin1") for m in TOKEN_RE.findall(b))
            elif e.type in (0, 16):
                tail = b""
                for chunk in a.stream(e):
                    buf = tail + chunk
                    tokens.update(m.decode("latin1") for m in TOKEN_RE.findall(buf))
                    tail = buf[-80:]
    found = match_tokens(tokens, want)
    with open(args.output, "w", encoding="utf-8", newline="\n") as f:
        for n in sorted(found.values()):
            f.write(n + "\n")
    print("%d of %d cache block members named -> %s" % (len(found), len(want), args.output))


def block_members(a, e, names, words):
    """Return [(hash, name or None, bytes)] for cache block entry e."""
    members = list(parse_block(a.read(e)))
    guess_names(members, words, names)
    return [(h, names.get(h), b) for h, b in members]


def archive_words(a):
    words = [e.name for e in a.entries]
    if a.base:
        words += [e.name for e in a.base.entries]
    return words


def cmd_list(args):
    a = Archive(args.archive)
    names = load_names([args.names])
    words = archive_words(a) if args.blocks else []
    print("# %s: version %d, base %r, %d entries, %d stored here"
          % (os.path.basename(a.path), a.version, a.base_name or None,
             len(a.entries), sum(e.local for e in a.entries)))
    print("index\ttype\tusize\tcsize\twhere\tname")
    for e in a.entries:
        where = "%#x" % e.offset if e.local else "base[%d]" % e.crc
        tname = TYPE_INFO.get(e.type, (str(e.type),))[0]
        print("%d\t%s\t%d\t%d\t%s\t%s" % (e.index, tname, e.usize, e.csize, where, e.name))
        if args.blocks and e.type == 2:
            src, se = a.source(e)
            for h, n, b in block_members(src, se, names, words):
                print("\t\t%d\t\t%016x\t%s/%s" % (len(b), h, e.name, n or "?"))


def cmd_extract(args):
    a = Archive(args.archive)
    names = load_names([args.names])
    words = archive_words(a)
    flt = args.filter.lower() if args.filter else None
    root = os.path.abspath(args.outdir)
    os.makedirs(root, exist_ok=True)
    n_files = n_bytes = 0
    rows = []
    for e in a.entries:
        if not e.local and not args.resolve:
            continue
        src, se = a.source(e)
        if e.type == 2 and not args.raw and not args.no_blocks:
            bdir = os.path.join(root, "CacheBlock", e.name)
            for h, n, b in block_members(src, se, names, words):
                fn = n or "%016x" % h
                if flt and flt not in e.name.lower() + "/" + fn:
                    continue
                mp = safe_join(root, os.path.join(bdir, fn))
                os.makedirs(os.path.dirname(mp), exist_ok=True)
                with open(mp, "wb") as o:
                    o.write(b)
                rows.append((os.path.relpath(mp, root), e.type, "%016x" % h, len(b)))
                n_files += 1
                n_bytes += len(b)
            continue
        if flt and flt not in e.name.lower():
            continue
        path = safe_join(root, out_path(root, e))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        size = 0
        with open(path, "wb") as o:
            if not args.raw and (e.type in PACKED_TYPES or e.type == 0):
                data = unpack(e, b"".join(src.stream(se)))
                o.write(data)
                size = len(data)
            else:
                for chunk in src.stream(se):
                    o.write(chunk)
                    size += len(chunk)
        rows.append((os.path.relpath(path, root), e.type, "", size))
        n_files += 1
        n_bytes += size
    with open(os.path.join(root, "_index.csv"), "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["path", "type", "hash", "size"])
        w.writerows(rows)
    print("%s: %d files, %d bytes -> %s" % (os.path.basename(a.path), n_files, n_bytes, root))


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    pl = sub.add_parser("list", help="list the directory")
    pl.add_argument("archive")
    pl.add_argument("--blocks", action="store_true", help="also list cache block members")
    pl.add_argument("--names", help="file with candidate names for cache block members")
    pe = sub.add_parser("extract", help="extract entries")
    pe.add_argument("archive")
    pe.add_argument("outdir")
    pe.add_argument("--filter", help="only entries whose name contains this (case-insensitive)")
    pe.add_argument("--raw", action="store_true", help="write stored data without unpacking")
    pe.add_argument("--resolve", action="store_true",
                    help="patch archives: also extract unchanged entries from the base archive")
    pe.add_argument("--no-blocks", action="store_true", help="write cache blocks as one file")
    pe.add_argument("--names", help="file with candidate names for cache block members")
    pn = sub.add_parser("names", help="harvest cache block member names from archives")
    pn.add_argument("archives", nargs="+")
    pn.add_argument("-o", "--output", required=True, help="names file to write")
    args = p.parse_args(argv)
    try:
        {"list": cmd_list, "extract": cmd_extract, "names": cmd_names}[args.cmd](args)
    except OSError as exc:  # output pipe closed early (e.g. "| head")
        if args.cmd != "list" or exc.errno not in (22, 32):
            raise
        os.dup2(os.open(os.devnull, os.O_WRONLY), sys.stdout.fileno())


if __name__ == "__main__":
    main()
