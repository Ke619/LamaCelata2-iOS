#!/usr/bin/env python3
"""Extract one small, self-contained function from an i386 Mach-O as a test fixture.

Only functions from open-source libraries the game links statically may be
extracted (allow-listed name prefixes), and only up to a size cap. The output is
<out>/<symbol>.bin (raw code bytes) and <out>/<symbol>.json (provenance and a
quick analysis of what the code references).

Usage:
  tools/extract_fixture.py BINARY SYMBOL [SYMBOL ...] --out fixtures/ac2-1.0.1
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys

ALLOWED_PREFIXES = ("_lzo", "___lzo", "_adler32", "_crc32", "_inflate", "_deflate")
MAX_SIZE = 16 * 1024

MH_MAGIC = 0xFEEDFACE
LC_SEGMENT = 0x1
LC_SYMTAB = 0x2
N_STAB = 0xE0
N_TYPE = 0x0E
N_SECT = 0x0E


def parse_macho(data):
    magic, cputype, _, _, ncmds, _, _ = struct.unpack_from("<7I", data, 0)
    if magic != MH_MAGIC or cputype != 7:
        sys.exit("Not an i386 Mach-O (thin) file")
    sections, symtab = [], None
    off = 28
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<2I", data, off)
        if cmd == LC_SEGMENT:
            nsects = struct.unpack_from("<I", data, off + 48)[0]
            for i in range(nsects):
                s = off + 56 + i * 68
                sect = data[s:s + 16].rstrip(b"\0").decode()
                seg = data[s + 16:s + 32].rstrip(b"\0").decode()
                addr, size, fileoff = struct.unpack_from("<3I", data, s + 32)
                sections.append({"segname": seg, "sectname": sect, "addr": addr,
                                 "size": size, "offset": fileoff})
        elif cmd == LC_SYMTAB:
            symtab = struct.unpack_from("<4I", data, off + 8)
        off += cmdsize
    if symtab is None:
        sys.exit("No LC_SYMTAB")
    symoff, nsyms, stroff, _ = symtab
    syms = []
    for i in range(nsyms):
        strx, ntype, nsect, _, value = struct.unpack_from("<IBBhI", data, symoff + i * 12)
        if ntype & N_STAB or (ntype & N_TYPE) != N_SECT:
            continue
        end = data.index(b"\0", stroff + strx)
        syms.append((value, nsect, data[stroff + strx:end].decode("latin1")))
    return sections, syms


def symbol_extent(sections, syms, name):
    hits = [s for s in syms if s[2] == name]
    if not hits:
        sys.exit(f"Symbol not found: {name}")
    addr, nsect, _ = hits[0]
    sect = sections[nsect - 1]
    following = sorted({a for a, n, _ in syms if n == nsect and a > addr})
    end = following[0] if following else sect["addr"] + sect["size"]
    return addr, end - addr, sect


def analyse(binary, name, addr, size):
    """Summarise references using otool, when it is available (macOS)."""
    if not shutil.which("otool"):
        return None
    out = subprocess.run(["otool", "-tV", "-p", name, binary], capture_output=True,
                         text=True, check=True).stdout
    ins = [l for l in out.splitlines() if re.match(r"^[0-9a-f]{8}\t", l)
           and addr <= int(l[:8], 16) < addr + size]
    body = [l.split("\t", 1)[1] for l in ins]
    targets = [int(m.group(1), 16) for b in body
               for m in [re.search(r"^j\w+\s+0x([0-9a-f]+)", b)] if m]
    return {
        "instructions": len(ins),
        "mnemonics": sorted({b.split("\t")[0] for b in body}),
        "calls": [b for b in body if b.startswith("call")],
        "indirect_branches": [b for b in body if re.match(r"^(jmp|call)\w*\s+\*", b)],
        "jumps_leaving_function": [hex(t) for t in targets if not addr <= t < addr + size],
        "absolute_memory_refs": [b for b in body
                                 if re.search(r"(?<![-\w])0x[0-9a-f]{5,}\(|\$0x00[0-9a-f]{4,}\b", b)],
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("binary")
    ap.add_argument("symbols", nargs="+")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    data = open(a.binary, "rb").read()
    sections, syms = parse_macho(data)
    os.makedirs(a.out, exist_ok=True)
    for name in a.symbols:
        if not name.startswith(ALLOWED_PREFIXES):
            sys.exit(f"Refusing {name}: not an allow-listed open-source library symbol")
        addr, size, sect = symbol_extent(sections, syms, name)
        if size > MAX_SIZE:
            sys.exit(f"Refusing {name}: {size} bytes exceeds the {MAX_SIZE}-byte cap")
        start = sect["offset"] + (addr - sect["addr"])
        code = data[start:start + size]
        meta = {
            "symbol": name,
            "vmaddr": f"0x{addr:08x}",
            "size": size,
            "section": f"{sect['segname']},{sect['sectname']}",
            "code_sha256": hashlib.sha256(code).hexdigest(),
            "source_binary": {"name": os.path.basename(a.binary),
                              "sha1": hashlib.sha1(data).hexdigest()},
            "analysis": analyse(a.binary, name, addr, size),
        }
        base = os.path.join(a.out, name.lstrip("_"))
        open(base + ".bin", "wb").write(code)
        with open(base + ".json", "w") as fh:
            json.dump(meta, fh, indent=2)
            fh.write("\n")
        an = meta["analysis"] or {}
        print(f"{name}: {size} bytes at {meta['vmaddr']}, "
              f"{an.get('instructions', '?')} instructions, "
              f"calls={len(an.get('calls', []))}, "
              f"indirect={len(an.get('indirect_branches', []))}, "
              f"abs_refs={len(an.get('absolute_memory_refs', []))}")


if __name__ == "__main__":
    main()
