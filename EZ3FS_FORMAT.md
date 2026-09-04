# EZ3FS cartridge archive format 1.2

EZ3FS is an independent, archive-only format for the 32-MiB EZF Advance III
NOR flash. It contains no EZ3 loader, menu, ROM catalog, partition table, or
FAT filesystem. Host software accesses it through the cartridge's proprietary
raw read/erase/program protocol.

All integers are unsigned and little-endian. Images are padded with `0xFF` to
a 64-KiB programming boundary and may not exceed `0x02000000` bytes.

## Header

The 64-byte header starts at byte zero.

| Offset | Size | Field |
|---:|---:|---|
| `0x00` | 8 | `45 5A 33 46 53 0D 0A 1A` (`EZ3FS`) magic |
| `0x08` | 2 | format major (`1`) |
| `0x0A` | 2 | format minor (`2`) |
| `0x0C` | 4 | header size (`64`) |
| `0x10` | 4 | index-entry size (`288`) |
| `0x14` | 4 | file count |
| `0x18` | 8 | index offset (`64`) |
| `0x20` | 8 | data offset |
| `0x28` | 8 | padded image size |
| `0x30` | 4 | CRC-32 of all index entries |
| `0x34` | 4 | header CRC-32, calculated with this field zero |
| `0x38` | 8 | reserved, zero |

The data offset is the first 64-KiB boundary following the index.

## Index entry

| Offset | Size | Field |
|---:|---:|---|
| `0x00` | 256 | NUL-terminated UTF-8 relative path |
| `0x100` | 8 | data offset |
| `0x108` | 8 | exact data size |
| `0x110` | 4 | IEEE CRC-32 of file data |
| `0x114` | 4 | flags: bit 0 denotes a directory |
| `0x118` | 8 | modification time as Unix seconds; zero means unavailable |

Paths use `/`, must be relative, and may not contain empty, `.` or `..`
components. File entries are packed in index order and must not overlap.
Directory entries have zero offset, size, and CRC. The root directory is
implicit and is never stored as an entry.

Format 1.0 and 1.1 images remain readable. Version 1.0 only contains file
entries and requires zero flags. Versions before 1.2 have no persisted
modification time; hosts may display the mount time instead.

For migration, EZ3FS readers also accept the legacy eight-byte EZFS magic
`45 5A 46 53 0D 0A 1A 0A`. Builders always emit the EZ3FS magic.
