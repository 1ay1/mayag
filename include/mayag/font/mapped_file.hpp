#pragma once
// mayag::typo::MappedFile — read a font without reading a font
//
// Font discovery has to inspect every file in every font directory: its
// family name, its weight, and which scripts its cmap covers. Doing that by
// slurping each file whole means a cold start reads EVERYTHING installed --
// measured on a normal Linux box, 801 MB across 972 files, including 20 MB
// CJK faces read in full to learn their name.
//
// Almost none of those bytes are wanted. A font is a table directory plus a
// set of tables; discovery touches the directory, `name`, `head`, `OS/2`,
// `cmap`, and a few glyph lookups. That is a handful of pages scattered
// through the file, not the file.
//
// Mapping makes the difference automatic. The parser already works on a
// std::span, so it is handed a mapped region instead of a heap buffer and
// nothing else changes: pages fault in only where the parser actually reads,
// the kernel shares them across processes, and a large font costs whatever
// its header and cmap cost rather than its size.
//
// The fallback is a plain read, so a platform without mmap (or a file that
// cannot be mapped) still works -- correctness never depends on the
// optimisation being available.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <jaal/core/frozen.hpp>
#include <jaal/core/sendable.hpp>

#if defined(_WIN32)
#  include "../core/win32.hpp"   // windows.h, with its keyword macros removed
#  undef IN
#  undef OUT
#  undef OPTIONAL
#else
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace mayag::typo {

/// A read-only view of a file's bytes, mapped where the platform allows.
///
/// Move-only, closes on destruction. `data()` is empty when the file could
/// not be opened at all; callers treat that as "not a font" and move on.
class MappedFile {
  public:
    MappedFile() = default;

    explicit MappedFile(const std::string& path) { open(path); }

    ~MappedFile() { close(); }

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    MappedFile(MappedFile&& other) noexcept { move_from(other); }
    MappedFile& operator=(MappedFile&& other) noexcept {
        if (this != &other) { close(); move_from(other); }
        return *this;
    }

    [[nodiscard]] std::span<const std::uint8_t> data() const noexcept {
        return {ptr_, size_};
    }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] bool mapped() const noexcept { return mapped_; }

  private:
    void open(const std::string& path) {
#if defined(_WIN32)
        HANDLE fh = ::CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                  nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (fh == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER sz{};
        if (::GetFileSizeEx(fh, &sz) && sz.QuadPart > 0) {
            HANDLE mh = ::CreateFileMappingA(fh, nullptr, PAGE_READONLY, 0, 0, nullptr);
            if (mh != nullptr) {
                void* p = ::MapViewOfFile(mh, FILE_MAP_READ, 0, 0, 0);
                if (p != nullptr) {
                    ptr_    = static_cast<const std::uint8_t*>(p);
                    size_   = static_cast<std::size_t>(sz.QuadPart);
                    mapping_ = mh;
                    mapped_ = true;
                    ::CloseHandle(fh);
                    return;
                }
                ::CloseHandle(mh);
            }
        }
        ::CloseHandle(fh);
        read_fallback(path);
#else
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return;
        struct stat st {};
        if (::fstat(fd, &st) == 0 && st.st_size > 0) {
            void* p = ::mmap(nullptr, static_cast<std::size_t>(st.st_size),
                             PROT_READ, MAP_PRIVATE, fd, 0);
            if (p != MAP_FAILED) {
                ptr_    = static_cast<const std::uint8_t*>(p);
                size_   = static_cast<std::size_t>(st.st_size);
                mapped_ = true;
                // The descriptor is not needed once mapped: the mapping keeps
                // the file alive on its own.
                ::close(fd);
                return;
            }
        }
        ::close(fd);
        read_fallback(path);
#endif
    }

    /// Plain read, for platforms or files where mapping is unavailable.
    void read_fallback(const std::string& path) {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (f == nullptr) return;
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (n > 0) {
            owned_.resize(static_cast<std::size_t>(n));
            const std::size_t got = std::fread(owned_.data(), 1, owned_.size(), f);
            owned_.resize(got);
            ptr_  = owned_.data();
            size_ = owned_.size();
        }
        std::fclose(f);
    }

    void close() noexcept {
        if (mapped_ && ptr_ != nullptr) {
#if defined(_WIN32)
            ::UnmapViewOfFile(const_cast<std::uint8_t*>(ptr_));
            if (mapping_ != nullptr) ::CloseHandle(mapping_);
#else
            ::munmap(const_cast<std::uint8_t*>(ptr_), size_);
#endif
        }
        ptr_ = nullptr;
        size_ = 0;
        mapped_ = false;
        owned_.clear();
#if defined(_WIN32)
        mapping_ = nullptr;
#endif
    }

    void move_from(MappedFile& other) noexcept {
        owned_  = std::move(other.owned_);
        mapped_ = other.mapped_;
        size_   = other.size_;
        // A fallback read owns its buffer, so re-point at the moved vector
        // rather than the source's dangling data pointer.
        ptr_    = mapped_ ? other.ptr_ : owned_.data();
#if defined(_WIN32)
        mapping_ = other.mapping_;
        other.mapping_ = nullptr;
#endif
        other.ptr_ = nullptr;
        other.size_ = 0;
        other.mapped_ = false;
    }

    const std::uint8_t*       ptr_  = nullptr;
    std::size_t               size_ = 0;
    bool                      mapped_ = false;
    std::vector<std::uint8_t> owned_;   // only used by read_fallback
#if defined(_WIN32)
    HANDLE mapping_ = nullptr;
#endif
};

}  // namespace mayag::typo

// A MappedFile may change threads and be shared frozen. Its one raw pointer
// is the read-only mapping (or owned_'s buffer) THIS object owns: nothing
// else frees it, and the mapping is PROT_READ / PAGE_READONLY -- no writes
// exist to race with. Checked field by field on a reflection build:
// every other field is proven, and ptr_ is the only one vouched for.
template <> inline constexpr bool jaal::sendable_opt_in<mayag::typo::MappedFile> = true;
template <> inline constexpr bool jaal::frozen_opt_in<mayag::typo::MappedFile>   = true;
