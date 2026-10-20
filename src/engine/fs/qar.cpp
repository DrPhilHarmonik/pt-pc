#include "engine/fs/qar.h"

#include <chrono>
#include <cstring>
#include <utility>

#include "engine/core/log.h"
#include "engine/fs/io_stats.h"
#include "engine/platform/os.h"

namespace pt {
namespace {

constexpr uint64_t kFooterSize = 0x24;

}

bool QarArchive::Open(const std::filesystem::path& path) {
    auto source = OpenFileSource(path);
    if (!source) {
        LogError("qar: cannot open {}", pt::os::PathToUtf8(path));
        return false;
    }
    return Open(std::move(source), pt::os::PathToUtf8(path.filename()));
}

bool QarArchive::Open(std::shared_ptr<ByteSource> source, std::string_view name) {
    if (!source) {
        LogError("qar: nothing to read for {}", name);
        return false;
    }
    source_ = std::move(source);
    if (source_->Size() < kFooterSize) {
        LogError("qar: {} is too small to hold a QAR", name);
        return false;
    }
    uint8_t footer[kFooterSize];
    if (!source_->ReadAt(source_->Size() - kFooterSize, footer, sizeof(footer))) {
        LogError("qar: cannot read the footer of {}", name);
        return false;
    }
    uint32_t count;
    uint16_t magic;
    uint32_t table_offset16;
    std::memcpy(&count, footer + 0x10, 4);
    std::memcpy(&magic, footer + 0x16, 2);
    std::memcpy(&table_offset16, footer + 0x18, 4);
    if (magic != 0x7161) {
        LogError("qar: {} has no QAR footer", name);
        return false;
    }
    /* The table cannot be larger than the file it sits in; a bigger count is a bad footer. */
    if (uint64_t(count) * 16 > source_->Size()) {
        LogError("qar: {} has {} entries, more than its {} bytes hold", name, count, source_->Size());
        return false;
    }
    std::vector<uint8_t> table(size_t(count) * 16);
    if (!source_->ReadAt(uint64_t(table_offset16) << 4, table.data(), table.size())) {
        LogError("qar: cannot read the table of {}", name);
        return false;
    }
    entries_.clear();
    entries_.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t code;
        uint32_t offset16;
        uint32_t size;
        std::memcpy(&code, table.data() + i * 16, 8);
        std::memcpy(&offset16, table.data() + i * 16 + 8, 4);
        std::memcpy(&size, table.data() + i * 16 + 12, 4);
        entries_[code] = {static_cast<uint64_t>(offset16) << 4, size};
    }
    LogInfo("qar: {} entries in {}", entries_.size(), name);
    return true;
}

std::optional<std::vector<uint8_t>> QarArchive::Read(uint64_t code) const {
    auto it = entries_.find(code);
    if (it == entries_.end()) {
        return std::nullopt;
    }
    std::vector<uint8_t> data(it->second.size);
    const auto read_start = std::chrono::steady_clock::now();
    IoSimulatedLatency();
    if (!source_->ReadAt(it->second.offset, data.data(), data.size())) {
        return std::nullopt;
    }
    IoRecord(data.size(), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - read_start).count(), 0.0);
    return data;
}

}
