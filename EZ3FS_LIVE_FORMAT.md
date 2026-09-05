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
`0.35.4`.

## Geometry and layout

An image is exactly `0x02000000` bytes and is divided into 512 logical blocks
of 64 KiB:

| Logical blocks | Purpose |
|---:|---|
| 0 and 1 | Alternating generation-numbered superblocks |
| 2 through 511 | Copy-on-write file data and reclaimable space |

```text
block 0          superblock generation A
block 1          superblock generation B
blocks 2..511    copy-on-write file extents and unused 0xFF blocks
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

1. Locate a contiguous erased extent that does not overlap an active file.
2. Program complete 64-KiB blocks, padding the final block with `0xFF`.
3. Build a manifest containing the new file extent.
4. Erase the inactive superblock.
5. Program the inactive superblock with generation + 1.
6. Read back and verify every physical block transaction, reopening the USB
   session for metadata or error recovery when required.

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

Normal data allocation is copy-on-write. Replacing or deleting a file makes
its old extent unreachable without erasing it immediately. Mounting derives
an initial allocation cursor from committed entries without scanning the free tail.
Before programming a file, the allocator searches for a contiguous erased
extent and skips programmed blocks leaked by interrupted writes. This prevents
unsafe NOR `0 -> 1` programming attempts while avoiding a full allocation scan
at mount time.

`live-gc` and `live-card-gc` sweep blocks 2 through 511, erase only blocks not
referenced by the selected committed generation, and make the resulting holes
available to the circular contiguous-extent allocator. The cartridge command
must run while the filesystem is unmounted. An interrupted collection is safe
to repeat because the collector first synchronizes the current manifest into
the alternate superblock, then protects active extents and both metadata
blocks from erasure. Cartridge collection restarts the writer session between
inspection and each erase because the USB bridge does not reliably accept a
flash erase directly after a read transaction.

`live-compact` and `live-card-compact` first perform the same garbage sweep,
then relocate active file extents toward block 2. Each relocation is a
transaction with this ordering:

1. Read and checksum the source file.
2. Program its new erased extent and verify the physical writes.
3. Commit a manifest generation referencing the new extent.
4. Commit the same manifest to the other superblock.
5. Erase and verify the old extent.

The source remains intact until both metadata blocks reference the verified
destination. A power loss before the first commit leaves the old generation
active; a power loss later leaves either the old or new data as harmless
unreferenced garbage. The cartridge command requires an unmounted filesystem.

`live-list` reports an available-block estimate. Unknown remnants from an
interrupted write are removed from that estimate when allocation probes them
or when garbage collection scans the complete data area.

`live-space` and `live-card-space` perform a read-only data-area scan. Their
report distinguishes active extents, erased reusable blocks, and unreferenced
programmed blocks; it also reports the largest contiguous erased extent before
and after garbage collection. A smaller post-GC extent than total available
space indicates fragmentation caused by active file placement.

File allocation normally uses the first suitable erased extent without a
full-device maintenance scan. If allocation fails, the writer automatically
collects garbage and retries. When the remaining capacity is sufficient but
fragmented, it transactionally compacts active extents and retries once more.
Command-line and writable FUSE callers report these maintenance transitions;
a final allocation failure is exposed as out-of-space.

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
./build/cmake/ez3fs live-gc cartridge.ez3live
./build/cmake/ez3fs live-compact cartridge.ez3live
./build/cmake/ez3fs live-space cartridge.ez3live
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
./build/cmake/ez3fs live-card-gc
./build/cmake/ez3fs live-card-compact
./build/cmake/ez3fs live-card-space
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
Garbage collection and compaction are invoked automatically if an ordinary
copy-on-write allocation cannot proceed, so manual maintenance is not required
for correctness during a mounted write.

New FUSE files remain in the mount backend's pending state until flush, fsync,
or release successfully commits their contents. After a commit failure, the
mount stays readable but rejects later mutations before invoking the backend.
It reports the cached failure to lifecycle callbacks without replaying the
flash transaction during teardown. Program and erase verification rechecks a
mismatched block through fresh USB sessions before retrying it. macOS cartridge
mounts use a 600-second daemon timeout, suppress AppleDouble traffic, and
accept but discard extended attributes because the format does not store them.
Contiguous data blocks share one captured-protocol programming session, with a
session transition only at each 8-MiB hardware window boundary. A failed data
extent is retried at another erased location before the mount reports failure.
USB writer reinitialization uses bounded retries, and a failed reinitialization
stops maintenance before another erase or program command can be issued.

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

- Compaction is best-effort: a file can move only when a sufficiently large
  erased extent already exists below its current extent. Pathological layouts
  may therefore retain some fragmentation.
- Each flushed logical file is written to a new contiguous extent.
- Each committed mutation writes a new superblock generation.
- Physical erase/program readback still bounds maximum write speed.
- No concurrent cartridge commands while a direct mount is active.
- No FAT compatibility, partition table, EZ3 menu, or original loader support.
- Packed EZ3FS and EZ3FS-LIVE images cannot be interchanged.
