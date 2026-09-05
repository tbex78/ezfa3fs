# EZFA3FS transactional format

This document describes the 32 MiB EZFA3FS format implemented by application version **0.45.18**. The standard layout is format **2.0.0**; the slotted direct-boot layout is **2.1.0**.

EZFA3FS is an independent indexed filesystem for EZ-Flash Advance III NOR flash. It is not FAT, has no partition table, and does not use the original EZ3 menu or ROM patching. All multibyte integers are little-endian.

## Flash geometry

An image is exactly 33,554,432 bytes: 512 logical blocks of 65,536 bytes.

The physical erase geometry is asymmetric:

- Logical block 0 consists of eight 8 KiB erase sectors.
- Logical blocks 1 through 510 each use one 64 KiB erase sector.
- Logical block 511 consists of eight 8 KiB erase sectors.

Allocation and manifests use logical blocks. The cartridge adapter translates erase operations on blocks 0 and 511 into physical sector commands. Programming remains one capture-proven 64 KiB transaction per logical block, including blocks 0 and 511.

## Layouts

Standard layout:

| Blocks | Purpose |
|---:|---|
| 0 and 1 | Alternating metadata superblocks |
| 2 through 511 | File data and erased free space |

Its magic is `EZ3LIVE1`, major `2`, minor `0`.

Direct-boot layout:

| Blocks | Purpose |
|---:|---|
| 0 through `boot_slot_blocks - 1` | Reserved boot-ROM slot |
| `boot_slot_blocks` through 509 | Other file data and free space |
| 510 and 511 | Alternating metadata superblocks |

Its magic is `EZFA3FS1`, major `2`, minor `1`. An empty direct-boot format initially reserves 256 blocks (16 MiB). A format supplied with a ROM reserves at least its required size. The slot can expand toward block 509 while the required blocks are available.

## Superblock

Each metadata block starts with this 32-byte header:

| Offset | Size | Meaning |
|---:|---:|---|
| `0x00` | 8 | Layout magic |
| `0x08` | 2 | Major version, `2` |
| `0x0A` | 2 | Layout minor version |
| `0x0C` | 8 | Monotonically increasing generation |
| `0x14` | 4 | Manifest length |
| `0x18` | 4 | Manifest CRC32 |
| `0x1C` | 4 | Commit marker |

The manifest begins at `0x20`; unused bytes remain `0xFF`. Cartridge commits program and verify only the required metadata prefix rounded to the writer's 8 KiB granularity.

On open, both layout-appropriate superblocks are inspected. A candidate is valid only when its magic, version, bounds, commit marker, manifest CRC32, entries, and allocation invariants pass. The valid candidate with the greatest generation is active.

## Manifest

A standard manifest begins with one 32-bit entry count. A direct-boot manifest begins with a 32-bit boot-slot size followed by the 32-bit entry count.

Each entry then has a 32-byte fixed header followed by its UTF-8 path:

| Offset | Size | Meaning |
|---:|---:|---|
| `0x00` | 2 | Path length |
| `0x02` | 1 | Flags; bit 0 marks a directory |
| `0x03` | 1 | Reserved |
| `0x04` | 4 | First logical data block |
| `0x08` | 4 | Contiguous logical block count |
| `0x0C` | 8 | File size |
| `0x14` | 4 | File CRC32 |
| `0x18` | 8 | Modification time as Unix seconds |
| `0x20` | variable | UTF-8 path |

Directories have no data extent. Empty regular files also use no blocks. File extents must be in the layout's data region, large enough for the declared size, and non-overlapping.

## Namespace rules

- Paths are relative UTF-8 strings separated by `/`.
- Leading or trailing `/`, empty components, `.`, and `..` are invalid.
- Paths are unique and cannot descend through a regular file.
- Parent directories must exist.
- Renaming a directory also renames its descendants in the committed manifest.

## Transactions and allocation

Data is copy-on-write. A new or replaced file receives a contiguous erased extent, which is programmed and verified before metadata refers to it. The new manifest is written to the inactive superblock with generation `active + 1` and read back. Until that succeeds, the prior valid generation remains authoritative.

Blocks dropped by a new generation become unreferenced garbage; they need not be erased during the metadata commit. Allocation prefers an erased contiguous extent. If none is large enough, the implementation can collect garbage and compact active files. Explicit maintenance before a large copy makes latency more predictable.

## Direct-boot rules

- The first persistent file must be a non-empty root-level `.gba` file.
- No directory may be committed before the boot ROM exists.
- The boot ROM begins at block 0 and may use at most blocks 0 through 509.
- The ROM is immutable while present; delete it before installing another.
- Deleting it erases only logical block 0, immediately invalidating the old GBA header and entry point.
- Installing its replacement erases nonblank blocks in the replacement extent in window-level batches, verifies the batch, and retries only failed blocks individually; stale blocks beyond a smaller replacement remain reserved and inaccessible.
- After deletion, the next persistent file must again be a non-empty root-level `.gba` file.
- Other files and directories live after the reserved slot.

An empty `.gba` created through FUSE is transient and is not committed until it contains data and its final handle is released.

## Verification and recovery

Verification checks image size, versions, both superblocks, generation selection, manifest CRC32, entries, namespace hierarchy, reserved ranges, extent bounds and overlap, and every regular-file CRC32. SHA-256 is not stored; extract a file and use `shasum -a 256` for cryptographic comparison.

Every cartridge erase and program is verified by readback. Transient USB or writer failures are retried up to three times, with writer reinitialization when possible. If a mounted mutation cannot be verified, later mutations are rejected until remounting. The last committed superblock is the recovery point.

## Space management

`gc` erases programmed blocks not referenced by the active manifest. `compact` relocates active extents to create a larger contiguous erased region, while leaving a direct-boot ROM fixed at block zero.

`space` and `card-space` report active data, erased reusable blocks, unreferenced programmed blocks, potentially available space, largest current and post-GC extents, fragmentation, and whether collection is recommended. Cartridge GC and compaction require an unmounted filesystem and confirmation.

## Cartridge mounts and macFUSE

A read-only cartridge mount pulls and verifies a complete snapshot. A writable mount opens from metadata and performs direct block transactions; optional `--verify` performs a full allocation scan first.

Reads use a lazy verified cache, refreshed after program and invalidated after erase. It can grow to all 512 blocks (32 MiB) if every block is accessed.

FUSE stages ordinary file writes in host memory. `flush` and `fsync` report health; a dirty file commits on its final `release`, coalescing Finder and `cp` writes into one transaction.

Directories report mode `0755`, files `0644`. Unsupported ownership, mode, flags, timestamp-setting, and extended attributes are accepted as compatibility no-ops. `.DS_Store` and `._*` files live in a transient in-memory Finder overlay and never enter the manifest.

## Commands

Image operations:

```text
format, list, verify, mkdir, put, get, rm, rmdir,
gc, compact, space, mount
```

Cartridge operations:

```text
card-mount, card-pull, card-write, card-gc,
card-compact, card-space
```

Raw diagnostics:

```text
card-read-block, card-erase-plan, card-erase-block,
card-program-block
```

Raw erase and program commands bypass allocation. Never use them on active data or metadata unless deliberately performing low-level recovery.

## Limits

- Storage is fixed at 32 MiB.
- Files occupy contiguous 64 KiB logical extents; fragmentation can require compaction.
- The complete manifest must fit in one metadata block.
- Permissions, ownership, links, sparse files, persistent extended attributes, and persistent Finder metadata are unsupported.
- FAT and partition tools cannot mount the format.
- Direct boot is experimental and requires compatible external boot logic.
