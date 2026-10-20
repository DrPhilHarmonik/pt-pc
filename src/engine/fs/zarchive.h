#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pt {

/* Read-only reader for ZArchive (.zar), the packed game image shadPS4 mounts as /app0
 * (https://github.com/Exzap/ZArchive). The archive holds a game folder at its root and compresses it
 * in 64 KiB blocks with zstd, so a read of any range of any file touches only the blocks it needs:
 * a .zar of a CUSA01127 dump serves chunk1.psarc and texture.qar the same way the folder does
 * (engine/fs/byte_source.h, engine/fs/vfs.cpp).
 *
 * Layout, all integers big-endian: compressed data, offset records (18 bytes per 16 blocks), names,
 * file tree, metadata, and a 144-byte footer holding the six section (offset, size) pairs, the
 * archive size, the version and the magic. File-tree records are 16 bytes: a name offset with the
 * file flag, then (offset, size) for a file or (first child, count) for a directory. The root
 * directory is record 0 and has no name.
 *
 * Thread-safe: reads from one archive may come from several threads. Blocks decompress through a
 * 4 MiB cache (64 blocks), so walking an archive sequentially costs one decompression per block. */
class ZArchive {
public:
    /* Missing node (a path that is not in the archive). */
    static constexpr int64_t kNone = -1;
    static constexpr uint32_t kBlockSize = 65536;

    ZArchive() = default;
    ZArchive(const ZArchive&) = delete;
    ZArchive& operator=(const ZArchive&) = delete;
    ~ZArchive();

    bool Open(const std::filesystem::path& path);
    void Close();
    bool IsOpen() const { return file_ != nullptr; }

    /* Bytes of the archive file itself, and how many file-tree records it holds. */
    uint64_t Size() const { return size_; }
    uint32_t Entries() const { return static_cast<uint32_t>(nodes_.size()); }

    /* Why the last Open() failed, or "" after a success. */
    const std::string& Error() const { return error_; }

    /* True when the file looks like a .zar (its footer matches); lighter than Open(), which reads
     * the tables too. */
    static bool LooksLikeArchive(const std::filesystem::path& path);

    /* Record of a path inside the archive ("/sce_sys/param.sfo", "eboot.bin", "" for the root),
     * kNone when it does not exist. Names are matched without regard to ASCII case, as the format
     * defines. */
    int64_t Find(std::string_view path) const;
    bool IsFile(int64_t node) const;
    /* Files only; 0 for a directory or a missing node. */
    uint64_t EntrySize(int64_t node) const;

    /* Listing of a directory node: writes the child's name and returns true, or returns false past
     * the end. */
    bool Child(int64_t node, uint32_t index, std::string& name, bool* is_file) const;

    /* Exactly length bytes at offset of a file; false when the range is outside the file or one of
     * its blocks cannot be read. */
    bool ReadAt(int64_t node, uint64_t offset, void* buffer, size_t length) const;

    /* A whole file, or nothing when it is missing, is a directory or cannot be read. */
    std::optional<std::vector<uint8_t>> Read(int64_t node) const;
    std::optional<std::vector<uint8_t>> Read(std::string_view path) const;

private:
    struct Node {
        uint32_t name = 0;
        bool file = false;
        uint64_t offset = 0;
        uint64_t size = 0;
        uint32_t first = 0;
        uint32_t count = 0;
    };
    struct OffsetRecord {
        uint64_t base = 0;
        uint16_t size[16] = {};
    };
    struct CacheBlock {
        uint64_t block = 0;
        uint64_t age = 0;
        std::vector<uint8_t> data;
    };

    bool Fail(std::string_view reason);
    bool ReadRaw(uint64_t offset, void* buffer, size_t length) const;
    std::string_view NameOf(uint32_t offset) const;
    const uint8_t* BlockData(uint64_t block) const;

    mutable std::mutex mutex_;
    mutable std::FILE* file_ = nullptr;
    uint64_t size_ = 0;
    std::vector<Node> nodes_;
    std::vector<OffsetRecord> records_;
    std::vector<uint8_t> names_;
    uint64_t data_offset_ = 0;
    uint64_t data_size_ = 0;
    mutable std::vector<CacheBlock> cache_;
    mutable std::uint64_t clock_ = 0;
    mutable std::vector<uint8_t> scratch_;
    std::string error_;
};

}
