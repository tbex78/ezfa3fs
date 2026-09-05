#pragma once

#include "ez3fs/live_filesystem.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ez3fs::live {

class CachedBlockDevice final : public BlockDevice {
public:
    explicit CachedBlockDevice(BlockDevice& device);

    bool read(std::size_t offset,std::uint8_t* destination,std::size_t size,
              std::string& error) const override;
    bool program(std::size_t offset,const std::uint8_t* source,
                 std::size_t size,std::string& error) override;
    bool programBlocks(std::size_t first_block,const std::uint8_t* source,
                       std::size_t block_count,std::size_t& completed_blocks,
                       std::string& error) override;
    bool eraseBlock(std::size_t block,std::string& error) override;
    bool replaceMetadataBlock(std::size_t block,const std::uint8_t* source,
                              std::size_t size,std::string& error) override;
    bool prepareForErase(std::string& error) override;

private:
    using CachedBlock = std::vector<std::uint8_t>;

    bool loadBlock(std::size_t block,std::string& error) const;
    void invalidate(std::size_t first_block,std::size_t block_count) noexcept;
    void store(std::size_t block,const std::uint8_t* source);

    BlockDevice& device_;
    mutable std::vector<std::unique_ptr<CachedBlock>> blocks_;
};

} // namespace ez3fs::live
