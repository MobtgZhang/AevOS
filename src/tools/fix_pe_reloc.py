#!/usr/bin/env python3
"""
Minimal PE/COFF fixup for non-x86 EFI binaries.

Works around objcopy quirks when generating EFI applications
for aarch64 / riscv64 / loongarch64.
"""
import struct
import sys
import os

PE_SUBSYSTEM_EFI_APP = 10


def fix_pe(path):
    with open(path, 'r+b') as f:
        data = bytearray(f.read())

    if len(data) < 0x40:
        return

    pe_off = struct.unpack_from('<I', data, 0x3C)[0]
    if pe_off + 4 > len(data):
        return
    if data[pe_off:pe_off + 4] != b'PE\x00\x00':
        return

    coff_off = pe_off + 4
    opt_off = coff_off + 20

    if opt_off + 70 > len(data):
        return

    magic = struct.unpack_from('<H', data, opt_off)[0]
    if magic != 0x20B:  # PE32+
        return

    # Ensure Subsystem = EFI Application
    struct.pack_into('<H', data, opt_off + 68, PE_SUBSYSTEM_EFI_APP)

    # Align SizeOfHeaders to FileAlignment
    file_align = struct.unpack_from('<I', data, opt_off + 36)[0]
    if file_align == 0:
        file_align = 512
    size_of_headers = struct.unpack_from('<I', data, opt_off + 60)[0]
    aligned_hdr = (size_of_headers + file_align - 1) & ~(file_align - 1)
    if aligned_hdr != size_of_headers:
        struct.pack_into('<I', data, opt_off + 60, aligned_hdr)

    with open(path, 'wb') as f:
        f.write(data)


if __name__ == '__main__':
    for path in sys.argv[1:]:
        if os.path.isfile(path):
            fix_pe(path)
