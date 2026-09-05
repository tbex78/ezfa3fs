# EZFA3FS design origins

EZFA3FS is a new, independent filesystem designed for the EZ-Flash Advance III cartridge. It is not an official or recovered EZ-Flash filesystem, and it is not intended to reproduce the original EZ3 menu image layout.

The design combines evidence collected from the cartridge with established filesystem and NOR-flash techniques.

## Sources of knowledge

### Existing project code

The original cartridge-access code in this repository supplied the starting knowledge for:

- Detecting and opening the EZ-Flash Advance III USB writer.
- Sending commands through its USB bulk endpoints.
- Reading cartridge contents.
- Erasing and programming the NOR flash.
- Selecting the cartridge's four 8 MiB access windows.
- Resetting the writer between read and write operations.

The earlier packed EZ3FS experiment also demonstrated that an independent indexed image could store ordinary files without the original EZ3 menu.

### USB packet captures

Packet captures from the original EZ3 software were used to understand command sequences that were not documented publicly. These included:

- Writer initialization and readiness polling.
- Flash identification probes.
- Read-mode and write-mode transitions.
- Window selection.
- Erase commands.
- Program transactions and completion responses.
- Post-write cleanup sequences.

The captures describe how to communicate with the hardware. They do not describe EZFA3FS itself; its on-cartridge format was designed independently.

### Companion direct-boot work

The experimental direct-ROM programming work in the companion `ezfadvanceIII` project established that a GBA ROM can be executed when its header and entry point are present at cartridge byte offset zero.

That requirement led to the EZFA3FS direct-boot layout:

- The boot ROM begins at logical block 0.
- An explicit boot slot reserves space for the ROM.
- Additional filesystem data is allocated after that slot.
- Metadata is stored at the end of the cartridge in blocks 510 and 511.

### Real-hardware testing

Many protocol details could only be established empirically. Hardware tests reported during development were used to refine:

- Physical erase-sector addresses.
- Read and write mapping transitions.
- Writer restart behavior.
- USB stall recovery.
- Completion-response handling.
- Readback verification.
- Retry ordering.
- Metadata placement and prefix programming.
- Finder and macFUSE compatibility.
- Garbage collection and compaction behavior.
- Direct-boot ROM deletion and replacement.

Failures such as `LIBUSB_ERROR_PIPE`, incomplete erase responses, stale read mappings, and program readback mismatches revealed state transitions that were not apparent from a successful capture alone.

Programming requires a second capture-derived activation after the manager probe: three successful `0x98` readiness exchanges separated by one-second quiet intervals. A command echo alone is not proof that the bridge accepted a following flash operation; omitting this activation can produce nonzero erase status or blank program readback despite an apparently accepted command.

## Hardware constraints

### NOR-flash programming

NOR flash can program a bit only from `1` to `0`. Returning a bit from `0` to `1` requires erasing its physical sector.

For each byte, programming without erasing is possible only when:

```text
(old_byte & new_byte) == new_byte
```

For example:

```text
0xFF -> 0x91  possible
0xF1 -> 0x91  possible
0x81 -> 0x91  impossible without erase
```

This constraint is why EZFA3FS uses erased free blocks, copy-on-write replacement, readback verification, and explicit erase-before-recovery behavior.

### Cartridge capacity and geometry

The cartridge exposes 32 MiB, represented by EZFA3FS as 512 logical blocks of 64 KiB.

The physical erase geometry is asymmetric:

- Logical block 0 consists of eight 8 KiB physical erase sectors.
- Logical blocks 1 through 510 each use one 64 KiB physical erase sector.
- Logical block 511 consists of eight 8 KiB physical erase sectors.

The filesystem uses uniform logical blocks while the cartridge adapter translates the first and last logical blocks into the required physical commands.

## Filesystem engineering techniques

EZFA3FS applies established storage-system techniques to the cartridge's constraints.

### Indexed manifest

A manifest records each file or directory together with its:

- Relative path.
- Entry type.
- File size.
- Modification time.
- CRC32.
- First logical block.
- Contiguous block count.

Directories have no data extent. Files occupy contiguous logical blocks so reads can be translated directly into cartridge offsets.

### Alternating superblocks

Two metadata superblocks are used. Each valid superblock contains a generation number and a CRC32-protected manifest.

During a commit, the new manifest is written to the inactive superblock with a greater generation. The previous generation remains available until the new one has been programmed and verified. On open, EZFA3FS selects the newest valid generation.

This provides transactional metadata recovery without needing an in-place journal.

### Copy-on-write data

New and replacement files are written to erased blocks that are not referenced by the active manifest. Their contents are verified before the manifest is updated.

If programming fails, the previous manifest still references the previous file data. A partially programmed new extent is treated as unavailable or unreferenced rather than becoming the authoritative file.

### Garbage collection

Deleting or replacing a normal file removes its extent from the new manifest. Its old blocks still physically contain data but are now unreferenced garbage.

Garbage collection erases these blocks and returns them to the reusable erased pool. Deferring the erasure keeps ordinary namespace transactions short.

### Compaction

Because files require contiguous extents, enough total free space does not guarantee that a large file can be allocated. Compaction relocates active files to combine smaller free regions into a larger erased extent.

### Integrity checks

CRC32 values protect manifests and file contents against accidental corruption. Standalone hardware erases and all programmed data are checked through readback. Direct-boot replacement deliberately avoids a read-mode transition between erase and program, then verifies the final programmed extent.

CRC32 is not a cryptographic hash. SHA-256 comparisons are performed externally after extracting or pulling a file.

## Direct-boot adaptations

Direct-boot hardware does not understand the filesystem manifest. It executes data directly from cartridge offset zero. EZFA3FS therefore treats the boot ROM differently from ordinary files.

- The first persistent entry must be a non-empty root-level `.gba` file.
- Its extent begins at logical block 0.
- The boot ROM cannot be modified in place while present.
- Other files are placed after the reserved boot slot.
- Removing the ROM commits a manifest without it and erases logical block 0 to invalidate the old GBA header and entry point.
- Remaining old ROM blocks are erased lazily if a replacement ROM needs them.
- Replacement programming inspects its required extent, then keeps stale-block erasure and ROM programming in one writer session before final readback verification.

This design keeps boot-ROM deletion fast while respecting NOR-flash programming rules during replacement.

## macFUSE integration

FUSE and Finder introduce behavior that is not present in the on-cartridge format. The mount implementation adapts those operations to EZFA3FS rather than storing unsupported metadata.

- Writes are staged in host memory and committed when the final file handle is released.
- Directory and file modes are reported as fixed values.
- Unsupported ownership, permission, flag, timestamp-setting, and extended-attribute changes are accepted as compatibility no-ops.
- `.DS_Store` and `._*` files are stored only in a transient in-memory overlay.
- A failed cartridge transaction places the mount in a failed state so later operations cannot silently proceed from uncertain hardware state.

These rules were refined using both terminal operations and Finder copies on macOS.

## What is original to EZFA3FS

The following are project-specific design decisions rather than recovered official EZ-Flash structures:

- The `EZ3LIVE1` and `EZFA3FS1` format identities.
- The manifest record layout.
- Alternating superblock locations and generation selection.
- Copy-on-write allocation rules.
- CRC32-protected file entries and manifests.
- Garbage collection and compaction policy.
- The direct-boot reserved-slot model.
- The live macFUSE mutation model.
- Recovery and retry policies built around verified logical blocks.

The USB command protocol and flash geometry are hardware-facing discoveries. The filesystem built on top of them is an independent design.

## Current scope

EZFA3FS currently provides:

- Files and directories.
- Persistent modification times produced by filesystem writes.
- Transactional live cartridge mutations.
- Image creation and editing.
- Read-only and writable macFUSE mounts.
- Direct-boot and multi-file layouts.
- Garbage collection, compaction, and space inspection.
- Pullback and complete-image verification.

It intentionally does not provide FAT compatibility, a partition table, symlinks, hard links, sparse files, persistent permissions, ownership, extended attributes, or Finder metadata.
