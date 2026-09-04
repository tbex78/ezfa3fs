# EZ3FS-LIVE format 1.0.0

EZ3FS-LIVE is the experimental copy-on-write filesystem for the 32-MiB
EZ-Flash Advance III NOR geometry. It is incompatible with packed EZ3FS
images and has a distinct `EZ3LIVE\0` magic. This prototype is currently
available through a persistent 32-MiB image backend and an in-memory simulator;
physical-cartridge integration is not enabled yet.

Command-line workflow:

```sh
ez3fs live-format cartridge.ez3live
ez3fs live-mkdir cartridge.ez3live docs
ez3fs live-put cartridge.ez3live README.md docs/README.md
ez3fs live-list cartridge.ez3live
ez3fs live-verify cartridge.ez3live
ez3fs live-mount cartridge.ez3live /Volumes/EZ3FS-LIVE
ez3fs live-get cartridge.ez3live docs/README.md recovered.md
ez3fs live-rm cartridge.ez3live docs/README.md
ez3fs live-rmdir cartridge.ez3live docs
ez3fs live-card-write cartridge.ez3live
```

Mutating commands commit the image before exiting, so changes survive separate
processes. The image is exactly 32 MiB and is not compatible with packed
`.ez3fs` images.

`live-card-write` is destructive: it erases and replaces the complete
cartridge after validating the image and requiring the confirmation phrase
`WRITE EZ3FS-LIVE`.

The cartridge is divided into 512 blocks of 64 KiB. Blocks 0 and 1 are
generation-numbered redundant superblocks. File data starts at block 2 and is
append-only. Updating a file allocates new blocks and commits a new manifest;
the previous generation remains valid until the new superblock is complete.

Each superblock contains the magic, format version, generation, manifest
length, manifest CRC-32, and a commit marker. The manifest records paths,
file sizes, modification times, CRC-32 values, and allocated block extents.
Directories have no data blocks. Deletes create a new manifest without the
entry; obsolete data blocks are reclaimed by a future garbage collector.

NOR programming enforces one-way `1 -> 0` bit transitions. Erasing is only
possible for a complete 64-KiB block. A torn write to the inactive superblock
is ignored on open, which recovers the newest complete generation.
