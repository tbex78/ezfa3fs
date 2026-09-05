#include "ez3fs/cached_block_device.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace ez3fs::live {

CachedBlockDevice::CachedBlockDevice(BlockDevice& device)
    : device_(device),blocks_(NorFlash::block_count) {}

bool CachedBlockDevice::loadBlock(std::size_t block,std::string& error) const {
    if(block>=NorFlash::block_count) {
        error="cached block read is out of range";
        return false;
    }
    if(blocks_[block]) {
        error.clear();
        return true;
    }
    auto bytes=std::make_unique<CachedBlock>(NorFlash::block_size);
    if(!device_.read(block*NorFlash::block_size,bytes->data(),bytes->size(),error))
        return false;
    blocks_[block]=std::move(bytes);
    error.clear();
    return true;
}

bool CachedBlockDevice::read(std::size_t offset,std::uint8_t* destination,
                             std::size_t size,std::string& error) const {
    if(offset>NorFlash::capacity||size>NorFlash::capacity-offset) {
        error="cached block read is out of range";
        return false;
    }
    std::size_t copied=0;
    while(copied<size) {
        const auto absolute=offset+copied;
        const auto block=absolute/NorFlash::block_size;
        const auto within=absolute%NorFlash::block_size;
        const auto count=std::min(size-copied,NorFlash::block_size-within);
        if(!loadBlock(block,error))return false;
        std::memcpy(destination+copied,blocks_[block]->data()+within,count);
        copied+=count;
    }
    error.clear();
    return true;
}

void CachedBlockDevice::invalidate(std::size_t first_block,
                                   std::size_t block_count) noexcept {
    const auto end=std::min(NorFlash::block_count,first_block+block_count);
    for(auto block=first_block;block<end;++block)blocks_[block].reset();
}

void CachedBlockDevice::store(std::size_t block,const std::uint8_t* source) {
    auto bytes=std::make_unique<CachedBlock>(NorFlash::block_size);
    std::copy_n(source,NorFlash::block_size,bytes->begin());
    blocks_[block]=std::move(bytes);
}

bool CachedBlockDevice::program(std::size_t offset,const std::uint8_t* source,
                                std::size_t size,std::string& error) {
    if(offset>NorFlash::capacity||size>NorFlash::capacity-offset) {
        error="cached block program is out of range";
        return false;
    }
    const auto first=offset/NorFlash::block_size;
    const auto count=size==0?0:
        (offset%NorFlash::block_size+size+NorFlash::block_size-1)/
            NorFlash::block_size;
    invalidate(first,count);
    if(!device_.program(offset,source,size,error))return false;
    if(offset%NorFlash::block_size==0&&size==NorFlash::block_size)
        store(first,source);
    error.clear();
    return true;
}

bool CachedBlockDevice::programBlocks(std::size_t first_block,
                                      const std::uint8_t* source,
                                      std::size_t block_count,
                                      std::size_t& completed_blocks,
                                      std::string& error) {
    invalidate(first_block,block_count);
    const bool programmed=device_.programBlocks(first_block,source,block_count,
                                                completed_blocks,error);
    if(programmed) {
        const auto safe_count=std::min(block_count,completed_blocks);
        for(std::size_t index=0;index<safe_count;++index)
            store(first_block+index,source+index*NorFlash::block_size);
    }
    return programmed;
}

bool CachedBlockDevice::eraseBlock(std::size_t block,std::string& error) {
    invalidate(block,1);
    if(!device_.eraseBlock(block,error))return false;
    auto bytes=std::make_unique<CachedBlock>(NorFlash::block_size,0xFF);
    blocks_[block]=std::move(bytes);
    error.clear();
    return true;
}

bool CachedBlockDevice::replaceMetadataBlock(std::size_t block,
                                             const std::uint8_t* source,
                                             std::size_t size,
                                             std::string& error) {
    invalidate(block,1);
    if(!device_.replaceMetadataBlock(block,source,size,error))return false;
    if(size==NorFlash::block_size)store(block,source);
    error.clear();
    return true;
}

bool CachedBlockDevice::prepareForErase(std::string& error) {
    return device_.prepareForErase(error);
}

} // namespace ez3fs::live
