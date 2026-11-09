"""Offline check of every text the co-op mod shows (no game, no network, nothing is started).

The rule: every text the mod adds or changes is in the game's selected language. TimeShift ships nine string
tables (Strings/strings_<language>.txt; bin/steamwars.ini [Language] picks one) and the mod appends its texts to
each (data/coop_strings.py), so in the game the engine's own choice applies. The installer, the shortcut's launcher
and the setup exe have no game language yet: they pick Windows' language among the same nine (else English).

Checked here (FAIL = a gap):
- data/coop_strings.py: every entry in all nine languages, nothing empty, the same {STRING} / line breaks as the
  English text, no text equal to English unless SAME_OK_DATA allows it, only characters the game's font is known
  to have (FONT_GLYPHS), no key twice;
- the ids the recipes (data/*.py), the frames (data/ui/*.ui) and the DLL (native/*.c) show: a mod id (COOP_*,
  CTRL_PAD_*) must be in the table, a stock id (UI_*, GAME_*) is the game's own text, anything else is a
  hard-coded text (FAIL); the pad names the DLL gives the engine (native/gamepad.c) must have their CTRL_ text;
- tools/installer.ps1, tools/launcher.ps1, tools/mkinstaller/stub.c: the nine languages, every English key in
  each, the same {0} placeholders, nothing equal to English unless allowed, every L "key" the scripts use exists,
  no error or message box with a hard-coded text.
Notes (no FAIL): keys a language has beyond English, the known open items (KNOWN_OPEN), the readme languages.

Usage: python tests/test_texts.py [--strings <folder with the game's strings_<language>.txt>]
  --strings (a lab copy's extracted Strings folder): also checks that the stock ids the mod uses exist in every
  table, that no mod key is a stock key, and the font characters against those tables.
Hooked into native/tests/run.sh. Exit code 1 on any FAIL.
"""
import ast
import os
import re
import sys

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
from data import coop_strings  # noqa: E402

# the game's languages (TimeShift 1.2: nine string tables) and the same as Windows' two-letter codes
GAME_LANGS = ["english", "russian", "german", "french", "italian", "spanish", "polish", "czech", "hungarian"]
WIN_LANGS = ["en", "ru", "de", "fr", "es", "it", "pl", "cs", "hu"]

# texts equal to English on purpose: key -> the languages ("*" = all)
SAME_OK_DATA = {
    # the labels printed on the pad (Xbox layout), the same in every language like the keyboard's key names
    "CTRL_PAD_A": "*", "CTRL_PAD_B": "*", "CTRL_PAD_X": "*", "CTRL_PAD_Y": "*", "CTRL_PAD_LB": "*", "CTRL_PAD_RB": "*",
    "CTRL_PAD_LT": "*", "CTRL_PAD_RT": "*", "CTRL_PAD_BACK": "*", "CTRL_PAD_START": "*",
}
SAME_OK_INSTALLER = {
    "browse_zip": "*",            # "Zip..." (a file type)
    "ini_set": "*", "ini_back": "*",   # "bin\steamwars.ini: arcName = ..." (a file name and its key)
    "error": {"es"},              # "Error: " is Spanish too
    "browse_dir": {"pl"},         # "Folder..." is Polish too
}
SAME_OK_LAUNCHER = {"title": "*"}   # "TimeShift Co-op" (the mod's name)
# installer errors that never reach the window's message: caught and written to the log only
INSTALLER_LOG_ONLY = {"no icon in $exe"}   # (Make-Shortcuts: "coop.ico: ..." in the log, the shortcut gets the exe's icon)

# Characters beyond ASCII the game's font is known to have: the font (font_main / font_regular, one for every
# language) draws what the stock tables use, listed here, plus U+0401 (Cyrillic capital Io: the font has a page
# starting at it). Anything else (the guillemets, dashes, ellipsis) is not proven to draw.
FONT_GLYPHS = set(
    "\xa0\xa1\xa7\xa9\xaa\xae\xbf\xc0\xc1\xc4\xc8\xc9\xcc\xcd\xd1\xd3\xd6\xda\xdc\xdd\xdf\xe0\xe1\xe2\xe4\xe7\xe8\xe9"
    "\xea\xec\xed\xee\xf1\xf2\xf3\xf4\xf6\xf9\xfa\xfb\xfc\xfdĄąĆćČčďĘę"
    "ĚěŁłŃńŇňŐőœŘřŚśŠšŤ"
    "ťŮůŰűŹźŻżŽž“”„™Ёё"
    + "".join(chr(c) for c in range(0x410, 0x450)))

# gaps known and listed as open (they need a native change and a live check): printed as notes
KNOWN_OPEN = {
    # native/gamepad.c hook_key_name: the stick directions' names, shown raw on the controls screen (no CTRL_ text)
    "PAD_LSTICK (Right)", "PAD_LSTICK (Left)", "PAD_LSTICK (Back)", "PAD_LSTICK (Forw)",
    "PAD_RSTICK (Right)", "PAD_RSTICK (Left)", "PAD_RSTICK (Down)", "PAD_RSTICK (Up)",
}

checks = 0
fails = 0


def check(cond, name):
    global checks, fails
    checks += 1
    if cond:
        print("ok   " + name)
    else:
        fails += 1
        print("FAIL " + name)


def note(text):
    print("note " + text)


def read(rel):
    raw = open(os.path.join(ROOT, rel), "rb").read()
    if raw.startswith(b"\xef\xbb\xbf"):
        raw = raw[3:]
    return raw.decode("utf-8").replace("\r\n", "\n")


def same_ok(table, key, lang):
    v = table.get(key)
    return v == "*" or (v is not None and lang in v)


def placeholders(text, rx):
    return sorted(re.findall(rx, text))


# ---------------------------------------------------------------- the game's string tables (data/coop_strings.py)

def check_data(stock):
    strings = coop_strings.STRINGS
    langs = coop_strings.LANGS
    check(langs == GAME_LANGS, "coop_strings.LANGS are the game's nine languages (%s)" % ", ".join(langs))
    keys = [k for k, _ in strings]
    dup = sorted({k for k in keys if keys.count(k) > 1})
    check(not dup, "coop_strings: no key twice (%s)" % ", ".join(dup))
    missing, empty, same, ph, glyph = [], [], [], [], []
    for key, texts in strings:
        en = texts.get("english", "")
        for lg in GAME_LANGS:
            t = texts.get(lg)
            if t is None:
                missing.append("%s/%s" % (key, lg))
                continue
            if not t.strip():
                empty.append("%s/%s" % (key, lg))
            if lg != "english" and t == en and not same_ok(SAME_OK_DATA, key, lg):
                same.append("%s/%s" % (key, lg))
            if placeholders(t, r"\{STRING\}|\{PARAM\}|\n") != placeholders(en, r"\{STRING\}|\{PARAM\}|\n"):
                ph.append("%s/%s" % (key, lg))
            bad = sorted({c for c in t if not (" " <= c <= "~") and c not in FONT_GLYPHS})
            if bad:
                glyph.append("%s/%s %s" % (key, lg, " ".join("U+%04X" % ord(c) for c in bad)))
        extra = sorted(set(texts) - set(GAME_LANGS))
        if extra:
            missing.append("%s: unknown language(s) %s" % (key, ", ".join(extra)))
    check(not missing, "coop_strings: every entry in all nine languages (%s)" % "; ".join(missing))
    check(not empty, "coop_strings: no empty text (%s)" % "; ".join(empty))
    check(not same, "coop_strings: no text equal to English outside SAME_OK_DATA (%s)" % "; ".join(same))
    check(not ph, "coop_strings: {STRING}, {PARAM} and line breaks as in English (%s)" % "; ".join(ph))
    check(not glyph, "coop_strings: only characters the game's font has (%s)" % "; ".join(glyph))
    stale = sorted(k for k in SAME_OK_DATA if k not in keys)
    check(not stale, "SAME_OK_DATA names only existing keys (%s)" % ", ".join(stale))
    if stock:
        clash = sorted(k for k in keys if any(k in stock[lg] for lg in stock))
        check(not clash, "coop_strings: no mod key is a stock key (%s)" % ", ".join(clash))
        union = set()
        for lg in stock:
            for v in stock[lg].values():
                union.update(v)
        unproven = sorted(c for c in FONT_GLYPHS if c not in union and c != "Ё")
        check(not unproven, "FONT_GLYPHS: every listed character is used by a stock table (%s)" %
              " ".join("U+%04X" % ord(c) for c in unproven))
    return set(keys)


# ---------------------------------------------------------------- ids the frames, recipes and the DLL show

def ui_ids():
    """(file, id) of every text id in data/ui/*.ui and the recipes (text = / hint = / SetText / AddItem /
    ShowErrorMsg with a literal)."""
    out = []
    files = sorted("data/ui/" + f for f in os.listdir(os.path.join(ROOT, "data", "ui")) if f.endswith(".ui"))
    files += sorted("data/" + f for f in os.listdir(os.path.join(ROOT, "data"))
                    if f.endswith(".py") and f not in ("coop_strings.py", "__init__.py"))
    for rel in files:
        t = read(rel)
        if rel.endswith(".py"):   # (a recipe: the frame text it writes is in its string constants, not its code)
            t = "\n".join(n.value for n in ast.walk(ast.parse(t)) if isinstance(n, ast.Constant) and isinstance(n.value, str))
        for m in re.finditer(r"^\s*(?:text|hint)\s*=\s*(\S+)", t, re.M):
            out.append((rel, m.group(1)))
        for m in re.finditer(r"\.(?:SetText|AddItem|ShowErrorMsg)\(\"([^\"]*)\"", t):
            out.append((rel, m.group(1).split(",", 1)[0]))
    return out


def check_ids(mod_keys, stock):
    hard, unknown, stock_used = [], [], set()
    for rel, i in ui_ids():
        if re.fullmatch(r"\d+", i):
            continue   # (a number: the player limit's 2 / 3 / 4)
        if i.startswith("COOP_") or i.startswith("CTRL_"):
            if i not in mod_keys:
                unknown.append("%s: %s" % (rel, i))
        elif re.fullmatch(r"(?:UI|GAME)_[A-Za-z0-9_%]+", i):
            if "%" not in i:
                stock_used.add(i)
        else:
            hard.append("%s: %s" % (rel, i))
    check(not unknown, "frames and recipes: every COOP_/CTRL_ id is in coop_strings (%s)" % "; ".join(unknown))
    check(not hard, "frames and recipes: no hard-coded text, only ids (%s)" % "; ".join(hard))
    # the DLL: HUD lines and the main menu's error (COOP_* ids, arguments after commas)
    nat, used = [], set()
    for f in sorted(os.listdir(os.path.join(ROOT, "native"))):
        if not f.endswith(".c"):
            continue
        t = read("native/" + f)
        for m in re.finditer(r"\"(COOP_[A-Z0-9_]+)(?=[\",\\])", t):
            used.add(m.group(1))
            if m.group(1) not in mod_keys:
                nat.append("%s: %s" % (f, m.group(1)))
    check(bool(used) and not nat, "native: every COOP_ id the DLL shows is in coop_strings (%d used; %s)" %
          (len(used), "; ".join(nat)))
    # the pad names the DLL gives the engine: prompts show the CTRL_<name> text
    g = read("native/gamepad.c")
    m = re.search(r"static int THISCALL hook_key_name\(.*?\n\}", g, re.S)
    names = re.findall(r"L\"([^\"]+)\"", m.group(0)) if m else []
    raw, open_ = [], []
    for n in dict.fromkeys(names):
        if "CTRL_" + n in mod_keys:
            continue
        (open_ if n in KNOWN_OPEN else raw).append(n)
    check(bool(names) and not raw, "native/gamepad.c: every pad name has its CTRL_ text (%d names; %s)" %
          (len(names), ", ".join(raw)))
    if open_:
        note("open (KNOWN_OPEN): the controls screen shows these pad names as they are, in every language: " +
             ", ".join(open_))
    if stock:
        miss = sorted("%s/%s" % (i, lg) for i in stock_used for lg in stock if i not in stock[lg])
        check(not miss, "stock ids the mod shows exist in every table (%d ids; %s)" % (len(stock_used), "; ".join(miss)))
        # the docstring of coop_strings names the stock strings the menu uses
        doc = set(re.findall(r"\b(UI_[A-Z0-9_]+)", coop_strings.__doc__ or ""))
        doc = {d for d in doc if not d.endswith("_")}
        miss = sorted("%s/%s" % (i, lg) for i in doc for lg in stock if i not in stock[lg])
        check(not miss, "stock ids named in coop_strings' docstring exist in every table (%s)" % "; ".join(miss))


# ---------------------------------------------------------------- installer, launcher, setup exe

def ps_blocks(text, start):
    """{lang: {key: text}} of a hashtable of hashtables that starts at the line `start` (one key per line)."""
    i = text.index(start)
    out, cur, bad = {}, None, []
    for ln in text[i:].split("\n")[1:]:
        m = re.match(r"^    (\w+) = @\{\s*$", ln)
        if cur is None and m:
            cur = out.setdefault(m.group(1), {})
            continue
        if cur is not None and re.match(r"^    \}\s*$", ln):
            cur = None
            continue
        if cur is None and re.match(r"^\}\s*$", ln):
            break
        if cur is not None:
            m = re.match(r'^\s+"([\w.]+)"\s*=\s*"((?:[^"`]|`.)*)"\s*$', ln)
            if m:
                cur[m.group(1)] = m.group(2)
            elif ln.strip():
                bad.append(ln.strip()[:60])
    return out, bad


def ps_inline_blocks(text, start):
    """{lang: {key: text}} of `$T = @{` with one `xx = @{ k = "v"; ... }` line per language."""
    i = text.index(start)
    out, bad = {}, []
    for ln in text[i:].split("\n")[1:]:
        if re.match(r"^\}\s*$", ln):
            break
        m = re.match(r"^    (\w+) = @\{ (.*) \}\s*$", ln)
        if not m:
            if ln.strip():
                bad.append(ln.strip()[:60])
            continue
        body = m.group(2)
        pairs = re.findall(r'(\w+) = "((?:[^"`]|`.)*)"', body)
        out[m.group(1)] = dict(pairs)
        rest = re.sub(r'(\w+) = "((?:[^"`]|`.)*)"', "", body).replace(";", "").strip()
        if rest:
            bad.append("%s: %s" % (m.group(1), rest[:60]))
    return out, bad


def check_table(name, tables, same_tab, used_keys):
    langs = list(tables)
    miss_l = [lg for lg in WIN_LANGS if lg not in tables]
    check(not miss_l, "%s: the nine languages (%s; missing %s)" % (name, ", ".join(langs), ", ".join(miss_l)))
    en = tables.get("en", {})
    missing, empty, same, ph, extra = [], [], [], [], []
    for lg, t in tables.items():
        for k, v in en.items():
            if k not in t:
                missing.append("%s/%s" % (lg, k))
                continue
            if not t[k].strip():
                empty.append("%s/%s" % (lg, k))
            if lg != "en" and t[k] == v and not same_ok(same_tab, k, lg):
                same.append("%s/%s" % (lg, k))
            if placeholders(t[k], r"\{\d+\}") != placeholders(v, r"\{\d+\}"):
                ph.append("%s/%s" % (lg, k))
        extra += ["%s/%s" % (lg, k) for k in t if k not in en]
    check(not missing, "%s: every English key in every language (%s)" % (name, "; ".join(missing)))
    check(not empty, "%s: no empty text (%s)" % (name, "; ".join(empty)))
    check(not same, "%s: no text equal to English outside the allow-list (%s)" % (name, "; ".join(same)))
    check(not ph, "%s: the same {0}.. placeholders as English (%s)" % (name, "; ".join(ph)))
    if extra:
        note("%s: keys beyond English (unused until English has them): %s" % (name, ", ".join(extra)))
    undefined = sorted(k for k in used_keys if k not in en)
    check(not undefined, "%s: every L \"key\" the script uses has an English text (%d used; %s)" %
          (name, len(used_keys), ", ".join(undefined)))


def check_tools():
    t = read("tools/installer.ps1")
    tables, bad = ps_blocks(t, "$script:TEXTS = @{")
    check(not bad, "installer: the text table parses (%s)" % "; ".join(bad))
    used = set(re.findall(r"\bL \"(\w+)\"", t))
    check_table("installer", tables, SAME_OK_INSTALLER, used)
    m = re.search(r"\[-Lang ([a-z|]+)\]", t)
    check(m is not None and m.group(1).split("|") == WIN_LANGS, "installer: the usage line lists the languages (%s)" %
          (m.group(1) if m else "none"))
    lits = [x for x in re.findall(r"\bthrow \"([^\"]*)\"", t) if x not in INSTALLER_LOG_ONLY]
    check(not lits, "installer: no hard-coded text in errors (%s)" % "; ".join(lits))

    t = read("tools/launcher.ps1")
    tables, bad = ps_inline_blocks(t, "$T = @{")
    check(not bad, "launcher: the text table parses (%s)" % "; ".join(bad))
    used = set(re.findall(r"\bL \"(\w+)\"", t))
    check_table("launcher", tables, SAME_OK_LAUNCHER, used)
    lits = [ln.strip() for ln in t.split("\n") if re.search(r"\bthrow \"", ln) or re.search(r"MessageBox\]::Show\(\"", ln)]
    check(not lits, "launcher: no hard-coded text in errors and message boxes (%s)" % "; ".join(lits))

    t = read("tools/mkinstaller/stub.c")
    m = re.search(r"TEXTS\[(\d+)\]\[M_COUNT\] = \{(.*?)\n\};", t, re.S)
    blocks = re.findall(r"\{ /\* (\w+) \*/\n(.*?)\n    \},", m.group(2), re.S) if m else []
    stub = {lg: re.findall(r'L"((?:[^"\\]|\\.)*)"', body) for lg, body in blocks}
    n_msg = len(re.search(r"enum \{ (M_[A-Z_, ]+), M_COUNT \}", t).group(1).split(","))
    check(m is not None and int(m.group(1)) == len(blocks) and [lg for lg, _ in blocks] == WIN_LANGS,
          "setup exe (stub.c): the nine languages in order (%s)" % ", ".join(lg for lg, _ in blocks))
    short = [lg for lg, v in stub.items() if len(v) != n_msg]
    check(not short, "setup exe (stub.c): %d messages in every language (%s)" % (n_msg, ", ".join(short)))
    same = ["%s/%d" % (lg, i) for lg, v in stub.items() if lg != "en" for i, s in enumerate(v)
            if i < len(stub.get("en", [])) and s == stub["en"][i]]
    check(not same, "setup exe (stub.c): no message equal to English (%s)" % ", ".join(same))
    sw = dict((c, int(i)) for c, i in re.findall(r"case LANG_(\w+): l = (\d+);", t))
    want = {"RUSSIAN": 1, "GERMAN": 2, "FRENCH": 3, "SPANISH": 4, "ITALIAN": 5, "POLISH": 6, "CZECH": 7, "HUNGARIAN": 8}
    check(sw == want, "setup exe (stub.c): Windows' language picks its row (%s)" % sw)

    readmes = sorted(f for f in os.listdir(os.path.join(ROOT, "docs")) if f.startswith("release-readme"))
    note("players' readmes shipped: %s (the installer opens the Russian one on Russian Windows, else the English one)" %
         ", ".join(readmes))


def load_stock(folder):
    out = {}
    for lg in GAME_LANGS:
        p = os.path.join(folder, "strings_%s.txt" % lg)
        if not os.path.exists(p):
            continue
        t = open(p, "rb").read().decode("utf-16-le").lstrip("﻿")
        d = {}
        for ln in t.split("\r\n"):
            if "\t" in ln:
                k, v = ln.split("\t", 1)
                d[k] = v.strip().strip('"')
        for k in [k for k in d if k.endswith("_PC")]:   # (the engine: a key ending in _PC replaces the plain one)
            d[k[:-3]] = d[k]
        out[lg] = d
    return out


def main(argv):
    stock = None
    if "--strings" in argv:
        folder = argv[argv.index("--strings") + 1]
        stock = load_stock(folder)
        check(sorted(stock) == sorted(GAME_LANGS), "--strings: the game's nine tables in %s (%s)" %
              (folder, ", ".join(sorted(stock))))
    keys = check_data(stock)
    check_ids(keys, stock)
    check_tools()
    print("")
    print("test_texts: %d/%d passed" % (checks - fails, checks))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
