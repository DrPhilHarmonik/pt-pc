#include "engine/fs/zarchive.h"

#include <zstd.h>

#include <algorithm>
#include <cstring>

#include "engine/platform/os.h"

namespace pt {
namespace {

constexpr uint32_t kMagic = 0x169f52d6u;
constexpr uint32_t kVersion = 0x61bf3a01u;
constexpr uint32_t kRecordsPerOffset = 16;   // block sizes stored in one offset record
constexpr uint32_t kOffsetRecordSize = 40;   // on disk: base offset (8) and 16 sizes (2 each)
constexpr uint32_t kTreeRecordSize = 16;
constexpr uint32_t kFooterSize = 144;
constexpr size_t kCacheBlocks = 64;          // 4 MiB of decompressed blocks
constexpr uint64_t kMaxNames = 64ull << 20;  // tables larger than this are not a game dump
constexpr uint64_t kMaxTree = 128ull << 20;
constexpr uint64_t kMaxRecords = 32ull << 20;  // 838,000 blocks (52 GiB) of game data
constexpr uint32_t kMaxDepth = 64;

uint16_t Be16(const uint8_t* p) { return uint16_t((uint16_t(p[0]) << 8) | p[1]); }

uint32_t Be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

uint64_t Be64(const uint8_t* p) { return (uint64_t(Be32(p)) << 32) | Be32(p + 4); }

/* The format defines names as case-insensitive ASCII. */
bool SameName(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

}

ZArchive::~ZArchive() { Close(); }

bool ZArchive::Fail(std::string_view reason) {
    error_.assign(reason);
    return false;
}

void ZArchive::Close() {
    if (file_) std::fclose(file_);
    file_ = nullptr;
    size_ = 0;
    nodes_.clear();
    records_.clear();
    names_.clear();
    cache_.clear();
    scratch_.clear();
    data_offset_ = 0;
    data_size_ = 0;
    clock_ = 0;
}

bool ZArchive::ReadRaw(uint64_t offset, void* buffer, size_t length) const {
    if (!file_ || offset > size_ || length > size_ - offset) return false;
    if (os::SeekFile(file_, static_cast<int64_t>(offset), SEEK_SET) != 0) return false;
    return std::fread(buffer, 1, length, file_) == length;
}

std::string_view ZArchive::NameOf(uint32_t offset) const {
    if (offset >= names_.size()) return {};
    uint32_t size = names_[offset] & 0x7Fu;
    size_t start = size_t(offset) + 1;
    if (names_[offset] & 0x80u) {
        if (start >= names_.size()) return {};
        size |= uint32_t(names_[start]) << 7;
        ++start;
    }
    if (start > names_.size() || size > names_.size() - start) return {};
    return std::string_view(reinterpret_cast<const char*>(names_.data() + start), size);
}

bool ZArchive::Open(const std::filesystem::path& path) {
    Close();
    std::error_code error;
    const auto on_disk = std::filesystem::file_size(path, error);
    if (error) return Fail("cannot open the archive");
    file_ = os::OpenFile(path, "rb");
    if (!file_) return Fail("cannot open the archive");
    size_ = on_disk;
    const auto fail = [this](std::string_view reason) {
        Close();
        return Fail(reason);
    };
    if (size_ <= kFooterSize) return fail("not a ZArchive (too small)");

    uint8_t footer[kFooterSize];
    if (!ReadRaw(size_ - kFooterSize, footer, sizeof(footer))) return fail("cannot read the footer");
    if (Be32(footer + kFooterSize - 4) != kMagic || Be32(footer + kFooterSize - 8) != kVersion ||
        Be64(footer + kFooterSize - 16) != size_) {
        return fail("not a ZArchive (bad footer)");
    }
    uint64_t sections[6][2] = {};
    for (unsigned i = 0; i < 6; ++i) {
        sections[i][0] = Be64(footer + i * 16);
        sections[i][1] = Be64(footer + i * 16 + 8);
        if (sections[i][0] > size_ || sections[i][1] > size_ - sections[i][0]) {
            return fail("not a ZArchive (section outside the file)");
        }
    }
    data_offset_ = sections[0][0];
    data_size_ = sections[0][1];
    const uint64_t offsets_size = sections[1][1], names_size = sections[2][1], tree_size = sections[3][1];
    if (!offsets_size || offsets_size % kOffsetRecordSize || names_size > kMaxNames || !tree_size ||
        tree_size % kTreeRecordSize || tree_size > kMaxTree || offsets_size > kMaxRecords) {
        return fail("not a ZArchive (bad table sizes)");
    }
    records_.resize(size_t(offsets_size / kOffsetRecordSize));
    nodes_.resize(size_t(tree_size / kTreeRecordSize));
    names_.resize(size_t(names_size));
    scratch_.resize(kBlockSize);
    cache_.resize(kCacheBlocks);
    for (CacheBlock& block : cache_) block.data.resize(kBlockSize);

    std::vector<uint8_t> raw_offsets(size_t(offsets_size));
    std::vector<uint8_t> raw_tree(size_t(tree_size));
    const bool tables = ReadRaw(sections[1][0], raw_offsets.data(), raw_offsets.size()) &&
                        (names_.empty() || ReadRaw(sections[2][0], names_.data(), names_.size())) &&
                        ReadRaw(sections[3][0], raw_tree.data(), raw_tree.size());
    if (!tables) {
        return fail("cannot read the archive tables");
    }
    for (size_t i = 0; i < records_.size(); ++i) {
        const uint8_t* entry = raw_offsets.data() + i * kOffsetRecordSize;
        OffsetRecord& record = records_[i];
        record.base = Be64(entry);
        for (unsigned j = 0; j < kRecordsPerOffset; ++j) record.size[j] = Be16(entry + 8 + j * 2);
    }
    for (size_t i = 0; i < nodes_.size(); ++i) {
        const uint8_t* entry = raw_tree.data() + i * kTreeRecordSize;
        Node& node = nodes_[i];
        const uint32_t name_and_flag = Be32(entry);
        node.name = name_and_flag & 0x7FFFFFFFu;
        node.file = (name_and_flag & 0x80000000u) != 0;
        const uint32_t low = Be32(entry + 4), high = Be32(entry + 12);
        if (node.file) {
            /* Files: offset low, size low, then 16 bits each of the extensions. */
            node.offset = uint64_t(low) | (uint64_t(high & 0xFFFFu) << 32);
            node.size = uint64_t(Be32(entry + 8)) | (uint64_t(high >> 16) << 32);
        } else {
            /* Directories: first child record and its count. */
            node.first = low;
            node.count = Be32(entry + 8);
        }
    }
    if (nodes_[0].file || !NameOf(nodes_[0].name).empty()) return fail("not a ZArchive (bad root)");
    error_.clear();
    return true;
}

bool ZArchive::LooksLikeArchive(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size <= kFooterSize) return false;
    std::FILE* file = os::OpenFile(path, "rb");
    if (!file) return false;
    uint8_t footer[kFooterSize];
    const bool read = os::SeekFile(file, static_cast<int64_t>(size - kFooterSize), SEEK_SET) == 0 &&
                      std::fread(footer, 1, sizeof(footer), file) == sizeof(footer);
    std::fclose(file);
    if (!read) return false;
    return Be32(footer + kFooterSize - 4) == kMagic && Be32(footer + kFooterSize - 8) == kVersion &&
           Be64(footer + kFooterSize - 16) == uint64_t(size);
}

bool ZArchive::IsFile(int64_t node) const {
    return node >= 0 && size_t(node) < nodes_.size() && nodes_[size_t(node)].file;
}

uint64_t ZArchive::EntrySize(int64_t node) const {
    return IsFile(node) ? nodes_[size_t(node)].size : 0;
}

int64_t ZArchive::Find(std::string_view path) const {
    if (nodes_.empty()) return kNone;
    int64_t node = 0;
    int64_t current = 0;
    unsigned depth = 0;
    for (size_t at = 0;;) {
        while (at < path.size() && (path[at] == '/' || path[at] == '\\')) ++at;
        if (at >= path.size()) break;
        const size_t start = at;
        while (at < path.size() && path[at] != '/' && path[at] != '\\') ++at;
        const std::string_view part = path.substr(start, at - start);
        if (part == "." || part == "..") return kNone;
        if (++depth > kMaxDepth) return kNone;
        const Node& dir = nodes_[size_t(current)];
        if (dir.file || dir.first > nodes_.size() || dir.count > nodes_.size() - dir.first) return kNone;
        bool found = false;
        for (uint32_t i = 0; i < dir.count; ++i) {
            if (SameName(NameOf(nodes_[dir.first + i].name), part)) {
                current = node = int64_t(dir.first + i);
                found = true;
                break;
            }
        }
        if (!found) return kNone;
    }
    return node;
}

bool ZArchive::Child(int64_t node, uint32_t index, std::string& name, bool* is_file) const {
    if (node < 0 || size_t(node) >= nodes_.size()) return false;
    const Node& dir = nodes_[size_t(node)];
    if (dir.file || dir.first > nodes_.size() || dir.count > nodes_.size() - dir.first || index >= dir.count) return false;
    const Node& child = nodes_[dir.first + index];
    name.assign(NameOf(child.name));
    if (is_file) *is_file = child.file;
    return true;
}

/* The 64 KiB block `block`, decompressed into the cache; nullptr when it cannot be read. The caller
 * holds mutex_. */
const uint8_t* ZArchive::BlockData(uint64_t block) const {
    if (block >= uint64_t(records_.size()) * kRecordsPerOffset) return nullptr;
    size_t victim = 0;
    for (size_t i = 0; i < cache_.size(); ++i) {
        CacheBlock& slot = cache_[i];
        if (slot.age && slot.block == block) {
            slot.age = ++clock_;
            return slot.data.data();
        }
        if (cache_[victim].age == 0 || slot.age < cache_[victim].age) victim = i;
    }
    const OffsetRecord& record = records_[size_t(block / kRecordsPerOffset)];
    const uint32_t sub = uint32_t(block % kRecordsPerOffset);
    uint64_t offset = record.base;
    for (uint32_t i = 0; i < sub; ++i) offset += uint64_t(record.size[i]) + 1;
    const uint32_t stored = uint32_t(record.size[sub]) + 1;
    if (offset > data_size_ || stored > data_size_ - offset) return nullptr;
    CacheBlock& slot = cache_[victim];
    slot.age = 0;
    if (!ReadRaw(data_offset_ + offset, stored == kBlockSize ? slot.data.data() : scratch_.data(), stored)) return nullptr;
    if (stored != kBlockSize) {
        /* Blocks are stored uncompressed only when compression does not help; every block holds a
         * full 64 KiB, the last one padded with zeros. */
        const size_t produced = ZSTD_decompress(slot.data.data(), kBlockSize, scratch_.data(), stored);
        if (ZSTD_isError(produced) || produced != kBlockSize) return nullptr;
    }
    slot.block = block;
    slot.age = ++clock_;
    return slot.data.data();
}

bool ZArchive::ReadAt(int64_t node, uint64_t offset, void* buffer, size_t length) const {
    if (length && !buffer) return false;
    if (!IsFile(node)) return false;
    const Node& file = nodes_[size_t(node)];
    if (offset > file.size || length > file.size - offset) return false;
    if (!length) return true;
    uint64_t raw = file.offset + offset;
    uint64_t remaining = length;
    auto* out = static_cast<uint8_t*>(buffer);
    std::lock_guard lock(mutex_);
    while (remaining) {
        const uint64_t block = raw / kBlockSize;
        const uint32_t within = uint32_t(raw % kBlockSize);
        const uint64_t step = std::min<uint64_t>(remaining, kBlockSize - within);
        const uint8_t* data = BlockData(block);
        if (!data) return false;
        std::memcpy(out, data + within, size_t(step));
        raw += step;
        out += step;
        remaining -= step;
    }
    return true;
}

std::optional<std::vector<uint8_t>> ZArchive::Read(int64_t node) const {
    if (!IsFile(node)) return std::nullopt;
    std::vector<uint8_t> data(size_t(EntrySize(node)));
    if (!data.empty() && !ReadAt(node, 0, data.data(), data.size())) return std::nullopt;
    return data;
}

std::optional<std::vector<uint8_t>> ZArchive::Read(std::string_view path) const {
    const int64_t node = Find(path);
    if (node == kNone) return std::nullopt;
    return Read(node);
}

}
