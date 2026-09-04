#pragma once

#include "ez3fs/live_filesystem.hpp"
#include "ez3fs/mount_backend.hpp"

namespace ez3fs {

class LiveMountBackend final : public MountBackend {
public:
    explicit LiveMountBackend(live::Filesystem& filesystem) : filesystem_(filesystem) {}
    bool lookup(const std::string& path,MountNode& node) const override;
    bool list(const std::string& path,std::vector<std::string>& children) const override;
    bool read(const std::string& path,std::size_t offset,std::size_t size,std::vector<std::uint8_t>& bytes) const override;
    bool writable() const noexcept override { return true; }
    bool createDirectory(const std::string& path,std::string& error) override { return filesystem_.createDirectory(path,error); }
    bool createFile(const std::string& path,std::string& error) override;
    bool write(const std::string& path,std::size_t offset,const std::uint8_t* bytes,std::size_t size,std::string& error) override;
    bool truncate(const std::string& path,std::size_t size,std::string& error) override;
    bool removeFile(const std::string& path,std::string& error) override { return filesystem_.removeFile(path,error); }
    bool removeDirectory(const std::string& path,std::string& error) override { return filesystem_.removeDirectory(path,error); }
    bool rename(const std::string&,const std::string&,std::string& error) override { error="live rename is not implemented";return false; }
    bool commit(std::string& error) override { error.clear();return true; }
private:
    live::Filesystem& filesystem_;
};

} // namespace ez3fs
