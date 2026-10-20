#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine/fs/byte_source.h"

namespace pt {

class QarArchive {
public:
    QarArchive() = default;
    QarArchive(const QarArchive&) = delete;
    QarArchive& operator=(const QarArchive&) = delete;

    /* texture.qar on disk. */
    bool Open(const std::filesystem::path& path);
    /* Any range of bytes that holds a QAR: a .zar of the game folder serves it from inside the
     * archive (engine/fs/byte_source.h). name goes in the log lines. */
    bool Open(std::shared_ptr<ByteSource> source, std::string_view name = {});

    bool Contains(uint64_t code) const { return entries_.contains(code); }
    std::optional<std::vector<uint8_t>> Read(uint64_t code) const;
    size_t Count() const { return entries_.size(); }

private:
    struct Entry {
        uint64_t offset;
        uint32_t size;
    };

    std::shared_ptr<ByteSource> source_;
    std::unordered_map<uint64_t, Entry> entries_;
};

}
