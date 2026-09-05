# Legacy EZ3FS packed format 1.2

This document describes the packed `.ez3fs` format retained by `ezfs-legacy`. Application version **0.45.18** reads format minors 0 through 2 and writes format **1.2**.

EZ3FS is an indexed archive occupying the complete 32 MiB cartridge. It is not FAT32, has no partition table, and does not contain the original EZ3 menu. Changing an image repacks its index and data; use EZFA3FS for direct transactional cartridge updates.

All multibyte integers are little-endian.

## Image layout

| Offset | Size | Meaning |
|---:|---:|---|
| `0x0000` | 8 | ASCII magic `EZ3FS001` |
| `0x0008` | 2 | Major version, `1` |
| `0x000A` | 2 | Minor version, `2` |
| `0x000C` | 4 | Entry count |
| `0x0010` | 4 | Index byte length |
| `0x0014` | 4 | CRC32 of the complete index |
| `0x0018` | 8 | Total image size, exactly `33,554,432` bytes |
| `0x0020` | 32 | Reserved, zero-filled |
| `0x0040` | variable | Index records |
| aligned | variable | File payloads |
| remainder | variable | `0xFF` padding |

The index has `entry_count` records of 288 bytes. Payloads start after the index rounded up to 512 bytes. Every non-empty payload is followed by padding to the next 512-byte boundary.

## Index record

| Offset | Size | Meaning |
|---:|---:|---|
| `0x000` | 2 | UTF-8 path length |
| `0x002` | 1 | Flags; bit 0 marks a directory |
| `0x003` | 1 | Reserved, zero |
| `0x004` | 4 | File CRC32; zero for directories |
| `0x008` | 8 | Payload offset; zero for directories |
| `0x010` | 8 | File size; zero for directories |
| `0x018` | 256 | NUL-padded UTF-8 path |
| `0x118` | 8 | Modification time as Unix seconds |

Minor 0 supported files without timestamps. Minor 1 added directories. Minor 2 added modification times. Unknown flags, invalid paths, duplicate paths, overlapping payloads, and out-of-image payloads are invalid.

## Namespace and integrity

- Paths are relative and use `/` separators.
- Leading or trailing `/`, empty components, `.`, and `..` are rejected.
- Every path is unique and no entry may have a file as an ancestor.
- Parent directories must exist for nested content.
- Directory entries have no payload.

Verification checks the header, supported version, exact image size, index size and CRC32, record invariants, path hierarchy, payload layout, and every file CRC32. CRC32 detects corruption but is not cryptographic; compare extracted files with `shasum -a 256` when needed.

## Image workflow

```sh
./build/cmake/ezfs-legacy create archive.ez3fs file1 file2
./build/cmake/ezfs-legacy mkdir archive.ez3fs documents
./build/cmake/ezfs-legacy add archive.ez3fs notes.txt documents/notes.txt
./build/cmake/ezfs-legacy list archive.ez3fs
./build/cmake/ezfs-legacy verify archive.ez3fs
./build/cmake/ezfs-legacy extract archive.ez3fs extracted
```

`create`, `mkdir`, `add`, and `rm` produce a newly packed image rather than updating independent flash blocks.

## Cartridge and recovery workflow

```sh
./build/cmake/ezfs-legacy card-info
./build/cmake/ezfs-legacy card-list
./build/cmake/ezfs-legacy card-verify
./build/cmake/ezfs-legacy card-extract output-directory
./build/cmake/ezfs-legacy card-pull cartridge.ez3fs
./build/cmake/ezfs-legacy card-write cartridge.ez3fs
```

`card-write` verifies the image, asks for confirmation, erases the complete cartridge, programs all 32 MiB, and verifies the result.

The legacy writable mount uses a staging image:

```sh
./build/cmake/ezfs-legacy card-mount mountpoint --writable staging.ez3fs --foreground
./build/cmake/ezfs-legacy card-status staging.ez3fs
./build/cmake/ezfs-legacy card-commit staging.ez3fs
./build/cmake/ezfs-legacy card-recover staging.ez3fs
```

Mounted changes are saved to staging. `card-commit` performs the full cartridge rewrite; `card-recover` restores or validates an interrupted staged transaction. This is separate from EZFA3FS live block updates.

## Limits

Legacy EZ3FS supports files, directories, and modification timestamps. It does not support FAT tools, partitions, direct boot, symlinks, hard links, ownership, permissions, extended attributes, sparse files, or journaling. Its full-image rewrite model is unsuitable for frequent live mutations.
