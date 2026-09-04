# EZ3FS

EZ3FS is an independent indexed archive filesystem for the 32-MiB EZF Advance
III NOR flash geometry. It contains no EZ3 loader, menu, ROM catalog, FAT
volume, or GBA ROM patching. Current version: **0.6.0**.

Version 0.4.0 added read-only physical-cartridge inspection and extraction.
Version 0.4.1 recognizes an EZ3FS signature as a safe fallback when a genuine
EZ3 cartridge does not return one of the two previously captured flash IDs.
Version 0.5.0 adds confirmed raw cartridge programming with full read-back
verification.
Version 0.6.0 adds read-only mounting directly from a physical cartridge.
See [EZ3FS_FORMAT.md](EZ3FS_FORMAT.md) for the binary format.

## Build

```sh
cmake -S . -B build/cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build/cmake --target ez3fs
```

Or:

```sh
make
```

FUSE mounting is enabled automatically when `pkg-config fuse3` is available.
Physical-cartridge commands are enabled when `pkg-config libusb-1.0` is
available. Other commands remain available when either dependency is absent.

## Image operations

```sh
./ez3fs create cartridge.ez3fs
./ez3fs mkdir cartridge.ez3fs documents
./ez3fs add cartridge.ez3fs local.txt documents/local.txt
./ez3fs rm cartridge.ez3fs documents/local.txt
./ez3fs rmdir cartridge.ez3fs documents
./ez3fs list cartridge.ez3fs
./ez3fs verify cartridge.ez3fs
./ez3fs extract cartridge.ez3fs output
```

## FUSE/macFUSE

Mounting is read-only by default:

```sh
mkdir mountpoint
./ez3fs mount cartridge.ez3fs mountpoint
```

Writable mounting must be explicit:

```sh
./ez3fs mount cartridge.ez3fs mountpoint --writable --foreground
```

Changes are staged and committed to the image on metadata mutations, flush,
`fsync`, and clean unmount.

## Physical cartridge (read-only)

Connect the EZ-Flash Advance III USB writer and use:

```sh
./ez3fs card-info
./ez3fs card-list
./ez3fs card-verify
./ez3fs card-extract output
```

These commands recognize an EZ3FS image beginning at cartridge offset zero.
The inspection and extraction commands never program or erase cartridge
memory. To erase the complete cartridge, program an image at offset zero, and
verify every programmed byte:

```sh
./ez3fs card-write cartridge.ez3fs
```

The command validates the EZ3FS image before opening the USB device and
requires the exact confirmation text `WRITE EZ3FS` before any modification.

Mount the physical cartridge read-only through FUSE/macFUSE:

```sh
mkdir -p mountpoint
./ez3fs card-mount mountpoint
```

The image is read and verified before mounting, then the USB session is
closed. Changes through this mount are rejected; use a local writable mount
and `card-write` when you intentionally want to replace cartridge contents.
