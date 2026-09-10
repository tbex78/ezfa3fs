#include "ezfa3fs/mount_session.hpp"

#include "ezfa3fs/timestamp.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <thread>
#include <utility>

namespace ezfa3fs {

MountSession::MountSession(
    std::unique_ptr<MountBackend> backend,
    IdleMaintenance idle_maintenance,
    bool fuse_trace_enabled,
    FsyncPolicy fsync_policy)
    : backend_(std::move(backend)),
      mounted_at_(currentUnixTimestamp()),
      fuse_trace_enabled_(fuse_trace_enabled),
      fsync_policy_(fsync_policy),
      idle_maintenance_(std::move(idle_maintenance)) {

    statfs_capacity_bytes_=backend_->capacityBytes();
    refreshStatfsSnapshot();

    if(idle_maintenance_)
        idle_maintenance_thread_=
            std::thread(&MountSession::idleMaintenanceLoop,this);

    if(backend_->writable())
        deferred_commit_thread_=
            std::thread(&MountSession::deferredCommitLoop,this);
}

MountSession::~MountSession() {
    {
        std::lock_guard<std::mutex> lock(deferred_commit_mutex_);
        stop_deferred_commit_=true;
        deferred_commit_condition_.notify_all();
    }

    {
        std::lock_guard<std::mutex> lock(activity_mutex_);
        stop_idle_maintenance_=true;
        activity_condition_.notify_all();
    }

    if(idle_maintenance_thread_.joinable())
        idle_maintenance_thread_.join();

    if(deferred_commit_thread_.joinable())
        deferred_commit_thread_.join();
}

void MountSession::refreshStatfsSnapshot() {
    statfs_free_bytes_.store(
        backend_->freeBytes(),
        std::memory_order_relaxed);

    statfs_entry_count_.store(
        backend_->entryCount(),
        std::memory_order_relaxed);
}

void MountSession::noteActivity(
    IdleMaintenanceImpact maintenance_impact,
    bool deferred_commit_relevant) {

    const auto now=std::chrono::steady_clock::now();

    if(idle_maintenance_) {
        std::lock_guard<std::mutex> lock(activity_mutex_);

        last_foreground_activity_=now;
        ++activity_generation_;

        // Cartridge-facing requests interrupt an active maintenance pass.
        // Only mutations invalidate its scan and restart the full idle window.
        if(maintenance_impact==IdleMaintenanceImpact::reschedule) {
            last_activity_=now;
            ++maintenance_request_generation_;
        }

        activity_condition_.notify_all();
    }

    if(deferred_commit_relevant) {
        std::lock_guard<std::mutex> lock(deferred_commit_mutex_);

        if(!deferred_commit_paths_.empty()) {
            deferred_commit_deadline_=now+deferred_commit_delay;
            ++deferred_commit_generation_;
            deferred_commit_condition_.notify_all();
        }
    }
}

void MountSession::deferredCommitLoop() {
    for(;;) {
        std::uint64_t generation=0;

        {
            std::unique_lock<std::mutex> lock(deferred_commit_mutex_);
            deferred_commit_condition_.wait(lock,[&] {
                return stop_deferred_commit_||
                       !deferred_commit_paths_.empty();
            });

            if(stop_deferred_commit_)
                return;

            generation=deferred_commit_generation_;
            const auto deadline=deferred_commit_deadline_;
            const bool interrupted=deferred_commit_condition_.wait_until(
                lock,deadline,[&] {
                    return stop_deferred_commit_||
                           deferred_commit_generation_!=generation;
                });

            if(stop_deferred_commit_)
                return;

            if(interrupted)
                continue;
        }

        // Activity may have reset the deadline while this worker waited for a
        // foreground cartridge operation. Verify the generation only after
        // acquiring the same mutex used by all mutations, then atomically take
        // the finalized paths.
        std::unique_lock<std::mutex> session_lock(mutex_);
        std::vector<std::string> paths;

        {
            std::lock_guard<std::mutex> lock(deferred_commit_mutex_);

            if(stop_deferred_commit_)
                return;

            if(deferred_commit_generation_!=generation||
               std::chrono::steady_clock::now()<deferred_commit_deadline_)
                continue;

            paths=takeDeferredCommitPaths();
        }

        std::string error;
        if(!mutationAllowed(error)||
           !finishCommit(backend_->commitFiles(paths,error),error))
            std::cerr
                <<"EZFA3FS deferred commit failed: "
                <<error<<'\n';
    }
}

void MountSession::idleMaintenanceLoop() {
    const auto never_serviced=
        std::numeric_limits<std::uint64_t>::max();

    std::uint64_t serviced_request_generation=never_serviced;
    bool pass_in_progress=false;
    bool resynchronize_next=false;

    // A foreground request may spend several seconds waiting behind a
    // destructive cartridge operation. Its original activity timestamp can
    // therefore already be stale when GC finally releases the session mutex.
    // Always give queued foreground work a fresh quiet window from the actual
    // pause point before allowing maintenance to resume.
    auto foreground_resume_not_before=
        std::chrono::steady_clock::time_point::min();

    for(;;) {
        std::uint64_t activity_generation=0;
        std::uint64_t request_generation=0;

        {
            std::unique_lock<std::mutex> lock(activity_mutex_);

            activity_condition_.wait(lock,[&] {
                return stop_idle_maintenance_||
                       pass_in_progress||
                       maintenance_request_generation_!=
                           serviced_request_generation;
            });

            if(stop_idle_maintenance_)
                return;

            // Require one complete maintenance-idle interval. Any meaningful
            // FUSE activity restarts this countdown.
            for(;;) {
                activity_generation=activity_generation_;
                request_generation=maintenance_request_generation_;

                auto deadline=
                    last_activity_+idle_maintenance_delay;

                // Once a pass has already started, read-only foreground
                // traffic should pause it long enough for a complete FUSE
                // request burst to finish, but not for another ten seconds.
                if(pass_in_progress) {
                    deadline=std::max(
                        deadline,
                        last_foreground_activity_+
                            foreground_resume_delay);

                    deadline=std::max(
                        deadline,
                        foreground_resume_not_before);
                }

                const bool interrupted=
                    activity_condition_.wait_until(
                        lock,deadline,[&] {
                            return stop_idle_maintenance_||
                                   activity_generation_!=
                                       activity_generation;
                        });

                if(stop_idle_maintenance_)
                    return;

                if(!interrupted)
                    break;
            }
        }

        if(pass_in_progress)
            std::cerr
                <<"Writable cartridge idle; resuming background "
                  "garbage collection.\n";

        const bool fresh_pass=!pass_in_progress;
        bool first_step=fresh_pass;

        for(;;) {
            bool interrupted_before_lock=false;
            bool maintenance_changed_before_lock=false;

            {
                std::lock_guard<std::mutex> lock(activity_mutex_);

                if(stop_idle_maintenance_)
                    return;

                interrupted_before_lock=
                    activity_generation_!=activity_generation;
                maintenance_changed_before_lock=
                    maintenance_request_generation_!=request_generation;
            }

            if(interrupted_before_lock) {
                if(pass_in_progress) {
                    // Read-only activity should pause GC so foreground FUSE
                    // requests win, but only mutations invalidate the
                    // manifest synchronization established for this pass.
                    if(maintenance_changed_before_lock)
                        resynchronize_next=true;

                    foreground_resume_not_before=
                        std::chrono::steady_clock::now()+
                        foreground_resume_delay;

                    std::cerr
                        <<"EZFA3FS background garbage collection paused: "
                          "filesystem activity resumed.\n";
                }
                break;
            }

            bool complete=false;
            bool maintenance_ok=false;
            std::string error;
            bool interrupted_before_step=false;
            bool maintenance_changed_before_step=false;

            {
                // Cartridge-facing FUSE callbacks use this same mutex. A scan
                // step inspects one block; the completed pass reclaims its
                // accumulated stale blocks in one batch while it is held.
                std::unique_lock<std::mutex> session_lock(mutex_);

                {
                    std::lock_guard<std::mutex> lock(activity_mutex_);

                    if(stop_idle_maintenance_)
                        return;

                    interrupted_before_step=
                        activity_generation_!=activity_generation;
                    maintenance_changed_before_step=
                        maintenance_request_generation_!=request_generation;
                }

                if(!interrupted_before_step) {
                    maintenance_ok=idle_maintenance_(
                        first_step,
                        resynchronize_next,
                        complete,
                        error);

                    // GC may have reclaimed or reserved blocks. Publish the
                    // resulting allocation state before releasing mutex_.
                    refreshStatfsSnapshot();
                }
            }

            if(interrupted_before_step) {
                if(pass_in_progress) {
                    if(maintenance_changed_before_step)
                        resynchronize_next=true;

                    foreground_resume_not_before=
                        std::chrono::steady_clock::now()+
                        foreground_resume_delay;

                    std::cerr
                        <<"EZFA3FS background garbage collection paused: "
                          "filesystem activity resumed.\n";
                }
                break;
            }

            first_step=false;
            resynchronize_next=false;

            if(!maintenance_ok) {
                std::cerr
                    <<"EZFA3FS idle garbage collection failed: "
                    <<error<<'\n';

                // Avoid repeatedly hammering a failing cartridge while
                // nothing else changes. A later real mutation can schedule a
                // fresh maintenance pass.
                serviced_request_generation=request_generation;
                pass_in_progress=false;
                break;
            }

            bool activity_changed=false;
            bool maintenance_changed=false;

            {
                std::lock_guard<std::mutex> lock(activity_mutex_);
                activity_changed=
                    activity_generation_!=activity_generation;
                maintenance_changed=
                    maintenance_request_generation_!=request_generation;
            }

            if(complete) {
                serviced_request_generation=request_generation;
                pass_in_progress=false;
                break;
            }

            pass_in_progress=true;

            if(activity_changed) {
                if(maintenance_changed)
                    resynchronize_next=true;

                foreground_resume_not_before=
                    std::chrono::steady_clock::now()+
                    foreground_resume_delay;

                std::cerr
                    <<"EZFA3FS background garbage collection paused: "
                      "filesystem activity resumed.\n";
                break;
            }

            // Do not monopolize the session mutex between fast scan steps.
            std::this_thread::yield();
        }
    }
}


bool MountSession::mutationAllowed(std::string& error) const {
    if (!commit_failed_) {
        error.clear();
        return true;
    }
    error = commit_error_;
    return false;
}

bool MountSession::deferFileCommit(
    const std::string& path,
    std::string& error) {

    if(!mutationAllowed(error))
        return false;

    {
        std::lock_guard<std::mutex> lock(deferred_commit_mutex_);
        deferred_commit_paths_.insert(path);
        deferred_commit_deadline_=
            std::chrono::steady_clock::now()+deferred_commit_delay;
        ++deferred_commit_generation_;
    }

    deferred_commit_condition_.notify_all();
    error.clear();
    return true;
}

std::vector<std::string> MountSession::takeDeferredCommitPaths(
    const std::string& additional_path) {

    if(!additional_path.empty())
        deferred_commit_paths_.insert(additional_path);

    std::vector<std::string> paths(
        deferred_commit_paths_.begin(),
        deferred_commit_paths_.end());

    deferred_commit_paths_.clear();
    ++deferred_commit_generation_;
    return paths;
}

bool MountSession::flushFileCommits(
    const std::string& path,
    std::string& error) {

    if(!mutationAllowed(error))
        return false;

    std::vector<std::string> paths;
    {
        std::lock_guard<std::mutex> lock(deferred_commit_mutex_);
        paths=takeDeferredCommitPaths(path);
    }

    deferred_commit_condition_.notify_all();
    return finishCommit(backend_->commitFiles(paths,error),error);
}

bool MountSession::synchronizeFile(
    const std::string& path,
    std::string& error) {

    return fsync_policy_==FsyncPolicy::deferred?
        deferFileCommit(path,error):
        flushFileCommits(path,error);
}

bool MountSession::commit(std::string& error) {
    if (!mutationAllowed(error)) return false;

    {
        std::lock_guard<std::mutex> lock(deferred_commit_mutex_);
        takeDeferredCommitPaths();
    }

    deferred_commit_condition_.notify_all();
    return finishCommit(backend_->commit(error),error);
}

bool MountSession::commitFile(const std::string& path,std::string& error) {
    if (!mutationAllowed(error)) return false;

    {
        std::lock_guard<std::mutex> lock(deferred_commit_mutex_);
        deferred_commit_paths_.erase(path);
        ++deferred_commit_generation_;
    }

    deferred_commit_condition_.notify_all();
    return finishCommit(backend_->commitFile(path,error),error);
}

bool MountSession::finishCommit(bool committed,std::string& error) {
    refreshStatfsSnapshot();

    if (committed) return true;
    commit_failed_ = true;
    commit_error_ = error.empty() ? "mount commit failed" : error;
    error = commit_error_;
    return false;
}

bool MountSession::shouldReportFailure(const std::string& error) noexcept {
    if (error.empty()) return false;
    if (!commit_failed_ || error != commit_error_) return true;
    if (commit_failure_reported_) return false;
    commit_failure_reported_ = true;
    return true;
}

} // namespace ezfa3fs
