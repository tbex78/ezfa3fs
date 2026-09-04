# EZ3FS

EZ3FS is an independent filesystem tool for the 32-MiB EZ-Flash Advance III
NOR cartridge. It does not contain the original EZ3 menu, loader, ROM catalog,
FAT partition, or ROM-patching workflow.

Application version: **0.31.0**.

Two incompatible formats are supported:

| Format | Purpose | Cartridge updates |
|---|---|---|
| EZ3FS 1.2 (`.ez3fs`) | Compact indexed archive | Complete cartridge rewrite |
| EZ3FS-LIVE 1.0.0 (`.ez3live`) | Transactional copy-on-write filesystem | Individual 64-KiB block transactions |

See [EZ3FS_FORMAT.md](EZ3FS_FORMAT.md) and
[EZ3FS_LIVE_FORMAT.md](EZ3FS_LIVE_FORMAT.md) for their binary layouts and
consistency rules.

## Project status

The following operations have been tested on a physical cartridge:

- Complete EZ3FS and EZ3FS-LIVE programming with byte-for-byte verification.
- Reading, erasing, programming, and verifying individual cartridge blocks.
- Reading and mounting cartridge contents through FUSE/macFUSE.
- Direct writable EZ3FS-LIVE mounting from the command line and Finder.
- Creating, replacing, reading, and deleting files.
- Creating, traversing, renaming, and deleting directories.
- Recovery from transient USB endpoint stalls and erase/program readback
  mismatches through bounded verified retries.

Direct writable mounting buffers FUSE write chunks until flush or close, uses
range-based reads, and verifies successful data-block transactions without
reconnecting. Metadata verification and USB-error recovery still reopen the
device when required. Every programmed or erased block retains readback
verification.

EZ3FS-LIVE does not yet implement garbage collection. Replaced and deleted
file blocks are not reused, so repeated writes reduce the free-block count
until the image is reformatted or rebuilt.

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

The resulting executable is:

```sh
./build/cmake/ez3fs --version
```

A direct Make build is also available:

```sh
make
./ez3fs --version
```

Commands that do not need FUSE or USB remain available when those optional
dependencies are absent.

## EZ3FS packed images

Create and edit a compact `.ez3fs` image:

```sh
./build/cmake/ez3fs create cartridge.ez3fs
./build/cmake/ez3fs mkdir cartridge.ez3fs documents
./build/cmake/ez3fs add cartridge.ez3fs local.txt documents/local.txt
./build/cmake/ez3fs list cartridge.ez3fs
./build/cmake/ez3fs verify cartridge.ez3fs
./build/cmake/ez3fs extract cartridge.ez3fs output
./build/cmake/ez3fs rm cartridge.ez3fs documents/local.txt
./build/cmake/ez3fs rmdir cartridge.ez3fs documents
```

Mount an image read-only, or explicitly mount it writable:

```sh
mkdir -p mountpoint
./build/cmake/ez3fs mount cartridge.ez3fs mountpoint
./build/cmake/ez3fs mount cartridge.ez3fs mountpoint --writable --foreground
```

Writable image mounts rebuild and atomically replace the local image after
metadata mutations, `flush`, `fsync`, and clean unmount. They do not access a
physical cartridge.

## Packed EZ3FS on a cartridge

Read-only cartridge commands do not erase or program flash:

```sh
./build/cmake/ez3fs card-info
./build/cmake/ez3fs card-list
./build/cmake/ez3fs card-verify
./build/cmake/ez3fs card-extract output
./build/cmake/ez3fs card-pull cartridge-backup.ez3fs
./build/cmake/ez3fs card-mount mountpoint
```

`card-pull` verifies the archive and refuses to overwrite an existing output
path. A read-only cartridge mount loads and verifies the image, closes the USB
session, and then exposes the in-memory contents through FUSE.

Program a packed image only when a complete cartridge replacement is intended:

```sh
./build/cmake/ez3fs card-write cartridge.ez3fs
```

The image is validated before USB programming begins. The command displays a
warning, asks for yes/no confirmation, erases the complete cartridge, programs
the image at offset zero, and verifies every byte.

### Staged writable cartridge workflow

Packed EZ3FS does not support live block updates. Its writable cartridge
workflow therefore copies the cartridge into a local staging image:

```sh
./build/cmake/ez3fs card-mount mountpoint \
  --writable working.ez3fs --foreground
```

The cartridge is unchanged while mounted. Inspect or commit the staging image
after unmounting:

```sh
./build/cmake/ez3fs card-status working.ez3fs
./build/cmake/ez3fs card-commit working.ez3fs
```

`card-status` is read-only. `card-commit` shows the added, modified, and
deleted paths; if the raw images differ, it requests yes/no confirmation and
performs a complete verified cartridge rewrite.

Before a staged writable mount starts, EZ3FS creates
`working.ez3fs.recovery.ez3fs`. It removes the snapshot after a clean unmount
and preserves it after an interrupted mount. Restore the staging image with:

```sh
./build/cmake/ez3fs card-recover working.ez3fs
```

Recovery requests yes/no confirmation and only changes the local staging
image. It never writes to the cartridge.

## EZ3FS-LIVE images

Create and modify an exact 32-MiB transactional image:

```sh
./build/cmake/ez3fs live-format cartridge.ez3live
./build/cmake/ez3fs live-mkdir cartridge.ez3live documents
./build/cmake/ez3fs live-put cartridge.ez3live local.txt documents/local.txt
./build/cmake/ez3fs live-list cartridge.ez3live
./build/cmake/ez3fs live-verify cartridge.ez3live
./build/cmake/ez3fs live-get cartridge.ez3live documents/local.txt output.txt
./build/cmake/ez3fs live-rm cartridge.ez3live documents/local.txt
./build/cmake/ez3fs live-rmdir cartridge.ez3live documents
```

When `live-put` has no destination argument, the source path is used as the
destination:

```sh
./build/cmake/ez3fs live-put cartridge.ez3live documents/readme.txt
```

Local EZ3FS-LIVE FUSE mounting is read-only:

```sh
./build/cmake/ez3fs live-mount cartridge.ez3live mountpoint --foreground
```

## EZ3FS-LIVE on a cartridge

Program or pull a complete EZ3FS-LIVE image:

```sh
./build/cmake/ez3fs live-card-write cartridge.ez3live
./build/cmake/ez3fs live-card-pull cartridge-backup.ez3live
```

`live-card-write` validates the exact 32-MiB image, asks for yes/no
confirmation, replaces the complete cartridge, and verifies it.
`live-card-pull` reads all 32 MiB, validates the newest generation and every
file checksum, and writes the local output image.

Mount the cartridge as a verified read-only snapshot:

```sh
./build/cmake/ez3fs live-card-mount mountpoint
```

For direct writes to cartridge flash, foreground mode is mandatory:

```sh
./build/cmake/ez3fs live-card-mount mountpoint \
  --writable --foreground
```

The command asks for yes/no confirmation, scans the cartridge allocation,
keeps the USB session open, and commits Finder or terminal mutations directly
through copy-on-write transactions. Do not run another cartridge command or
disconnect the writer while this mount is active.

Unmount from another terminal before disconnecting:

```sh
diskutil unmount mountpoint       # macOS
fusermount3 -u mountpoint         # Linux
```

macOS may create `.DS_Store` and `._*` AppleDouble files. They are ordinary
EZ3FS-LIVE entries and consume flash blocks like other files. Close Finder
windows before deleting them if Finder immediately recreates them.

## Block diagnostics

The following commands are intended for hardware diagnosis and development:

```sh
./build/cmake/ez3fs live-card-read-block 2 block-2.bin
./build/cmake/ez3fs live-card-erase-plan 2
./build/cmake/ez3fs live-card-erase-block 2
./build/cmake/ez3fs live-card-program-block 2 block-2.bin
```

`live-card-erase-plan` is always a dry run. Direct erase and program commands
are restricted to data blocks 2 through 511, request yes/no confirmation, and
perform readback verification. Blocks 0 and 1 contain EZ3FS-LIVE metadata and
cannot be modified by these diagnostic commands.

## Safety notes

- Keep a verified `card-pull` or `live-card-pull` backup before experiments.
- Never disconnect the USB writer during erase, program, or writable mounting.
- Use only one cartridge command or mount at a time.
- A failed EZ3FS-LIVE transaction keeps the previous valid superblock
  generation. Remounting selects the newest complete generation.
- Physical writes are verified. Transient stalls and mismatches are retried up
  to three times; an operation fails with an I/O error if verification still
  does not match.
- EZ3FS and EZ3FS-LIVE are incompatible. Use commands matching the format on
  the cartridge.
