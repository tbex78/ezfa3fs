#include "ez3fs/live_filesystem.hpp"

#include <cstdlib>

namespace { void require(bool condition) { if(!condition)std::abort(); } }

int main()
{
    ez3fs::live::NorFlash flash;std::string error;
    require(ez3fs::live::Filesystem::format(flash,error));
    ez3fs::live::Filesystem filesystem(flash);
    require(ez3fs::live::Filesystem::open(flash,filesystem,error));
    require(filesystem.generation()==1);
    require(filesystem.createDirectory("docs",error));
    require(filesystem.putFile("docs/readme.txt",{'o','k'},1234,error));
    std::vector<std::uint8_t> bytes;
    require(filesystem.readFile("docs/readme.txt",bytes,error));
    require(bytes==std::vector<std::uint8_t>({'o','k'}));
    const auto generation=filesystem.generation();
    require(filesystem.putFile("docs/readme.txt",{'n','e','w'},1235,error));
    require(filesystem.generation()>generation);

    const auto free_before_data_failure=filesystem.freeBlocks();
    flash.failNextProgramAfter(8);
    require(!filesystem.putFile("docs/partial.txt",{'x'},1236,error));
    require(filesystem.freeBlocks()==free_before_data_failure-1);

    const auto before=filesystem.generation();
    const auto free_before=filesystem.freeBlocks();
    flash.failNextProgramAfter(8);
    require(!filesystem.createDirectory("interrupted",error));
    require(filesystem.freeBlocks()==free_before);
    ez3fs::live::Filesystem reopened(flash);
    require(ez3fs::live::Filesystem::open(flash,reopened,error));
    require(reopened.generation()==before);
    require(reopened.entries().size()==2);
    require(reopened.freeBlocks()==free_before_data_failure-1);
    require(!reopened.createDirectory("docs/readme.txt",error));
    require(reopened.removeFile("docs/readme.txt",error));
    require(reopened.removeDirectory("docs",error));
}
