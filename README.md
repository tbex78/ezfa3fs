# EZFA3FS

EZFA3FS is an independent filesystem toolkit for the EZ-Flash Advance III cartridge. It does not use the original EZ3 menu, loader, ROM patching, FAT32, or a partition table.

The project provides the `ezfa3fs` program for the transactional **EZFA3FS**
format.

Application version: **0.50.7**. EZFA3FS is experimental format **0.1.0** in its standard layout and **0.2.0** in its slotted direct-boot layout. Images using the former 2.0.0 and 2.1.0 identifiers remain readable for now. Legacy EZFA3FS remains format **1.2** but are not supported in the last software version.

The FUSE/macFUSE and real-cartridge workflow has been exercised with directories, file creation and reading, replacement, deletion, recursive deletion, large GBA ROM copies, garbage collection, compaction, cartridge pullback, verification, and SHA-256 comparison with source files.

Cartridge reads use save-safe two-byte control transfers. By default, before a writable cartridge session initializes the flash writer, EZFA3FS snapshots all four 32-KiB save banks. On clean unmount it restores all four snapshots when the current contents differ and bank 1 starts with the writer marker `00 04`. Otherwise it restores a nonzero snapshot only when all four current banks are zero. If neither condition matches, it performs no save-bank writes. Pass `--skip-snapshot-restore` on `card-mount` to disable this save-preservation workflow for that mount: no save-safety USB reconnect is requested, no pre-writer save snapshot is captured, and no save snapshot is restored after unmount. Always unmount cleanly before disconnecting the linker.

## Build

Requirements are CMake, a C++17 compiler, libusb-1.0, and FUSE when mount support is wanted.

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

Add `--verify` to either mount mode to verify referenced file data before mounting. A writable mount without `--verify` reads only the metadata needed to start, which is much faster on cartridges containing large files. Without `--verify`, `card-mount` does not run the full referenced-file checksum verification.

For writable mounts, `--skip-snapshot-restore` disables the normal save-preservation workflow for that mount. EZFA3FS does not request the pre-writer or post-unmount USB reconnect, does not capture the pre-writer save snapshot, and does not restore save memory after unmount. Without this option, the existing snapshot/reconnect/restore behavior is unchanged.

Unmount from another terminal with `umount mountpoint`.

## Direct boot

The experimental direct-boot layout places one root-level GBA ROM at cartridge byte offset zero for compatible direct-boot logic in the companion [`ezfadvanceIII`](https://github.com/tbex78/ezfadvanceIII) project.

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
ezfa3fs card-mount MOUNTPOINT --foreground [--verify] [--verbose] [--logfile=PATH]
ezfa3fs card-mount MOUNTPOINT --writable --foreground [--verify] [--verbose] [--logfile=PATH] [--skip-snapshot-restore]
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

## MacFUSE behavior

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

## Current limits

- Storage is fixed at 32 MiB and files occupy contiguous 64 KiB logical blocks.
- The format has no FAT compatibility or partition table.
- Symlinks, hard links, sparse files, persistent permissions, ownership, extended attributes, and Finder metadata are unsupported.
- Hardware reliability and throughput depend on the EZ-Flash writer, USB connection, and NOR flash.
- Direct boot is experimental and requires compatible external boot logic.

## Disclaimer

### Independent project / no affiliation or endorsement

This is an **independent, unofficial, community-developed project**. It is **not affiliated with, associated with, authorized by, endorsed by, sponsored by, supported by, or otherwise connected with Nintendo Co., Ltd., any Nintendo affiliate, the EZ-Flash Team, or any related manufacturer, developer, distributor, or rights holder**.

Nintendo, Game Boy Advance, EZ-Flash, EZF Advance III, and any other product names, trademarks, service marks, logos, or brands referenced by this project remain the property of their respective owners. Their use in this repository is solely for identification, compatibility, interoperability, technical documentation, and descriptive purposes and does not imply any affiliation, endorsement, sponsorship, approval, or support.

**Neither Nintendo nor the EZ-Flash Team provides support for this project.** Questions, bug reports, compatibility issues, device problems, or damage arising from this software should not be directed to Nintendo, the EZ-Flash Team, or their respective affiliates, employees, distributors, or support channels.

### Project origin and purpose

This project was started because the original software and drivers for the **EZF Advance III** are available for and operational with **Windows XP**, an operating system that is now very old and no longer a practical or desirable platform for many users.

The purpose of this project is therefore to research, document, and develop an independent alternative that can help preserve continued use of existing EZF Advance III hardware on modern Unix-like systems, without requiring the original Windows XP environment. The project is focused on compatibility and interoperability with hardware that users already own; it is not intended to represent, replace, or imply official software, drivers, support, or endorsement from Nintendo or the EZ-Flash Team.

We hope that someone with the necessary technical knowledge and interest will **fork this repository and continue the project further**. This repository is shared as a working community-developed alternative and as a record of the work already done, in the hope that others may improve, correct, document, and extend it.

This software and project are provided **“AS IS” and “AS AVAILABLE,” without warranty of any kind**, express or implied.

This project remains under active development and may contain bugs, incomplete features, incorrect assumptions, or unexpected behavior, particularly on hardware and configurations that have not yet been tested. Use of this software may cause data loss, corruption, malfunction, permanent damage, or otherwise render an **EZF Advance III device partially or completely unusable (“bricked”)**.

A substantial portion of this project was created through **“vibe coding,” reverse engineering, experimentation, and the use of AI-assisted development tools, including ChatGPT and Codex**. As a result, the code may contain errors, inaccurate implementations, undocumented behavior, or functionality that has not been thoroughly tested or independently verified.

The owner of this Git repository **does not claim to possess the technical expertise, engineering qualifications, or detailed knowledge necessary to guarantee the correctness or safety of the software**. The repository owner may also be unable to provide technical support, debugging assistance, device recovery assistance, repair instructions, or further development support if the software causes problems or damages an EZF Advance III device.

By downloading, installing, modifying, executing, flashing, or otherwise using this software, you acknowledge and accept that you do so **entirely at your own risk**.

To the maximum extent permitted by applicable law, the author(s), contributor(s), and maintainer(s) of this project shall not be liable for any direct, indirect, incidental, special, consequential, or other damages arising from or related to the use or inability to use this software, including, without limitation, damage to hardware, loss or corruption of data, loss of functionality, device failure, or the permanent bricking of an EZF Advance III device.

**You are solely responsible for understanding the risks, making appropriate backups where possible, verifying the software before use, and determining whether you are willing to accept the possibility of permanently damaging your EZF Advance III device.**

Do not use this software on any device that you are not prepared to potentially damage or lose. Use this project only if you fully understand and accept these risks.
