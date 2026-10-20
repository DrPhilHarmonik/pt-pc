#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace pt {

class ZArchive;

/* The read-only bytes an archive reads its tables and its entries from: a whole file on disk, or
 * one file inside a mounted .zar, whose 64 KiB blocks are decompressed on demand. chunk1.psarc and
 * texture.qar read through this, so a .zar of the game folder needs no copy of them on disk
 * (engine/fs/vfs.cpp mounts it). */
class ByteSource {
public:
    virtual ~ByteSource() = default;

    virtual uint64_t Size() const = 0;

    /* Exactly size bytes at offset; false when the range is outside the source or cannot be read.
     * Thread-safe: the archives read from several threads. */
    virtual bool ReadAt(uint64_t offset, void* buffer, size_t size) const = 0;
};

/* A whole file; nullptr when it cannot be opened. */
std::shared_ptr<ByteSource> OpenFileSource(const std::filesystem::path& path);

/* A file inside a mounted .zar (a node from ZArchive::Find); nullptr for a missing node. The source
 * keeps the archive open for as long as it lives. */
std::shared_ptr<ByteSource> OpenArchiveEntrySource(std::shared_ptr<ZArchive> archive, int64_t node);

}
