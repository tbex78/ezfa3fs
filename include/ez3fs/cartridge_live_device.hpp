#pragma once

#include "ez3fs/live_filesystem.hpp"
#include "ez3fs/cartridge_storage.hpp"

#include <iosfwd>

namespace ez3fs {

class CartridgeLiveDevice final : public live::BlockDevice {
public:
    explicit CartridgeLiveDevice(CartridgeStorage& storage) : storage_(storage) {}
    bool read(std::size_t offset,std::uint8_t* destination,std::size_t size,std::string& error) const override;
    bool program(std::size_t offset,const std::uint8_t* source,std::size_t size,std::string& error) override;
    bool programBlocks(std::size_t first_block,const std::uint8_t* source,
                       std::size_t block_count,std::size_t& completed_blocks,
                       std::string& error) override;
    bool eraseBlock(std::size_t block,std::string& error) override;
    bool prepareForErase(std::string& error) override;
private:
    CartridgeStorage& storage_;
};

} // namespace ez3fs
