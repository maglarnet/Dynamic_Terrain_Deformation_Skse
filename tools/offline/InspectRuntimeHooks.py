"""Read-only EXE/Address Library audit of terrain's three call hooks.

Requires capstone. Supply --exe and --library paths; optional --out saves JSON.
Supports compressed format 1/2 and dense format 5 Address Libraries.
Checks instruction boundaries and exact unmodified call targets, not just E8.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

from capstone import Cs, CS_ARCH_X86, CS_MODE_64, CS_OP_IMM


def library(path):
    data = Path(path).read_bytes()
    fmt, *version = struct.unpack_from("<5I", data)
    if fmt == 5:
        count = struct.unpack_from("<I", data, 92)[0]
        assert len(data) == 96 + count * 4, "invalid dense library length"
        offsets = dict(enumerate(struct.unpack_from(f"<{count}I", data, 96)))
    elif fmt in (1, 2):
        cursor = 20

        def read(code):
            nonlocal cursor
            value = struct.unpack_from("<" + code, data, cursor)[0]
            cursor += struct.calcsize(code)
            return value

        cursor += 4 + struct.unpack_from("<I", data, cursor)[0]
        pointer_size, count = read("I"), read("I")
        offsets, previous_id, previous_offset = {}, 0, 0

        def delta(kind, previous):
            if kind == 0: return read("Q")
            if kind == 1: return previous + 1
            if kind == 2: return previous + read("B")
            if kind == 3: return previous - read("B")
            if kind == 4: return previous + read("H")
            if kind == 5: return previous - read("H")
            if kind == 6: return read("H")
            if kind == 7: return read("I")
            raise ValueError("unknown compressed mapping")

        for _ in range(count):
            kind = read("B")
            ident = delta(kind & 15, previous_id)
            high = kind >> 4
            offset = delta(high & 7, previous_offset // pointer_size if high & 8 else previous_offset)
            if high & 8: offset *= pointer_size
            offsets[ident] = offset
            previous_id, previous_offset = ident, offset
    else:
        raise ValueError(f"unsupported library format {fmt}")
    return version, offsets


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True)
    parser.add_argument("--library", required=True)
    parser.add_argument("--out")
    args = parser.parse_args()
    data = Path(args.exe).read_bytes()
    version, offsets = library(args.library)
    pe = struct.unpack_from("<I", data, 60)[0]
    assert data[pe:pe + 4] == b"PE\0\0"
    optional = pe + 24
    assert struct.unpack_from("<H", data, optional)[0] == 0x20B, "requires PE64"
    sections_at = optional + struct.unpack_from("<H", data, pe + 20)[0]
    sections = [struct.unpack_from("<IIII", data, sections_at + i * 40 + 8)
                for i in range(struct.unpack_from("<H", data, pe + 6)[0])]

    def read(rva, size):
        for virtual_size, base, raw_size, raw in sections:
            if base <= rva and rva + size <= base + raw_size:
                return data[raw + rva - base:raw + rva - base + size]
        raise ValueError(f"RVA {rva:X} has no complete raw range")

    # PE64 exception directory supplies instruction-aligned code regions.
    # A function can have chained unwind entries (the main-update caller does).
    pdata_rva, pdata_size = struct.unpack_from("<II", data, optional + 112 + 3 * 8)
    bounds = list(struct.iter_unpack("<III", read(pdata_rva, pdata_size)))
    current = version == [1, 7, 104, 0]
    if not current and version != [1, 6, 640, 0]:
        raise ValueError("audit profiles currently cover 1.6.640.0 and 1.7.104.0 only")
    sites = [("render_depth", 36559, 0x3B3 if current else 0x395, 107139),
             ("main_update", 36544, 0x160, 36564),
             ("post_opaque_snow", 106583, 0x308, 106587)]
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    decoder.detail = True
    report = {"exe": args.exe, "sha256": hashlib.sha256(data).hexdigest(),
              "library_version": version, "calls": []}
    for name, caller_id, delta, target_id in sites:
        start = offsets[caller_id]
        region, end, _ = next(b for b in bounds if b[0] <= start + delta < b[1])
        instructions = list(decoder.disasm(read(region, end - region), region))
        index = next(i for i, ins in enumerate(instructions) if ins.address == start + delta)
        ins = instructions[index]
        assert ins.mnemonic == "call" and ins.size == 5 and ins.bytes[0] == 0xE8
        assert ins.operands[0].type == CS_OP_IMM and ins.operands[0].imm == offsets[target_id]
        report["calls"].append({"name": name, "caller_id": caller_id,
                                "offset": hex(delta), "rva": hex(ins.address),
                                "bytes": bytes(ins.bytes).hex(" "), "target_id": target_id,
                                "target_rva": hex(offsets[target_id]),
                                "context": [f"{i.address:X}: {i.mnemonic} {i.op_str}"
                                            for i in instructions[max(0, index - 3):index + 2]]})
    output = json.dumps(report, indent=2)
    if args.out:
        Path(args.out).parent.mkdir(parents=True, exist_ok=True)
        Path(args.out).write_text(output + "\n", encoding="utf8")
    print(output)


if __name__ == "__main__":
    main()
