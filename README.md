# EZFA3FS

EZFA3FS is an independent filesystem toolkit for the EZ-Flash Advance III cartridge. It does not use the original EZ3 menu, loader, ROM patching, FAT32, or a partition table.

The project provides the `ezfa3fs` program for the transactional **EZFA3FS**
format.

Application version: **0.50.4**. EZFA3FS is experimental format **0.1.0** in its standard layout and **0.2.0** in its slotted direct-boot layout. Images using the former 2.0.0 and 2.1.0 identifiers remain readable. Legacy EZFA3FS remains format **1.2**.

The macOS/macFUSE and real-cartridge workflow has been exercised with directories, file creation and reading, replacement, deletion, recursive deletion, large GBA ROM copies, garbage collection, compaction, cartridge pullback, verification, and SHA-256 comparison with source files.

Cartridge reads use save-safe two-byte control transfers. Before a writable cartridge session initializes the flash writer, EZFA3FS snapshots all four 32-KiB save banks. A clean unmount restores and verifies that snapshot, including after a filesystem mutation error. Always unmount before disconnecting the linker so this restoration can finish.

## Build

Requirements are CMake, a C++17 compiler, libusb-1.0, and macFUSE when mount support is wanted.

```sh
cmake -S . -B build/cmake
cmake --build build/cmake
ctest --test-dir build/cmake --output-on-failure
```

The binary is `build/cmake/ezfa3fs`.

## Standard image workflow

```sh
./build/cmake/ezfa3fs format cartridge.ezfa3fs
./build/cmake/ezfa3fs mkdir cartridge.ezfa3fs documents
./build/cmake/ezfa3fs put cartridge.ezfa3fs test.txt documents/test.txt
./build/cmake/ezfa3fs list cartridge.ezfa3fs
./build/cmake/ezfa3fs verify cartridge.ezfa3fs
./build/cmake/ezfa3fs card-write cartridge.ezfa3fs
```

`card-write` verifies the image, asks for `y/N` confirmation, then erases, programs, and verifies the complete 32 MiB cartridge. Pass `--skip-verification` to omit only the final cartridge read-back verification:

```sh
./build/cmake/ezfa3fs card-write cartridge.ezfa3fs --skip-verification
```

Format the cartridge directly without first creating and writing a complete image. The command erases the cartridge, fills all four save banks with `0x00`, writes only the required filesystem metadata, and verifies the cleared save banks and metadata:

```sh
./build/cmake/ezfa3fs card-format
./build/cmake/ezfa3fs card-format --direct-boot
```

Mount the cartridge read-only:

```sh
mkdir -p mountpoint
./build/cmake/ezfa3fs card-mount mountpoint --foreground
```

Mount with direct transactional writes:

```sh
./build/cmake/ezfa3fs card-mount mountpoint --writable --foreground
```

Add `--verify` for a full allocation scan before mounting. The default writable mount reads only the metadata needed to start, which is much faster on cartridges containing large files. Unmount from another terminal with `umount mountpoint`.

## Direct boot

The experimental direct-boot layout places one root-level GBA ROM at cartridge byte offset zero for compatible direct-boot logic in the companion `ezfadvanceIII` project.

Create it with a ROM:

```sh
./build/cmake/ezfa3fs format --direct-boot direct-boot.ezfa3fs game.gba
```

Or create an empty image with a 16 MiB boot slot and copy the first ROM through a writable mount later:

```sh
./build/cmake/ezfa3fs format --direct-boot direct-boot.ezfa3fs
```

The first persistent file must be a non-empty root-level `.gba` file. The slot expands when possible if the ROM needs more room. Additional files and directories live after the reserved slot, so direct boot remains compatible with a multi-file filesystem.

The ROM at offset zero is immutable while present. Delete it, then copy a new root-level `.gba` file to replace it. Deletion erases only logical block 0 to invalidate the old ROM immediately. When a replacement is copied, its required stale blocks are erased and the ROM is programmed in one capture-derived writer session; programmed blocks are then verified before the manifest is committed. Retries resume at the first unverified block. The reserved slot remains available. A direct-boot ROM may occupy at most 510 blocks (31.875 MiB).

## Command reference

Image commands:

```text
ezfa3fs format IMAGE.ezfa3fs
ezfa3fs format --direct-boot IMAGE.ezfa3fs [ROM.gba]
ezfa3fs list IMAGE.ezfa3fs
ezfa3fs verify IMAGE.ezfa3fs
ezfa3fs mkdir IMAGE.ezfa3fs DIRECTORY
ezfa3fs put IMAGE.ezfa3fs SOURCE_FILE [DESTINATION]
ezfa3fs get IMAGE.ezfa3fs FILE OUTPUT_FILE
ezfa3fs rm IMAGE.ezfa3fs FILE
ezfa3fs rmdir IMAGE.ezfa3fs DIRECTORY
ezfa3fs gc IMAGE.ezfa3fs
ezfa3fs compact IMAGE.ezfa3fs
ezfa3fs space IMAGE.ezfa3fs
ezfa3fs mount IMAGE.ezfa3fs MOUNTPOINT [--writable] [--foreground]
```

When `DESTINATION` is omitted from `put`, the source path is also the destination.

Cartridge commands:

```text
ezfa3fs card-mount MOUNTPOINT [--foreground]
ezfa3fs card-mount MOUNTPOINT --writable --foreground [--verify]
ezfa3fs card-pull IMAGE.ezfa3fs
ezfa3fs card-format [--direct-boot]
ezfa3fs card-write IMAGE.ezfa3fs [--skip-verification]
ezfa3fs card-gc
ezfa3fs card-compact
ezfa3fs card-space
```

Low-level diagnostics:

```text
ezfa3fs card-read-block BLOCK OUTPUT.bin
ezfa3fs card-erase-plan BLOCK
ezfa3fs card-erase-block BLOCK
ezfa3fs card-program-block BLOCK INPUT.bin
```

Raw erase and program commands modify cartridge blocks after confirmation. Standard metadata occupies blocks 0 and 1; direct-boot metadata occupies blocks 510 and 511. Using raw commands on metadata or active data can destroy the filesystem.

## macFUSE behavior

Ordinary writes are staged in host memory. `flush` and `fsync` report mount health; a dirty file is committed when its final handle is released. This coalesces the many small writes issued by Finder and `cp` into one cartridge transaction.

Directories are reported as mode `0755` and files as `0644`. Unsupported ownership, mode, flag, timestamp-setting, and extended-attribute changes are accepted as compatibility no-ops. Finder `.DS_Store` and AppleDouble `._*` files are held only in memory and disappear on unmount; they do not consume flash.

Do not use macFUSE `noappledouble` or `noapplexattr` options. Finder can interpret those rejections as copy failures.

Live cartridge reads retry transient USB failures by reopening the validated writer session and restoring the requested mapping. If a mutation cannot be verified after its retries, the mount rejects subsequent mutations until remounted. The previously committed generation remains the recovery point.

## Verify copied data

```sh
./build/cmake/ezfa3fs card-pull pulled.ezfa3fs
./build/cmake/ezfa3fs verify pulled.ezfa3fs
./build/cmake/ezfa3fs get pulled.ezfa3fs path/to/game.gba recovered.gba
shasum -a 256 source.gba recovered.gba
```

EZFA3FS stores CRC32 values for corruption detection. SHA-256 is calculated externally.

## Performance and maintenance

EZFA3FS caches cartridge reads lazily in host memory, up to 32 MiB when every block is touched. Repeated reads and directory access are therefore faster without a full scan during normal mount startup.

NOR operations remain slow: changed blocks are programmed and read back, and metadata is written to the alternate superblock and verified. Hardware failures are retried up to three times. Large copies can take materially longer than ordinary host-filesystem copies.

Inspect capacity and fragmentation with:

```sh
./build/cmake/ezfa3fs card-space
```

It reports active, erased, and unreferenced blocks; potentially available capacity; largest extents; fragmentation; and whether garbage collection is recommended. Run maintenance only while unmounted:

```sh
./build/cmake/ezfa3fs card-gc
./build/cmake/ezfa3fs card-compact
```

Garbage collection erases unreferenced blocks. Compaction relocates active files to form a larger contiguous erased extent. Allocation may invoke maintenance automatically when no suitable extent remains, so keeping erased space available avoids a long pause during a copy.

## Format references

- [EZFA3FS format](EZFA3FS_FORMAT.md)
- [EZFA3FS design origins](EZFA3FS_DESIGN_ORIGINS.md)
- [Incremental background garbage collection plan](BACKGROUND_GC_PLAN.md)

## Current limits

- Storage is fixed at 32 MiB and files occupy contiguous 64 KiB logical blocks.
- The format has no FAT compatibility or partition table.
- Symlinks, hard links, sparse files, persistent permissions, ownership, extended attributes, and Finder metadata are unsupported.
- Hardware reliability and throughput depend on the EZ-Flash writer, USB connection, and NOR flash.
- Direct boot is experimental and requires compatible external boot logic.
