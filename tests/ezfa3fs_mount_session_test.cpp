#include "ezfa3fs/mount_session.hpp"

#include <cstdlib>
#include <memory>
#include <string>
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
}
