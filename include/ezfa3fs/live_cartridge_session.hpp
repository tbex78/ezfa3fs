#pragma once

#include "ezfa3fs/cached_block_device.hpp"
#include "ezfa3fs/cartridge_live_device.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace ezfa3fs {

class CoalescingMetadataBlockDevice final : public live::BlockDevice {
public:
    explicit CoalescingMetadataBlockDevice(live::BlockDevice& device);
    ~CoalescingMetadataBlockDevice();

    CoalescingMetadataBlockDevice(
        const CoalescingMetadataBlockDevice&) = delete;
    CoalescingMetadataBlockDevice& operator=(
        const CoalescingMetadataBlockDevice&) = delete;

    bool read(std::size_t offset,std::uint8_t* destination,
              std::size_t size,std::string& error) const override;

    bool program(std::size_t offset,const std::uint8_t* source,
                 std::size_t size,std::string& error) override;

    bool programBlocks(std::size_t first_block,const std::uint8_t* source,
                       std::size_t block_count,
                       std::size_t& completed_blocks,
                       std::string& error) override;

    bool eraseBlock(std::size_t block,std::string& error) override;

    bool eraseBlocks(const std::vector<std::size_t>& blocks,
                     std::string& error) override;

    bool replaceBlocks(std::size_t first_block,
                       const std::uint8_t* source,
                       std::size_t block_count,
                       const std::vector<std::size_t>& erase_blocks,
                       std::size_t& completed_blocks,
                       std::string& error) override;

    bool replaceMetadataBlock(std::size_t block,
                              const std::uint8_t* source,
                              std::size_t size,
                              std::string& error) override;

    bool prepareForErase(std::string& error) override;
    bool prepareForProgram(std::string& error) override;

    bool flush(std::string& error);

private:
    inline static constexpr std::chrono::seconds idle_delay{5};

    bool flushLocked(std::string& error);
    void markActivityLocked();
    void idleLoop();

    live::BlockDevice& device_;

    // The timer thread is independent of the FUSE request thread, so all
    // accesses to the underlying cartridge device are serialized here.
    mutable std::mutex mutex_;
    std::condition_variable condition_;

    bool stop_ = false;
    bool idle_flush_failed_ = false;
    std::uint64_t activity_generation_ = 0;
    std::chrono::steady_clock::time_point last_activity_;

    std::optional<std::size_t> pending_metadata_block_;
    std::vector<std::uint8_t> pending_metadata_;

    // Keep this last so every state member above is initialized before the
    // worker can begin using the object.
    std::thread idle_thread_;
};

class LiveCartridgeSession final {
public:
    LiveCartridgeSession();
    ~LiveCartridgeSession();
    LiveCartridgeSession(const LiveCartridgeSession&) = delete;
    LiveCartridgeSession& operator=(const LiveCartridgeSession&) = delete;

    bool open(std::string& error,bool verify_referenced_data = false,
              live::Filesystem::ScanProgress verification_progress = {});
    bool close(std::string& error);
    bool isOpen() const noexcept { return open_; }
    live::Filesystem& filesystem() noexcept { return filesystem_; }

private:
    CartridgeStorage storage_;
    CartridgeLiveDevice device_;
    CoalescingMetadataBlockDevice metadata_device_;
    live::CachedBlockDevice cached_device_;
    live::Filesystem filesystem_;
    bool open_ = false;
};

} // namespace ezfa3fs
