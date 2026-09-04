#include "ez3fs/virtual_filesystem.hpp"
#include "ez3fs/virtual_mount_backend.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
namespace {
void require(bool condition) { if(!condition) std::abort(); }
struct TemporaryImage final {
    std::filesystem::path path=std::filesystem::temp_directory_path()/"ez3fs-virtual-mount-backend-test.ez3fs";
    TemporaryImage(){std::error_code ignored;std::filesystem::remove(path,ignored);}
    ~TemporaryImage(){std::error_code ignored;std::filesystem::remove(path,ignored);}
};
}
int main() {
    ez3fs::VirtualFilesystem fs({{"docs",{},true},{"docs/readme",{'o','l','d'},false}},true);
    ez3fs::NodeInfo info;require(fs.lookup("/",info)&&info.directory);require(fs.lookup("/docs/readme",info)&&info.size==3);
    std::vector<std::string> children;require(fs.list("/docs",children));require(children.size()==1&&children[0]=="readme");
    std::string error;const std::uint8_t replacement[]={'n','e','w'};
    require(fs.truncate("/docs/readme",0,error));require(fs.write("/docs/readme",0,replacement,3,error));
    require(fs.createDirectory("/media",error));require(fs.createFile("/media/empty",error));
    require(fs.rename("/media","/assets",error));require(fs.lookup("/assets/empty",info));
    require(fs.removeFile("/assets/empty",error));require(fs.removeDirectory("/assets",error));require(fs.dirty());
    ez3fs::VirtualFilesystem read_only({},false);require(!read_only.createDirectory("blocked",error));

    TemporaryImage image;
    ez3fs::VirtualMountBackend backend({{"saved.txt",{'o','l','d'},false}},true,image.path);
    require(backend.truncate("/saved.txt",0,error));
    require(backend.write("/saved.txt",0,replacement,3,error));
    require(backend.commit(error));
    std::ifstream input(image.path,std::ios::binary);
    std::vector<std::uint8_t> bytes(std::istreambuf_iterator<char>(input),{});
    ez3fs::Archive archive;require(archive.open(std::move(bytes),error));require(archive.verify(error));
    const auto contents=archive.contents();require(contents.size()==1);require(contents[0].bytes==std::vector<std::uint8_t>({'n','e','w'}));
}
