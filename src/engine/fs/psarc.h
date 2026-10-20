#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine/fs/byte_source.h"

namespace pt {

class Psarc {
public:
    Psarc() = default;
    Psarc(const Psarc&) = delete;
    Psarc& operator=(const Psarc&) = delete;

    /* chunk1.psarc on disk. */
    bool Open(const std::filesystem::path& path);
    /* Any range of bytes that holds a PSARC: a .zar of the game folder serves it from inside the
     * archive (engine/fs/byte_source.h). name goes in the log lines. */
    bool Open(std::shared_ptr<ByteSource> source, std::string_view name = {});

    const std::vector<std::string>& Names() const { return names_; }
    bool Contains(std::string_view name) const;
    std::optional<std::vector<uint8_t>> Read(std::string_view name) const;
    std::optional<uint64_t> Size(std::string_view name) const;

private:
    struct Entry {
        uint32_t first_block;
        uint64_t size;
        uint64_t offset;
    };

    std::optional<std::vector<uint8_t>> ReadEntry(size_t index) const;

    std::shared_ptr<ByteSource> source_;
    uint32_t block_size_ = 0;
    std::vector<Entry> entries_;
    std::vector<uint32_t> block_sizes_;
    std::vector<std::string> names_;
    std::unordered_map<std::string, size_t> index_;
};

}
