#include "engine/fs/byte_source.h"

#include <cstdio>
#include <mutex>
#include <utility>

#include "engine/fs/zarchive.h"
#include "engine/platform/os.h"

namespace pt {
namespace {

class FileSource final : public ByteSource {
public:
    FileSource(std::FILE* file, uint64_t size) : file_(file), size_(size) {}
    FileSource(const FileSource&) = delete;
    FileSource& operator=(const FileSource&) = delete;
    ~FileSource() override {
        if (file_) std::fclose(file_);
    }

    uint64_t Size() const override { return size_; }

    bool ReadAt(uint64_t offset, void* buffer, size_t size) const override {
        if (!size) return true;
        if (!buffer || offset > size_ || size > size_ - offset) return false;
        std::lock_guard lock(mutex_);
        if (os::SeekFile(file_, static_cast<int64_t>(offset), SEEK_SET) != 0) return false;
        return std::fread(buffer, 1, size, file_) == size;
    }

private:
    mutable std::mutex mutex_;
    std::FILE* file_ = nullptr;
    uint64_t size_ = 0;
};

class ArchiveSource final : public ByteSource {
public:
    ArchiveSource(std::shared_ptr<ZArchive> archive, int64_t node) : archive_(std::move(archive)), node_(node) {}

    uint64_t Size() const override { return archive_->EntrySize(node_); }
    bool ReadAt(uint64_t offset, void* buffer, size_t size) const override {
        return archive_->ReadAt(node_, offset, buffer, size);
    }

private:
    std::shared_ptr<ZArchive> archive_;
    int64_t node_ = ZArchive::kNone;
};

}

std::shared_ptr<ByteSource> OpenFileSource(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) return nullptr;
    std::FILE* file = os::OpenFile(path, "rb");
    if (!file) return nullptr;
    return std::make_shared<FileSource>(file, size);
}

std::shared_ptr<ByteSource> OpenArchiveEntrySource(std::shared_ptr<ZArchive> archive, int64_t node) {
    if (!archive || !archive->IsFile(node)) return nullptr;
    return std::make_shared<ArchiveSource>(std::move(archive), node);
}

}
