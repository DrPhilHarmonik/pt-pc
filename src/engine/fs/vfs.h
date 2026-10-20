#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine/fs/fpk.h"
#include "engine/fs/psarc.h"
#include "engine/fs/qar.h"
#include "engine/fs/zarchive.h"

namespace pt {

class Vfs {
public:
    int MissingCorePackages() const;
    /* The game files: the extracted CUSA01127 folder, or a .zar of it (a .zar of the whole dump
     * works too, the game files are found inside). */
    bool Mount(const std::filesystem::path& game);

    const std::filesystem::path& GameDir() const { return game_dir_; }
    /* True when the game files come from a .zar; GameDir() is that archive, not a folder. */
    bool IsArchive() const { return zar_ != nullptr; }
    const Psarc& Archive() const { return archive_; }
    const QarArchive& Textures() const { return textures_; }

    static std::string ToArchivePath(std::string_view asset_path);

    std::shared_ptr<FoxPackage> LoadPackage(std::string_view path);
    std::vector<std::shared_ptr<FoxPackage>> LoadedPackages() const;
    void UnloadPackage(std::string_view path);

    std::optional<std::vector<uint8_t>> ReadFile(std::string_view path) const;
    std::optional<std::vector<uint8_t>> ReadFileUntraced(std::string_view path) const;

private:
    bool MountFolder(const std::filesystem::path& game_dir);
    bool MountArchive(const std::filesystem::path& file);
    /* Folder inside the .zar that holds chunk1.psarc and texture.qar ("" for its root), or "" when
     * neither the root nor a folder two deep has them. */
    std::string FindArchiveRoot(const ZArchive& zar) const;
    /* A file next to the game archives: on disk for a folder, inside the .zar for an archive. */
    std::optional<std::vector<uint8_t>> ReadLoose(std::string_view path) const;
    void ReportDataDifferences() const;

    std::filesystem::path game_dir_;
    Psarc archive_;
    QarArchive textures_;
    std::shared_ptr<ZArchive> zar_;
    std::string archive_root_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<FoxPackage>> packages_;
};

}
