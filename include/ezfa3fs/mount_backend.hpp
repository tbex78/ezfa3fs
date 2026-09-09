#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ezfa3fs {

struct MountNode final {
    bool directory = false;
    std::uint64_t size = 0;
    std::uint64_t modified_time = 0;
};

class MountBackend {
public:
    virtual ~MountBackend() = default;
    virtual bool lookup(const std::string& path,MountNode& node) const = 0;
    virtual bool list(const std::string& path,std::vector<std::string>& children) const = 0;
    virtual bool read(const std::string& path,std::size_t offset,std::size_t size,
                      std::vector<std::uint8_t>& bytes) const = 0;
    virtual bool writable() const noexcept = 0;
    virtual bool createDirectory(const std::string& path,std::string& error) = 0;
    virtual bool createFile(const std::string& path,std::string& error) = 0;
    virtual bool write(const std::string& path,std::size_t offset,const std::uint8_t* bytes,
                       std::size_t size,std::string& error) = 0;
    virtual bool truncate(const std::string& path,std::size_t size,std::string& error) = 0;
    virtual bool removeFile(const std::string& path,std::string& error) = 0;
    virtual bool removeDirectory(const std::string& path,std::string& error) = 0;
    virtual bool rename(const std::string& from,const std::string& to,std::string& error) = 0;
    virtual bool commitFile(const std::string& path,std::string& error) {
        (void)path;
        return commit(error);
    }
    virtual bool commitFiles(
        const std::vector<std::string>& paths,
        std::string& error) {
        (void)paths;
        return commit(error);
    }
    virtual bool commit(std::string& error) = 0;
    virtual std::uint64_t capacityBytes() const noexcept = 0;
    virtual std::uint64_t freeBytes() const = 0;
    virtual std::size_t entryCount() const noexcept = 0;
};

} // namespace ezfa3fs
