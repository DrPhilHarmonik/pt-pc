#include "engine/fs/vfs.h"

#include <chrono>
#include <cstdio>
#include <iterator>

#include "engine/core/log.h"
#include "engine/fs/byte_source.h"
#include "engine/fs/io_stats.h"
#include "engine/fs/mods.h"
#include "engine/platform/os.h"

namespace pt {

bool Vfs::Mount(const std::filesystem::path& game) {
    game_dir_ = game;
    std::error_code error;
    if (std::filesystem::is_directory(game, error)) {
        return MountFolder(game);
    }
    if (std::filesystem::is_regular_file(game, error)) {
        return MountArchive(game);
    }
    LogError("vfs: {} is neither the extracted CUSA01127 folder nor a .zar archive of it", pt::os::PathToUtf8(game));
    return false;
}

bool Vfs::MountFolder(const std::filesystem::path& game_dir) {
    const auto psarc_path = game_dir / "chunk1.psarc";
    if (!std::filesystem::exists(psarc_path)) {
        LogError("vfs: {} not found, game dir must be the extracted CUSA01127 folder", pt::os::PathToUtf8(psarc_path));
        return false;
    }
    if (!archive_.Open(psarc_path)) {
        return false;
    }
    const auto qar_path = game_dir / "texture.qar";
    if (!std::filesystem::exists(qar_path) || !textures_.Open(qar_path)) {
        LogError("vfs: {} missing or unreadable", pt::os::PathToUtf8(qar_path));
        return false;
    }
    ReportDataDifferences();
    return true;
}

bool Vfs::MountArchive(const std::filesystem::path& file) {
    auto zar = std::make_shared<ZArchive>();
    if (!zar->Open(file)) {
        LogError("vfs: {} is not a readable .zar: {}", pt::os::PathToUtf8(file), zar->Error());
        return false;
    }
    const std::string root = FindArchiveRoot(*zar);
    const auto inside = [&root](std::string_view name) {
        return root.empty() ? std::string(name) : root + "/" + std::string(name);
    };
    const auto psarc = zar->Find(inside("chunk1.psarc"));
    const auto qar = zar->Find(inside("texture.qar"));
    if (psarc == ZArchive::kNone || !zar->IsFile(psarc)) {
        LogError("vfs: {} has no chunk1.psarc at its root or two folders deep; pack the CUSA01127 folder (or the "
                 "dump that holds it)",
                 pt::os::PathToUtf8(file));
        return false;
    }
    if (qar == ZArchive::kNone || !zar->IsFile(qar)) {
        LogError("vfs: {} has no texture.qar beside {}", pt::os::PathToUtf8(file), inside("chunk1.psarc"));
        return false;
    }
    if (!archive_.Open(OpenArchiveEntrySource(zar, psarc), inside("chunk1.psarc")) ||
        !textures_.Open(OpenArchiveEntrySource(zar, qar), inside("texture.qar"))) {
        return false;
    }
    zar_ = zar;
    archive_root_ = root;
    LogInfo("vfs: game files from {} ({} entries inside the .zar)", pt::os::PathToUtf8(file), zar->Entries());
    ReportDataDifferences();
    return true;
}

std::string Vfs::FindArchiveRoot(const ZArchive& zar) const {
    const auto has_game_files = [&zar](const std::string& root) {
        const auto inside = [&root](std::string_view name) {
            return root.empty() ? std::string(name) : root + "/" + std::string(name);
        };
        const auto psarc = zar.Find(inside("chunk1.psarc"));
        const auto qar = zar.Find(inside("texture.qar"));
        return psarc != ZArchive::kNone && zar.IsFile(psarc) && qar != ZArchive::kNone && zar.IsFile(qar);
    };
    if (has_game_files("")) return {};
    std::vector<std::string> level{""};
    for (int depth = 0; depth < 2 && !level.empty(); ++depth) {
        std::vector<std::string> next;
        for (const std::string& dir : level) {
            const auto node = zar.Find(dir);
            if (node == ZArchive::kNone || zar.IsFile(node)) continue;
            for (uint32_t index = 0;; ++index) {
                std::string name;
                bool is_file = true;
                if (!zar.Child(node, index, name, &is_file)) break;
                if (is_file) continue;
                const std::string child = dir.empty() ? name : dir + "/" + name;
                if (has_game_files(child)) return child;
                if (next.size() < 256) next.push_back(child);
            }
        }
        level = std::move(next);
    }
    return {};
}

namespace {
constexpr const char* kCoreArchive[] = {
    "as/sh/level/common/resident.fpk",
    "as/sh/level/promotion/pt_2014/start/pt14_start.fpk",
    "as/sh/level/promotion/pt_2014/hallway/pt14_hallway.fpk",
    "as/sh/level/promotion/pt_2014/hallway_maze_A/pt14_hallway_maze_A.fpk",
    "as/sh/level/promotion/pt_2014/hallway_maze_B/pt14_hallway_maze_B.fpk",
    "as/sh/level/promotion/pt_2014/hallway_maze_C/pt14_hallway_maze_C.fpk",
    "as/sh/level/promotion/pt_2014/ending/ending.fpk",
    "as/sh/level_asset/chara/player/game_object/player2_common_motion.fpk",
    "as/sh/level_asset/chara/player/game_object/plparts_normal.fpk",
    "as/sh/ui/ui_default_data.fpk",
    "as/sh/ui/ui_default_lang.fpk",
    "as/sh/level/ui/subtitles/EngVoice/EngText/subtitle.fpk",
};
}

int Vfs::MissingCorePackages() const {
    int missing = 0;
    for (const char* path : kCoreArchive) {
        if (!archive_.Contains(path)) ++missing;
    }
    return missing;
}

std::optional<std::vector<uint8_t>> Vfs::ReadLoose(std::string_view path) const {
    const std::string name = ToArchivePath(path);
    if (zar_) {
        return zar_->Read(archive_root_.empty() ? name : archive_root_ + "/" + name);
    }
    const auto loose = game_dir_ / std::filesystem::path(name);
    if (!std::filesystem::is_regular_file(loose)) {
        return std::nullopt;
    }
    const auto read_start = std::chrono::steady_clock::now();
    FILE* f = os::OpenFile(loose, "rb");
    if (!f) return std::nullopt;
    IoSimulatedLatency();
    std::vector<uint8_t> data(static_cast<size_t>(std::filesystem::file_size(loose)));
    const size_t read = std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    IoRecord(read, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - read_start).count(), 0.0);
    if (read != data.size()) {
        return std::nullopt;
    }
    return data;
}

void Vfs::ReportDataDifferences() const {
    if (const auto source = ReadLoose("source.txt")) {
        const std::string_view text(reinterpret_cast<const char*>(source->data()), source->size());
        for (size_t at = 0; at <= text.size();) {
            size_t end = text.find('\n', at);
            if (end == std::string_view::npos) end = text.size();
            std::string line(text.substr(at, end - at));
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.starts_with("warning=")) {
                LogWarn("data: {}", line.substr(8));
            } else if (!line.empty()) {
                LogInfo("data: {}", line);
            }
            at = end + 1;
        }
    }

    int missing = 0;
    for (const char* path : kCoreArchive) {
        if (!archive_.Contains(path)) {
            LogWarn("data: {} missing from chunk1.psarc, dependent content will not load", path);
            ++missing;
        }
    }
    if (missing == 0) {
        LogInfo("data: {} core packages present", std::size(kCoreArchive));
    }
}

std::string Vfs::ToArchivePath(std::string_view asset_path) {
    std::string path(asset_path);
    for (char& c : path) {
        if (c == '\\') {
            c = '/';
        }
    }
    constexpr std::string_view kAssets = "/Assets/";
    if (path.starts_with(kAssets)) {
        return "as/" + path.substr(kAssets.size());
    }
    while (path.starts_with('/')) {
        path.erase(0, 1);
    }
    return path;
}

std::shared_ptr<FoxPackage> Vfs::LoadPackage(std::string_view path) {
    const std::string archive_path = ToArchivePath(path);
    {
        std::lock_guard lock(mutex_);
        auto it = packages_.find(archive_path);
        if (it != packages_.end()) {
            return it->second;
        }
    }
    auto data = mods::ReadOverride(archive_path);
    if (data) {
        LogInfo("vfs: {} from a mod", archive_path);
    } else {
        data = archive_.Read(archive_path);
    }
    if (!data) {
        LogError("vfs: package {} not found", archive_path);
        return nullptr;
    }
    auto package = std::make_shared<FoxPackage>();
    if (!package->Load(archive_path, std::move(*data))) {
        return nullptr;
    }
    LogInfo("vfs: loaded {} ({} files)", archive_path, package->Entries().size());
    std::lock_guard lock(mutex_);
    packages_[archive_path] = package;
    return package;
}

std::vector<std::shared_ptr<FoxPackage>> Vfs::LoadedPackages() const {
    std::lock_guard lock(mutex_);
    std::vector<std::shared_ptr<FoxPackage>> out;
    for (const auto& [name, package] : packages_) {
        out.push_back(package);
    }
    return out;
}

void Vfs::UnloadPackage(std::string_view path) {
    std::lock_guard lock(mutex_);
    packages_.erase(ToArchivePath(path));
}

std::optional<std::vector<uint8_t>> Vfs::ReadFile(std::string_view path) const {
    if (IoTraceOn()) {
        const auto start = std::chrono::steady_clock::now();
        auto data = ReadFileUntraced(path);
        IoTrace("read", path, data ? data->size() : 0, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        return data;
    }
    return ReadFileUntraced(path);
}

std::optional<std::vector<uint8_t>> Vfs::ReadFileUntraced(std::string_view path) const {
    if (auto data = mods::ReadOverride(path)) {
        return data;
    }
    {
        std::lock_guard lock(mutex_);
        for (const auto& [name, package] : packages_) {
            if (const auto* entry = package->Find(path)) {
                return package->Read(*entry);
            }
        }
    }
    if (auto data = archive_.Read(ToArchivePath(path))) {
        return data;
    }
    return ReadLoose(path);
}

}
