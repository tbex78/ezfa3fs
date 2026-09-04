#pragma once

#include "ez3fs/archive.hpp"
#include "ez3fs/byte_storage.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <iosfwd>

namespace ez3fs::live {

inline constexpr std::string_view format_version = "1.0.0";

class BlockDevice {
public:
    virtual ~BlockDevice() = default;
    virtual bool read(std::size_t offset,std::uint8_t* destination,std::size_t size,std::string& error) const = 0;
    virtual bool program(std::size_t offset,const std::uint8_t* source,std::size_t size,std::string& error) = 0;
    virtual bool eraseBlock(std::size_t block,std::string& error) = 0;
};

class NorFlash final : public BlockDevice {
public:
    static constexpr std::size_t block_size = 0x10000;
    static constexpr std::size_t block_count = 0x200;
    static constexpr std::size_t capacity = block_size*block_count;

    NorFlash();
    bool load(const std::string& path,std::string& error);
    bool load(const std::vector<std::uint8_t>& bytes,std::string& error);
    bool load(ByteStorage& storage,std::string& error);
    bool load(ByteStorage& storage,std::ostream& progress,std::string& error);
    bool save(const std::string& path,std::string& error) const;
    bool read(std::size_t offset,std::uint8_t* destination,std::size_t size,std::string& error) const override;
    bool program(std::size_t offset,const std::uint8_t* source,std::size_t size,std::string& error) override;
    bool eraseBlock(std::size_t block,std::string& error) override;
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
    using ScanProgress = std::function<void(std::size_t,std::size_t)>;

    explicit Filesystem(BlockDevice& flash) : flash_(flash) {}
    static bool format(BlockDevice& flash,std::string& error);
    static bool open(BlockDevice& flash,Filesystem& filesystem,std::string& error,
                     ScanProgress progress = {});

    bool createDirectory(const std::string& path,std::string& error);
    bool putFile(const std::string& path,const std::vector<std::uint8_t>& bytes,
                 std::uint64_t modified_time,std::string& error);
    bool removeFile(const std::string& path,std::string& error);
    bool removeDirectory(const std::string& path,std::string& error);
    bool rename(const std::string& from,const std::string& to,std::string& error);
    bool readFile(const std::string& path,std::vector<std::uint8_t>& bytes,
                  std::string& error) const;
    bool readFileRange(const std::string& path,std::size_t offset,std::size_t size,
                       std::vector<std::uint8_t>& bytes,std::string& error) const;
    bool verify(std::string& error) const;
    const std::vector<Entry>& entries() const noexcept { return entries_; }
    std::uint64_t generation() const noexcept { return generation_; }
    std::size_t freeBlocks() const noexcept;

private:
    bool commit(std::string& error);
    bool parentExists(const std::string& path) const;
    Entry* find(const std::string& path);
    const Entry* find(const std::string& path) const;
    BlockDevice& flash_;
    std::vector<Entry> entries_;
    std::uint64_t generation_ = 0;
    std::size_t active_superblock_ = 0;
    std::size_t next_free_block_ = 2;
};

} // namespace ez3fs::live
