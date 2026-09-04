#pragma once

#include "ez3fs/archive.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ez3fs::live {

inline constexpr std::string_view format_version = "1.0.0";

class NorFlash final {
public:
    static constexpr std::size_t block_size = 0x10000;
    static constexpr std::size_t block_count = 0x200;
    static constexpr std::size_t capacity = block_size*block_count;

    NorFlash();
    bool load(const std::string& path,std::string& error);
    bool save(const std::string& path,std::string& error) const;
    bool read(std::size_t offset,std::uint8_t* destination,std::size_t size,
              std::string& error) const;
    bool program(std::size_t offset,const std::uint8_t* source,std::size_t size,
                 std::string& error);
    bool eraseBlock(std::size_t block,std::string& error);
    void failNextProgramAfter(std::size_t bytes) noexcept { fault_bytes_=bytes; }

private:
    std::vector<std::uint8_t> bytes_;
    std::optional<std::size_t> fault_bytes_;
};

struct Entry final {
    std::string name;
    std::uint64_t size = 0;
    std::uint64_t modified_time = 0;
    std::uint32_t crc32 = 0;
    std::uint32_t first_block = 0;
    std::uint32_t block_count = 0;
    bool directory = false;
};

class Filesystem final {
public:
    explicit Filesystem(NorFlash& flash) : flash_(flash) {}
    static bool format(NorFlash& flash,std::string& error);
    static bool open(NorFlash& flash,Filesystem& filesystem,std::string& error);

    bool createDirectory(const std::string& path,std::string& error);
    bool putFile(const std::string& path,const std::vector<std::uint8_t>& bytes,
                 std::uint64_t modified_time,std::string& error);
    bool removeFile(const std::string& path,std::string& error);
    bool removeDirectory(const std::string& path,std::string& error);
    bool readFile(const std::string& path,std::vector<std::uint8_t>& bytes,
                  std::string& error) const;
    bool verify(std::string& error) const;
    const std::vector<Entry>& entries() const noexcept { return entries_; }
    std::uint64_t generation() const noexcept { return generation_; }
    std::size_t freeBlocks() const noexcept;

private:
    bool commit(std::string& error);
    bool parentExists(const std::string& path) const;
    Entry* find(const std::string& path);
    const Entry* find(const std::string& path) const;
    NorFlash& flash_;
    std::vector<Entry> entries_;
    std::uint64_t generation_ = 0;
    std::size_t active_superblock_ = 0;
    std::size_t next_free_block_ = 2;
};

} // namespace ez3fs::live
