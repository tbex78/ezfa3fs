#pragma once
#include "ez3fs/archive.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ez3fs {
struct NodeInfo {
    bool directory=false;
    std::uint64_t size=0;
    std::uint64_t modified_time=0;
};

class VirtualFilesystem final {
public:
    VirtualFilesystem(std::vector<InputFile> contents, bool writable);
    bool lookup(const std::string& path, NodeInfo& info) const;
    bool list(const std::string& path, std::vector<std::string>& children) const;
    bool read(const std::string& path, std::size_t offset, std::size_t size,
              std::vector<std::uint8_t>& output) const;
    bool createDirectory(const std::string& path, std::string& error);
    bool createFile(const std::string& path, std::string& error);
    bool write(const std::string& path, std::size_t offset,
               const std::uint8_t* data, std::size_t size, std::string& error);
    bool truncate(const std::string& path, std::size_t size, std::string& error);
    bool removeFile(const std::string& path, std::string& error);
    bool removeDirectory(const std::string& path, std::string& error);
    bool rename(const std::string& from, const std::string& to, std::string& error);
    bool writable() const noexcept { return writable_; }
    bool dirty() const noexcept { return dirty_; }
    void markClean() noexcept { dirty_=false; }
    const std::vector<InputFile>& contents() const noexcept { return contents_; }
private:
    static std::string normalize(const std::string& path);
    bool parentExists(const std::string& path) const;
    std::vector<InputFile>::iterator find(const std::string& path);
    std::vector<InputFile>::const_iterator find(const std::string& path) const;
    std::vector<InputFile> contents_;
    bool writable_=false;
    bool dirty_=false;
};
} // namespace ez3fs
