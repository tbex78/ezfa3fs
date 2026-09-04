#include "ez3fs/virtual_filesystem.hpp"
#include <cstdlib>
#include <string>
namespace { void require(bool condition) { if(!condition) std::abort(); } }
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
}
