#pragma once

#include "ez3fs/byte_storage.hpp"

#include <array>
#include <memory>

namespace ez3fs {

class CartridgeProgrammer;

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

private:
    friend class CartridgeProgrammer;
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ez3fs
