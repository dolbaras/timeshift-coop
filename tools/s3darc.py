#!/usr/bin/env python3
"""s3darc.py - read and write TimeShift (Saber3D engine, 2007) .s3darc archives.

Usage (Python 3, standard library only):
    python -I s3darc.py list    <archive> [--blocks] [--names FILE]
    python -I s3darc.py extract <archive> <outdir> [--filter SUBSTR] [--raw]
                                [--resolve] [--no-blocks] [--names FILE]
    python -I s3darc.py names   <archive> [<archive> ...] -o FILE
    python -I s3darc.py pack-patch <base archive> [<moddir>] -o <patch archive> [--raw]
    python -I s3darc.py verify  <archive> <other archive> [--blocks]
    python -I s3darc.py lg-ssl  list|extract|replace|check <file.lg> [...]
    (list, extract, pack-patch and verify take --search DIR to find base archives
    that are not next to the archive)

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
    pack-patch  writes a patch archive over <base archive> (any archive,
             including a patch: patches chain).  <moddir> is laid out like
             extract's output: <Type>/<name> replaces or adds a directory entry,
             CacheBlock/<block>/<file> replaces or adds a block member (file
             name = member name, or its 16-hex-digit key).  Files equal to the
             base are left as references.  Without <moddir>: a directory-only
             patch (every entry a reference).
    verify   compares the resolved content (size + CRC) of every entry of two
             archives; exit status 1 if they differ.
    lg-ssl   object texts (properties and "#ssl" scripts) in an extracted .lg:
             list [--all] | extract <dir> [--all] | check (parse + rebuild) |
             replace <index|object name> --text FILE -o OUT [--grow].

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
    def __init__(self, path, search=()):
        self.path = path
        self.search = list(search)
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
            for d in [os.path.dirname(self.path)] + self.search:
                cand = os.path.join(d, self.base_name)
                if os.path.isfile(cand):
                    self._base = Archive(cand, self.search)
                    break
            else:
                raise ValueError("base archive %s not found" % self.base_name)
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
    a = Archive(args.archive, args.search)
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
    a = Archive(args.archive, args.search)
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


# ---------------------------------------------------------------------------
# Writing patch archives
# ---------------------------------------------------------------------------

ZLEVEL = 1            # the shipped archives use zlib level 1 everywhere
BLOCK_GROUP = 8192    # a cache block group is closed once it exceeds this size
FOLDER_TYPE = {v[0].lower(): k for k, v in TYPE_INFO.items()}
HEX16 = re.compile(r"^[0-9a-fA-F]{16}$")


def wrap(type_, data):
    """Inverse of unpack(): add the length prefix / inner zlib layer."""
    if type_ == 0:
        return struct.pack("<I", len(data)) + data
    if type_ in PACKED_TYPES:
        z = zlib.compress(data, ZLEVEL)
        return struct.pack("<II", len(data), len(z)) + b"\0\0" + z
    return data


def build_block(members):
    """Build a CacheBlock from {key: bytes} the way the shipped tools do:
    keys ascending, a group is closed once its size exceeds 8192 bytes,
    every group deflated at level 1."""
    groups, cur, size = [], [], 0
    for key in sorted(members):
        cur.append(key)
        size += len(members[key])
        if size > BLOCK_GROUP:
            groups.append(cur)
            cur, size = [], 0
    if cur:
        groups.append(cur)
    head, blobs = [], []
    for g in groups:
        z = zlib.compress(b"".join(members[k] for k in g), ZLEVEL)
        blobs.append(z)
        head.append(struct.pack("<II", len(z), len(g)))
        head.extend(struct.pack("<QI", k, len(members[k])) for k in g)
    total = sum(len(z) for z in blobs)
    return (struct.pack("<III", len(groups), len(members), total)
            + b"".join(head) + b"".join(blobs))


def _record(data):
    z = zlib.compress(data, ZLEVEL)
    return struct.pack("<II", len(data), len(z)) + z


def scan_moddir(moddir, raw=False):
    """Read a mod folder laid out like extract's output.
    Returns ({(type, file_name): path}, {block_name: {key: path}})."""
    files, blocks = {}, {}
    for folder in sorted(os.listdir(moddir)):
        fdir = os.path.join(moddir, folder)
        if not os.path.isdir(fdir):
            continue
        low = folder.lower()
        if low == "cacheblock":
            for block in sorted(os.listdir(fdir)):
                bdir = os.path.join(fdir, block)
                if not os.path.isdir(bdir):
                    continue
                mem = blocks.setdefault(block.lower(), {})
                for fn in sorted(os.listdir(bdir)):
                    key = int(fn, 16) if HEX16.match(fn) else s3d_hash64(fn)
                    mem[key] = os.path.join(bdir, fn)
            continue
        if low in FOLDER_TYPE:
            t = FOLDER_TYPE[low]
        elif re.match(r"^type\d+$", low):
            t = int(low[4:])
        else:
            raise ValueError("unknown type folder %r" % folder)
        for fn in sorted(os.listdir(fdir)):
            path = os.path.join(fdir, fn)
            if os.path.isfile(path):
                files[(t, fn)] = path
    return files, blocks


def _resolve_name(t, fn, by_key):
    """Map an extracted file name back to the directory name."""
    if (t, fn.lower()) in by_key:
        return by_key[(t, fn.lower())]
    ext = TYPE_INFO.get(t, ("", ""))[1]
    if ext and fn.lower().endswith(ext):
        stem = fn[:-len(ext)]
        if (t, stem.lower()) in by_key:
            return by_key[(t, stem.lower())]
        return stem
    return fn


def pack_patch(base_path, moddir, out_path, search=(), raw=False, log=print):
    """Write a patch archive over base_path containing the changes in moddir.
    Unchanged entries become references (offset -1) into the base."""
    base = Archive(base_path, search)
    by_key = {(e.type, e.name.lower()): e.name for e in base.entries}
    base_idx = {(e.type, e.name.lower()): e for e in base.entries}
    files, blocks = scan_moddir(moddir) if moddir else ({}, {})

    # local content: (type, name) -> bytes producer
    local = {}
    for (t, fn), path in files.items():
        name = _resolve_name(t, fn, by_key)
        with open(path, "rb") as f:
            data = f.read()
        if not raw:
            data = wrap(t, data)
        be = base_idx.get((t, name.lower()))
        if be is not None:
            src, se = base.source(be)
            if se.usize == len(data) and se.crc == zlib.crc32(data):
                continue                       # identical to the base: keep the reference
        local[(t, name.lower())] = (name, data)
    for bname, mem in blocks.items():
        be = base_idx.get((2, bname))
        old = dict(parse_block(base.read(be))) if be is not None else {}
        new = dict(old)
        for key, path in mem.items():
            with open(path, "rb") as f:
                new[key] = f.read()
        if new == old:
            continue
        changed = sum(1 for k in new if old.get(k) != new[k])
        log("block %s: %d members changed or added" % (bname, changed))
        local[(2, bname)] = (be.name if be is not None else bname, build_block(new))

    tmp = out_path + ".tmp"
    n_local = 0
    with open(tmp, "wb") as out:
        out.write(_record(struct.pack("<I", 1)))
        bn = os.path.basename(base_path).encode("latin1")
        out.write(_record(struct.pack("<I", len(bn)) + bn))
        out.write(b"\0\0\0\0")                 # dir_offset, patched below
        dir_rows = []
        order = [(e.type, e.name.lower()) for e in base.entries]
        order += sorted(k for k in local if k not in base_idx)
        for i, key in enumerate(order):
            if key in local:
                name, data = local[key]
                z = zlib.compress(data, ZLEVEL)
                off = out.tell()
                out.write(z)
                dir_rows.append((name, key[0], off, zlib.crc32(data), len(data), len(z)))
                n_local += 1
            else:
                e = base_idx[key]
                src, se = base.source(e)
                dir_rows.append((e.name, e.type, -1, e.index, se.usize, se.csize))
        dir_off = out.tell()
        d = [struct.pack("<I", len(dir_rows))]
        for name, t, off, crc, us, cs in dir_rows:
            nb = name.encode("latin1")
            d.append(struct.pack("<I", len(nb)) + nb + struct.pack("<IiIII", t, off, crc, us, cs))
        out.write(_record(b"".join(d)))
        hdr_end = len(_record(struct.pack("<I", 1))) + len(_record(struct.pack("<I", len(bn)) + bn))
        out.seek(hdr_end)
        out.write(struct.pack("<I", dir_off))
    os.replace(tmp, out_path)
    log("%s: %d entries, %d stored, base %s" % (out_path, len(dir_rows), n_local,
                                                 os.path.basename(base_path)))
    return n_local


def cmd_pack_patch(args):
    pack_patch(args.base, args.moddir, args.output, args.search, args.raw)


def cmd_verify(args):
    """Compare the resolved content of two archives entry by entry."""
    a = Archive(args.archive, args.search)
    b = Archive(args.other, args.search)
    ka = {(e.type, e.name.lower()): e for e in a.entries}
    kb = {(e.type, e.name.lower()): e for e in b.entries}
    only_a, only_b = sorted(set(ka) - set(kb)), sorted(set(kb) - set(ka))
    diff = []
    for k in sorted(set(ka) & set(kb)):
        sa, ea = a.source(ka[k])
        sb, eb = b.source(kb[k])
        if (ea.usize, ea.crc) != (eb.usize, eb.crc):
            diff.append(k)
    print("entries: %d vs %d; only in first: %d; only in second: %d; different content: %d"
          % (len(ka), len(kb), len(only_a), len(only_b), len(diff)))
    for k in (only_a + only_b + diff)[:40]:
        print("  ", TYPE_INFO.get(k[0], (k[0],))[0], k[1])
    if args.blocks:
        for k in diff:
            if k[0] != 2:
                continue
            ma = dict(parse_block(a.read(ka[k])))
            mb = dict(parse_block(b.read(kb[k])))
            d2 = [h for h in set(ma) | set(mb) if ma.get(h) != mb.get(h)]
            print("   block %s: %d vs %d members, %d differ" % (k[1], len(ma), len(mb), len(d2)))
    return not (only_a or only_b or diff)


# ---------------------------------------------------------------------------
# .lg scene files: chunk tree and object text (properties + #ssl scripts)
# ---------------------------------------------------------------------------
#
# A .lg file is a tree of chunks "u16 tag, u32 end" where end is the ABSOLUTE
# file offset just past the chunk.  A container chunk has a tag-specific
# header of p bytes after the 6-byte chunk header, then child chunks running
# exactly to its end.  Level objects are tag 0xF0 chunks; their name is in a
# 0xF4 child (NUL-terminated), their property text, including any "#ssl"
# script section, in a 0x1BA chunk (text + NUL) inside a 0xFD child.

LG_TEXT, LG_OBJECT, LG_NAME = 0x1BA, 0xF0, 0xF4
LG_MAXP = 512


class LgNode:
    __slots__ = ("tag", "start", "end", "prefix", "children")

    def __init__(self, tag, start, end):
        self.tag, self.start, self.end = tag, start, end
        self.prefix, self.children = 0, None


def _lg_seq(d, o, end):
    out = []
    while o < end:
        if o + 6 > end:
            return None
        tag, e = struct.unpack_from("<HI", d, o)
        if tag == 0 or tag > 0x7FF or e < o + 6 or e > end:
            return None
        out.append(LgNode(tag, o, e))
        o = e
    return out


def lg_parse(d):
    """Parse the whole file into a list of top-level LgNode trees."""
    top = _lg_seq(d, 0, len(d))
    if top is None:
        raise ValueError("not a chunk file")
    stack = list(top)
    while stack:
        n = stack.pop()
        if n.tag == LG_TEXT:
            continue                            # text is always a leaf
        pl = n.end - n.start - 6
        for p in range(0, min(pl - 6, LG_MAXP) + 1):
            ch = _lg_seq(d, n.start + 6 + p, n.end)
            if ch:
                n.prefix, n.children = p, ch
                stack.extend(ch)
                break
    return top


def lg_serialize(d, nodes, out):
    for n in nodes:
        if n.children is None:
            out.append(d[n.start:n.end])
        else:
            out.append(d[n.start:n.start + 6 + n.prefix])
            lg_serialize(d, n.children, out)


def lg_texts(d, top):
    """Yield (index, text node, object name) for every text chunk."""
    res = []

    def walk(nodes, owner):
        for n in nodes:
            name = owner
            if n.tag == LG_OBJECT and n.children:
                for c in n.children:
                    if c.tag == LG_NAME:
                        raw = d[c.start + 6:c.end]
                        name = raw.split(b"\0")[0].decode("latin1")
                        break
            if n.tag == LG_TEXT:
                res.append((n, owner))
            elif n.children:
                walk(n.children, name)
    walk(top, None)
    return [(i, n, nm) for i, (n, nm) in enumerate(res)]


def _lg_select(d, top, which):
    texts = lg_texts(d, top)
    if which.isdigit():
        hit = [t for t in texts if t[0] == int(which)]
    else:
        hit = [t for t in texts if t[2] == which and b"#ssl" in d[t[1].start:t[1].end]]
        if not hit:
            hit = [t for t in texts if t[2] == which]
    if len(hit) != 1:
        raise ValueError("%d text chunks match %r; use the index from 'lg-ssl list'"
                         % (len(hit), which))
    return hit[0]


def lg_replace(d, top, node, new_text, grow=False):
    """Return new file bytes with node's text replaced.  A text that fits is
    padded with spaces so that no offset in the file changes; a longer text
    needs grow=True, which shifts every chunk end behind the insertion."""
    if b"\0" in new_text:
        raise ValueError("text must not contain NUL bytes")
    old_len = node.end - node.start - 7
    if len(new_text) <= old_len:
        body = new_text + b" " * (old_len - len(new_text))
        return d[:node.start + 6] + body + d[node.end - 1:]
    if not grow:
        raise ValueError("new text is %d bytes longer than the old one; shorten it "
                         "or use --grow" % (len(new_text) - old_len))
    delta = len(new_text) - old_len
    ins = node.end - 1                          # position of the NUL terminator
    out = bytearray(d[:node.start + 6] + new_text + d[ins:])
    stack = list(top)
    while stack:
        n = stack.pop()
        if n.end > ins:
            pos = n.start if n.start < ins else n.start + delta
            struct.pack_into("<I", out, pos + 2, n.end + delta)
        if n.children:
            stack.extend(n.children)
    out = bytes(out)
    # the shifted file must parse into the same tree shape
    if _lg_shape(out, lg_parse(out)) != _lg_shape(d, top):
        raise ValueError("chunk tree changed shape after the shift; not written")
    return out


def _lg_shape(d, top):
    shape, stack = [], list(top)
    while stack:
        n = stack.pop()
        shape.append((n.tag, n.prefix, len(n.children or ())))
        if n.children:
            stack.extend(n.children)
    return shape


def cmd_lg_ssl(args):
    with open(args.lg, "rb") as f:
        d = f.read()
    top = lg_parse(d)
    if args.action == "check":
        out = []
        lg_serialize(d, top, out)
        same = b"".join(out) == d
        texts = lg_texts(d, top)
        print("%s: %d bytes, %d text chunks (%d with #ssl), rebuild %s"
              % (os.path.basename(args.lg), len(d), len(texts),
                 sum(b"#ssl" in d[n.start:n.end] for _, n, _ in texts),
                 "byte-identical" if same else "DIFFERENT"))
        return same
    if args.action == "list":
        for i, n, nm in lg_texts(d, top):
            text = d[n.start + 6:n.end - 1]
            if not args.all and b"#ssl" not in text:
                continue
            ssl = text.split(b"#ssl", 1)[1] if b"#ssl" in text else b""
            print("%d\t%#x\t%d\t%d\t%s" % (i, n.start, len(text), len(ssl), nm or "?"))
        return True
    if args.action == "extract":
        os.makedirs(args.target, exist_ok=True)
        n_out = 0
        for i, n, nm in lg_texts(d, top):
            text = d[n.start + 6:n.end - 1]
            if not args.all and b"#ssl" not in text:
                continue
            safe = re.sub(r"[^A-Za-z0-9_.\-]", "_", nm or "unnamed")
            with open(os.path.join(args.target, "%05d_%s.txt" % (i, safe)), "wb") as f:
                f.write(text)
            n_out += 1
        print("%d texts -> %s" % (n_out, args.target))
        return True
    if args.action == "replace":
        if not args.text or not args.output:
            raise SystemExit("replace needs --text FILE and -o OUTPUT")
        i, node, nm = _lg_select(d, top, args.target)
        with open(args.text, "rb") as f:
            new = f.read()
        res = lg_replace(d, top, node, new, args.grow)
        with open(args.output, "wb") as f:
            f.write(res)
        print("text %d (%s): %d -> %d bytes, file %d -> %d bytes -> %s"
              % (i, nm or "?", node.end - node.start - 7, len(new), len(d), len(res), args.output))
        return True


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
    pp = sub.add_parser("pack-patch", help="build a patch archive over a base archive")
    pp.add_argument("base", help="archive the patch applies to (e.g. patch.s3darc)")
    pp.add_argument("moddir", nargs="?", help="folder laid out like extract's output")
    pp.add_argument("-o", "--output", required=True, help="patch archive to write")
    pp.add_argument("--raw", action="store_true", help="files are stored bytes (no wrapping)")
    pv = sub.add_parser("verify", help="compare the resolved content of two archives")
    pv.add_argument("archive")
    pv.add_argument("other")
    pv.add_argument("--blocks", action="store_true", help="also compare cache block members")
    pg = sub.add_parser("lg-ssl", help="object texts / #ssl scripts in a .lg scene file")
    pg.add_argument("action", choices=["list", "extract", "replace", "check"])
    pg.add_argument("lg", help=".lg file (extracted)")
    pg.add_argument("target", nargs="?", help="extract: output folder; replace: index or object name")
    pg.add_argument("--text", help="replace: file with the new text")
    pg.add_argument("-o", "--output", help="replace: .lg file to write")
    pg.add_argument("--grow", action="store_true",
                    help="replace: allow a longer text (shifts chunk offsets; verify in game)")
    pg.add_argument("--all", action="store_true", help="list/extract texts without #ssl too")
    for sp in (pl, pe, pp, pv):
        sp.add_argument("--search", action="append", default=[],
                        help="extra folder to look for base archives in")
    args = p.parse_args(argv)
    try:
        ok = {"list": cmd_list, "extract": cmd_extract, "names": cmd_names,
              "pack-patch": cmd_pack_patch, "verify": cmd_verify,
              "lg-ssl": cmd_lg_ssl}[args.cmd](args)
        if ok is False:
            sys.exit(1)
    except ValueError as exc:
        sys.exit("error: %s" % exc)
    except OSError as exc:  # output pipe closed early (e.g. "| head")
        if args.cmd != "list" or exc.errno not in (22, 32):
            raise
        os.dup2(os.open(os.devnull, os.O_WRONLY), sys.stdout.fileno())


if __name__ == "__main__":
    main()
