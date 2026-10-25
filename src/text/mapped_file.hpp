#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>

namespace weft {
    // A file's bytes mapped read-only into memory: the system reads a page from disk when it is first touched, so a font
    // file's faces cost what they read of it (its tables and the glyphs drawn), not the whole file. Windows and POSIX
    // systems. While it is mapped, Windows lets others read, rename and delete the file but not write it in place or
    // truncate it; elsewhere the file must not shrink.
    class MappedFile final {
    public:
        // The file mapped, or null when it cannot be (no such file, empty, or a system that does not map files).
        static std::unique_ptr<const MappedFile> open(const std::filesystem::path& path);
        ~MappedFile();
        MappedFile(const MappedFile&) = delete;
        MappedFile& operator=(const MappedFile&) = delete;

        std::span<const char> bytes() const noexcept {
            return {data_, size_};
        }

    private:
        MappedFile(const char* data, std::size_t size) : data_(data), size_(size) {
        }

        const char* data_{};
        std::size_t size_{};
    };
} // namespace weft
