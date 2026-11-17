#!/usr/bin/env python3
"""Finds the WorldAmbiance record inside an AC2 Mac .forge archive.

  python3 find_ambiance.py DataMAC_Firenze.forge --mod "Loose Files" --out found

What it does: a .forge keeps its data in compressed sections (the format is the
one in tests/forge and tests/gl/forge_shaders.cpp: an 8-byte marker, a header,
a table of chunk sizes, then per chunk a checksum and LZO1X data or raw bytes).
The script decompresses every section of the archive and looks for something
shaped like the mod's record. The mod's 21 files are all the same size and share
the same layout, so the bytes they have in common (type hashes and the like, not
the per-world IDs) make a fingerprint. Where the fingerprint lines up in the
decompressed data, that is the record. The best matches are written out.

Needs only Python 3. `pip install lzallright` makes it much faster (it is used
when installed); without it a built-in pure-Python LZO1X decoder runs.

Only reads: it never writes to the archive. What it writes (into --out) is a few
small files of about 2 KB each; keep them out of the repository.
"""
import argparse
import glob
import hashlib
import mmap
import os
import struct
import sys

MARKER = bytes([0x33, 0xAA, 0xFB, 0x57, 0x99, 0xFA, 0x04, 0x10])


# ---- LZO1X decoding --------------------------------------------------------

def lzo1x_decompress_py(src):
    """A pure-Python LZO1X decoder (the minilzo structure). Raises ValueError on bad data."""
    out = bytearray()
    ip = 0
    n = len(src)
    try:
        t = src[0]
        mode = "run"
        if t > 17:
            ip = 1
            t -= 17
            out += src[ip:ip + t]
            ip += t
            if t < 4:
                t = src[ip]
                ip += 1
                mode = "match"
            else:
                mode = "first"
        while True:
            if mode == "run":
                t = src[ip]
                ip += 1
                if t >= 16:
                    mode = "match"
                else:
                    if t == 0:
                        while src[ip] == 0:
                            t += 255
                            ip += 1
                        t += 15 + src[ip]
                        ip += 1
                    t += 3
                    out += src[ip:ip + t]
                    ip += t
                    mode = "first"
                    continue
            if mode == "first":
                t = src[ip]
                ip += 1
                if t >= 16:
                    mode = "match"
                else:
                    pos = len(out) - (1 + 0x0800) - (t >> 2) - (src[ip] << 2)
                    ip += 1
                    if pos < 0:
                        raise ValueError("bad distance")
                    for k in range(3):
                        out.append(out[pos + k])
                    t = src[ip - 2] & 3
                    if t == 0:
                        mode = "run"
                        continue
                    out += src[ip:ip + t]
                    ip += t
                    t = src[ip]
                    ip += 1
                    mode = "match"
            while mode == "match":
                if t >= 64:
                    pos = len(out) - 1 - ((t >> 2) & 7) - (src[ip] << 3)
                    ip += 1
                    cnt = (t >> 5) + 1
                elif t >= 32:
                    t &= 31
                    if t == 0:
                        while src[ip] == 0:
                            t += 255
                            ip += 1
                        t += 31 + src[ip]
                        ip += 1
                    pos = len(out) - 1 - ((src[ip] | (src[ip + 1] << 8)) >> 2)
                    ip += 2
                    cnt = t + 2
                elif t >= 16:
                    pos = len(out) - ((t & 8) << 11)
                    t &= 7
                    if t == 0:
                        while src[ip] == 0:
                            t += 255
                            ip += 1
                        t += 7 + src[ip]
                        ip += 1
                    pos -= (src[ip] | (src[ip + 1] << 8)) >> 2
                    ip += 2
                    if pos == len(out):
                        return bytes(out)  # end of stream
                    pos -= 0x4000
                    cnt = t + 2
                else:
                    pos = len(out) - 1 - (t >> 2) - (src[ip] << 2)
                    ip += 1
                    cnt = 2
                if pos < 0:
                    raise ValueError("bad distance")
                for k in range(cnt):
                    out.append(out[pos + k])
                t = src[ip - 2] & 3
                if t == 0:
                    mode = "run"
                    break
                out += src[ip:ip + t]
                ip += t
                t = src[ip]
                ip += 1
    except IndexError:
        raise ValueError("truncated")


try:  # the fast path, when available
    import lzallright

    def lzo_decompress(data, size):
        return lzallright.LZOCompressor.decompress(bytes(data), size)

    FAST = "lzallright"
except Exception:  # noqa: BLE001
    try:
        import lzo as _lzo

        def lzo_decompress(data, size):
            return _lzo.decompress(bytes(data), False, size)

        FAST = "python-lzo"
    except Exception:  # noqa: BLE001
        lzo_decompress = None
        FAST = None


def decompress_chunk(data, size):
    if lzo_decompress is not None:
        try:
            out = lzo_decompress(data, size)
            if len(out) == size:
                return out
        except Exception:  # noqa: BLE001
            pass
    out = lzo1x_decompress_py(data)
    if len(out) != size:
        raise ValueError("wrong size")
    return out


# ---- sections --------------------------------------------------------------

def decode_section(buf, s):
    """Decodes the section whose marker is at s. Returns (bytes, end) or (None, reason)."""
    n = len(buf)
    if s + 17 > n:
        return None, "short"
    codec = buf[s + 10]
    if codec != 0:
        return None, "codec%d" % codec  # the Windows archives use codec 2; this script is for the Mac's
    chunks = struct.unpack_from("<H", buf, s + 15)[0]
    data = s + 17 + 4 * chunks
    if data > n:
        return None, "bad"
    out = bytearray()
    for k in range(chunks):
        u, c = struct.unpack_from("<HH", buf, s + 17 + 4 * k)
        data += 4  # checksum
        if c > u or data + c > n:
            return None, "bad"
        piece = buf[data:data + c]
        if c < u:
            try:
                out += decompress_chunk(piece, u)
            except ValueError:
                return None, "bad"
        else:
            out += piece
        data += c
    return bytes(out), data


def sections(buf):
    """Yields (index, offset, decoded bytes) for every decodable section, and counts what it skips."""
    stats = {"ok": 0, "skipped": {}}
    at = 0
    idx = 0
    while True:
        s = buf.find(MARKER, at)
        if s < 0:
            break
        out, info = decode_section(buf, s)
        if out is None:
            stats["skipped"][info] = stats["skipped"].get(info, 0) + 1
            at = s + 1
            continue
        stats["ok"] += 1
        yield idx, s, out, stats
        idx += 1
        at = info


# ---- the fingerprint -------------------------------------------------------

def load_mod_files(path):
    files = sorted(glob.glob(os.path.join(path, "**", "*.WorldAmbiance"), recursive=True))
    if not files:
        sys.exit("no .WorldAmbiance files under %r" % path)
    data = [open(f, "rb").read() for f in files]
    sizes = {len(d) for d in data}
    if len(sizes) != 1:
        sys.exit("the mod files differ in size: %s" % sorted(sizes))
    return files, data


def fingerprint(data):
    """Offsets where all the mod files agree on 4 bytes that look like a hash, not a plain value."""
    ref = data[0]
    size = len(ref)
    anchors = []
    off = 0
    while off + 4 <= size:
        w = ref[off:off + 4]
        if len(set(w)) >= 3 and all(d[off:off + 4] == w for d in data):
            anchors.append((off, w))
            off += 4
        else:
            off += 1
    return size, anchors


def find_candidates(buf, size, anchors, seeds, min_score):
    found = {}
    for so, sw in seeds:
        p = buf.find(sw)
        while p >= 0:
            start = p - so
            if start >= 0 and start + size <= len(buf):
                hits = sum(1 for o, w in anchors if buf[start + o:start + o + 4] == w)
                score = hits / len(anchors)
                if score >= min_score:
                    found[start] = score
            p = buf.find(sw, p + 1)
    # drop near-duplicates (a start inside another candidate's record)
    best = []
    for start in sorted(found, key=lambda s: -found[s]):
        if all(abs(start - b) >= size for b, _ in best):
            best.append((start, found[start]))
    return best


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("forge", nargs="+", help="DataMAC_<World>.forge file(s)")
    ap.add_argument("--mod", required=True, help='the mod\'s "Loose Files" folder (its 21 .WorldAmbiance files)')
    ap.add_argument("--out", default="found", help="where to write the candidates (default: ./found)")
    ap.add_argument("--min-score", type=float, default=0.5, help="share of the fingerprint that must line up (default 0.5)")
    args = ap.parse_args()

    files, data = load_mod_files(args.mod)
    size, anchors = fingerprint(data)
    print("mod: %d files of %d bytes; fingerprint of %d anchors" % (len(files), size, len(anchors)))
    if len(anchors) < 8:
        sys.exit("too few common bytes to make a fingerprint")
    # seeds: anchors that occur once in a mod file, so a hit is a real lead
    seeds = [(o, w) for o, w in anchors if data[0].count(w) == 1][:4] or anchors[:4]
    print("LZO: %s" % (FAST or "built-in pure Python (slow: pip install lzallright)"))
    os.makedirs(args.out, exist_ok=True)

    for forge in args.forge:
        name = os.path.basename(forge)
        print("\n%s" % name)
        with open(forge, "rb") as f:
            buf = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
            results = []
            last = None
            print("  %d MB; decoding its sections (progress below, Ctrl+C to stop)" % (len(buf) >> 20))
            for idx, off, dec, stats in sections(buf):
                last = stats
                if idx % 50 == 0:
                    sys.stderr.write("\r  %d%% of the file, %d sections, %d candidate(s) so far   "
                                     % (100 * off // max(len(buf), 1), idx, len(results)))
                    sys.stderr.flush()
                for start, score in find_candidates(dec, size, anchors, seeds, args.min_score):
                    results.append((score, idx, off, start, dec))
            sys.stderr.write("\r" + " " * 70 + "\r")
            if last is None:
                print("  no decodable sections (is this a Mac archive?)")
                continue
            print("  sections decoded: %d; skipped: %s" % (last["ok"], last["skipped"] or "none"))
            results.sort(key=lambda r: -r[0])
            if not results:
                print("  no match at %.0f%% or more. Try --min-score 0.3" % (args.min_score * 100))
                continue
            for score, idx, off, start, dec in results[:6]:
                exact = dec[start:start + size]
                wide = dec[start:start + size + 512]
                tag = "%s_s%d_o%d" % (os.path.splitext(name)[0], idx, start)
                open(os.path.join(args.out, tag + "_%dbytes.bin" % size), "wb").write(exact)
                open(os.path.join(args.out, tag + "_wide.bin"), "wb").write(wide)
                same = sum(1 for a, b in zip(exact, data[0]) if a == b)
                print("  %3.0f%% match  section %d (at file offset %d), record at +%d  md5 %s  (%d of %d bytes equal the first mod file)"
                      % (score * 100, idx, off, start, hashlib.md5(exact).hexdigest()[:8], same, size))
            print("  wrote %d candidate(s) to %s/" % (min(len(results), 6), args.out))


if __name__ == "__main__":
    main()
