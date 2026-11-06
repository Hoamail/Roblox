"""Match a function from velext v1.0.0 against v1.0.2 by exact k-mer voting.

Builds are from the same source line: most functions survive with small edits,
so exact 32-byte windows still hit. The modal (target - source) delta tells us
where the function moved; the vote count tells us how sure we are.
"""
import struct
import sys
from collections import Counter

OLD = r"C:\Users\adminn\Documents\video\vel_work\velext_v1.exe"
NEW = r"C:\Users\adminn\ext_project\vel_runtime.exe"


def sections(path):
    with open(path, "rb") as f:
        data = f.read()
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, e_lfanew + 6)[0]
    opt_size = struct.unpack_from("<H", data, e_lfanew + 20)[0]
    sec_at = e_lfanew + 24 + opt_size
    out = []
    for i in range(nsec):
        off = sec_at + i * 40
        name = data[off:off + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, off + 8)
        out.append(dict(name=name, va=vaddr, vsize=vsize, raw=rawptr,
                        rawsize=rawsize, file=data))
    return out


def text_of(secs):
    for s in secs:
        if s["name"] == ".text":
            return s["file"][s["raw"]:s["raw"] + s["rawsize"]], s["va"]
    raise SystemExit("no .text")


def main(old_rva, length=512, k=32, stride=8):
    osec = sections(OLD)
    nsec = sections(NEW)
    obuf, ova = text_of(osec)
    nbuf, nva = text_of(nsec)
    o_off = old_rva - 0x1000  # .text is va=base+0x1000 -> raw offset rva-0x1000
    fn = obuf[o_off:o_off + length]

    votes = Counter()
    samples = 0
    for i in range(0, max(1, len(fn) - k), stride):
        pat = fn[i:i + k]
        if len(pat) < k:
            break
        pos = nbuf.find(pat)
        while pos != -1:
            votes[pos - i] += 1
            pos = nbuf.find(pat, pos + 1)
        samples += 1

    print(f"function rva 0x{old_rva:x} len={len(fn)} k={k} samples={samples}")
    for delta, n in votes.most_common(6):
        print(f"  delta={delta:+#x} votes={n}/{samples} -> v1.0.2 rva 0x{old_rva + delta:x}")
    if not votes:
        print("  no exact match (function rewritten or moved across edits)")


if __name__ == "__main__":
    rva = int(sys.argv[1], 0) if len(sys.argv) > 1 else 0xD9CC0
    ln = int(sys.argv[2], 0) if len(sys.argv) > 2 else 512
    main(rva, ln)
