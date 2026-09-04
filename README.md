# EZ3FS

EZ3FS is an independent indexed archive filesystem for the 32-MiB EZF Advance
III NOR flash geometry. It contains no EZ3 loader, menu, ROM catalog, FAT
volume, or GBA ROM patching. Current version: **0.3.2**.

Version 0.3.2 operates on local `.ez3fs` images. Physical-cartridge access is
not implemented. See [EZ3FS_FORMAT.md](EZ3FS_FORMAT.md) for the binary format.

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
Without it, all non-mount commands remain available.

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
