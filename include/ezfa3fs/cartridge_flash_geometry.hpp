#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ezfa3fs {

struct CartridgeEraseSector final {
    unsigned window = 0;
    std::uint32_t word_address = 0;
};

class CartridgeFlashGeometry final {
public:
    static constexpr std::size_t logical_block_size=0x10000;
    static constexpr std::size_t boot_sector_size=0x2000;

    static std::vector<CartridgeEraseSector> sectorsForLogicalBlock(
        std::size_t block) {
        return sectorsCoveringBlockPrefix(block,logical_block_size);
    }

    static std::vector<CartridgeEraseSector> sectorsCoveringBlockPrefix(
        std::size_t block,std::size_t byte_count) {
        constexpr std::size_t blocks_per_window=128;
        const auto window=static_cast<unsigned>(block/blocks_per_window);
        const auto local=static_cast<std::uint32_t>(
            (block%blocks_per_window)*0x8000u);
        std::vector<CartridgeEraseSector> sectors;
        // EZFA III exposes 8-MiB flash windows.  Runtime erase verification
        // proves that logical block 255, at the top of window 1, uses the
        // device's 8-KiB boot-sector geometry as well: erasing it as one
        // nominal 64-KiB sector left data starting exactly at byte 0x2000.
        //
        // Keep the already-qualified split blocks at the global cartridge
        // boundaries and include the observed window-1 top boot block.
        // The cartridge's physical flash geometry exposes four logical
        // 64-KiB blocks as groups of eight 8-KiB boot sectors. Runtime erase
        // verification confirmed the two central boundary blocks as well:
        // treating blocks 255 or 256 as one 64-KiB sector leaves unerased
        // data beginning exactly at byte 0x2000.
        const bool split_boot_block=
            block==0||
            block==255||
            block==256||
            block==511;
        const auto count=split_boot_block?
            static_cast<unsigned>((byte_count+boot_sector_size-1)/boot_sector_size):1u;
        sectors.reserve(count);
        for(unsigned index=0;index<count;++index)
            sectors.push_back({window,local+index*0x1000u});
        return sectors;
    }
};

} // namespace ezfa3fs
