# pieboot

A small EL2 boot shim that lets xnu boot from a Linux-style boot path
(Raspberry Pi firmware, U-Boot or QEMU `-kernel`). xnu expects iBoot's
contract instead:

- entry at EL1 with the MMU off;
- `x0` = the physical address of `boot_args`;
- an Apple flattened device tree (ADT).

`pieboot.bin` carries a Linux arm64 `Image` header, so firmware loads it like
a Linux kernel and passes the FDT in `x0`. The xnu kernel comes from the
initrd, either as a bare Mach-O or as a pieboot bundle.

## Build

```bash
make
```

This builds `pieboot.bin` with clang-20 and ld.lld-20 (Ubuntu's `clang-20`,
`lld-20` and `llvm-20` packages). It is position-independent and relocates
itself.

`make test` runs the kernel's placement (`place.c`) on the build host,
through `place-test.c`'s layouts.

## What it does

1. Parses the FDT. It looks up the console (`stdout-path` or the first
   `arm,pl011`), the first `/memory` range, the initrd from
   `/chosen linux,initrd-*`, the timebase from `CNTFRQ_EL0`, and the boot
   CPU's MPIDR.
2. Loads the Mach-O kernel as low in the first RAM range as it fits, with
   what follows it (step 4): above pieboot itself (whose PSCI service and
   secondary-CPU entry stay in use), every `/memreserve/` entry there,
   32 MB (below that, xnu's mapping of its physical aperture wraps) and
   the display pool (below), and clear of the FDT, the initrd and the
   framebuffer. xnu manages RAM only
   from the kernel's physical base up, so RAM below it is lost; the FDT
   and the initrd may lie above it, as on the Pi 4 and Pi 5, whose
   firmware leaves them just below `0x2F000000`, and become free memory
   once the kernel runs (pieboot has copied what it needs from them). On
   those boards the kernel goes at 48 MB (on the Pi 4 at 112 MB, above its
   display pool). The base is congruent to the
   kernel's virtual base modulo 32 MB, since the kernel's bootstrap page
   tables map it with L2 blocks.
3. Builds the ADT:
   - **`/chosen`:** `dram-base`, `dram-size`, a 256-byte `random-seed`, and
     the `memory-map` `RAMDisk` entry.
   - **`/cpus`:** `cpuN` nodes with `reg` = MPIDR.
   - **`/defaults`:** `serial-device`.
   - **`/arm-io`:** every FDT device. Its `ranges` base is the lowest device
     address, and `reg` values become `{u64 offset, u64 size}` pairs relative
     to it, translated through any buses in between. Those buses (such as
     the Pi 4's `emmc2bus`) get an empty `ranges`, a 1:1 map for
     `IODTResolveAddressing`; their FDT map stays as `fdt,ranges`.
   - **Interrupt properties** (`interrupts`, `interrupt-parent`,
     `#interrupt-cells`, `interrupt-map`, `interrupt-map-mask`) are converted
     to native byte order.
   - **`interrupt-parent`** is written out explicitly wherever the FDT
     inherits it, because IOKit doesn't follow FDT inheritance.
   - **Other properties** keep their FDT (big-endian) encoding.
4. Places the DT, RAM disk (a copy) and `boot_args` after the kernel. The command
   line is FDT `bootargs` plus the bundle's `bootargs`. The RAM disk is
   padded with zeroes to a 16 KB multiple, because memdev counts whole pages.
5. Drops from EL2 to EL1 (`HCR_EL2.RW`, timer access, `CNTVOFF` = 0) and
   enters the kernel. Each CPU leaves `VBAR_EL2` and an EL2 stack behind
   for the PSCI service below.

Other details of the converted tree:

- **GIC interrupts** become one cell each, the GIC interrupt ID (SPI n →
  n + 32, PPI n → n + 16), and the GIC's `#interrupt-cells` becomes 1.
  IOKit's interrupt controllers take the first specifier cell as the vector
  number, as with Apple's AIC.
- **CPU nodes** carry their PMU interrupt; `AppleARMSMP` requires an
  interrupt on every CPU nub.
- **`/chosen`** also gets:
  - `pieboot,secondary-entry`: the EL2→EL1 trampoline for PSCI `CPU_ON`;
  - the NVRAM geometry (`nvram-bank-size`/`-count`/`current-bank`; NVRAM
    starts empty and lives in RAM);
  - `kernel-ctrr-to-be-enabled` = 0, since there is no CTRR.
  - `pieboot,image`, `pieboot,fdt`, `pieboot,initrd` and, when the FDT has
    any, `pieboot,memreserve`: where pieboot itself, the firmware's FDT and
    initrd and its `/memreserve/` entries were, as `{u64 base, u64 size}`
    pairs, for the record (`ioreg -p IODeviceTree -n chosen`).
- **`/options`** is added, empty; IOKit publishes NVRAM there.
- **Display pool:** RAM set aside for the buffers a display scans out,
  which the IOSurface kext (1.3 and later) hands out: on the BCM2711, whose
  display reaches only the low GB, 96 MB by default (none elsewhere;
  `pieboot.dmapool=SIZE` sets it). Without it, wired pages scattered
  through the low GB leave xnu's contiguous allocator no 4K buffer's run
  there minutes after a boot, and soon no 1080p one. It lies low in the
  first RAM range, in the lowest gap above pieboot that holds it on a 2 MB
  boundary, clear of the `/memreserve/` entries (it may cover the FDT and
  the initrd, and an entry that only reserves either) and a framebuffer,
  and the kernel goes above it, so the VM never has it. It is published as
  `/chosen pieboot,display-pool` (`{u64 base, u64 size}`) and as a
  write-combined `/defaults pmap-io-ranges` entry (`{u64 base, u64 size,
  u32 wimg, u32 signature}`: `VM_WIMG_WCOMB`, `'DPOL'`): xnu maps memory
  it does not manage as a device's whatever cache mode IOKit asks for,
  unless such an entry says otherwise, and those entries must lie outside
  the memory it manages (so the pool cannot be a DRAM hole above the
  kernel). None is carved where it would leave less than half of the
  first RAM range or reach above 1 GB.
- **`pieboot,dma-ranges`**, on a device whose bus reaches RAM at other
  addresses than the CPU's: the FDT `dma-ranges` of every bus level above
  it, composed, as `{u64 CPU address, u64 bus address, u64 size}` triples.
- **`pieboot,bus-reg`**, on a device with `reg` ranges the CPU cannot reach
  (the BCM2712 MIP's MSI doorbell is a PCI address): all of its `reg`
  ranges as `{u64 address on the parent bus, u64 size}`. `reg` keeps only
  those the CPU can reach.
- **Panic region:** the top 512 KB of the last RAM range is kept out of
  the kernel's memory and published as `/pram` `reg` with
  `/chosen/embedded-panic-log-size`, iBoot's panic region. The kernel
  writes its panic log there, and since RAM survives a warm reset, pieboot
  prints the previous boot's panic log when it finds one. Unlike iBoot, it
  leaves the log in place: the kernel copies it out for user space and
  clears the region. As on Apple hardware, `dram-size` covers the
  region, so the kernel maps it as normal memory: mapped as a device, the
  packed panic header's unaligned fields faulted and panics printed
  nothing.
- **RAM:** all `/memory` ranges are merged. The kernel manages the span from
  the first to the last, and the gaps (the VideoCore carve-out, the
  peripheral window on 8 GB boards) go to `/chosen/dram-holes` as
  `{u64 base, u64 size}` pairs. The kernel keeps holes out of the VM and the
  physical aperture, and maps them only as I/O (the framebuffer lives in
  one). `pieboot.addmem=BASE,SIZE` in the boot-args adds a range the
  firmware does not report: QEMU's `raspi4b` describes only the first bank
  of its 2 GB.

## PSCI for spin-table boards

The Pi 4's stock armstub (and QEMU's `raspi4b`) offers no PSCI; secondary
cores wait on spin tables. When the FDT has no PSCI node and the shim runs
at EL2, `psci.c` serves PSCI 0.2 itself over `hvc`, and the ADT gets an
`/arm-io/psci` node with `method = "hvc"`:

- `PSCI_VERSION` returns 0.2.
- `CPU_ON` releases the target core's spin table to `pieboot_spin_entry`,
  which jumps to the requested entry with `x0` = the context ID.
- `SYSTEM_RESET` uses the BCM2835 PM watchdog.
- `SYSTEM_OFF` parks the CPU.

With firmware PSCI (TF-A, QEMU virt) the shim stays out of the way.

## Framebuffer

pieboot uses the firmware's `simple-framebuffer` (32 or 16 bpp). On
Raspberry Pi boards without one (`brcm,bcm2835-mbox` in the FDT), `mbox.c`
asks the VideoCore firmware for a 32 bpp framebuffer at the current display
size, or 1024×768 when none is reported. The kernel gets it in
`boot_args.Video`.

By default the screen is black with a pie (🥧) in the middle, and pieboot's
messages go only to the UART. The kernel gets `v_display` = 1, graphics
mode, as from Apple's `boot.efi`: xnu keeps its text console off the
screen, so the pie stays.

- **`-v`** in the boot-args (the FDT's or the bundle's) turns the screen
  into a text console: pieboot's messages, then the kernel's, since it gets
  `v_display` = 0. With `serial=3`, xnu moves the console to the UART only
  once KDP is set up.
- **No kernel to boot** (no initrd, an initrd that is neither a Mach-O
  kernel nor a bundle, a bundle without the kernel, a broken Mach-O): a
  question mark (❓) flashes over the pie, the ACT LED in step. With `-v`,
  the error is on the screen instead.
- **Any other fatal error** turns the screen to text, with pieboot's
  messages so far (the last 4 KB) and the error.
- **A previous boot's panic log** is shown as text for 15 seconds, then
  the pie comes back.

The pie and the question mark are Noto Emoji images, in `pie.c`, made by
`mkpie.py` from the 128 px PNGs of noto-emoji's 3D set (tag
`v2026-09-24-unicode18_0`).

## Bundles

```bash
./mkbundle.py -o xnu.bundle -k kernel.release.bcm2711 -r ramdisk.dmg -a "rd=md0"
```

A bundle (magic `PIEBUNDL`) carries one or more kernels, each with the
board compatibles and CPU features it needs, an optional RAM disk and
optional boot-args. pieboot runs the first kernel that fits the board,
unless the boot-arg `pieboot-kernel=NAME` names one. The format is
documented in `mkbundle.py`.

## Boot-args

pieboot reads these from the FDT's `bootargs` and the bundle's:

- `-v`: text on the screen, pieboot's and the kernel's, instead of the pie.
- `pieboot-kernel=NAME`: the bundle's kernel to run.
- `pieboot.addmem=BASE,SIZE`: RAM the firmware does not report.
- `pieboot.memhole=BASE,SIZE`: a range to take out of RAM.
- `pieboot.dmapool=SIZE`: the display pool's size, in bytes or with K, M
  or G (`96M`); 0 for none. By default 96 MB on the BCM2711, none
  elsewhere.

## Running and debugging

QEMU boots pieboot with `-kernel`, the bundle as the initrd. On `virt`,
with a GICv2 and EL2:

```bash
qemu-system-aarch64 -M virt,virtualization=on,gic-version=2 -cpu cortex-a72 -m 2G -nographic -kernel pieboot.bin -initrd xnu.bundle -append "debug=0x8 serial=3"
```

`-M raspi4b` works too, with `pieboot.addmem` for the RAM its FDT leaves
out. The boot-args here are only examples.

On hardware, pieboot prints on the UART and, with `-v`, on HDMI; the ACT
LED signals where it got to.

## License

pieboot is under the BSD 2-Clause license (see `LICENSE`). The console
font in `font.c` is Spleen 8x16 by Frederic Cambus, also BSD 2-Clause; its
copyright notice is kept in that file. The pie and question mark in `pie.c`
are Noto Emoji images, Copyright 2013 Google LLC, under the SIL Open Font
License 1.1 (see `LICENSE-noto-emoji`).
