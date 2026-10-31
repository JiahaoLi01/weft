#include "text/mapped_file.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace weft {
#if defined(_WIN32)
    std::unique_ptr<const MappedFile> MappedFile::open(const std::filesystem::path& path) {
        // Others may read the file meanwhile, and rename or delete it (a new file saved in its place); none may write it
        // in place while it is mapped, and the mapped view keeps it from being truncated.
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return nullptr;
        }
        LARGE_INTEGER size{};
        const HANDLE mapping = GetFileSizeEx(file, &size) && size.QuadPart > 0
                                   ? CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr)
                                   : nullptr;
        CloseHandle(file);
        if (!mapping) {
            return nullptr;
        }
        // The view keeps the mapping, which keeps the file.
        const void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
        CloseHandle(mapping);
        if (!view) {
            return nullptr;
        }
        return std::unique_ptr<const MappedFile>(
            new MappedFile(static_cast<const char*>(view), static_cast<std::size_t>(size.QuadPart)));
    }

    MappedFile::~MappedFile() {
        UnmapViewOfFile(data_);
    }
#else
    std::unique_ptr<const MappedFile> MappedFile::open(const std::filesystem::path& path) {
        const int file = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (file < 0) {
            return nullptr;
        }
        struct stat status{};
        void* view = MAP_FAILED;
        if (fstat(file, &status) == 0 && status.st_size > 0) {
            view = mmap(nullptr, static_cast<std::size_t>(status.st_size), PROT_READ, MAP_PRIVATE, file, 0);
        }
        // The mapping keeps the file.
        close(file);
        if (view == MAP_FAILED) {
            return nullptr;
        }
        return std::unique_ptr<const MappedFile>(
            new MappedFile(static_cast<const char*>(view), static_cast<std::size_t>(status.st_size)));
    }

    MappedFile::~MappedFile() {
        munmap(const_cast<char*>(data_), size_);
    }
#endif
} // namespace weft
