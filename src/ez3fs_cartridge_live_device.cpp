#include "ez3fs/cartridge_live_device.hpp"

#include <algorithm>
#include <iostream>
#include <vector>

namespace ez3fs {
namespace { constexpr std::size_t block_size=live::NorFlash::block_size; }

bool CartridgeLiveDevice::read(std::size_t offset,std::uint8_t* destination,std::size_t size,std::string& error) const {
    return const_cast<CartridgeStorage&>(storage_).readLiveFilesystem(
        offset,destination,size,error);
}
bool CartridgeLiveDevice::program(std::size_t offset,const std::uint8_t* source,std::size_t size,std::string& error) {
    if(offset%block_size||size!=block_size||offset>=live::NorFlash::capacity){error="cartridge live programming requires one aligned 64-KiB block";return false;}
    std::vector<std::uint8_t> bytes(source,source+size);return storage_.programLiveFilesystemBlock(offset/block_size,bytes,error);
}
bool CartridgeLiveDevice::programBlocks(std::size_t first_block,
                                        const std::uint8_t* source,
                                        std::size_t block_count,
                                        std::size_t& completed_blocks,
                                        std::string& error) {
    if(first_block>=live::NorFlash::block_count||
       block_count>live::NorFlash::block_count-first_block) {
        completed_blocks=0;error="cartridge live extent is out of range";return false;
    }
    std::vector<std::uint8_t> bytes(source,source+block_count*block_size);
    return storage_.programLiveFilesystemExtent(first_block,bytes,
                                                completed_blocks,error);
}
bool CartridgeLiveDevice::eraseBlock(std::size_t block,std::string& error) {
    if(block>=live::NorFlash::block_count){error="cartridge live erase block is out of range";return false;}
    return storage_.eraseLiveFilesystemBlock(block,error);
}
bool CartridgeLiveDevice::eraseBlocks(const std::vector<std::size_t>& blocks,
                                      std::string& error) {
    return storage_.eraseLiveFilesystemBlocks(blocks,error);
}
bool CartridgeLiveDevice::replaceMetadataBlock(
    std::size_t block,const std::uint8_t* source,
    std::size_t size,std::string& error) {
    if(block>=live::NorFlash::block_count||size!=block_size) {
        error="cartridge live replacement requires one 64-KiB block";return false;
    }
    return storage_.replaceLiveFilesystemMetadata(
        block,std::vector<std::uint8_t>(source,source+size),error);
}
bool CartridgeLiveDevice::prepareForErase(std::string& error) {
    return storage_.restartLiveWriteSession(error);
}
bool CartridgeLiveDevice::prepareForProgram(std::string& error) {
    return storage_.restartLiveWriteSession(error);
}
} // namespace ez3fs
