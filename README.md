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

## What it does

1. Parses the FDT. It looks up the console (`stdout-path` or the first
   `arm,pl011`), the first `/memory` range, the initrd from
   `/chosen linux,initrd-*`, the timebase from `CNTFRQ_EL0`, and the boot
   CPU's MPIDR.
2. Loads the Mach-O kernel at a 2 MB-aligned physical base past everything
   the firmware placed in RAM. The kernel's bootstrap page tables use 2 MB
   blocks.
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
4. Places the DT, RAM disk and `boot_args` after the kernel. The command
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
- **`/options`** is added, empty; IOKit publishes NVRAM there.
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
  prints the previous boot's panic log when it finds one (then marks it
  read, as iBoot does). As on Apple hardware, `dram-size` covers the
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

On Raspberry Pi boards (`brcm,bcm2835-mbox` in the FDT), `mbox.c` asks the
VideoCore firmware for a 32 bpp framebuffer at the current display size,
or 1024×768 when none is reported, and fills `boot_args.Video`. xnu's video
console then shows the boot log, alongside the serial console. With
`serial=3`, xnu moves the console to the UART only once KDP is set up.

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

- `pieboot-kernel=NAME`: the bundle's kernel to run.
- `pieboot.addmem=BASE,SIZE`: RAM the firmware does not report.
- `pieboot.memhole=BASE,SIZE`: a range to take out of RAM.

## Running and debugging

QEMU boots pieboot with `-kernel`, the bundle as the initrd. On `virt`,
with a GICv2 and EL2:

```bash
qemu-system-aarch64 -M virt,virtualization=on,gic-version=2 -cpu cortex-a72 -m 2G -nographic -kernel pieboot.bin -initrd xnu.bundle -append "debug=0x8 serial=3"
```

`-M raspi4b` works too, with `pieboot.addmem` for the RAM its FDT leaves
out. The boot-args here are only examples.

On hardware, pieboot prints on the UART and, once it has a framebuffer, on
HDMI; the ACT LED signals where it got to.

## License

pieboot is under the BSD 2-Clause license (see `LICENSE`). The console
font in `font.c` is Spleen 8x16 by Frederic Cambus, also BSD 2-Clause; its
copyright notice is kept in that file.
