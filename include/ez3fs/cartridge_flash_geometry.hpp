#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ez3fs {

struct CartridgeEraseSector final {
    unsigned window = 0;
    std::uint32_t word_address = 0;
};

class CartridgeFlashGeometry final {
public:
    static std::vector<CartridgeEraseSector> sectorsForLogicalBlock(
        std::size_t block) {
        constexpr std::size_t blocks_per_window=128;
        const auto window=static_cast<unsigned>(block/blocks_per_window);
        const auto local=static_cast<std::uint32_t>(
            (block%blocks_per_window)*0x8000u);
        std::vector<CartridgeEraseSector> sectors;
        const bool split_boot_block=block==0||block==511;
        const auto count=split_boot_block?8u:1u;
        sectors.reserve(count);
        for(unsigned index=0;index<count;++index)
            sectors.push_back({window,local+index*0x1000u});
        return sectors;
    }
};

} // namespace ez3fs
