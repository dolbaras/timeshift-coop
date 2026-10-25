#!/usr/bin/env python3
"""build_pak.py - build the co-op mod's data archives from the player's own TimeShift files.

    python -I build_pak.py <game folder> -o <out dir> [--recipe FILE ...] [--only-extra]
                           [--keep-work DIR] [--list]

The repository holds no game files. The mod's data changes are *recipes* (mod/data/*.py): anchored
find/replace edits of members of the player's own archives, plus wholly new files written by the mod.
This tool

1. opens <game>/preload/paks/patch.s3darc (and through it out.s3darc) with s3darc.py,
2. reads the members the recipes name (ps block members by file name or 16-hex key, string tables),
3. applies the recipes in order; every anchor must match exactly once, otherwise the build stops with
   the member and the anchor (another game version or another mod changed that text),
4. writes <out>/coop.s3darc (a patch over patch.s3darc with the changed and new members), the
   directory-only <out>/coop_ati.s3darc and <out>/coop_nv.s3darc (over patch_ati / patch_nv), and
   <out>/coop_build.txt (what was built from what),
5. reads coop.s3darc back and checks every changed member.

Nothing is written into the game folder. Installing = copying the three archives next to patch.s3darc
and setting [Preload] arcName = coop.s3darc in bin/steamwars.ini (CRLF kept), see mod/docs/building.md.

Recipes (default: mod/data RECIPES in order; --recipe adds more, e.g. the lab's test automation):
    a module with any of
      EDITS = [(member, [(anchor, replacement), ...]), ...]
      NEW_FILES = [(member, path relative to mod/data), ...]
      def build(ctx): ...                      (generated content, see Context below)
    member  = "CacheBlock/ps/<file name or 16-hex key>" or "Strings/<table>" (e.g. "Strings/strings_russian")
    anchor  = text of the member; runs of spaces/tabs match any run of spaces/tabs, line ends match CRLF
              or LF, trailing blanks are ignored. Must occur exactly once.
    replacement is inserted as written ("\\n" becomes CRLF in ps members).
"""
import argparse
import hashlib
import importlib.util
import os
import re
import shutil
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
MOD = os.path.dirname(HERE)
DATA = os.path.join(MOD, "data")
sys.path.insert(0, HERE)
import s3darc  # noqa: E402  (same folder)

CRLF = "\r\n"


class RecipeError(Exception):
    pass


def mod_version():
    """TS_VERSION from native/ts.h: the DLL and the data carry the same version.

    An installed copy (<game>/coop/tools, coop/data) has no native/: there it is coop/version.txt, which
    tools/package.ps1 writes from ts.h (the one source of the version)."""
    h = os.path.join(MOD, "native", "ts.h")
    if os.path.isfile(h):
        with open(h, encoding="latin-1") as f:
            m = re.search(r'#define\s+TS_VERSION\s+"([^"]+)"', f.read())
        if not m:
            raise RecipeError("TS_VERSION not found in native/ts.h")
        return m.group(1)
    v = os.path.join(MOD, "version.txt")
    if os.path.isfile(v):
        with open(v, encoding="ascii") as f:
            s = f.readline().strip()
        if s:
            return s
    raise RecipeError("no mod version: neither native/ts.h nor version.txt next to tools/")


def anchor_regex(anchor):
    """A regex for an anchor that ignores blank-run widths, trailing blanks and CR/LF differences."""
    lines = anchor.replace(CRLF, "\n").split("\n")
    ends_line = len(lines) > 1 and lines[-1] == ""   # (an anchor ending in a line end)
    if ends_line:
        lines.pop()
    parts = []
    for line in lines:
        body = line.rstrip(" \t")
        if not body.strip():
            parts.append(r"[ \t]*")
            continue
        lead = r"[ \t]*" if body[0] in " \t" else ""
        parts.append(lead + r"[ \t]+".join(re.escape(t) for t in body.split()))
    sep = r"[ \t]*\r?\n"
    return re.compile(sep.join(parts) + (sep if ends_line else ""))


class Member:
    """One member: decoded text, its codec and line ending, original bytes."""

    def __init__(self, path, raw, kind):
        self.path, self.orig, self.kind = path, raw, kind
        if kind == "strings":
            if not raw.startswith(b"\xff\xfe"):
                raise RecipeError("%s: not a UTF-16LE string table" % path)
            self.text = raw[2:].decode("utf-16-le")
        else:
            self.text = raw.decode("latin-1")
        self.created = False

    def encode(self):
        if self.kind == "strings":
            return b"\xff\xfe" + self.text.encode("utf-16-le")
        return self.text.encode("latin-1")


class Context:
    """What a recipe's build(ctx) gets.

    ctx.text(member)            current text of a member (after the earlier recipes)
    ctx.edit(member, edits)     [(anchor, replacement), ...], each anchor exactly once
    ctx.add(member, text)       a new member (text with \\n line ends; written with CRLF)
    ctx.data_file(rel)          text of a file in mod/data (UTF-8)
    ctx.string_tables()         ["strings_english", ...] present in the player's archives
    ctx.version                 the mod version (TS_VERSION)
    ctx.log(msg)
    """

    def __init__(self, game, archive, version, log):
        self.game, self.archive, self.version, self.log = game, archive, version, log
        self.members = {}
        self._ps = None
        self._by_key = {(e.type, e.name.lower()): e for e in archive.entries}

    # ---- reading the player's archives ----
    def _ps_block(self):
        if self._ps is None:
            e = self._by_key.get((2, "ps"))
            if e is None:
                raise RecipeError("no ps cache block in %s" % self.archive.path)
            self._ps = dict(s3darc.parse_block(self.archive.read(e)))
        return self._ps

    @staticmethod
    def _split(member):
        folder, _, name = member.partition("/")
        if folder == "CacheBlock":
            block, _, name = name.partition("/")
            if block != "ps" or not name:
                raise RecipeError("%s: only CacheBlock/ps/<name> members are supported" % member)
            return "ps", name
        if folder == "Strings" and name:
            return "strings", name
        raise RecipeError("%s: unknown member path" % member)

    def _load(self, member):
        if member in self.members:
            return self.members[member]
        kind, name = self._split(member)
        if kind == "ps":
            key = int(name, 16) if s3darc.HEX16.match(name) else s3darc.s3d_hash64(name)
            raw = self._ps_block().get(key)
            if raw is None:
                raise RecipeError("%s: not in the game's ps block (another game version?)" % member)
        else:
            e = self._by_key.get((14, name.lower()))
            if e is None:
                raise RecipeError("%s: no such string table in the game's archives" % member)
            raw = self.archive.read(e)
        m = Member(member, raw, kind)
        self.members[member] = m
        return m

    def text(self, member):
        return self._load(member).text

    def string_tables(self):
        return sorted(e.name for e in self.archive.entries if e.type == 14)

    def data_file(self, rel):
        with open(os.path.join(DATA, rel), encoding="utf-8") as f:
            return f.read()

    # ---- changing ----
    def edit(self, member, edits):
        m = self._load(member)
        crlf = m.kind == "ps"
        for anchor, repl in edits:
            rx = anchor_regex(anchor)
            hits = list(rx.finditer(m.text))
            if len(hits) != 1:
                first = anchor.strip().split("\n")[0][:70]
                raise RecipeError("%s: anchor found %d times (must be 1): %r" % (member, len(hits), first))
            new = repl.replace(CRLF, "\n")
            if crlf:
                new = new.replace("\n", CRLF)
            h = hits[0]
            m.text = m.text[:h.start()] + new + m.text[h.end():]

    def add(self, member, text):
        kind, name = self._split(member)
        if kind != "ps":
            raise RecipeError("%s: new members only in the ps block" % member)
        key = s3darc.s3d_hash64(name)
        if member in self.members or key in self._ps_block():
            raise RecipeError("%s: the game already has a member of that name" % member)
        if any(ord(c) > 0xff for c in text):
            raise RecipeError("%s: ps members are 8-bit text" % member)
        m = Member(member, b"", "ps")
        m.text = text.replace(CRLF, "\n").replace("\n", CRLF)
        m.created = True
        self.members[member] = m

    def set_text(self, member, text):
        self._load(member).text = text


def load_recipe(path):
    name = "coop_recipe_" + re.sub(r"\W", "_", os.path.splitext(os.path.basename(path))[0])
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def default_recipes():
    sys.path.insert(0, DATA)
    try:
        index = load_recipe(os.path.join(DATA, "__init__.py"))
    finally:
        sys.path.remove(DATA)
    return [os.path.join(DATA, n + ".py") for n in index.RECIPES]


def apply_recipe(ctx, path):
    r = load_recipe(path)
    label = os.path.basename(path)
    for member, edits in getattr(r, "EDITS", []):
        ctx.edit(member, edits)
    for member, rel in getattr(r, "NEW_FILES", []):
        ctx.add(member, ctx.data_file(rel))
    if hasattr(r, "build"):
        r.build(ctx)
    ctx.log("recipe %s applied" % label)


def check_ps_text(m):
    """Cheap structure checks of an edited ps text: script blocks still open and close."""
    t = m.text
    if t.count("<<<") != t.count(">>>"):
        raise RecipeError("%s: '<<<' and '>>>' do not pair up after the edits" % m.path)
    if m.path.endswith(".ui") or m.created:
        if "\r\n" in t and re.search(r"(?<!\r)\n", t):
            raise RecipeError("%s: mixed line ends" % m.path)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def build(game, out_dir, recipes, keep_work=None, log=print):
    paks = os.path.join(game, "preload", "paks")
    base = os.path.join(paks, "patch.s3darc")
    for n in ("patch.s3darc", "patch_ati.s3darc", "patch_nv.s3darc", "out.s3darc"):
        if not os.path.isfile(os.path.join(paks, n)):
            raise RecipeError("%s missing: not a TimeShift 1.2 game folder (%s)" % (n, paks))
    version = mod_version()
    archive = s3darc.Archive(base)
    ctx = Context(game, archive, version, log)
    for path in recipes:
        apply_recipe(ctx, path)

    changed = [m for m in ctx.members.values() if m.created or m.encode() != m.orig]
    for m in changed:
        if m.kind == "ps":
            check_ps_text(m)
    work = keep_work or tempfile.mkdtemp(prefix="ts_coop_pak_")
    if keep_work:
        shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work, exist_ok=True)
    try:
        for m in changed:
            kind, name = Context._split(m.path)
            sub = os.path.join(work, "CacheBlock", "ps") if kind == "ps" else os.path.join(work, "Strings")
            os.makedirs(sub, exist_ok=True)
            fn = name if kind == "ps" else name + ".txt"
            with open(os.path.join(sub, fn), "wb") as f:
                f.write(m.encode())
        os.makedirs(out_dir, exist_ok=True)
        coop = os.path.join(out_dir, "coop.s3darc")
        s3darc.pack_patch(base, work, coop, log=log)
        s3darc.pack_patch(os.path.join(paks, "patch_ati.s3darc"), None, os.path.join(out_dir, "coop_ati.s3darc"), log=log)
        s3darc.pack_patch(os.path.join(paks, "patch_nv.s3darc"), None, os.path.join(out_dir, "coop_nv.s3darc"), log=log)
    finally:
        if not keep_work:
            shutil.rmtree(work, ignore_errors=True)

    # read back: every changed member must come out of coop.s3darc as built
    back = s3darc.Archive(os.path.join(out_dir, "coop.s3darc"), [paks])
    keys = {(e.type, e.name.lower()): e for e in back.entries}
    ps = dict(s3darc.parse_block(back.read(keys[(2, "ps")])))
    for m in changed:
        kind, name = Context._split(m.path)
        if kind == "ps":
            key = int(name, 16) if s3darc.HEX16.match(name) else s3darc.s3d_hash64(name)
            got = ps.get(key)
        else:
            got = back.read(keys[(14, name.lower())])
        if got != m.encode():
            raise RecipeError("read-back of %s differs" % m.path)

    exe = os.path.join(game, "bin", "TimeShift.exe")
    lines = [
        "coop data build",
        "version = %s" % version,
        "base = patch.s3darc (sha256 %s)" % sha256(base),
        "exe sha256 = %s" % (sha256(exe) if os.path.isfile(exe) else "(not found)"),
        "recipes = %s" % ", ".join(os.path.basename(p) for p in recipes),
    ]
    lines += ["member %s %s %d bytes crc %08x" % ("new" if m.created else "changed", m.path, len(m.encode()),
                                                 zlib.crc32(m.encode())) for m in sorted(changed, key=lambda m: m.path)]
    with open(os.path.join(out_dir, "coop_build.txt"), "w", encoding="utf-8", newline="\r\n") as f:
        f.write("\n".join(lines) + "\n")
    log("%d members changed or added; version %s; read-back ok -> %s" % (len(changed), version, out_dir))
    return changed


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("game", help="TimeShift folder (the one with bin\\ and preload\\)")
    p.add_argument("-o", "--out", required=True, help="folder for coop.s3darc, coop_ati.s3darc, coop_nv.s3darc")
    p.add_argument("--recipe", action="append", default=[], help="an extra recipe file (applied after the mod's)")
    p.add_argument("--only-extra", action="store_true", help="only the --recipe files, not mod/data")
    p.add_argument("--keep-work", help="keep the edited members in this folder (s3darc.py extract layout)")
    p.add_argument("--list", action="store_true", help="print the recipe files and exit")
    a = p.parse_args(argv)
    recipes = ([] if a.only_extra else default_recipes()) + [os.path.abspath(r) for r in a.recipe]
    if a.list:
        print("\n".join(recipes))
        return
    if os.path.abspath(a.out).startswith(os.path.abspath(os.path.join(a.game, "preload")) + os.sep):
        sys.exit("error: write the archives outside the game folder, then install them")
    try:
        build(a.game, a.out, recipes, a.keep_work)
    except RecipeError as e:
        sys.exit("error: %s" % e)


if __name__ == "__main__":
    main()
