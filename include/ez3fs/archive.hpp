#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace ez3fs {
struct InputFile {
    std::string name;
    std::vector<std::uint8_t> bytes;
    bool directory = false;
    std::uint64_t modified_time = 0;
};
struct ArchiveEntry {
    std::string name;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::uint32_t crc32 = 0;
    bool directory = false;
    std::uint64_t modified_time = 0;
};
struct ArchiveImage {
    std::vector<std::uint8_t> bytes;
    std::vector<ArchiveEntry> entries;
};
class Crc32 final {
public:
    static std::uint32_t calculate(const std::uint8_t*, std::size_t) noexcept;
};
class ImageBuilder final {
public:
    static constexpr std::uint64_t cartridge_capacity = 0x02000000u;
    static constexpr std::uint64_t program_block_size = 0x00010000u;
    bool build(const std::vector<InputFile>&, ArchiveImage&, std::string&) const;
};
class Archive final {
public:
    bool open(std::vector<std::uint8_t>, std::string&);
    bool verify(std::string&) const;
    const std::vector<ArchiveEntry>& entries() const noexcept { return entries_; }
    const std::vector<std::uint8_t>& image() const noexcept { return image_; }
    std::vector<InputFile> contents() const;
private:
    std::vector<std::uint8_t> image_;
    std::vector<ArchiveEntry> entries_;
};
class ArchiveEditor final {
public:
    explicit ArchiveEditor(std::vector<InputFile> contents);
    bool createDirectory(const std::string& path, std::string& error);
    bool putFile(const std::string& path, std::vector<std::uint8_t> bytes,
                 std::string& error, std::uint64_t modified_time = 0);
    bool removeFile(const std::string& path, std::string& error);
    bool removeDirectory(const std::string& path, std::string& error);
    const std::vector<InputFile>& contents() const noexcept { return contents_; }
private:
    bool parentExists(const std::string& path) const;
    std::vector<InputFile> contents_;
};
} // namespace ez3fs
