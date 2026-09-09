#include "ezfa3fs/finder_metadata_mount_backend.hpp"
#include "ezfa3fs/mount_session.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
void require(bool condition) { if(!condition)std::abort(); }

class FailingBackend final : public ezfa3fs::MountBackend {
public:
    bool lookup(const std::string&,ezfa3fs::MountNode&) const override { return false; }
    bool list(const std::string&,std::vector<std::string>&) const override { return false; }
    bool read(const std::string&,std::size_t,std::size_t,
              std::vector<std::uint8_t>&) const override { return false; }
    bool writable() const noexcept override { return true; }
    bool createDirectory(const std::string&,std::string&) override { return true; }
    bool createFile(const std::string&,std::string&) override { return true; }
    bool write(const std::string&,std::size_t,const std::uint8_t*,std::size_t,
               std::string&) override { return true; }
    bool truncate(const std::string&,std::size_t,std::string&) override { return true; }
    bool removeFile(const std::string&,std::string&) override { return true; }
    bool removeDirectory(const std::string&,std::string&) override { return true; }
    bool rename(const std::string&,const std::string&,std::string&) override { return true; }
    bool commit(std::string& error) override {
        ++commit_count;error="simulated cartridge commit failure";return false;
    }
    std::uint64_t capacityBytes() const noexcept override { return 0; }
    std::uint64_t freeBytes() const override { return 0; }
    std::size_t entryCount() const noexcept override { return 0; }

    std::size_t commit_count = 0;
};

class RecordingBackend final : public ezfa3fs::MountBackend {
public:
    bool lookup(const std::string&,ezfa3fs::MountNode&) const override { return false; }
    bool list(const std::string&,std::vector<std::string>&) const override { return false; }
    bool read(const std::string&,std::size_t,std::size_t,
              std::vector<std::uint8_t>&) const override { return false; }
    bool writable() const noexcept override { return true; }
    bool createDirectory(const std::string&,std::string&) override { return true; }
    bool createFile(const std::string&,std::string&) override { return true; }
    bool write(const std::string&,std::size_t,const std::uint8_t*,std::size_t,
               std::string&) override { return true; }
    bool truncate(const std::string&,std::size_t,std::string&) override { return true; }
    bool removeFile(const std::string&,std::string&) override { return true; }
    bool removeDirectory(const std::string&,std::string&) override { return true; }
    bool rename(const std::string&,const std::string&,std::string&) override { return true; }
    bool commitFiles(const std::vector<std::string>& paths,
                     std::string& error) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            batches_.push_back(paths);
        }
        condition_.notify_all();
        if(fail_) {
            error="simulated deferred commit failure";
            return false;
        }
        error.clear();
        return true;
    }
    bool commit(std::string& error) override { error.clear();return true; }
    std::uint64_t capacityBytes() const noexcept override { return 0; }
    std::uint64_t freeBytes() const override { return 0; }
    std::size_t entryCount() const noexcept override { return 0; }

    void failCommits() noexcept { fail_=true; }

    bool waitForBatches(std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock,std::chrono::seconds(3),[&] {
            return batches_.size()>=count;
        });
    }

    std::vector<std::vector<std::string>> batches() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return batches_;
    }

private:
    bool fail_ = false;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<std::vector<std::string>> batches_;
};
}

int main() {
    std::string error;
    auto backend=std::make_unique<FailingBackend>();
    auto* mounted_backend=backend.get();
    ezfa3fs::MountSession session(std::move(backend));
    require(session.mountedAt()!=0);
    require(session.mutationAllowed(error));

    require(!session.commitFile("/file",error));
    const auto commit_error=error;
    require(session.commitFailed());
    require(!session.mutationAllowed(error));
    require(error==commit_error);
    require(!session.commit(error));
    require(error==commit_error);
    require(mounted_backend->commit_count==1);
    require(session.shouldReportFailure(error));
    require(!session.shouldReportFailure(error));

    auto traced_backend=std::make_unique<FailingBackend>();
    ezfa3fs::MountSession traced(
        std::move(traced_backend),{},true);
    require(traced.fuseTraceEnabled());
    require(traced.fsyncPolicy()==ezfa3fs::FsyncPolicy::strict);
    require(!traced.writebackEnabled());

    auto relaxed_backend=std::make_unique<FailingBackend>();
    ezfa3fs::MountSession relaxed(
        std::move(relaxed_backend),{},false,
        ezfa3fs::FsyncPolicy::deferred);
    require(relaxed.fsyncPolicy()==ezfa3fs::FsyncPolicy::deferred);
    require(relaxed.writebackEnabled());

    {
        auto recording=std::make_unique<RecordingBackend>();
        auto* observed=recording.get();
        ezfa3fs::FinderMetadataMountBackend finder(std::move(recording));
        require(finder.fileRemovalRequiresCommit("/game.gba"));
        require(finder.renameRequiresImmediateCommit(
            "/game.gba","/renamed.gba"));
        require(!finder.renameRequiresImmediateCommit(
            "/game.gba","/.fuse_hidden0000000100000001"));
        require(!finder.renameRequiresImmediateCommit(
            "/._game.gba","/._renamed.gba"));
        require(finder.createFile("/._game.gba",error));
        require(!finder.fileRemovalRequiresCommit("/._game.gba"));
        require(finder.removeFile("/._game.gba",error));
        require(finder.commitFiles(
            {"/game.gba","/.DS_Store","/._game.gba"},error));
        require(observed->batches()==
                std::vector<std::vector<std::string>>{{"/game.gba"}});
    }

    {
        auto recording=std::make_unique<RecordingBackend>();
        auto* observed=recording.get();
        ezfa3fs::MountSession batching(std::move(recording));

        {
            std::lock_guard<std::mutex> lock(batching.mutex());
            require(batching.deferFileCommit("/first.gba",error));
        }

        // Wait past the former 250-ms deadline, then simulate a read/open
        // operation that needs the main session lock. It must give Finder
        // another complete quiet window even though it does not schedule GC.
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        {
            std::lock_guard<std::mutex> lock(
                batching.activityMutex(false));
        }

        {
            std::lock_guard<std::mutex> lock(batching.mutex());
            require(batching.deferFileCommit("/second.gba",error));
        }

        require(observed->waitForBatches(1));
        require(observed->batches()==
                std::vector<std::vector<std::string>>{{
                    "/first.gba","/second.gba"}});
    }

    {
        auto recording=std::make_unique<RecordingBackend>();
        auto* observed=recording.get();
        ezfa3fs::MountSession synchronous(std::move(recording));

        {
            std::lock_guard<std::mutex> lock(synchronous.mutex());
            require(synchronous.deferFileCommit("/queued.gba",error));
            require(synchronous.synchronizeFile("/fsynced.gba",error));
        }

        require(observed->batches()==
                std::vector<std::vector<std::string>>{{
                    "/fsynced.gba","/queued.gba"}});
    }

    {
        auto recording=std::make_unique<RecordingBackend>();
        auto* observed=recording.get();
        ezfa3fs::MountSession deferred(
            std::move(recording),{},false,
            ezfa3fs::FsyncPolicy::deferred);

        {
            std::lock_guard<std::mutex> lock(deferred.mutex());
            require(deferred.synchronizeFile("/finder.gba",error));
            require(observed->batches().empty());
            require(deferred.commit(error));
        }
    }

    {
        auto recording=std::make_unique<RecordingBackend>();
        auto* observed=recording.get();
        observed->failCommits();
        ezfa3fs::MountSession failing_deferred(std::move(recording));

        {
            std::lock_guard<std::mutex> lock(failing_deferred.mutex());
            require(failing_deferred.deferFileCommit("/failure.gba",error));
        }

        require(observed->waitForBatches(1));

        {
            std::lock_guard<std::mutex> lock(failing_deferred.mutex());
            require(!failing_deferred.mutationAllowed(error));
            require(error=="simulated deferred commit failure");
        }
    }
}
