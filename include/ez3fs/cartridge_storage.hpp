#pragma once

#include "ez3fs/byte_storage.hpp"

#include <array>
#include <memory>
#include <vector>

namespace ez3fs {

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
    bool openForLiveWrite(std::string& error);
    bool eraseLiveBlock(std::size_t block,std::string& error);
    bool programLiveBlock(std::size_t block,const std::vector<std::uint8_t>& bytes,std::string& error);
    bool eraseLiveFilesystemBlock(std::size_t block,std::string& error);
    bool programLiveFilesystemBlock(std::size_t block,const std::vector<std::uint8_t>& bytes,std::string& error);

private:
    friend class CartridgeProgrammer;
    friend class CartridgeLiveDevice;
    bool readLiveBlockAfterWrite(std::size_t block,
                                 std::vector<std::uint8_t>& bytes,
                                 std::string& error,bool reopen_first);
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ez3fs
