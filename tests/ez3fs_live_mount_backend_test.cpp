#include "ez3fs/live_mount_backend.hpp"
#include <cstdlib>
namespace { void require(bool value){if(!value)std::abort();} }
int main() {
    ez3fs::live::NorFlash flash;std::string error;
    require(ez3fs::live::Filesystem::format(flash,error));
    ez3fs::live::Filesystem filesystem(flash);
    require(ez3fs::live::Filesystem::open(flash,filesystem,error));
    ez3fs::LiveMountBackend backend(filesystem);
    require(backend.capacityBytes()==ez3fs::live::NorFlash::capacity);
    require(backend.createDirectory("/docs",error));
    const auto generation_before_create=filesystem.generation();
    require(backend.createFile("/docs/test.txt",error));
    require(filesystem.generation()==generation_before_create);
    require(backend.entryCount()==2);
    std::vector<std::string> children;
    require(backend.list("/docs",children));
    require(children==std::vector<std::string>{"test.txt"});
    const auto generation_before_write=filesystem.generation();
    const std::uint8_t data[]={'o','k'};
    require(backend.write("/docs/test.txt",0,data,2,error));
    require(filesystem.generation()==generation_before_write);
    std::vector<std::uint8_t> output;
    require(backend.read("/docs/test.txt",0,2,output));
    require(output==std::vector<std::uint8_t>({'o','k'}));
    ez3fs::MountNode node;require(backend.lookup("/docs/test.txt",node));
    require(!node.directory&&node.size==2);
    const std::uint8_t deferred_data[]={'n','e','x','t'};
    require(backend.createFile("/docs/deferred.txt",error));
    require(backend.write("/docs/deferred.txt",0,deferred_data,sizeof(deferred_data),error));
    require(backend.commitFile("/docs/test.txt",error));
    require(filesystem.generation()>generation_before_write);
    require(filesystem.readFile("docs/test.txt",output,error));
    require(output==std::vector<std::uint8_t>({'o','k'}));
    require(!filesystem.readFile("docs/deferred.txt",output,error));
    require(backend.read("/docs/deferred.txt",0,sizeof(deferred_data),output));
    require(output==std::vector<std::uint8_t>({'n','e','x','t'}));
    require(backend.commitFile("/docs/deferred.txt",error));
    require(filesystem.readFile("docs/deferred.txt",output,error));
    require(output==std::vector<std::uint8_t>({'n','e','x','t'}));
    require(backend.removeFile("/docs/deferred.txt",error));
    const auto generation_before_transient=filesystem.generation();
    require(backend.createFile("/docs/transient.txt",error));
    require(backend.removeFile("/docs/transient.txt",error));
    require(filesystem.generation()==generation_before_transient);
    require(!backend.lookup("/docs/transient.txt",node));
    require(!backend.createFile("/missing/test.txt",error));
    require(backend.rename("/docs/test.txt","/docs/renamed.txt",error));
    require(backend.truncate("/docs/renamed.txt",1,error));
    require(backend.removeFile("/docs/renamed.txt",error));
    require(backend.removeDirectory("/docs",error));

    ez3fs::live::NorFlash persisted_flash;
    require(ez3fs::live::Filesystem::format(persisted_flash,error));
    ez3fs::live::Filesystem persisted_filesystem(persisted_flash);
    require(ez3fs::live::Filesystem::open(
        persisted_flash,persisted_filesystem,error));
    unsigned persistence_count=0;
    ez3fs::LiveMountBackend persisted_backend(
        persisted_filesystem,{},[&persistence_count](std::string& observer_error) {
            ++persistence_count;observer_error.clear();return true;
        });
    require(persisted_backend.createDirectory("/saved",error));
    require(persisted_backend.commit(error));
    require(persistence_count==1);
    require(persisted_backend.createFile("/saved/file.txt",error));
    require(persisted_backend.write("/saved/file.txt",0,data,sizeof(data),error));
    require(persisted_backend.commitFile("/saved/file.txt",error));
    require(persistence_count==2);

    ez3fs::live::NorFlash direct_flash;
    require(ez3fs::live::Filesystem::formatDirectBootEmpty(direct_flash,error));
    ez3fs::live::Filesystem direct_filesystem(direct_flash);
    require(ez3fs::live::Filesystem::open(direct_flash,direct_filesystem,error));
    ez3fs::LiveMountBackend direct_backend(direct_filesystem);
    const auto empty_generation=direct_filesystem.generation();
    require(direct_backend.createFile("/game.gba",error));
    require(direct_backend.commit(error));
    require(direct_filesystem.generation()==empty_generation);
    require(direct_backend.lookup("/game.gba",node));
    require(node.size==0);
    const std::uint8_t rom[]={'G','B','A'};
    require(direct_backend.write("/game.gba",0,rom,sizeof(rom),error));
    require(direct_backend.commit(error));
    require(direct_filesystem.generation()>empty_generation);
    require(direct_filesystem.readFile("game.gba",output,error));
    require(output==std::vector<std::uint8_t>({'G','B','A'}));
}
