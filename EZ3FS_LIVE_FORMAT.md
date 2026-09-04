# EZ3FS-LIVE format 1.0.0

EZ3FS-LIVE is the transactional, copy-on-write filesystem for the 32-MiB
EZ-Flash Advance III NOR cartridge. It supports files, directories, persisted
modification times, redundant metadata generations, and direct block-level
updates while a cartridge is mounted through FUSE/macFUSE.

The format is independent of the original EZ3 layout and incompatible with
packed EZ3FS images. It contains no EZ3 menu, loader, ROM catalog, partition
table, FAT filesystem, or ROM patches.

EZ3FS-LIVE 1.0.0 is experimental but has been exercised on physical hardware
from both terminal commands and Finder. The current application version is
`0.30.8`.

## Geometry and layout

An image is exactly `0x02000000` bytes and is divided into 512 logical blocks
of 64 KiB:

| Logical blocks | Purpose |
|---:|---|
| 0 and 1 | Alternating generation-numbered superblocks |
| 2 through 511 | Append-only file data |

```text
block 0          superblock generation A
block 1          superblock generation B
blocks 2..511    append-only file extents and unused 0xFF blocks
```

The cartridge's physical bottom-boot erase geometry is not completely
uniform. Logical block 0 is erased using eight 8-KiB physical sectors. The
filesystem block-device adapter hides this detail and presents uniform 64-KiB
logical blocks to the filesystem.

## Superblock

Each superblock occupies one complete logical block. All multibyte values are
unsigned and little-endian.

| Offset | Size | Field |
|---:|---:|---|
| `0x00` | 8 | Magic `EZ3LIVE\0` |
| `0x08` | 2 | Format major: `1` |
| `0x0A` | 2 | Format minor: `0` |
| `0x0C` | 8 | Generation number |
| `0x14` | 4 | Manifest length |
| `0x18` | 4 | IEEE CRC-32 of the manifest |
| `0x1C` | 4 | Commit marker `0xC0FF17ED` |
| `0x20` | variable | Manifest |
| after manifest | remaining | `0xFF` padding |

A superblock is valid only when its magic, version, commit marker, manifest
bounds, manifest CRC, and every manifest entry are valid. Mounting selects the
valid superblock with the highest generation. An invalid or interrupted newer
superblock is ignored.

## Manifest

The manifest starts with a 32-bit entry count followed by variable-length
entries. Each entry consists of a 32-byte fixed portion and its path bytes.

| Entry offset | Size | Field |
|---:|---:|---|
| `0x00` | 2 | Path length |
| `0x02` | 1 | Flags; bit 0 marks a directory |
| `0x03` | 1 | Reserved |
| `0x04` | 8 | Exact file size |
| `0x0C` | 8 | Modification time in Unix seconds |
| `0x14` | 4 | IEEE CRC-32 of file data |
| `0x18` | 4 | First logical data block |
| `0x1C` | 4 | Number of logical data blocks |
| `0x20` | path length | Relative path bytes, without a NUL terminator |

Paths use `/`, are relative, and cannot contain empty, `.` or `..` components.
Names must be unique. Parent directories must exist before child entries are
created.

Directories have zero size, CRC, first block, and block count. Zero-length
files have no data blocks. Non-empty file extents begin at block 2 or later,
remain within the 512-block image, and are protected by their stored CRC-32.

## Transaction model

A file create or replacement follows this sequence:

1. Allocate never-before-used data blocks from the free tail.
2. Program complete 64-KiB blocks, padding the final block with `0xFF`.
3. Build a manifest containing the new file extent.
4. Erase the inactive superblock.
5. Program the inactive superblock with generation + 1.
6. Reopen, read back, and verify each physical block transaction.

Directory changes, renames, and deletions only need a new manifest generation.
The previously active superblock remains valid until the replacement
superblock is completely programmed and passes validation.

Physical erase and program operations are checked through 64-KiB readback.
Transient USB endpoint stalls, nonblank erase results, and program mismatches
are retried up to three times. A partially programmed retry target is erased
before it is programmed again.

If all retries fail, the operation returns an I/O error. Remounting selects the
newest complete generation; an incomplete inactive superblock does not replace
the previous committed filesystem state.

## Allocation and recovery

Data allocation is append-only. Replacing or deleting a file makes its old
extent unreachable but does not erase or reuse it. On open, EZ3FS-LIVE scans
the data area and advances the allocation cursor past the highest programmed
block, including leaked blocks from interrupted writes. This prevents unsafe
NOR `0 -> 1` programming attempts after remounting.

`live-list` reports the remaining free tail blocks. There is no garbage
collector in format/application version 1.0.0/0.30.8. Space is recovered only
by creating a fresh image or rebuilding and completely rewriting the
cartridge.

## Local image commands

```sh
./build/cmake/ez3fs live-format cartridge.ez3live
./build/cmake/ez3fs live-mkdir cartridge.ez3live documents
./build/cmake/ez3fs live-put cartridge.ez3live README.md documents/README.md
./build/cmake/ez3fs live-put cartridge.ez3live documents/local.txt
./build/cmake/ez3fs live-list cartridge.ez3live
./build/cmake/ez3fs live-verify cartridge.ez3live
./build/cmake/ez3fs live-get cartridge.ez3live documents/README.md recovered.md
./build/cmake/ez3fs live-rm cartridge.ez3live documents/README.md
./build/cmake/ez3fs live-rmdir cartridge.ez3live documents
```

`live-format` creates an exact 32-MiB image. Mutating image commands persist a
new generation before exiting. If `live-put` omits its destination, it uses the
source path as the destination.

Local image mounting is currently read-only:

```sh
./build/cmake/ez3fs live-mount cartridge.ez3live mountpoint --foreground
```

## Cartridge image commands

Read a complete physical cartridge into a local image:

```sh
./build/cmake/ez3fs live-card-pull cartridge-backup.ez3live
./build/cmake/ez3fs live-verify cartridge-backup.ez3live
```

Write a complete image when initially formatting or deliberately replacing a
cartridge:

```sh
./build/cmake/ez3fs live-card-write cartridge.ez3live
```

`live-card-write` validates the image before asking for yes/no confirmation.
It erases and programs the complete cartridge and performs byte-for-byte
verification.

## FUSE/macFUSE cartridge mounting

A read-only cartridge mount first reads and verifies all 32 MiB, closes the USB
session, and mounts an in-memory snapshot:

```sh
./build/cmake/ez3fs live-card-mount mountpoint
```

A direct writable mount requires foreground mode so the libusb session is not
inherited across FUSE daemonization:

```sh
./build/cmake/ez3fs live-card-mount mountpoint \
  --writable --foreground
```

After yes/no confirmation, the writable mount scans allocation with visible
progress, verifies the filesystem, and retains exclusive access to the USB
writer. Terminal and Finder operations are committed directly to flash.

Unmount from another terminal before disconnecting the writer:

```sh
diskutil unmount mountpoint       # macOS
fusermount3 -u mountpoint         # Linux
```

Finder can create `.DS_Store` and `._*` AppleDouble files. They are stored as
ordinary files and consume blocks. Close Finder windows before removing these
entries if Finder recreates them immediately.

## Diagnostic block commands

```sh
./build/cmake/ez3fs live-card-read-block BLOCK OUTPUT.bin
./build/cmake/ez3fs live-card-erase-plan BLOCK
./build/cmake/ez3fs live-card-erase-block BLOCK
./build/cmake/ez3fs live-card-program-block BLOCK INPUT.bin
```

- `live-card-read-block` accepts blocks 0 through 511 and writes exactly 64 KiB.
- `live-card-erase-plan` accepts blocks 0 through 511 and never modifies flash.
- Direct diagnostic erase and program are restricted to data blocks 2 through
  511 so they cannot overwrite active filesystem metadata.
- Direct erase and program require yes/no confirmation and verify readback.
- A program input must be exactly 64 KiB.

## Current limitations

- No garbage collection or reuse of obsolete data blocks.
- Each FUSE write request rewrites the complete logical file to new blocks.
- Each mutation commits a new superblock generation.
- Safety reconnects and readback make direct writable mounts slow.
- No concurrent cartridge commands while a direct mount is active.
- No FAT compatibility, partition table, EZ3 menu, or original loader support.
- Packed EZ3FS and EZ3FS-LIVE images cannot be interchanged.
