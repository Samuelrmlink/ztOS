#!/usr/bin/python3 -I
# SPDX-License-Identifier: GPL-2.0-or-later
"""check-bt-app: does a Bluetooth.apk fit the image's libbluetooth_jni.so?

    check-bt-app.py libbluetooth_jni.so Bluetooth.apk

The pinned Waydroid image has the JNI library but no Bluetooth app, so the
app comes from another LineageOS 20 build (docs/waydroid.md, "Shim build").
The app's Java code calls native methods that the JNI library registers with
RegisterNatives; if one is missing or has another signature, the app dies
with UnsatisfiedLinkError (or worse, a native method gets the wrong
arguments). This reads both files as data, runs nothing from them, and
compares:

  - the library's JNINativeMethod tables: each {name, signature, function}
    triple, found through the R_AARCH64_RELATIVE relocations that fill them;
  - every native method declared in the APK's dex files, with its class.

The tables lie next to each other, so each run of adjacent entries is split
into consecutive windows, each a subset of one class's native methods
(name+signature). Every class must have exactly one table, and every entry
must belong to one: RegisterNatives fails on a method the class doesn't
declare. Natives a class declares but the library never registers are listed
(stock AOSP 13 has three such in AdapterService; calling one would throw). Exit 0 when all match, 1 otherwise. The app is
untrusted input: run this with python3 -I, from a directory of its own.
"""
import struct
import sys
import zipfile

R_AARCH64_RELATIVE = 1027


# ---- ELF ----------------------------------------------------------------

def elf_tables(path):
    data = open(path, "rb").read()
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        raise SystemExit("%s: not a little-endian ELF64" % path)
    e_shoff, = struct.unpack_from("<Q", data, 0x28)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x3a)
    secs = []
    for i in range(e_shnum):
        name, stype, flags, addr, off, size = struct.unpack_from("<IIQQQQ", data, e_shoff + i * e_shentsize)
        secs.append([name, stype, flags, addr, off, size])
    shstr = secs[e_shstrndx]
    for s in secs:
        end = data.index(b"\0", shstr[4] + s[0])
        s[0] = data[shstr[4] + s[0]:end].decode()
    by = {s[0]: s for s in secs}

    def va_to_off(va):
        for s in secs:
            if s[1] != 8 and s[3] <= va < s[3] + s[5]:   # not NOBITS
                return s[4] + va - s[3]
        return None

    def in_sec(va, names):
        for n in names:
            s = by.get(n)
            if s and s[3] <= va < s[3] + s[5]:
                return True
        return False

    rel = {}
    for name in (".rela.dyn",):
        s = by.get(name)
        if not s:
            continue
        for o in range(s[4], s[4] + s[5], 24):
            r_off, r_info, r_add = struct.unpack_from("<QQq", data, o)
            if r_info & 0xffffffff == R_AARCH64_RELATIVE:
                rel[r_off] = r_add
    # RELR (SHT_RELR = 19): relative relocations whose addend is the word in place
    s = by.get(".relr.dyn")
    if s and s[1] == 19:
        where = 0
        for o in range(s[4], s[4] + s[5], 8):
            entry, = struct.unpack_from("<Q", data, o)
            if entry & 1 == 0:
                spots = [entry]
                where = entry + 8
            else:
                spots = [where + 8 * i for i in range(63) if entry >> (i + 1) & 1]
                where += 63 * 8
            for va in spots:
                fo = va_to_off(va)
                if fo is not None:
                    rel[va], = struct.unpack_from("<Q", data, fo)
    # Android's packed APS2 .rela.dyn is not read: lld puts relative ones in RELR
    if not rel:
        raise SystemExit("%s: no RELA RELATIVE relocations (packed?)" % path)

    def cstr(va):
        o = va_to_off(va)
        if o is None:
            return None
        end = data.find(b"\0", o, o + 4096)
        if end < 0:
            return None
        try:
            return data[o:end].decode("ascii")
        except UnicodeDecodeError:
            return None

    strsecs = (".rodata",)
    textsecs = (".text",)
    tables = []
    cur, last = [], None
    for off in sorted(rel):
        n, s, f = rel.get(off), rel.get(off + 8), rel.get(off + 16)
        ok = (s is not None and f is not None and in_sec(n, strsecs) and in_sec(s, strsecs)
              and in_sec(f, textsecs))
        sig = cstr(s) if ok else None
        name = cstr(n) if ok else None
        if ok and sig and sig.startswith("(") and ")" in sig and name and name.isidentifier():
            if last is not None and off == last + 24:
                cur.append((name, sig))
            else:
                if cur:
                    tables.append(cur)
                cur = [(name, sig)]
            last = off
    if cur:
        tables.append(cur)
    return tables


# ---- dex ----------------------------------------------------------------

def uleb(buf, pos):
    result = shift = 0
    while True:
        b = buf[pos]
        pos += 1
        result |= (b & 0x7f) << shift
        if b < 0x80:
            return result, pos
        shift += 7


def dex_natives(buf):
    if buf[:4] != b"dex\n":
        raise SystemExit("not a dex file")
    (string_ids_size, string_ids_off, type_ids_size, type_ids_off, proto_ids_size, proto_ids_off,
     field_ids_size, field_ids_off, method_ids_size, method_ids_off, class_defs_size,
     class_defs_off) = struct.unpack_from("<12I", buf, 0x38)

    def string(i):
        off, = struct.unpack_from("<I", buf, string_ids_off + 4 * i)
        _, p = uleb(buf, off)          # utf16 length
        end = buf.index(b"\0", p)
        return buf[p:end].decode("utf-8", "replace")   # MUTF-8; method names are ASCII

    def type_name(i):
        sidx, = struct.unpack_from("<I", buf, type_ids_off + 4 * i)
        return string(sidx)

    def proto(i):
        _, ret, params_off = struct.unpack_from("<III", buf, proto_ids_off + 12 * i)
        params = ""
        if params_off:
            n, = struct.unpack_from("<I", buf, params_off)
            for k in range(n):
                t, = struct.unpack_from("<H", buf, params_off + 4 + 2 * k)
                params += type_name(t)
        return "(%s)%s" % (params, type_name(ret))

    out = {}
    for c in range(class_defs_size):
        cls, _, _, _, _, _, class_data_off, _ = struct.unpack_from("<8I", buf, class_defs_off + 32 * c)
        if not class_data_off:
            continue
        p = class_data_off
        sf, p = uleb(buf, p)
        inf, p = uleb(buf, p)
        dm, p = uleb(buf, p)
        vm, p = uleb(buf, p)
        for _ in range(sf + inf):
            _, p = uleb(buf, p)
            _, p = uleb(buf, p)
        for count in (dm, vm):
            idx = 0
            for _ in range(count):
                diff, p = uleb(buf, p)
                flags, p = uleb(buf, p)
                _, p = uleb(buf, p)     # code_off
                idx += diff
                if flags & 0x100:       # ACC_NATIVE
                    _, proto_idx, name_idx = struct.unpack_from("<HHI", buf, method_ids_off + 8 * idx)
                    out.setdefault(type_name(cls), set()).add((string(name_idx), proto(proto_idx)))
    return out


def apk_natives(path):
    out = {}
    with zipfile.ZipFile(path) as z:
        for n in z.namelist():
            if n.startswith("classes") and n.endswith(".dex") and "/" not in n:
                for cls, ms in dex_natives(z.read(n)).items():
                    out.setdefault(cls, set()).update(ms)
    return out


def partition(run, natives):
    """Split one run of adjacent JNINativeMethod entries into consecutive
    windows, each a subset of one app class's native methods (a class may
    declare natives nobody registers: AOSP's AdapterService has three).
    Returns [(class or None, entries)]: None marks entries no class explains.
    Prefers few unexplained entries, then few unregistered declarations."""
    n = len(run)
    best = [None] * (n + 1)      # best[i]: ((unexplained, unregistered), parts) for run[i:]
    best[n] = ((0, 0), [])
    for i in range(n - 1, -1, -1):
        options = []
        for cls, ms in natives.items():
            j = i
            seen = set()
            while j < n and run[j] in ms and run[j] not in seen:
                seen.add(run[j])
                j += 1
            for k in range(i + 1, j + 1):
                (u, m), parts = best[k]
                options.append(((u, m + len(ms) - (k - i)), [(cls, run[i:k])] + parts))
        (u, m), parts = best[i + 1]
        if parts and parts[0][0] is None:
            options.append(((u + 1, m), [(None, [run[i]] + parts[0][1])] + parts[1:]))
        else:
            options.append(((u + 1, m), [(None, [run[i]])] + parts))
        best[i] = min(options, key=lambda o: o[0])
    return best[0][1]


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    runs = elf_tables(sys.argv[1])
    natives = apk_natives(sys.argv[2])
    print("%s: %d runs of JNI tables, %d methods" % (sys.argv[1], len(runs), sum(len(t) for t in runs)))
    print("%s: %d classes with native methods, %d methods" % (sys.argv[2], len(natives),
                                                            sum(len(m) for m in natives.values())))
    matched, stray = {}, []
    for run in runs:
        for cls, entries in partition(run, natives):
            if cls is None:
                stray.extend(entries)
            else:
                matched.setdefault(cls, []).append(set(entries))
    bad = 0
    for cls in sorted(natives):
        tables = matched.get(cls, [])
        if len(tables) == 1:
            unreg = natives[cls] - tables[0]
            print("  ok   %s (%d registered%s)" % (cls, len(tables[0]),
                  ", %d declared only" % len(unreg) if unreg else ""))
            for name, sig in sorted(unreg):
                print("         declared, never registered (UnsatisfiedLinkError if called): %s %s"
                      % (name, sig))
        else:
            bad += 1
            print("  FAIL %s (%d native methods): %s" % (cls, len(natives[cls]),
                  "no table in the library" if not tables else "%d tables" % len(tables)))
    if stray:
        bad += 1
        print("  FAIL the library registers %d methods no app class declares:" % len(stray))
        for name, sig in stray:
            print("         %s %s" % (name, sig))
    print("MATCH" if not bad else "MISMATCH")
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
