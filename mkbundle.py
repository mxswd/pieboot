#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""
Pack kernels, an optional RAM disk and optional boot-args into a pieboot
bundle, the initrd that pieboot loads.

usage: mkbundle.py -o OUT -k KERNEL [-k ...] [--default NAME] [-r RAMDISK] [-a "boot-args"]

A kernel is either a plain path (a bundle with one kernel, version 1) or
NAME=PATH[:COMPAT:COMPAT...][+FEATURE,FEATURE...]: one of several kernels,
for the boards whose device tree has one of those root compatibles and
whose CPU has those features (16k: the 16K translation granule; lse: LSE
atomics; pan: PAN). pieboot picks the first kernel that fits, else the
default (--default, or the first kernel without compatibles, or the
first), unless the boot-arg pieboot-kernel=NAME names one.

  mkbundle.py -o xnu.bundle \\
      -k bcm2712=kernel.development.bcm2712:brcm,bcm2712+16k,lse,pan \\
      -k bcm2711=kernel.development.bcm2711 -r root.hfs

Layout (little-endian), as parsed by main.c:
  char magic[8] = "PIEBUNDL"; u32 version; u32 count;
  count x { char name[16]; u64 offset; u64 size; }
  payloads, each 16 KB-aligned from the start of the bundle.
Version 1 has one entry "kernel". Version 2 has "kernel.NAME" entries and
"kernels", a text table of one line per kernel in matching order:
  NAME FEATURES COMPAT...       (FEATURES "-" when none)
and a last line "default NAME".
"""
import argparse
import re
import struct

MAGIC = b"PIEBUNDL"
ALIGN = 0x4000
FEATURES = {"16k", "lse", "pan"}


def align(n):
    return (n + ALIGN - 1) & ~(ALIGN - 1)


def parse_kernel(spec):
    """NAME=PATH[:COMPAT:...][+FEATURE,...] -> (name, path, compats, features)."""
    m = re.fullmatch(r"([A-Za-z0-9_-]+)=([^:+]+)(?::([^+]*))?(?:\+(.*))?", spec)
    if not m:
        raise SystemExit("mkbundle: bad kernel %r (NAME=PATH[:COMPAT:...][+FEATURE,...])" % spec)
    name, path, compats, feats = m.groups()
    if len(name) > 16 - len("kernel."):
        raise SystemExit("mkbundle: kernel name %r longer than 9 characters" % name)
    compats = [c for c in (compats or "").split(":") if c]
    feats = [f for f in (feats or "").split(",") if f]
    for f in feats:
        if f not in FEATURES:
            raise SystemExit("mkbundle: unknown CPU feature %r (known: %s)" % (f, ", ".join(sorted(FEATURES))))
    return name, path, compats, feats


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("-k", "--kernel", action="append", required=True,
                    help="Mach-O kernel, or NAME=PATH[:COMPAT:...][+FEATURE,...] (repeatable)")
    ap.add_argument("--default", help="the kernel for boards no other kernel matches")
    ap.add_argument("-r", "--ramdisk", help="RAM disk image (becomes md0)")
    ap.add_argument("-a", "--bootargs", help="appended to the FDT bootargs")
    args = ap.parse_args()

    entries = []
    if len(args.kernel) == 1 and "=" not in args.kernel[0]:
        version = 1
        with open(args.kernel[0], "rb") as f:
            entries.append(("kernel", f.read()))
    else:
        version = 2
        kernels = [parse_kernel(k) for k in args.kernel]
        names = [k[0] for k in kernels]
        if len(set(names)) != len(names):
            raise SystemExit("mkbundle: kernel names repeat")
        default = args.default or next((k[0] for k in kernels if not k[2]), names[0])
        if default not in names:
            raise SystemExit("mkbundle: no kernel named %r" % default)
        table = ""
        for name, path, compats, feats in kernels:
            with open(path, "rb") as f:
                entries.append(("kernel." + name, f.read()))
            table += "%s %s %s\n" % (name, ",".join(feats) or "-", " ".join(compats))
        table += "default %s\n" % default
        entries.append(("kernels", table.encode() + b"\0"))
    if args.ramdisk:
        with open(args.ramdisk, "rb") as f:
            entries.append(("ramdisk", f.read()))
    if args.bootargs:
        entries.append(("bootargs", args.bootargs.encode() + b"\0"))

    offset = align(16 + 32 * len(entries))
    placed = []
    for name, data in entries:
        placed.append((name, data, offset))
        offset = align(offset + len(data))

    with open(args.output, "wb") as out:
        out.write(MAGIC + struct.pack("<II", version, len(placed)))
        for name, data, off in placed:
            out.write(struct.pack("<16sQQ", name.encode(), off, len(data)))
        for _, data, off in placed:
            out.seek(off)
            out.write(data)
        out.truncate(offset)


if __name__ == "__main__":
    main()
