#pragma once

#include "ez3fs/cached_block_device.hpp"
#include "ez3fs/cartridge_live_device.hpp"

namespace ez3fs {

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

} // namespace ez3fs
