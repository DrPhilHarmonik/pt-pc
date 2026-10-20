# ZArchive (.zar)

`chunk1.psarc` and `texture.qar` do not have to sit in a folder: the game also plays them out of a `.zar`, the packed
game image [ZArchive](https://github.com/Exzap/ZArchive) creates and shadPS4 mounts as `/app0`. The archive holds a game
folder at its root and compresses it in 64 KiB blocks with zstd, so reading any range of any file decompresses only the
blocks that range touches.

The port reads it with `src/engine/fs/zarchive.h`; `chunk1.psarc` and `texture.qar` are read out of it through
`src/engine/fs/byte_source.h`, so the archives' own code does not know whether they are in a file or in a `.zar`
(`src/engine/fs/vfs.cpp`). The archive is mounted, never extracted.

## Footer (last 144 bytes, big-endian)

| offset | field |
| --- | --- |
| 0x00 | six section pairs, u64 offset and u64 size: compressed data, offset records, names, file tree, metadata, unknown |
| 0x60 | 32 bytes of zero |
| 0x80 | u64 archive size, the file's own length |
| 0x88 | u32 version, `0x61BF3A01` |
| 0x8C | u32 magic, `0x169F52D6` |

A file whose footer does not match these three values at that length is not a `.zar`; the reader refuses it before
reading anything else.

## Compressed data and the offset records

Files point into the data section, whose length is the first section pair's size. The data is cut in 64 KiB blocks. An
offset record is 40 bytes: a u64 base (where its first block starts in the data section) and 16 u16 sizes, one per block,
each **one less** than the bytes the block holds. `0xFFFF` therefore means an uncompressed 64 KiB block. A block's
offset is the record's base plus the sizes of the earlier blocks of that record; the reader also accepts a plain block
(no compression) whenever a size works out to a full 64 KiB, which is what the format's packer writes when zstd does
not shrink a block.

Every block decompresses to exactly 64 KiB; the last block of a file is padded with zeros.

## Names

The name table is a byte string. Each entry is a length, then the name bytes (Windows-1252, taken as opaque bytes): bit 7
of the first length byte adds 15 bits from a second byte (`size = (b0 & 0x7F) | (b1 << 7)`). Tree records store the
offset of the entry. The root directory's entry is a single zero byte, an empty name at offset 0.

## File tree

16-byte records, big-endian; record 0 is the root, which has no name:

| offset | file | directory |
| --- | --- | --- |
| 0x00 | u32 name offset, bit 31 set | u32 name offset, bit 31 clear |
| 0x04 | u32 file offset (low 32 bits) | u32 first child record |
| 0x08 | u32 size (low 32 bits) | u32 child count |
| 0x0C | u16 offset bits 32-47, u16 size bits 32-47 | unused |

A directory's children are contiguous records starting at its first child index. Paths are matched without regard to
ASCII case, and the root is `""`.

## What the port reads

`Vfs::Mount` accepts a folder with `chunk1.psarc` and `texture.qar`, or a `.zar` that has them at its root or up to two
folders deep (a `.zar` of the `CUSA01127` folder, of the dump that contains it, or of the dump's own root all work).
Everything else the game reads from the folder (`source.txt` for the installer's notes) is read from the archive at the
same place, and mods still override both. `sce_sys/pic1.png` is not read through the archives, so a `.zar` start has no
title picture and goes straight to its loading screen.

`tools/` has no packer: use ZArchive's own, which is the one shadPS4's `.zar` support writes.
