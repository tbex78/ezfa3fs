#pragma once

#include "ezfa3fs/byte_storage.hpp"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace ezfa3fs {

class CartridgeProgrammer;
class CartridgeLiveDevice;

class CartridgeStorage final : public ByteStorage {
public:
    static constexpr std::uint64_t cartridge_capacity = 0x02000000u;

    CartridgeStorage();
    ~CartridgeStorage() override;
    CartridgeStorage(const CartridgeStorage&) = delete;
    CartridgeStorage& operator=(const CartridgeStorage&) = delete;

    bool open(std::string& error);
    bool close(std::string& error);
    bool isOpen() const noexcept;
    std::array<std::uint8_t, 4> flashId() const noexcept;

    std::uint64_t capacity() const noexcept override;
    bool read(std::uint64_t offset, std::uint8_t* destination,
              std::size_t size, std::string& error) override;
    bool openForLiveWrite(
        std::string& error,
        bool preserve_save_snapshot = true);
    bool restartLiveWriteSession(std::string& error);
    bool readLiveFilesystem(std::uint64_t offset,std::uint8_t* destination,
                            std::size_t size,std::string& error);
    bool eraseLiveBlock(std::size_t block,std::string& error);
    bool programLiveBlock(std::size_t block,const std::vector<std::uint8_t>& bytes,std::string& error);
    bool eraseLiveFilesystemBlock(std::size_t block,std::string& error);
    bool eraseLiveFilesystemBlocks(const std::vector<std::size_t>& blocks,
                                   std::string& error);
    bool programLiveFilesystemBlock(std::size_t block,const std::vector<std::uint8_t>& bytes,std::string& error);
    bool replaceLiveFilesystemMetadata(std::size_t block,
                                       const std::vector<std::uint8_t>& bytes,
                                       std::string& error);
    bool programLiveFilesystemExtent(std::size_t first_block,
                                     const std::vector<std::uint8_t>& bytes,
                                     std::size_t& completed_blocks,
                                     std::string& error);
    bool programLiveFilesystemExtent(std::size_t first_block,
                                     const std::uint8_t* source,
                                     std::size_t block_count,
                                     std::size_t& completed_blocks,
                                     std::string& error);
    bool replaceLiveFilesystemExtent(
        std::size_t first_block,const std::vector<std::uint8_t>& bytes,
        const std::vector<std::size_t>& erase_blocks,
        std::size_t& completed_blocks,std::string& error);

private:
    friend class CartridgeProgrammer;
    friend class CartridgeLiveDevice;
    bool openLiveWriteSessionWithRetry(
        std::string& error,
        bool trust_validated_format,
        bool preserve_save_snapshot);
    bool readLiveBlockAfterWrite(std::size_t block,std::size_t size,
                                 std::vector<std::uint8_t>& bytes,
                                 std::string& error,bool reopen_first);
    bool verifyLiveBlockAfterWrite(std::size_t block,
                                   const std::vector<std::uint8_t>& expected,
                                   const char* operation,
                                   const std::string& operation_error,
                                   bool reopen_first,std::string& error);
    class Impl;

    // Preserve the initial live-mount save-safety policy across transient
    // writer-session restarts.
    bool live_write_preserve_save_snapshot_ = true;

    std::unique_ptr<Impl> impl_;
};

} // namespace ezfa3fs
