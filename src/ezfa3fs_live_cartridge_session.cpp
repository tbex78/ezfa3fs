#include "ezfa3fs/live_cartridge_session.hpp"

#include <iostream>
#include <utility>

namespace ezfa3fs {

CoalescingMetadataBlockDevice::CoalescingMetadataBlockDevice(
    live::BlockDevice& device)
    : device_(device) {}

bool CoalescingMetadataBlockDevice::read(
    std::size_t offset,std::uint8_t* destination,
    std::size_t size,std::string& error) const {
    return device_.read(offset,destination,size,error);
}

bool CoalescingMetadataBlockDevice::program(
    std::size_t offset,const std::uint8_t* source,
    std::size_t size,std::string& error) {
    return device_.program(offset,source,size,error);
}

bool CoalescingMetadataBlockDevice::programBlocks(
    std::size_t first_block,const std::uint8_t* source,
    std::size_t block_count,std::size_t& completed_blocks,
    std::string& error) {
    return device_.programBlocks(
        first_block,source,block_count,completed_blocks,error);
}

bool CoalescingMetadataBlockDevice::eraseBlock(
    std::size_t block,std::string& error) {
    // Before physically destroying any data, make the newest in-memory
    // manifest durable so the previous crash-safety guarantees remain valid.
    if(!flush(error))return false;
    return device_.eraseBlock(block,error);
}

bool CoalescingMetadataBlockDevice::eraseBlocks(
    const std::vector<std::size_t>& blocks,std::string& error) {
    if(!flush(error))return false;
    return device_.eraseBlocks(blocks,error);
}

bool CoalescingMetadataBlockDevice::replaceBlocks(
    std::size_t first_block,const std::uint8_t* source,
    std::size_t block_count,
    const std::vector<std::size_t>& erase_blocks,
    std::size_t& completed_blocks,std::string& error) {
    // A replacement containing erases may destroy blocks referenced by the
    // currently durable manifest. Publish the newest manifest first.
    if(!erase_blocks.empty()&&!flush(error)) {
        completed_blocks=0;
        return false;
    }

    return device_.replaceBlocks(
        first_block,source,block_count,erase_blocks,
        completed_blocks,error);
}

bool CoalescingMetadataBlockDevice::replaceMetadataBlock(
    std::size_t block,const std::uint8_t* source,
    std::size_t size,std::string& error) {
    if(block>=live::NorFlash::block_count||
       size!=live::NorFlash::block_size) {
        error="coalesced metadata replacement requires one 64-KiB block";
        return false;
    }

    // Do not touch NOR yet. CachedBlockDevice will retain this virtual
    // superblock, and a later commit simply replaces this pending generation.
    pending_metadata_block_=block;
    pending_metadata_.assign(source,source+size);

    error.clear();
    return true;
}

bool CoalescingMetadataBlockDevice::prepareForErase(
    std::string& error) {
    if(!flush(error))return false;
    return device_.prepareForErase(error);
}

bool CoalescingMetadataBlockDevice::prepareForProgram(
    std::string& error) {
    return device_.prepareForProgram(error);
}

bool CoalescingMetadataBlockDevice::flush(std::string& error) {
    if(!pending_metadata_block_) {
        error.clear();
        return true;
    }

    if(!device_.replaceMetadataBlock(
            *pending_metadata_block_,
            pending_metadata_.data(),
            pending_metadata_.size(),
            error)) {
        return false;
    }

    pending_metadata_block_.reset();
    pending_metadata_.clear();
    error.clear();
    return true;
}

LiveCartridgeSession::LiveCartridgeSession()
    : device_(storage_),
      metadata_device_(device_),
      cached_device_(metadata_device_),
      filesystem_(cached_device_) {}

LiveCartridgeSession::~LiveCartridgeSession() { std::string ignored;close(ignored); }

bool LiveCartridgeSession::open(
    std::string& error,bool verify_referenced_data,
    live::Filesystem::ScanProgress verification_progress) {
    if(open_){error.clear();return true;}
    if(!storage_.openForLiveWrite(error))return false;
    if(!live::Filesystem::open(metadata_device_,filesystem_,error)){
        std::string ignored;storage_.close(ignored);return false;
    }
    // Keep full referenced-file checksum verification strictly opt-in.
    // With verify_referenced_data == false, Filesystem::verify() is not called.
    if(verify_referenced_data){
        if(!filesystem_.verify(error,std::move(verification_progress))){
            std::string ignored;storage_.close(ignored);return false;
        }
    }
    open_=true;error.clear();return true;
}


bool LiveCartridgeSession::close(std::string& error) {
    if(!open_){error.clear();return true;}

    // Publish only the newest accumulated filesystem generation.
    if(!metadata_device_.flush(error))return false;

    if(!storage_.close(error))return false;

    open_=false;
    error.clear();
    return true;
}

} // namespace ezfa3fs
