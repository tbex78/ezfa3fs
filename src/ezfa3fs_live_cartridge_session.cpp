#include "ezfa3fs/live_cartridge_session.hpp"

#include <iostream>
#include <utility>

namespace ezfa3fs {

CoalescingMetadataBlockDevice::CoalescingMetadataBlockDevice(
    live::BlockDevice& device)
    : device_(device),
      last_activity_(std::chrono::steady_clock::now()),
      idle_thread_(&CoalescingMetadataBlockDevice::idleLoop,this) {}

CoalescingMetadataBlockDevice::~CoalescingMetadataBlockDevice() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_=true;
        condition_.notify_all();
    }

    if(idle_thread_.joinable())
        idle_thread_.join();
}

void CoalescingMetadataBlockDevice::markActivityLocked() const {
    last_activity_=std::chrono::steady_clock::now();
    ++activity_generation_;

    // A new mutation gives a previously failed idle flush another opportunity
    // after this new batch becomes idle.
    idle_flush_failed_=false;
    condition_.notify_all();
}

bool CoalescingMetadataBlockDevice::read(
    std::size_t offset,std::uint8_t* destination,
    std::size_t size,std::string& error) const {
    // Reads are real cartridge activity. Keep the idle metadata timer from
    // firing during lazy allocation scans, verification, or file reads.
    //
    // Refresh the timestamp after the read completes while still holding the
    // device mutex, so the idle worker cannot slip a metadata transaction
    // between this read and the activity update.
    std::lock_guard<std::mutex> lock(mutex_);

    const bool result=device_.read(offset,destination,size,error);
    markActivityLocked();
    return result;
}

bool CoalescingMetadataBlockDevice::program(
    std::size_t offset,const std::uint8_t* source,
    std::size_t size,std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);

    const bool result=device_.program(offset,source,size,error);

    // Measure idleness from the end of the hardware operation, not its start.
    markActivityLocked();
    return result;
}

bool CoalescingMetadataBlockDevice::programBlocks(
    std::size_t first_block,const std::uint8_t* source,
    std::size_t block_count,std::size_t& completed_blocks,
    std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);

    const bool result=device_.programBlocks(
        first_block,source,block_count,completed_blocks,error);

    markActivityLocked();
    return result;
}

bool CoalescingMetadataBlockDevice::eraseBlock(
    std::size_t block,std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);

    // A physical erase may destroy data referenced by the currently durable
    // manifest. Publish the newest manifest before allowing the erase.
    if(!flushLocked(error))
        return false;

    const bool result=device_.eraseBlock(block,error);
    markActivityLocked();
    return result;
}

bool CoalescingMetadataBlockDevice::eraseBlocks(
    const std::vector<std::size_t>& blocks,std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);

    if(!flushLocked(error))
        return false;

    const bool result=device_.eraseBlocks(blocks,error);
    markActivityLocked();
    return result;
}

bool CoalescingMetadataBlockDevice::replaceBlocks(
    std::size_t first_block,const std::uint8_t* source,
    std::size_t block_count,
    const std::vector<std::size_t>& erase_blocks,
    std::size_t& completed_blocks,std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Replacement without erases is copy-on-write and can remain batched.
    // If existing blocks will be erased, first make the newest manifest
    // durable.
    if(!erase_blocks.empty()&&!flushLocked(error)) {
        completed_blocks=0;
        return false;
    }

    const bool result=device_.replaceBlocks(
        first_block,source,block_count,erase_blocks,
        completed_blocks,error);

    markActivityLocked();
    return result;
}

bool CoalescingMetadataBlockDevice::replaceMetadataBlock(
    std::size_t block,const std::uint8_t* source,
    std::size_t size,std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);

    if(block>=live::NorFlash::block_count||
       size!=live::NorFlash::block_size) {
        error="coalesced metadata replacement requires one 64-KiB block";
        return false;
    }

    // Keep only the newest virtual generation. CachedBlockDevice immediately
    // exposes it to the live filesystem while the physical 510/511 update is
    // delayed until the mutation stream becomes idle.
    pending_metadata_block_=block;
    pending_metadata_.assign(source,source+size);

    markActivityLocked();
    error.clear();
    return true;
}

bool CoalescingMetadataBlockDevice::prepareForErase(
    std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Preserve the existing safety barrier before destructive operations.
    if(!flushLocked(error))
        return false;

    const bool result=device_.prepareForErase(error);
    markActivityLocked();
    return result;
}

bool CoalescingMetadataBlockDevice::prepareForProgram(
    std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);

    const bool result=device_.prepareForProgram(error);
    markActivityLocked();
    return result;
}

bool CoalescingMetadataBlockDevice::flushLocked(
    std::string& error) {
    if(!pending_metadata_block_) {
        idle_flush_failed_=false;
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
    idle_flush_failed_=false;

    condition_.notify_all();
    error.clear();
    return true;
}

bool CoalescingMetadataBlockDevice::flush(
    std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    return flushLocked(error);
}

void CoalescingMetadataBlockDevice::idleLoop() {
    std::unique_lock<std::mutex> lock(mutex_);

    for(;;) {
        condition_.wait(lock,[this] {
            return stop_||
                   (pending_metadata_block_.has_value()&&
                    !idle_flush_failed_);
        });

        if(stop_)
            return;

        const auto generation=activity_generation_;
        const auto deadline=last_activity_+idle_delay;

        // Any later cartridge mutation restarts the complete idle interval.
        const bool interrupted=condition_.wait_until(
            lock,deadline,[this,generation] {
                return stop_||
                       !pending_metadata_block_.has_value()||
                       idle_flush_failed_||
                       activity_generation_!=generation;
            });

        if(stop_)
            return;

        if(interrupted)
            continue;

        // We held the same mutex across the decision to flush, so no FUSE
        // request can begin a cartridge operation between "idle" and the
        // physical metadata transaction.
        std::string error;
        if(!flushLocked(error)) {
            // Do not hammer a failing cartridge every five seconds. Preserve
            // the pending generation for an explicit unmount flush, or retry
            // after the next real mutation.
            idle_flush_failed_=true;
            std::cerr
                <<"EZFA3FS idle metadata commit failed: "
                <<error<<'\n';
        }
    }
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
