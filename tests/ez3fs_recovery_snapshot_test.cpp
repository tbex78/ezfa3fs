#include "ez3fs/recovery_snapshot.hpp"
#include "ez3fs/new_image_file.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>

namespace { void require(bool condition) { if(!condition)std::abort(); } }

int main()
{
    namespace fs=std::filesystem;
    const auto unique=std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory=fs::temp_directory_path()/("ez3fs-recovery-test-"+std::to_string(unique));
    std::error_code error;require(fs::create_directory(directory,error));
    ez3fs::ArchiveImage image;std::string message;
    require(ez3fs::ImageBuilder{}.build({{"baseline.txt",{1,2,3}}},image,message));
    ez3fs::Archive baseline;require(baseline.open(image.bytes,message));
    const auto staging=directory/"working.ez3fs";
    ez3fs::NewImageFile staging_file(staging);require(staging_file.write(image.bytes,message));
    ez3fs::RecoverySnapshot snapshot(staging);require(snapshot.create(baseline,message));
    require(snapshot.exists(message));require(!snapshot.create(baseline,message));
    require(snapshot.restore(message));require(snapshot.clear(message));require(!snapshot.exists(message));
    std::error_code cleanup;fs::remove_all(directory,cleanup);
}
