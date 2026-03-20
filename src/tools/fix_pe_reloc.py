#!/usr/bin/env python3
"""
Fix the PE Base Relocation Data Directory size to match the .reloc
section's VirtualSize, preventing EDK2 from reading zero-padded
relocation blocks (SizeOfBlock==0 triggers RETURN_LOAD_ERROR).
"""
import struct, sys

path = sys.argv[1]
with open(path, "r+b") as f:
    data = f.read()
    pe = data.find(b"PE\x00\x00")
    if pe < 0:
        sys.exit("Not a PE file")

    num_sect = struct.unpack_from("<H", data, pe + 6)[0]
    opt_hdr_size = struct.unpack_from("<H", data, pe + 20)[0]
    sect_start = pe + 24 + opt_hdr_size

    # Data Directory entry 5 (Base Relocation) offset
    dd5_off = pe + 24 + 112 + 5 * 8  # optional_header + 112 + entry_index * 8

    for i in range(num_sect):
        s = sect_start + i * 40
        name = data[s:s+8].rstrip(b"\x00")
        if name == b".reloc":
            vsize = struct.unpack_from("<I", data, s + 8)[0]
            f.seek(dd5_off + 4)
            f.write(struct.pack("<I", vsize))
            break
