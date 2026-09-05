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
    require(backend.commit(error));
    require(filesystem.generation()>generation_before_write);
    require(filesystem.readFile("docs/test.txt",output,error));
    require(output==std::vector<std::uint8_t>({'o','k'}));
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
