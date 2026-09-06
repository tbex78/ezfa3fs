#pragma once

#include "ezfa3fs/live_filesystem.hpp"
#include "ezfa3fs/mount_backend.hpp"

#include <map>
#include <functional>
#include <utility>

namespace ezfa3fs {

class LiveMountBackend final : public MountBackend {
public:
    using PersistenceObserver=std::function<bool(std::string&)>;
    explicit LiveMountBackend(
        live::Filesystem& filesystem,
        live::Filesystem::MaintenanceObserver maintenance_observer = {},
        PersistenceObserver persistence_observer = {},bool writable = true)
        : filesystem_(filesystem),
          maintenance_observer_(std::move(maintenance_observer)),
          persistence_observer_(std::move(persistence_observer)),writable_(writable) {}
    bool lookup(const std::string& path,MountNode& node) const override;
    bool list(const std::string& path,std::vector<std::string>& children) const override;
    bool read(const std::string& path,std::size_t offset,std::size_t size,std::vector<std::uint8_t>& bytes) const override;
    bool writable() const noexcept override { return writable_; }
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
    PersistenceObserver persistence_observer_;
    bool writable_;
    std::map<std::string,PendingFile> pending_files_;
};

} // namespace ezfa3fs
