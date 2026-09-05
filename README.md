# EZ3FS

EZ3FS is an independent filesystem tool for the 32-MiB EZ-Flash Advance III
NOR cartridge. It does not contain the original EZ3 menu, loader, ROM catalog,
FAT partition, or ROM-patching workflow.

Application version: **0.44.0**.

`ez3fs` manages the live filesystem. `ezfs-legacy` preserves the packed-image
workflow. Two incompatible formats are supported:

| Format | Purpose | Cartridge updates |
|---|---|---|
| EZ3FS 1.2 (`.ez3fs`) | Compact indexed archive | Complete cartridge rewrite |
| EZFA3FS 2.0.0 (`.ezfa3fs`) | Transactional copy-on-write filesystem | Individual 64-KiB block transactions |

See [EZ3FS_FORMAT.md](EZ3FS_FORMAT.md) and
[EZFA3FS_FORMAT.md](EZFA3FS_FORMAT.md) for their binary layouts and
consistency rules.

## Project status

The following operations have been tested on a physical cartridge:

- Complete EZ3FS and EZFA3FS programming with byte-for-byte verification.
- Reading, erasing, programming, and verifying individual cartridge blocks.
- Reading and mounting cartridge contents through FUSE/macFUSE.
- Direct writable EZFA3FS mounting from the command line and Finder.
- Creating, replacing, reading, and deleting files.
- Creating, traversing, renaming, and deleting directories.
- Recovery from transient USB endpoint stalls and erase/program readback
  mismatches through bounded verified retries.

Direct writable mounting buffers FUSE write chunks until explicit `fsync` or
close, uses
range-based reads, and verifies successful data-block transactions without
reconnecting. Metadata verification and USB-error recovery still reopen the
device when required. Every programmed or erased block retains readback
verification.

EZFA3FS garbage collection can reclaim unreferenced blocks left by file
replacement, deletion, or interrupted writes. Active extents and both
metadata superblocks are never erased by the collector.

Unix permission bits are not part of either image format. FUSE exposes fixed
`0755` directory and `0644` file modes; `chmod` on a writable mount is
accepted as a compatibility no-op so standard copy tools can complete.

## Build

The project requires a C++17 compiler and CMake 3.20 or newer. FUSE 3 and
libusb support are enabled automatically when their `pkg-config` packages are
available.

```sh
cmake -S . -B build/cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build/cmake --parallel
ctest --test-dir build/cmake --output-on-failure
```

The build produces the EZFA3FS tool and the separate packed-image legacy
tool:

```sh
./build/cmake/ez3fs --version
./build/cmake/ezfs-legacy --version
```

A direct Make build is also available:

```sh
make
./ez3fs --version
./ezfs-legacy --version
```

Commands that do not need FUSE or USB remain available when those optional
dependencies are absent.

## Legacy packed EZ3FS images

`ezfs-legacy` owns the original packed `.ez3fs` workflow. It intentionally
exposes only archive creation, inspection, extraction, directory creation,
file add/remove, and the packed-cartridge commands below.

```sh
./build/cmake/ezfs-legacy create cartridge.ez3fs
./build/cmake/ezfs-legacy mkdir cartridge.ez3fs documents
./build/cmake/ezfs-legacy add cartridge.ez3fs local.txt documents/local.txt
./build/cmake/ezfs-legacy list cartridge.ez3fs
./build/cmake/ezfs-legacy verify cartridge.ez3fs
./build/cmake/ezfs-legacy extract cartridge.ez3fs output
./build/cmake/ezfs-legacy rm cartridge.ez3fs documents/local.txt
```

## Legacy packed EZ3FS on a cartridge

Read-only cartridge commands do not erase or program flash:

```sh
./build/cmake/ezfs-legacy card-info
./build/cmake/ezfs-legacy card-list
./build/cmake/ezfs-legacy card-verify
./build/cmake/ezfs-legacy card-extract output
./build/cmake/ezfs-legacy card-pull cartridge-backup.ez3fs
./build/cmake/ezfs-legacy card-mount mountpoint
```

`card-pull` verifies the archive and refuses to overwrite an existing output
path. A read-only cartridge mount loads and verifies the image, closes the USB
session, and then exposes the in-memory contents through FUSE.

Program a packed image only when a complete cartridge replacement is intended:

```sh
./build/cmake/ezfs-legacy card-write cartridge.ez3fs
```

The image is validated before USB programming begins. The command displays a
warning, asks for yes/no confirmation, erases the complete cartridge, programs
the image at offset zero, and verifies every byte.

### Staged writable cartridge workflow

Packed EZ3FS does not support live block updates. Its writable cartridge
workflow therefore copies the cartridge into a local staging image:

```sh
./build/cmake/ezfs-legacy card-mount mountpoint \
  --writable working.ez3fs --foreground
```

The cartridge is unchanged while mounted. Inspect or commit the staging image
after unmounting:

```sh
./build/cmake/ezfs-legacy card-status working.ez3fs
./build/cmake/ezfs-legacy card-commit working.ez3fs
```

`card-status` is read-only. `card-commit` shows the added, modified, and
deleted paths; if the raw images differ, it requests yes/no confirmation and
performs a complete verified cartridge rewrite.

Before a staged writable mount starts, EZ3FS creates
`working.ez3fs.recovery.ez3fs`. It removes the snapshot after a clean unmount
and preserves it after an interrupted mount. Restore the staging image with:

```sh
./build/cmake/ezfs-legacy card-recover working.ez3fs
```

Recovery requests yes/no confirmation and only changes the local staging
image. It never writes to the cartridge.

## EZFA3FS images

Create and modify an exact 32-MiB transactional image:

```sh
./build/cmake/ez3fs format cartridge.ezfa3fs
./build/cmake/ez3fs mkdir cartridge.ezfa3fs documents
./build/cmake/ez3fs put cartridge.ezfa3fs local.txt documents/local.txt
./build/cmake/ez3fs list cartridge.ezfa3fs
./build/cmake/ez3fs verify cartridge.ezfa3fs
./build/cmake/ez3fs get cartridge.ezfa3fs documents/local.txt output.txt
./build/cmake/ez3fs rm cartridge.ezfa3fs documents/local.txt
./build/cmake/ez3fs rmdir cartridge.ezfa3fs documents
./build/cmake/ez3fs gc cartridge.ezfa3fs
./build/cmake/ez3fs compact cartridge.ezfa3fs
./build/cmake/ez3fs space cartridge.ezfa3fs
```

Create a loaderless direct-boot image for the single-ROM experiment in
`ezfadvanceIII`:

```sh
./build/cmake/ez3fs format --direct-boot direct-boot.ezfa3fs ROM.gba
./build/cmake/ez3fs verify direct-boot.ezfa3fs
./build/cmake/ez3fs card-write direct-boot.ezfa3fs
```

This dedicated layout places the unchanged root-level `.gba` file at cartridge
offset zero. Its EZFA3FS metadata occupies the final 128 KiB, so the ROM limit
is 31.875 MiB. To prepare an empty cartridge and write the ROM later, omit the
ROM argument, program the empty image, then use a writable `card-mount` or
`put` once:

```sh
./build/cmake/ez3fs format --direct-boot empty-direct-boot.ezfa3fs
./build/cmake/ez3fs put empty-direct-boot.ezfa3fs ROM.gba
```

The first accepted file must be one root-level `.gba`; it is placed at offset
zero and pinned there while present. Later files and directories are stored only
after its reserved 64-KiB-block boot slot. The boot ROM can be deleted and
replaced through a writable mount. If its replacement is larger, the empty slot
grows automatically through adjacent unreferenced blocks; an occupied adjacent
extent produces an explicit capacity error. Finder sidecars are rejected until
the ROM is present.

When `put` has no destination argument, the source path is used as the
destination:

```sh
./build/cmake/ez3fs put cartridge.ezfa3fs documents/readme.txt
```

Local EZFA3FS FUSE mounting is read-only:

```sh
./build/cmake/ez3fs mount cartridge.ezfa3fs mountpoint --foreground
```

## EZFA3FS on a cartridge

Program or pull a complete EZFA3FS image:

```sh
./build/cmake/ez3fs card-write cartridge.ezfa3fs
./build/cmake/ez3fs card-pull cartridge-backup.ezfa3fs
```

`card-write` validates the exact 32-MiB image, asks for yes/no
confirmation, replaces the complete cartridge, and verifies it.
`card-pull` reads all 32 MiB, validates the newest generation and every
file checksum, and writes the local output image.

Reclaim unreferenced cartridge data blocks only while it is unmounted:

```sh
./build/cmake/ez3fs card-gc
./build/cmake/ez3fs card-compact
./build/cmake/ez3fs card-space
```

The mutating cartridge commands ask for confirmation and must run while the
FUSE mount is unmounted. Garbage collection preserves every block referenced
by the active generation and verifies each physical erase. Compaction first
collects garbage, then transactionally relocates active file extents toward
the start of the data area to create a larger contiguous free tail.

`space` and `card-space` are read-only. They report active, erased,
and reclaimable blocks together with the largest file extent available now
and after garbage collection. Run cartridge diagnostics only while the FUSE
mount is unmounted.

Mount the cartridge as a verified read-only snapshot:

```sh
./build/cmake/ez3fs card-mount mountpoint
```

For direct writes to cartridge flash, foreground mode is mandatory:

```sh
./build/cmake/ez3fs card-mount mountpoint \
  --writable --foreground
```

The default writable mount validates the redundant metadata generations, keeps
the USB session open, and starts without reading every referenced file. It does
not rescan the complete free tail during mount. To checksum every referenced
file before mounting, use the slower optional preflight, which displays
block-based progress:

```sh
./build/cmake/ez3fs card-mount mountpoint \
  --writable --foreground --verify
```

Finder and terminal mutations are committed directly through copy-on-write
transactions. If a write cannot find an erased extent,
the filesystem automatically collects garbage and, when necessary, compacts
active extents before retrying. The terminal reports when this slower
maintenance path begins. `ENOSPC` is returned only when no sufficient extent
can be made available. Do not run another cartridge command or disconnect the
writer while this mount is active.

On macOS, writable cartridge mounts use a 600-second daemon timeout for long
flash transactions and suppress AppleDouble and `.DS_Store` traffic. Extended
attribute writes are accepted and discarded because EZ3FS does not persist
them. Intermediate macFUSE `flush` requests only check mount health; explicit
`fsync` and final `release` commit each accumulated file once. Newly created
files remain pending until their contents commit successfully, so a failed copy
does not leave a committed zero-byte file.
Contiguous file blocks are programmed within one capture-compatible flash
write session, switching sessions only at 8-MiB hardware window boundaries.
Flash readback is reconciled through fresh USB sessions before a block is
erased and retried. If a commit still fails, the mount remains readable but
rejects every later mutation before it reaches the cartridge; unmount and
remount before attempting another write.
Writer reinitialization is bounded and retried; a failed reinitialization
aborts maintenance safely without issuing another USB erase or program command.

Unmount from another terminal before disconnecting:

```sh
diskutil unmount mountpoint       # macOS
fusermount3 -u mountpoint         # Linux
```

macOS may create `.DS_Store` and `._*` AppleDouble files. They are ordinary
EZFA3FS entries and consume flash blocks like other files. Close Finder
windows before deleting them if Finder immediately recreates them.

## Block diagnostics

The following commands are intended for hardware diagnosis and development:

```sh
./build/cmake/ez3fs card-read-block 2 block-2.bin
./build/cmake/ez3fs card-erase-plan 2
./build/cmake/ez3fs card-erase-block 2
./build/cmake/ez3fs card-program-block 2 block-2.bin
```

`card-erase-plan` is always a dry run. Direct erase and program commands
are restricted to blocks 2 through 511, request yes/no confirmation, and
perform readback verification. Blocks 0 and 1 contain transactional-layout
metadata and cannot be modified by these diagnostic commands. Direct-boot
metadata instead occupies blocks 510 and 511, so do not use the destructive
diagnostic commands on those blocks when that layout is installed.

## Safety notes

- Keep a verified `card-pull` backup before experiments.
- Never disconnect the USB writer during erase, program, or writable mounting.
- Use only one cartridge command or mount at a time.
- A failed EZFA3FS transaction keeps the previous valid superblock
  generation. Remounting selects the newest complete generation.
- Physical writes are verified. Transient stalls and mismatches are retried up
  to three times; an operation fails with an I/O error if verification still
  does not match.
- EZ3FS and EZFA3FS are incompatible. Use commands matching the format on
  the cartridge.
