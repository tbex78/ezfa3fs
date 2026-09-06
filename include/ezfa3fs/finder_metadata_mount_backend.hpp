#pragma once

#include "ezfa3fs/mount_backend.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ezfa3fs {

class FinderMetadataMountBackend final : public MountBackend {
public:
    explicit FinderMetadataMountBackend(std::unique_ptr<MountBackend> backend);

    bool lookup(const std::string& path,MountNode& node) const override;
    bool list(const std::string& path,std::vector<std::string>& children) const override;
    bool read(const std::string& path,std::size_t offset,std::size_t size,
              std::vector<std::uint8_t>& bytes) const override;
    bool writable() const noexcept override;
    bool createDirectory(const std::string& path,std::string& error) override;
    bool createFile(const std::string& path,std::string& error) override;
    bool write(const std::string& path,std::size_t offset,const std::uint8_t* bytes,
               std::size_t size,std::string& error) override;
    bool truncate(const std::string& path,std::size_t size,std::string& error) override;
    bool removeFile(const std::string& path,std::string& error) override;
    bool removeDirectory(const std::string& path,std::string& error) override;
    bool rename(const std::string& from,const std::string& to,
                std::string& error) override;
    bool commitFile(const std::string& path,std::string& error) override;
    bool commit(std::string& error) override;
    std::uint64_t capacityBytes() const noexcept override;
    std::uint64_t freeBytes() const override;
    std::size_t entryCount() const noexcept override;

    static bool isFinderMetadata(const std::string& path) noexcept;

private:
    struct TransientFile final {
        std::vector<std::uint8_t> bytes;
        std::uint64_t modified_time = 0;
        bool shadows_persisted_file = false;
    };

    static std::string normalize(const std::string& path);
    bool stage(const std::string& path,TransientFile*& file,std::string& error);

    std::unique_ptr<MountBackend> backend_;
    std::map<std::string,TransientFile> transient_files_;
};

} // namespace ezfa3fs
