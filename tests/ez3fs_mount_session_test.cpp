#include "ez3fs/live_filesystem.hpp"
#include "ez3fs/live_mount_backend.hpp"
#include "ez3fs/mount_session.hpp"

#include <cstdlib>
#include <memory>
#include <string>

namespace {
void require(bool condition) { if(!condition)std::abort(); }
}

int main() {
    ez3fs::live::NorFlash flash;
    std::string error;
    require(ez3fs::live::Filesystem::format(flash,error));
    ez3fs::live::Filesystem filesystem(flash);
    require(ez3fs::live::Filesystem::open(flash,filesystem,error));

    auto backend=std::make_unique<ez3fs::LiveMountBackend>(filesystem);
    auto* mounted_backend=backend.get();
    ez3fs::MountSession session(std::move(backend));
    require(session.mountedAt()!=0);
    require(session.mutationAllowed(error));
    require(mounted_backend->createFile("/pending",error));
    const std::uint8_t byte='x';
    require(mounted_backend->write("/pending",0,&byte,1,error));

    flash.failNextProgramAfter(8);
    require(!session.commit(error));
    const auto commit_error=error;
    require(session.commitFailed());
    require(!session.mutationAllowed(error));
    require(error==commit_error);
    require(!session.commit(error));
    require(error==commit_error);
    require(session.shouldReportFailure(error));
    require(!session.shouldReportFailure(error));
}
