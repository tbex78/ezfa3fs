#pragma once

#include "ez3fs/live_filesystem.hpp"
#include "ez3fs/mount_backend.hpp"

#include <map>
#include <utility>

namespace ez3fs {

class LiveMountBackend final : public MountBackend {
public:
    explicit LiveMountBackend(
        live::Filesystem& filesystem,
        live::Filesystem::MaintenanceObserver maintenance_observer = {})
        : filesystem_(filesystem),maintenance_observer_(std::move(maintenance_observer)) {}
    bool lookup(const std::string& path,MountNode& node) const override;
    bool list(const std::string& path,std::vector<std::string>& children) const override;
    bool read(const std::string& path,std::size_t offset,std::size_t size,std::vector<std::uint8_t>& bytes) const override;
    bool writable() const noexcept override { return true; }
    bool createDirectory(const std::string& path,std::string& error) override;
    bool createFile(const std::string& path,std::string& error) override;
    bool write(const std::string& path,std::size_t offset,const std::uint8_t* bytes,std::size_t size,std::string& error) override;
    bool truncate(const std::string& path,std::size_t size,std::string& error) override;
    bool removeFile(const std::string& path,std::string& error) override;
    bool removeDirectory(const std::string& path,std::string& error) override;
    bool rename(const std::string& from,const std::string& to,std::string& error) override;
    bool commitFile(const std::string& path,std::string& error) override;
    bool commit(std::string& error) override;
    std::uint64_t capacityBytes() const noexcept override { return live::NorFlash::capacity; }
    std::uint64_t freeBytes() const override { return filesystem_.freeBlocks()*live::NorFlash::block_size; }
    std::size_t entryCount() const noexcept override;
private:
    struct PendingFile final {
        std::vector<std::uint8_t> bytes;
        std::uint64_t modified_time = 0;
    };
    static std::string normalize(const std::string& path);
    bool stageFile(const std::string& path,PendingFile*& pending,std::string& error);
    bool commitReady(const PendingFile& pending) const noexcept;
    bool persistFile(const std::string& path,const PendingFile& pending,
                     std::string& error);
    live::Filesystem& filesystem_;
    live::Filesystem::MaintenanceObserver maintenance_observer_;
    std::map<std::string,PendingFile> pending_files_;
};

} // namespace ez3fs
