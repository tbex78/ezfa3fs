#pragma once

#include "ezfa3fs/cached_block_device.hpp"
#include "ezfa3fs/cartridge_live_device.hpp"

namespace ezfa3fs {

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
    live::CachedBlockDevice cached_device_;
    live::Filesystem filesystem_;
    bool open_ = false;
};

} // namespace ezfa3fs
