#pragma once

#include "ez3fs/archive.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ez3fs {

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
    virtual bool commit(std::string& error) = 0;
};

} // namespace ez3fs
