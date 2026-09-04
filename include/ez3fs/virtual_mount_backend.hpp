#pragma once

#include "ez3fs/mount_backend.hpp"
#include "ez3fs/virtual_filesystem.hpp"

#include <filesystem>
#include <optional>

namespace ez3fs {

class VirtualMountBackend final : public MountBackend {
public:
    VirtualMountBackend(std::vector<InputFile> contents,bool writable,
                        std::optional<std::filesystem::path> image={})
        : filesystem_(std::move(contents),writable),image_(std::move(image)) {}
    bool lookup(const std::string& path,MountNode& node) const override;
    bool list(const std::string& path,std::vector<std::string>& children) const override;
    bool read(const std::string& path,std::size_t offset,std::size_t size,std::vector<std::uint8_t>& bytes) const override;
    bool writable() const noexcept override { return filesystem_.writable(); }
    bool createDirectory(const std::string& path,std::string& error) override { return filesystem_.createDirectory(path,error); }
    bool createFile(const std::string& path,std::string& error) override { return filesystem_.createFile(path,error); }
    bool write(const std::string& path,std::size_t offset,const std::uint8_t* bytes,std::size_t size,std::string& error) override { return filesystem_.write(path,offset,bytes,size,error); }
    bool truncate(const std::string& path,std::size_t size,std::string& error) override { return filesystem_.truncate(path,size,error); }
    bool removeFile(const std::string& path,std::string& error) override { return filesystem_.removeFile(path,error); }
    bool removeDirectory(const std::string& path,std::string& error) override { return filesystem_.removeDirectory(path,error); }
    bool rename(const std::string& from,const std::string& to,std::string& error) override { return filesystem_.rename(from,to,error); }
    bool commit(std::string& error) override;
    std::uint64_t capacityBytes() const noexcept override { return ImageBuilder::cartridge_capacity; }
    std::uint64_t freeBytes() const override;
    std::size_t entryCount() const noexcept override { return filesystem_.contents().size(); }
private:
    VirtualFilesystem filesystem_;
    std::optional<std::filesystem::path> image_;
};

} // namespace ez3fs
