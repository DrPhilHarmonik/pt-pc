/* The .zar reader (engine/fs/zarchive.h) and the byte source chunk1.psarc and texture.qar read
 * through when the game files are an archive (engine/fs/byte_source.h).
 *
 * The test writes its own archive, so the reader is checked against a layout written here rather
 * than against itself: files small and large, a block of data zstd cannot compress (stored as it
 * is) beside one it can, names matched in another case, reads that start and end inside blocks, and
 * the files a broken archive must not open. Run: pt_zarchive_test [dir] */

#include "engine/fs/byte_source.h"
#include "engine/fs/vfs.h"
#include "engine/fs/zarchive.h"
#include "engine/platform/os.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <zstd.h>

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kBlock = pt::ZArchive::kBlockSize;

int failures = 0;

void Check(bool ok, const char* label) {
    std::printf("%s: %s\n", label, ok ? "PASS" : "FAIL");
    failures += !ok;
}

void PutBe(std::string& out, uint64_t value, int bytes) {
    for (int i = bytes - 1; i >= 0; --i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
}

void Write(const fs::path& path, const std::string& data) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

/* Data zstd cannot shrink (an LCG), for a block the archive stores uncompressed. */
std::string Noise(size_t size) {
    std::string out(size, '\0');
    uint32_t state = 0x12345678u;
    for (size_t i = 0; i < size; ++i) {
        state = state * 1103515245u + 12345u;
        out[i] = static_cast<char>(state >> 16);
    }
    return out;
}

/* A file tree of the archive being written: one Node per folder and per file. */
struct Node {
    std::string name;
    bool file = false;
    std::vector<Node> children;
    std::string data;
    uint32_t index = 0;
    uint32_t first = 0;
    uint32_t name_offset = 0;
    uint64_t offset = 0;
};

void Add(Node& root, const std::string& path, std::string data) {
    Node* dir = &root;
    for (size_t at = 0;;) {
        const size_t slash = path.find('/', at);
        const std::string part = path.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
        if (slash == std::string::npos) {
            Node file;
            file.name = part;
            file.file = true;
            file.data = std::move(data);
            dir->children.push_back(std::move(file));
            return;
        }
        Node* next = nullptr;
        for (Node& child : dir->children) {
            if (!child.file && child.name == part) next = &child;
        }
        if (!next) {
            dir->children.push_back(Node{});
            next = &dir->children.back();
            next->name = part;
        }
        dir = next;
        at = slash + 1;
    }
}

/* The archive of the format note (docs/formats/zarchive.md): the compressed data first, then the
 * offset records, the names, the file tree, and the 144-byte footer that closes it. */
std::string BuildArchive(Node& root) {
    std::vector<Node*> nodes{&root};
    for (size_t i = 0; i < nodes.size(); ++i) {
        Node* dir = nodes[i];
        if (dir->file) continue;
        dir->first = static_cast<uint32_t>(nodes.size());
        for (Node& child : dir->children) {
            child.index = static_cast<uint32_t>(nodes.size());
            nodes.push_back(&child);
        }
    }

    std::string names;
    for (Node* node : nodes) {
        node->name_offset = static_cast<uint32_t>(names.size());
        const size_t length = node->name.size();
        if (length < 0x80) {
            names.push_back(static_cast<char>(length));
        } else {
            names.push_back(static_cast<char>((length & 0x7F) | 0x80));
            names.push_back(static_cast<char>(length >> 7));
        }
        names += node->name;
    }

    /* Every file starts on a block boundary (the format does not require it, the test wants known
     * offsets) and the last block of a file is padded, so a block belongs to one file only. */
    std::string data;
    for (Node* node : nodes) {
        if (!node->file) continue;
        node->offset = data.size();
        data += node->data;
        data.append((kBlock - node->data.size() % kBlock) % kBlock, '\0');
    }

    std::string packed;
    std::vector<uint16_t> sizes;
    std::vector<char> compressed(kBlock);
    for (size_t at = 0; at < data.size(); at += kBlock) {
        const std::string block = data.substr(at, kBlock);
        const size_t written = ZSTD_compress(compressed.data(), compressed.size(), block.data(), block.size(), 1);
        const bool stored = ZSTD_isError(written) || written >= block.size();
        const std::string bytes = stored ? block : std::string(compressed.data(), written);
        packed += bytes;
        sizes.push_back(static_cast<uint16_t>(bytes.size() - 1));
    }

    /* One record per 16 blocks: the base offset of the first block and the 16 stored sizes, each
     * one less than the bytes it holds (0xFFFF for a block that was not compressed). */
    std::string offsets;
    uint64_t base = 0;
    for (size_t at = 0; at < sizes.size(); at += 16) {
        PutBe(offsets, base, 8);
        for (size_t i = 0; i < 16; ++i) {
            PutBe(offsets, at + i < sizes.size() ? sizes[at + i] : 0xFFFF, 2);
        }
        for (size_t i = 0; i < 16 && at + i < sizes.size(); ++i) base += uint64_t(sizes[at + i]) + 1;
    }

    std::string tree;
    for (Node* node : nodes) {
        PutBe(tree, uint64_t(node->name_offset) | (node->file ? 0x80000000ull : 0), 4);
        if (node->file) {
            PutBe(tree, node->offset, 4);
            PutBe(tree, node->data.size(), 4);
            PutBe(tree, 0, 4);
        } else {
            PutBe(tree, node->first, 4);
            PutBe(tree, node->children.size(), 4);
            PutBe(tree, 0, 4);
        }
    }

    std::string file = packed;
    const uint64_t offsets_at = file.size();
    file += offsets;
    const uint64_t names_at = file.size();
    file += names;
    const uint64_t tree_at = file.size();
    file += tree;
    const uint64_t total = file.size() + 144;

    std::string footer;
    PutBe(footer, 0, 8);
    PutBe(footer, packed.size(), 8);
    PutBe(footer, offsets_at, 8);
    PutBe(footer, offsets.size(), 8);
    PutBe(footer, names_at, 8);
    PutBe(footer, names.size(), 8);
    PutBe(footer, tree_at, 8);
    PutBe(footer, tree.size(), 8);
    PutBe(footer, file.size(), 8);
    PutBe(footer, 0, 8);
    PutBe(footer, file.size(), 8);
    PutBe(footer, 0, 8);
    footer.append(32, '\0');
    PutBe(footer, total, 8);
    PutBe(footer, 0x61bf3a01u, 4);
    PutBe(footer, 0x169f52d6u, 4);
    return file + footer;
}

/* One read of `length` bytes at `offset`, compared with the bytes the file must hold. */
bool Matches(const pt::ByteSource& source, uint64_t offset, size_t length, const std::string& data, const char* label) {
    std::vector<char> read(length);
    const bool ok = source.ReadAt(offset, read.data(), read.size());
    if (!ok) {
        std::printf("%s: FAIL (read refused at %llu)\n", label, static_cast<unsigned long long>(offset));
        return false;
    }
    return std::memcmp(read.data(), data.data() + offset, length) == 0;
}

void CheckWindows(const pt::ByteSource& source, const std::string& data, const char* what) {
    const auto label = [what](const char* part) { return std::string(what) + ": " + part; };
    Check(Matches(source, 0, 1, data, label("first byte").c_str()), label("first byte").c_str());
    Check(Matches(source, kBlock - 1, 2, data, label("a read across a block edge").c_str()),
          label("a read across a block edge").c_str());
    Check(Matches(source, kBlock + 123, 4000, data, label("a read inside the second block").c_str()),
          label("a read inside the second block").c_str());
    Check(Matches(source, 17, data.size() - 17, data, label("the rest of the file").c_str()),
          label("the rest of the file").c_str());
    std::vector<char> one(1);
    Check(!source.ReadAt(data.size() - 1, one.data(), 2) && !source.ReadAt(data.size(), one.data(), 1),
          label("a read past the end is refused").c_str());
    Check(source.Size() == data.size(), label("the size is the file's").c_str());
}

}

int main(int argc, char** argv) {
    const fs::path root = argc > 1 ? fs::path(argv[1]) / "zarchive-test"
                                   : fs::temp_directory_path() / ("pt-zarchive-" + std::to_string(pt::os::ProcessId()));
    std::error_code error;
    fs::remove_all(root, error);
    fs::create_directories(root, error);
    const fs::path archive_path = root / "pt.zar";

    const std::string psarc = Noise(kBlock) + std::string(kBlock, 'P') + std::string(12345, 'x');
    const std::string qar = "QAR texture data";
    Node tree;
    Add(tree, "eboot.bin", "not a copy of the executable");
    Add(tree, "sce_sys/pic1.png", std::string(100, 'I'));
    Add(tree, "CUSA01127/chunk1.psarc", psarc);
    Add(tree, "CUSA01127/texture.qar", qar);
    Write(archive_path, BuildArchive(tree));
    Write(root / "chunk1.psarc", psarc);
    Write(root / "notes.txt", "not an archive");

    Check(pt::ZArchive::LooksLikeArchive(archive_path), "looks like a .zar");
    Check(!pt::ZArchive::LooksLikeArchive(root / "notes.txt"), "a text file does not");
    Check(!pt::ZArchive::LooksLikeArchive(root / "missing.zar"), "a missing file does not");

    pt::ZArchive archive;
    Check(archive.Open(archive_path), "the archive opens");
    Check(archive.Error().empty(), "an opened archive reports no error");
    Check(archive.Entries() == 7, "the tree holds the root, two folders and four files");
    Check(archive.Size() == fs::file_size(archive_path), "its size is the file's");

    const auto psarc_node = archive.Find("CUSA01127/chunk1.psarc");
    const auto qar_node = archive.Find("CUSA01127/texture.qar");
    Check(psarc_node != pt::ZArchive::kNone && archive.IsFile(psarc_node), "a nested file is found");
    Check(qar_node != pt::ZArchive::kNone && archive.IsFile(qar_node), "its neighbour is found");
    Check(archive.Find("/cusa01127/CHUNK1.PSARC") == psarc_node, "names match in any case");
    Check(archive.Find("cusa01127//chunk1.psarc") == psarc_node, "repeated separators are fine");
    Check(archive.Find("CUSA01127") != pt::ZArchive::kNone && !archive.IsFile(archive.Find("CUSA01127")),
          "a folder is not a file");
    Check(archive.Find("CUSA01127/missing.fpk") == pt::ZArchive::kNone, "a missing file is none");
    Check(archive.Find("../chunk1.psarc") == pt::ZArchive::kNone && archive.Find("CUSA01127/..") == pt::ZArchive::kNone,
          "a path may not climb out of the archive");
    Check(archive.EntrySize(psarc_node) == psarc.size(), "a file's size is its own");
    Check(archive.EntrySize(archive.Find("CUSA01127")) == 0, "a folder has no size");

    std::string listed;
    for (uint32_t i = 0;; ++i) {
        std::string name;
        bool is_file = false;
        if (!archive.Child(archive.Find("CUSA01127"), i, name, &is_file)) break;
        listed += (is_file ? "f:" : "d:") + name + " ";
    }
    Check(listed == "f:chunk1.psarc f:texture.qar ", "a folder lists its children in order");
    Check(!archive.Child(archive.Find("CUSA01127"), 2, listed, nullptr), "the listing ends after them");

    const auto whole = archive.Read(psarc_node);
    Check(whole && whole->size() == psarc.size() && std::memcmp(whole->data(), psarc.data(), psarc.size()) == 0,
          "a file that spans blocks is read whole");
    const auto by_path = archive.Read("cusa01127/chunk1.psarc");
    Check(by_path && by_path->size() == psarc.size() && std::memcmp(by_path->data(), psarc.data(), psarc.size()) == 0,
          "it is the same by path");
    const auto empty = archive.Read(pt::ZArchive::kNone);
    Check(!empty, "a missing node reads nothing");
    std::vector<char> tiny(1);
    Check(archive.ReadAt(archive.Find("CUSA01127"), 0, tiny.data(), 1) == false, "a folder cannot be read as a file");

    /* The byte source chunk1.psarc reads through when it sits inside a .zar, against the same bytes
     * in a file of their own. */
    auto closed = std::make_shared<pt::ZArchive>();
    Check(!pt::OpenArchiveEntrySource(closed, psarc_node), "a closed archive gives no source");
    auto reopened = std::make_shared<pt::ZArchive>();
    if (reopened->Open(archive_path)) {
        const auto entry = pt::OpenArchiveEntrySource(reopened, reopened->Find("CUSA01127/chunk1.psarc"));
        if (entry) {
            CheckWindows(*entry, psarc, "the .zar entry source");
        } else {
            Check(false, "the .zar entry source is made");
        }
        Check(!pt::OpenArchiveEntrySource(reopened, reopened->Find("CUSA01127")), "a folder gives no source");
        const auto file = pt::OpenFileSource(root / "chunk1.psarc");
        if (file) {
            CheckWindows(*file, psarc, "the file source");
        } else {
            Check(false, "the file source opens");
        }
    } else {
        Check(false, "the archive reopens for the byte sources");
    }

    /* What the game must refuse: a .zar whose chunk1.psarc is not a PSARC, and a path that is not
     * there at all. */
    {
        pt::Vfs vfs;
        Check(!vfs.Mount(archive_path), "a .zar whose chunk1.psarc is not a PSARC does not mount");
        Check(!vfs.IsArchive(), "a refused archive leaves no game files mounted");
        Check(!vfs.Mount(root / "missing.zar"), "a missing folder or archive does not mount");
    }

    fs::remove_all(root, error);
    std::printf("zarchive: %d failures\n", failures);
    return failures ? 1 : 0;
}
