#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""从 Unicode UCD 生成 ports/core/src/ucd_width_table.inc（静态区间表）。

对照上游 iTerm2 sources/CharacterSets/iTermCharacterWidth.c 与 ScreenChar.m
(StringToScreenChars) 的分类：全宽(W/F)、东亚歧义(A)、组合(Mn/Me/Mc + Hangul V/T)、
可忽略(Default_Ignorable_Code_Point，除 ZWJ/ZWNJ/VS/TAG 外，它们并入字素)、
Extended_Pictographic、Emoji_Modifier_Base、Emoji_Modifier、接受 VS16 的 emoji。

数据来源顺序：--ucd-dir 指定目录 → 联网下载 https://www.unicode.org/Public/<ver>/ucd/
→ 系统已有数据（/usr/share/unicode, /usr/share/unicode-data）。
用法：gen_width_table.py [--version 16.0.0|latest] [--ucd-dir DIR] [-o OUT]
"""
import argparse, os, re, sys, urllib.request

FILES = {
    "EastAsianWidth.txt": "ucd/EastAsianWidth.txt",
    "DerivedGeneralCategory.txt": "ucd/extracted/DerivedGeneralCategory.txt",
    "DerivedCoreProperties.txt": "ucd/DerivedCoreProperties.txt",
    "HangulSyllableType.txt": "ucd/HangulSyllableType.txt",
    "emoji-data.txt": "ucd/emoji/emoji-data.txt",
    "emoji-variation-sequences.txt": "ucd/emoji/emoji-variation-sequences.txt",
}
SYS_DIRS = ["/usr/share/unicode", "/usr/share/unicode-data", "/usr/share/unicode/ucd"]

def find_local(name, dirs):
    for d in dirs:
        for root, _, files in os.walk(d):
            if name in files:
                return os.path.join(root, name)
    return None

def load(name, args, cache):
    p = find_local(name, [args.ucd_dir]) if args.ucd_dir else None
    if not p:
        url = f"https://www.unicode.org/Public/{args.version}/{FILES[name]}"
        dst = os.path.join(cache, name)
        try:
            if not os.path.exists(dst):
                urllib.request.urlretrieve(url, dst)
            p = dst
        except Exception as e:  # 离线：退回系统数据
            print(f"download failed {url}: {e}", file=sys.stderr)
            p = find_local(name, SYS_DIRS)
    if not p:
        sys.exit(f"missing UCD file {name}")
    with open(p, encoding="utf-8") as f:
        return f.read(), p

def parse(text):
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        f = [x.strip() for x in line.split(";")]
        r = f[0].split("..")
        lo = int(r[0], 16); hi = int(r[-1], 16)
        yield lo, hi, f[1:]

def ranges(cps):
    out = []
    for c in sorted(cps):
        if out and out[-1][1] + 1 == c:
            out[-1][1] = c
        else:
            out.append([c, c])
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default="16.0.0")
    ap.add_argument("--ucd-dir")
    ap.add_argument("-o", default=os.path.join(os.path.dirname(__file__), "..", "src", "ucd_width_table.inc"))
    args = ap.parse_args()
    cache = os.path.join(os.environ.get("TMPDIR", "/tmp"), f"itcore-ucd-{args.version}")
    os.makedirs(cache, exist_ok=True)
    src = {n: load(n, args, cache) for n in FILES}

    wide, amb, comb, ign, pict, mbase, mod, vs16 = (set() for _ in range(8))
    for lo, hi, f in parse(src["EastAsianWidth.txt"][0]):
        for c in range(lo, hi + 1):
            if f[0] in ("W", "F"): wide.add(c)
            elif f[0] == "A": amb.add(c)
    for lo, hi, f in parse(src["DerivedGeneralCategory.txt"][0]):
        if f[0] in ("Mn", "Me", "Mc"):
            comb.update(range(lo, hi + 1))
    for lo, hi, f in parse(src["HangulSyllableType.txt"][0]):
        if f[0] in ("V", "T"):
            comb.update(range(lo, hi + 1))
    joiners = {0x200C, 0x200D} | set(range(0xFE00, 0xFE10)) | set(range(0xE0100, 0xE01F0)) | set(range(0xE0020, 0xE0080))
    for lo, hi, f in parse(src["DerivedCoreProperties.txt"][0]):
        if f[0] == "Default_Ignorable_Code_Point":
            ign.update(range(lo, hi + 1))
    comb |= joiners
    ign -= joiners
    comb -= ign
    for lo, hi, f in parse(src["emoji-data.txt"][0]):
        s = set(range(lo, hi + 1))
        if f[0] == "Extended_Pictographic": pict |= s
        elif f[0] == "Emoji_Modifier_Base": mbase |= s
        elif f[0] == "Emoji_Modifier": mod |= s
    for line in src["emoji-variation-sequences.txt"][0].splitlines():
        m = re.match(r"\s*([0-9A-F]+)\s+FE0F\s*;", line)
        if m: vs16.add(int(m.group(1), 16))
    # 组合/可忽略优先于全宽；Emoji_Modifier（肤色）作为组合并入（上游按修饰序列合并）
    comb |= mod
    wide -= comb | ign
    amb -= comb | ign | wide
    out = os.path.abspath(args.o)
    with open(out, "w", encoding="utf-8") as o:
        o.write("// 由 ports/core/tools/gen_width_table.py 生成，勿手改。\n")
        o.write(f"// Unicode {args.version}; 来源: " + ", ".join(os.path.basename(v[1]) for v in src.values()) + "\n")
        o.write(f"#define ITCORE_UCD_VERSION \"{args.version}\"\n")
        for name, s in (("wide", wide), ("ambiguous", amb), ("combining", comb), ("ignorable", ign),
                        ("ext_pict", pict), ("modifier_base", mbase), ("vs16_base", vs16)):
            rs = ranges(s)
            o.write(f"static const UcdRange kUcd_{name}[] = {{\n")
            for i in range(0, len(rs), 4):
                o.write("    " + " ".join(f"{{0x{a:X}, 0x{b:X}}}," for a, b in rs[i:i+4]) + "\n")
            o.write("};\n")
    print(f"wrote {out}")

if __name__ == "__main__":
    main()
