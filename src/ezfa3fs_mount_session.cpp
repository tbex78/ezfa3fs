#include "ezfa3fs/mount_session.hpp"

#include "ezfa3fs/timestamp.hpp"

#include <iostream>
#include <limits>
#include <thread>
#include <utility>

namespace ezfa3fs {

MountSession::MountSession(
    std::unique_ptr<MountBackend> backend,
    IdleMaintenance idle_maintenance)
    : backend_(std::move(backend)),
      mounted_at_(currentUnixTimestamp()),
      idle_maintenance_(std::move(idle_maintenance)) {

    if(idle_maintenance_)
        idle_maintenance_thread_=
            std::thread(&MountSession::idleMaintenanceLoop,this);
}

MountSession::~MountSession() {
    {
        std::lock_guard<std::mutex> lock(activity_mutex_);
        stop_idle_maintenance_=true;
        activity_condition_.notify_all();
    }

    if(idle_maintenance_thread_.joinable())
        idle_maintenance_thread_.join();
}

void MountSession::noteActivity(bool maintenance_relevant) {
    if(!idle_maintenance_)
        return;

    std::lock_guard<std::mutex> lock(activity_mutex_);

    last_activity_=std::chrono::steady_clock::now();
    ++activity_generation_;

    if(maintenance_relevant)
        ++maintenance_request_generation_;

    activity_condition_.notify_all();
}

void MountSession::idleMaintenanceLoop() {
    const auto never_serviced=
        std::numeric_limits<std::uint64_t>::max();

    std::uint64_t serviced_request_generation=never_serviced;
    bool pass_in_progress=false;
    bool resynchronize_next=false;

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

                const auto deadline=
                    last_activity_+idle_maintenance_delay;

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
                // The normal FUSE callbacks use this same mutex. Only one GC
                // block is inspected/reclaimed while it is held.
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
                }
            }

            if(interrupted_before_step) {
                if(pass_in_progress) {
                    if(maintenance_changed_before_step)
                        resynchronize_next=true;

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

bool MountSession::commit(std::string& error) {
    if (!mutationAllowed(error)) return false;
    return finishCommit(backend_->commit(error),error);
}

bool MountSession::commitFile(const std::string& path,std::string& error) {
    if (!mutationAllowed(error)) return false;
    return finishCommit(backend_->commitFile(path,error),error);
}

bool MountSession::finishCommit(bool committed,std::string& error) {
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
