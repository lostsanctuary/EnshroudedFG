"""Embed a SPIR-V binary as a C++ uint32_t array.

Usage: python -I tools/embed_spirv.py <input.spv> <output.h> <array_name>
"""
import struct
import sys


def main():
    src, dst, name = sys.argv[1], sys.argv[2], sys.argv[3]
    data = open(src, "rb").read()
    if len(data) % 4:
        raise SystemExit(f"{src}: size {len(data)} is not a multiple of 4")
    words = struct.unpack(f"<{len(data) // 4}I", data)
    lines = [", ".join(f"0x{w:08x}" for w in words[i:i + 8]) for i in range(0, len(words), 8)]
    with open(dst, "w", newline="\n") as f:
        f.write(f"// generated from {src.replace(chr(92), '/').split('/')[-1]} by tools/embed_spirv.py - do not edit\n")
        f.write("#pragma once\n#include <cstdint>\n")
        f.write(f"static const uint32_t {name}[] = {{\n    " + ",\n    ".join(lines) + "\n};\n")


if __name__ == "__main__":
    main()
