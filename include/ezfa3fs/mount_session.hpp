#pragma once

#include "ezfa3fs/mount_backend.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace ezfa3fs {

class MountSession final {
public:
    explicit MountSession(std::unique_ptr<MountBackend> backend);

    MountBackend& backend() noexcept { return *backend_; }
    std::mutex& mutex() noexcept { return mutex_; }
    std::uint64_t mountedAt() const noexcept { return mounted_at_; }

    bool mutationAllowed(std::string& error) const;
    bool commitFile(const std::string& path,std::string& error);
    bool commit(std::string& error);
    bool commitFailed() const noexcept { return commit_failed_; }
    bool shouldReportFailure(const std::string& error) noexcept;

private:
    bool finishCommit(bool committed,std::string& error);
    std::unique_ptr<MountBackend> backend_;
    std::mutex mutex_;
    std::uint64_t mounted_at_ = 0;
    bool commit_failed_ = false;
    bool commit_failure_reported_ = false;
    std::string commit_error_;
};

} // namespace ezfa3fs
