#include "ez3fs/mount_session.hpp"

#include "ez3fs/timestamp.hpp"

#include <utility>

namespace ez3fs {

MountSession::MountSession(std::unique_ptr<MountBackend> backend)
    : backend_(std::move(backend)), mounted_at_(currentUnixTimestamp()) {}

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

} // namespace ez3fs
