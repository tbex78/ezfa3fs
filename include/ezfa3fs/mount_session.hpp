#pragma once

#include "ezfa3fs/mount_backend.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace ezfa3fs {

class MountSession final {
public:
    using IdleMaintenance=
        std::function<bool(bool,bool,bool&,std::string&)>;

    explicit MountSession(
        std::unique_ptr<MountBackend> backend,
        IdleMaintenance idle_maintenance = {});
    ~MountSession();

    MountBackend& backend() noexcept { return *backend_; }
    std::mutex& mutex() noexcept { return mutex_; }

    // Signals meaningful filesystem activity before waiting for the main
    // session mutex. This lets a queued FUSE request stop idle maintenance
    // after the current single-block step.
    std::mutex& activityMutex(bool maintenance_relevant = true) {
        noteActivity(maintenance_relevant);
        return mutex_;
    }

    void noteActivity(bool maintenance_relevant = true);

    std::uint64_t mountedAt() const noexcept { return mounted_at_; }

    bool mutationAllowed(std::string& error) const;
    bool commitFile(const std::string& path,std::string& error);
    bool commit(std::string& error);
    bool commitFailed() const noexcept { return commit_failed_; }
    bool shouldReportFailure(const std::string& error) noexcept;

private:
    inline static constexpr std::chrono::seconds
        idle_maintenance_delay{10};

    // A paused GC pass needs only a short quiet gap after read-only FUSE
    // traffic. This groups bursts such as ls/Finder getattr calls without
    // making them restart the full maintenance-idle interval.
    inline static constexpr std::chrono::milliseconds
        foreground_resume_delay{250};

    bool finishCommit(bool committed,std::string& error);
    void idleMaintenanceLoop();

    std::unique_ptr<MountBackend> backend_;
    std::mutex mutex_;
    std::uint64_t mounted_at_ = 0;
    bool commit_failed_ = false;
    bool commit_failure_reported_ = false;
    std::string commit_error_;

    IdleMaintenance idle_maintenance_;

    std::mutex activity_mutex_;
    std::condition_variable activity_condition_;
    bool stop_idle_maintenance_ = false;
    std::uint64_t activity_generation_ = 0;
    std::uint64_t maintenance_request_generation_ = 0;
    std::chrono::steady_clock::time_point last_activity_ =
        std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_foreground_activity_ =
        std::chrono::steady_clock::now();

    // Keep the worker last so every state member it accesses exists before
    // the thread starts and remains alive until after it is joined.
    std::thread idle_maintenance_thread_;
};

} // namespace ezfa3fs
