#pragma once

#include "ezfa3fs/mount_backend.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace ezfa3fs {

class MountSession final {
public:
    using IdleMaintenance=
        std::function<bool(bool,bool,bool&,std::string&)>;

    explicit MountSession(
        std::unique_ptr<MountBackend> backend,
        IdleMaintenance idle_maintenance = {},
        bool fuse_trace_enabled = false);
    ~MountSession();

    MountBackend& backend() noexcept { return *backend_; }
    std::mutex& mutex() noexcept { return mutex_; }

    // Protects in-memory mount state used by metadata-only FUSE operations.
    // Idle cartridge maintenance deliberately does not take this mutex, so
    // getattr/readdir/xattr requests can complete while flash erase or
    // verification is in progress.
    std::mutex& metadataMutex() noexcept {
        return metadata_mutex_;
    }

    std::mutex& metadataActivityMutex() {
        noteActivity(false,false);
        return metadata_mutex_;
    }

    // Signals meaningful filesystem activity before waiting for the main
    // session mutex. This lets a queued FUSE request stop idle maintenance
    // after the current single-block step.
    std::mutex& activityMutex(bool maintenance_relevant = true) {
        noteActivity(maintenance_relevant,true);
        return mutex_;
    }

    void noteActivity(
        bool maintenance_relevant = true,
        bool deferred_commit_relevant = true);

    struct StatfsSnapshot final {
        std::uint64_t capacity_bytes = 0;
        std::uint64_t free_bytes = 0;
        std::size_t entry_count = 0;
    };

    StatfsSnapshot statfsSnapshot() const noexcept {
        return {
            statfs_capacity_bytes_,
            statfs_free_bytes_.load(std::memory_order_relaxed),
            statfs_entry_count_.load(std::memory_order_relaxed)
        };
    }

    // The caller must already serialize filesystem mutation/cartridge state.
    // Readers consume only the atomically published snapshot and therefore
    // never wait for a flash erase or verification operation.
    void refreshStatfsSnapshot();

    std::uint64_t mountedAt() const noexcept { return mounted_at_; }
    bool fuseTraceEnabled() const noexcept { return fuse_trace_enabled_; }

    bool mutationAllowed(std::string& error) const;
    // These methods require mutex() to be held by the caller. Final release
    // uses the deferred form so adjacent Finder copies can share one flash
    // transaction; fsync uses the flush form as an immediate durability
    // boundary.
    bool deferFileCommit(const std::string& path,std::string& error);
    bool flushFileCommits(const std::string& path,std::string& error);
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

    inline static constexpr std::chrono::milliseconds
        deferred_commit_delay{1000};

    bool finishCommit(bool committed,std::string& error);
    std::vector<std::string> takeDeferredCommitPaths(
        const std::string& additional_path = {});
    void deferredCommitLoop();
    void idleMaintenanceLoop();

    std::unique_ptr<MountBackend> backend_;

    // Serializes cartridge operations and filesystem mutations.
    std::mutex mutex_;

    // Serializes the RAM-visible directory/pending-file state independently
    // from slow cartridge maintenance. Mutations take both mutexes.
    std::mutex metadata_mutex_;

    // statfs must never inspect live allocation state while background GC is
    // erasing flash. Writers/maintenance publish fresh values here while they
    // already own the main filesystem serialization.
    std::uint64_t statfs_capacity_bytes_ = 0;
    std::atomic<std::uint64_t> statfs_free_bytes_{0};
    std::atomic<std::size_t> statfs_entry_count_{0};

    std::uint64_t mounted_at_ = 0;
    bool fuse_trace_enabled_ = false;
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

    std::mutex deferred_commit_mutex_;
    std::condition_variable deferred_commit_condition_;
    std::set<std::string> deferred_commit_paths_;
    std::uint64_t deferred_commit_generation_ = 0;
    std::chrono::steady_clock::time_point deferred_commit_deadline_ =
        std::chrono::steady_clock::now();
    bool stop_deferred_commit_ = false;

    // Keep the worker last so every state member it accesses exists before
    // the thread starts and remains alive until after it is joined.
    std::thread idle_maintenance_thread_;
    std::thread deferred_commit_thread_;
};

} // namespace ezfa3fs
