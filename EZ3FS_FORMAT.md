# EZ3FS packed archive format 1.2

EZ3FS is a compact indexed archive designed for the 32-MiB EZ-Flash Advance
III NOR cartridge. It is independent of the original EZ3 layout and contains
no loader, menu, ROM catalog, partition table, FAT filesystem, or patched ROM
metadata.

EZ3FS 1.2 is the format emitted by `ezfs-legacy` application `0.44.0`. The application
also reads format 1.0, format 1.1, and the legacy EZFS magic described below.
It is incompatible with the transactional EZFA3FS format.

All multibyte integers are unsigned and little-endian. Paths are relative and
use `/` as their separator.

## Image layout

```text
offset 0
+-------------------------------+
| 64-byte header                |
+-------------------------------+
| file_count × 288-byte entries |
+-------------------------------+
| 0xFF padding to 64 KiB        |
+-------------------------------+  data_offset
| packed file data              |
+-------------------------------+
| 0xFF padding to 64 KiB        |
+-------------------------------+  image_size
```

The data area begins at the first 64-KiB boundary after the index. File data is
stored consecutively in index order; individual files are not block-aligned.
The complete image is padded with `0xFF` to a 64-KiB programming boundary and
cannot exceed `0x02000000` bytes.

## Header

The header occupies bytes `0x00` through `0x3F`.

| Offset | Size | Field |
|---:|---:|---|
| `0x00` | 8 | Magic `45 5A 33 46 53 0D 0A 1A` |
| `0x08` | 2 | Format major: `1` |
| `0x0A` | 2 | Format minor: `2` |
| `0x0C` | 4 | Header size: `64` |
| `0x10` | 4 | Index-entry size: `288` |
| `0x14` | 4 | Entry count |
| `0x18` | 8 | Index offset: `64` |
| `0x20` | 8 | Data offset |
| `0x28` | 8 | Padded image size |
| `0x30` | 4 | IEEE CRC-32 of all index-entry bytes |
| `0x34` | 4 | Header CRC-32 with this field set to zero |
| `0x38` | 8 | Reserved; emitted as zero |

The header CRC protects all 64 header bytes. The index CRC protects exactly
`entry_count × entry_size` bytes beginning at `index_offset`.

## Index entry

Each entry is 288 bytes.

| Offset | Size | Field |
|---:|---:|---|
| `0x00` | 256 | NUL-terminated relative path |
| `0x100` | 8 | File-data offset |
| `0x108` | 8 | Exact file-data size |
| `0x110` | 4 | IEEE CRC-32 of file data |
| `0x114` | 4 | Flags; bit 0 marks a directory |
| `0x118` | 8 | Modification time in Unix seconds |

A path is at most 255 bytes and cannot:

- Be empty or absolute.
- Contain `\` or an embedded NUL.
- Contain empty, `.` or `..` components.
- Duplicate another entry path.

The root directory is implicit and is never stored. Directory entries have
zero data offset, size, and CRC. File extents must remain inside the declared
image, must not overlap, and must not point into the header or index.

A zero modification time means that the timestamp is unavailable. FUSE uses
the mount time as a display fallback for entries without a stored timestamp.

## Integrity and validation

An image is accepted only when its structural fields, bounds, paths, flags,
index CRC, header CRC, and file extents are valid. Full
verification additionally calculates and checks every file-data CRC.

Use:

```sh
./build/cmake/ezfs-legacy verify IMAGE.ez3fs
```

Cartridge reads apply the same parser and verifier. A cartridge with an
unrecognized flash identifier is accepted through the compatibility path only
when a valid EZ3FS signature exists at offset zero.

## Mutations and cartridge programming

Packed EZ3FS is not an in-place writable filesystem. Image edit commands and
writable image mounts rebuild the packed image, write a temporary image, and
replace the original. The physical-cartridge workflow stages edits locally and
then uses a complete erase/program/verify cycle.

```sh
./build/cmake/ezfs-legacy card-write IMAGE.ez3fs
```

`card-write` validates the archive before requesting yes/no confirmation. It
then erases all 32 MiB, programs the padded image at offset zero, and compares
every programmed byte with the source image.

For individual live cartridge updates, use EZFA3FS rather than this format.

## Compatibility

| Version | Reader behavior |
|---|---|
| 1.0 | File entries only; flags must be zero; no timestamp field |
| 1.1 | Directory entries supported; no persisted timestamp |
| 1.2 | Directory entries and modification timestamps supported |

Readers also recognize the legacy eight-byte EZFS magic
`45 5A 46 53 0D 0A 1A 0A`. Builders always emit the current EZ3FS magic.
