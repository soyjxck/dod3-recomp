#!/usr/bin/env python3
"""The launcher's and the setup's translations (src/lang.h, docs/TRANSLATING.md).

  python tools/lang.py update          lang/dod3.pot written again from the source's tr() calls, and every
                                       lang/<code>.po brought in line with it: its translations kept, new
                                       strings added empty, strings no longer in the source set aside (#~)
  python tools/lang.py check           fails if lang/dod3.pot is not what the source gives, or a .po has a
                                       placeholder its English lacks or the wrong number of plural forms;
                                       says how much of each language is translated
  python tools/lang.py new <code>      lang/<code>.po, empty, with the language's plural rule
  python tools/lang.py pseudo <file>   a made-up translation of every string (accents, a third longer,
                                       Cyrillic at the end), for tests: as lang/qps.po beside the executable
                                       with DOD3_UI_LANGUAGE=qps, a window shows in plain English only what
                                       the source did not mark

A string is taken from the first argument of tr, trf and TR_NOOP, the second of trc and trcf (the first is its
context), and the second and third of trn and trnf (one, many), where those are string literals. A comment
that starts TRANSLATORS: just before a call goes to the translators with it.
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
POT = os.path.join(ROOT, "lang", "dod3.pot")
CALLS = {"tr": "id", "trf": "id", "TR_NOOP": "id", "trc": "ctx", "trcf": "ctx", "trn": "n", "trnf": "n"}
OWN_NAME = ("language", "English")
OWN_NAME_NOTE = ('This language\'s own name, as the launcher\'s Settings list shows it '
                 '(a German translation has "Deutsch" here).')

# gettext's rules for the languages a translator is likely to start (the others: copy one from gettext's manual)
PLURALS = {
    "ja": "nplurals=1; plural=0;", "ko": "nplurals=1; plural=0;", "zh": "nplurals=1; plural=0;",
    "tr": "nplurals=2; plural=(n > 1);", "fr": "nplurals=2; plural=(n > 1);", "pt-BR": "nplurals=2; plural=(n > 1);",
    "ru": "nplurals=3; plural=(n%10==1 && n%100!=11 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2);",
    "uk": "nplurals=3; plural=(n%10==1 && n%100!=11 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2);",
    "pl": "nplurals=3; plural=(n==1 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2);",
    "cs": "nplurals=3; plural=(n==1 ? 0 : n>=2 && n<=4 ? 1 : 2);",
}
DEFAULT_PLURAL = "nplurals=2; plural=(n != 1);"


# ---- the source ---------------------------------------------------------------------

def c_string(src, i):
    """The string literal at src[i] ('"'): its text, and the index after it."""
    out = bytearray()
    j = i + 1
    simple = {"n": 10, "t": 9, "r": 13, "\\": 92, '"': 34, "'": 39, "?": 63, "0": 0, "a": 7, "b": 8, "f": 12, "v": 11}
    while src[j] != '"':
        c = src[j]
        if c != "\\":
            out += c.encode("utf-8")
            j += 1
            continue
        e = src[j + 1]
        if e == "x":
            m = re.compile(r"[0-9a-fA-F]+").match(src, j + 2)
            out.append(int(m.group(0), 16) & 0xFF)
            j = m.end()
        elif e in "01234567":
            m = re.compile(r"[0-7]{1,3}").match(src, j + 1)
            out.append(int(m.group(0), 8) & 0xFF)
            j = m.end()
        else:
            out.append(simple.get(e, ord(e)))
            j += 2
    return out.decode("utf-8"), j + 1


def skip_space(src, i, notes=None):
    """Past whitespace and comments from i; a TRANSLATORS: comment is added to notes."""
    n = len(src)
    while i < n:
        if src[i] in " \t\r\n":
            i += 1
        elif src.startswith("//", i):
            e = src.find("\n", i)
            e = n if e < 0 else e
            if notes is not None:
                take_note(src[i + 2:e], notes)
            i = e
        elif src.startswith("/*", i):
            e = src.index("*/", i)
            if notes is not None:
                take_note(src[i + 2:e], notes)
            i = e + 2
        else:
            break
    return i


def take_note(comment, notes):
    text = " ".join(part.strip(" \t*") for part in comment.split("\n")).strip()
    if text.startswith("TRANSLATORS:"):
        notes.append(text[len("TRANSLATORS:"):].strip())


def literal(src, i):
    """Adjacent string literals from i (after spaces): their text and the index after, or None."""
    i = skip_space(src, i)
    if i >= len(src) or src[i] != '"':
        return None
    text = ""
    while i < len(src) and src[i] == '"':
        part, i = c_string(src, i)
        text += part
        i = skip_space(src, i)
    return text, i


def skip_argument(src, i):
    """Past one argument (to its comma) from i; the index after the comma, or None."""
    depth = 0
    n = len(src)
    while i < n:
        c = src[i]
        if c == '"':
            _, i = c_string(src, i)
            continue
        if c == "'":
            i += 1
            while src[i] != "'":
                i += 2 if src[i] == "\\" else 1
        elif c in "([{":
            depth += 1
        elif c in ")]}":
            if depth == 0:
                return None
            depth -= 1
        elif c == "," and depth == 0:
            return i + 1
        i += 1
    return None


def extract_file(path, rel, found):
    src = open(path, encoding="utf-8").read()
    n = len(src)
    i = 0
    notes = []
    word = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
    while i < n:
        c = src[i]
        if c in " \t\r\n" or src.startswith("//", i) or src.startswith("/*", i):
            i = skip_space(src, i, notes)
            continue
        if c == '"':
            _, i = c_string(src, i)
            notes = []
            continue
        if c == "'":
            i += 1
            while src[i] != "'":
                i += 2 if src[i] == "\\" else 1
            i += 1
            continue
        m = word.match(src, i)
        if not m:
            if c == ";":
                notes = []
            i += 1
            continue
        name, i = m.group(0), m.end()
        kind = CALLS.get(name)
        if not kind:
            continue
        j = skip_space(src, i)
        if j >= n or src[j] != "(":
            continue
        j += 1
        context, plural = None, None
        if kind == "ctx":
            got = literal(src, j)
            if not got:
                continue
            context, j = got
            j = skip_space(src, j)
            if src[j] != ",":
                continue
            j += 1
        elif kind == "n":
            j = skip_argument(src, j)
            if j is None:
                continue
        got = literal(src, j)
        if not got:
            continue
        text, j = got
        if kind == "n":
            if src[j] != ",":
                continue
            got = literal(src, j + 1)
            if not got:
                raise SystemExit("%s: %s(...) needs two literals (one, many) at %r" % (rel, name, text))
            plural, j = got
        if not text:
            continue
        key = (context, text)
        e = found.setdefault(key, {"files": [], "plural": None, "notes": []})
        if rel not in e["files"]:
            e["files"].append(rel)
        if plural is not None:
            if e["plural"] not in (None, plural):
                raise SystemExit("%s: %r has two plural forms: %r and %r" % (rel, text, e["plural"], plural))
            e["plural"] = plural
        for note in notes:
            if note not in e["notes"]:
                e["notes"].append(note)
        notes = []
        i = j


def extract():
    """Every marked string of the source, in the order found: {(context, text): {files, plural, notes}}."""
    found = {OWN_NAME: {"files": [], "plural": None, "notes": [OWN_NAME_NOTE]}}
    files = []
    for ext in ("cpp", "h", "mm", "c"):
        files += glob.glob(os.path.join(ROOT, "src", "**", "*." + ext), recursive=True)
    for path in sorted(files, key=lambda p: os.path.relpath(p, ROOT).replace("\\", "/")):
        extract_file(path, os.path.relpath(path, ROOT).replace("\\", "/"), found)
    for (context, text), e in found.items():
        if e["plural"] is None:
            continue
        for other in found:
            if other[1] == text and other != (context, text):
                raise SystemExit("%r is both a plural and a plain string" % text)
    return found


# ---- .po files ------------------------------------------------------------------------

def quote(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"').replace("\t", "\\t").replace("\n", "\\n") + '"'


def unquote(line):
    a = line.index('"')
    out = ""
    j = a + 1
    while j < len(line) and line[j] != '"':
        if line[j] == "\\" and j + 1 < len(line):
            out += {"n": "\n", "t": "\t", "r": "\r"}.get(line[j + 1], line[j + 1])
            j += 2
        else:
            out += line[j]
            j += 1
    return out


def read_po(path):
    """(header text, {(context, id): {str: [..], plural, fuzzy}}, leading comment lines, obsolete lines)."""
    lines = open(path, encoding="utf-8-sig").read().replace("\r\n", "\n").split("\n")
    entries, head, obsolete = {}, [], []
    header = ""
    cur = None
    field = None
    seen_any = False

    def flush():
        nonlocal cur, field, header, seen_any
        if cur and cur["has_id"]:
            if cur["id"] == "" and cur["plural"] is None:
                header = cur["str"][0] if cur["str"] else ""
            else:
                entries[(cur["context"], cur["id"])] = {"str": cur["str"], "plural": cur["plural"], "fuzzy": cur["fuzzy"]}
            seen_any = True
        cur, field = None, None

    def start():
        nonlocal cur
        if cur is None:
            cur = {"context": None, "id": "", "plural": None, "str": [], "fuzzy": False, "has_id": False}

    for raw in lines:
        line = raw.strip()
        if not line:
            flush()
            continue
        if line.startswith("#~"):
            obsolete.append(line)
            continue
        if line.startswith("#"):
            if cur and cur["str"]:
                flush()
            if line.startswith("#,") and "fuzzy" in line:
                start()
                cur["fuzzy"] = True
            elif not seen_any and cur is None and not line.startswith(("#:", "#.", "#,")):
                head.append(raw)
            continue
        if line.startswith('"'):
            if cur is not None and field is not None:
                if field[0] == "str":
                    cur["str"][field[1]] += unquote(line)
                else:
                    cur[field[0]] = (cur[field[0]] or "") + unquote(line)
            continue
        if line.startswith("msgctxt"):
            if cur and cur["has_id"]:
                flush()
            start()
            cur["context"] = unquote(line)
            field = ("context",)
        elif line.startswith("msgid_plural"):
            start()
            cur["plural"] = unquote(line)
            field = ("plural",)
        elif line.startswith("msgid"):
            if cur and cur["has_id"]:
                flush()
            start()
            cur["has_id"] = True
            cur["id"] = unquote(line)
            field = ("id",)
        elif line.startswith("msgstr"):
            start()
            k = int(line[7:line.index("]")]) if line.startswith("msgstr[") else 0
            while len(cur["str"]) <= k:
                cur["str"].append("")
            cur["str"][k] = unquote(line)
            field = ("str", k)
    flush()
    return header, entries, head, obsolete


def header_text(language, plural):
    return ("Project-Id-Version: dod3-recomp\n"
            "MIME-Version: 1.0\n"
            "Content-Type: text/plain; charset=UTF-8\n"
            "Content-Transfer-Encoding: 8bit\n"
            "Language: %s\n"
            "Plural-Forms: %s\n" % (language, plural))


def nplurals_of(header):
    m = re.search(r"nplurals\s*=\s*(\d+)", header)
    return int(m.group(1)) if m else 2


def write_po(path, head, header, found, have, obsolete):
    """A catalogue in the template's order: `have` gives the translations there are."""
    out = list(head)
    out += ['msgid ""', 'msgstr ""'] + [quote(l + "\n") for l in header.rstrip("\n").split("\n")] + [""]
    forms = nplurals_of(header)
    for (context, text), e in found.items():
        old = have.get((context, text)) if have is not None else None
        for note in e["notes"]:
            out.append("#. " + note)
        if e["files"]:
            out.append("#: " + " ".join(e["files"]))
        if old and old["fuzzy"]:
            out.append("#, fuzzy")
        if context is not None:
            out.append("msgctxt " + quote(context))
        out.append("msgid " + quote(text))
        strs = old["str"] if old else []
        if e["plural"] is None:
            out.append("msgstr " + quote(strs[0] if strs else ""))
        else:
            out.append("msgid_plural " + quote(e["plural"]))
            for k in range(forms if have is not None else 2):
                out.append("msgstr[%d] %s" % (k, quote(strs[k] if k < len(strs) else "")))
        out.append("")
    gone = []
    if have is not None:
        for (context, text), old in have.items():
            if (context, text) in found or not any(old["str"]):
                continue
            if context is not None:
                gone.append("#~ msgctxt " + quote(context))
            gone.append("#~ msgid " + quote(text))
            if old["plural"] is not None:
                gone.append("#~ msgid_plural " + quote(old["plural"]))
                gone += ["#~ msgstr[%d] %s" % (k, quote(t)) for k, t in enumerate(old["str"])]
            else:
                gone.append("#~ msgstr " + quote(old["str"][0]))
    out += obsolete + gone
    while out and out[-1] == "":
        out.pop()
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out) + "\n")


POT_HEAD = ["# Drakengard 3 Recompiled: the launcher's and the setup's text, for translators.",
            "# Written by tools/lang.py from the source; docs/TRANSLATING.md says how to use it.",
            "#"]


def language_files():
    return sorted(glob.glob(os.path.join(ROOT, "lang", "*.po")))


def placeholders(text):
    return set(re.findall(r"\{(\d+)\}", text.replace("{{", "")))


# ---- the commands -----------------------------------------------------------------------

def cmd_update():
    found = extract()
    os.makedirs(os.path.dirname(POT), exist_ok=True)
    write_po(POT, POT_HEAD, header_text("", DEFAULT_PLURAL), found, None, [])
    print("lang/dod3.pot: %d strings" % len(found))
    for po in language_files():
        header, have, head, obsolete = read_po(po)
        write_po(po, head, header, found, have, obsolete)
        done = sum(1 for k in found if k in have and all(have[k]["str"]) and have[k]["str"])
        print("%s: %d of %d translated" % (os.path.relpath(po, ROOT).replace("\\", "/"), done, len(found)))


def cmd_check():
    found = extract()
    bad = 0
    if not os.path.exists(POT):
        print("lang/dod3.pot is missing: python tools/lang.py update")
        return 1
    _, listed, _, _ = read_po(POT)
    want = {k: e["plural"] for k, e in found.items()}
    got = {k: e["plural"] for k, e in listed.items()}
    if want != got:
        for k in want.keys() - got.keys():
            print("not in lang/dod3.pot: %r" % (k,))
        for k in got.keys() - want.keys():
            print("no longer in the source: %r" % (k,))
        print("lang/dod3.pot is out of date: python tools/lang.py update")
        bad += 1
    else:
        print("lang/dod3.pot: %d strings, as the source has them" % len(found))
    for po in language_files():
        rel = os.path.relpath(po, ROOT).replace("\\", "/")
        header, have, _, _ = read_po(po)
        forms = nplurals_of(header)
        done = 0
        for k, e in found.items():
            old = have.get(k)
            if old is None:
                print("%s: no entry for %r: python tools/lang.py update" % (rel, k))
                bad += 1
                continue
            strs = old["str"]
            if not any(strs):
                continue
            if e["plural"] is not None and (len(strs) != forms or not all(strs)):
                print("%s: %r needs %d plural forms, all of them" % (rel, k[1], forms))
                bad += 1
                continue
            allowed = placeholders(k[1]) | placeholders(e["plural"] or "")
            for t in strs:
                extra = placeholders(t) - allowed
                if extra:
                    print("%s: %r: the translation has {%s}, the English does not" % (rel, k[1], "}, {".join(sorted(extra))))
                    bad += 1
            done += 0 if old["fuzzy"] else 1
        print("%s: %d of %d translated" % (rel, done, len(found)))
    return 1 if bad else 0


def cmd_new(code):
    path = os.path.join(ROOT, "lang", code + ".po")
    if os.path.exists(path):
        raise SystemExit("lang/%s.po is there already" % code)
    plural = PLURALS.get(code, PLURALS.get(code.split("-")[0], DEFAULT_PLURAL))
    head = ["# Drakengard 3 Recompiled: the launcher's and the setup's text in this language (%s)." % code,
            "# docs/TRANSLATING.md says how to try it and how to send it.",
            "#"]
    found = extract()
    forms = nplurals_of(plural)
    have = {k: {"str": [""] * (forms if e["plural"] is not None else 1), "plural": e["plural"], "fuzzy": False}
            for k, e in found.items()}
    write_po(path, head, header_text(code, plural), found, have, [])
    print("lang/%s.po: %d strings to translate (%s)" % (code, len(found), plural))


ACCENTS = str.maketrans({"a": "\u00e1", "e": "\u00e9", "i": "\u00ed", "o": "\u00f3", "u": "\u00fa", "c": "\u00e7",
                         "n": "\u00f1", "y": "\u00fd", "A": "\u00c1", "E": "\u00c9", "I": "\u00cd", "O": "\u00d3",
                         "U": "\u00da", "C": "\u00c7", "N": "\u00d1", "Y": "\u00dd"})


def pseudo(text):
    parts = re.split(r"(\{\d+\})", text)
    body = "".join(p if re.fullmatch(r"\{\d+\}", p) else p.translate(ACCENTS) for p in parts)
    lead = len(body) - len(body.lstrip(" "))
    trail = len(body) - len(body.rstrip(" "))
    core = body.strip(" ")
    pad = "".join("\u0436\u0449\u044e\u044f\u044b"[k % 5] for k in range(max(1, len(core) * 3 // 10)))
    return " " * lead + "[" + core + " " + pad + "]" + " " * trail


def cmd_pseudo(path):
    found = extract()
    have = {}
    for k, e in found.items():
        if k == OWN_NAME:
            have[k] = {"str": ["Pseudo"], "plural": None, "fuzzy": False}
        elif e["plural"] is None:
            have[k] = {"str": [pseudo(k[1])], "plural": None, "fuzzy": False}
        else:
            have[k] = {"str": [pseudo(k[1]), pseudo(e["plural"])], "plural": e["plural"], "fuzzy": False}
    write_po(path, ["# A made-up translation, for tests (tools/lang.py pseudo)."], header_text("qps", DEFAULT_PLURAL),
             found, have, [])
    print("%s: %d strings" % (path, len(found)))


def main(argv):
    if len(argv) == 2 and argv[1] == "update":
        return cmd_update()
    if len(argv) == 2 and argv[1] == "check":
        return cmd_check()
    if len(argv) == 3 and argv[1] == "new":
        return cmd_new(argv[2])
    if len(argv) == 3 and argv[1] == "pseudo":
        return cmd_pseudo(argv[2])
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv) or 0)
