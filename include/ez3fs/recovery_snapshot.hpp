#pragma once

#include "ez3fs/archive.hpp"

#include <filesystem>
#include <string>

namespace ez3fs {

class RecoverySnapshot final {
public:
    explicit RecoverySnapshot(std::filesystem::path staging_path);

    const std::filesystem::path& stagingPath() const noexcept { return staging_; }
    std::filesystem::path recoveryPath() const;
    bool exists(std::string& error) const;
    bool create(const Archive& baseline,std::string& error) const;
    bool restore(std::string& error) const;
    bool clear(std::string& error) const;

private:
    std::filesystem::path staging_;
};

} // namespace ez3fs
