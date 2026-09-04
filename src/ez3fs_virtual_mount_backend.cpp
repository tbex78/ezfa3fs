#include "ez3fs/virtual_mount_backend.hpp"

#include <fstream>

namespace ez3fs {
bool VirtualMountBackend::lookup(const std::string& path,MountNode& node) const { NodeInfo info;if(!filesystem_.lookup(path,info))return false;node={info.directory,info.size,info.modified_time};return true; }
bool VirtualMountBackend::list(const std::string& path,std::vector<std::string>& children) const { return filesystem_.list(path,children); }
bool VirtualMountBackend::read(const std::string& path,std::size_t offset,std::size_t size,std::vector<std::uint8_t>& bytes) const { return filesystem_.read(path,offset,size,bytes); }
bool VirtualMountBackend::commit(std::string& error) {
    if(!filesystem_.dirty()){error.clear();return true;}
    if(!image_){error="EZ3FS in-memory mount cannot be committed";return false;}
    ArchiveImage built;if(!ImageBuilder{}.build(filesystem_.contents(),built,error))return false;
    auto temporary=*image_;temporary += ".fuse.tmp";
    if(std::filesystem::exists(temporary)){error="EZ3FS commit temporary image already exists";return false;}
    std::ofstream output(temporary,std::ios::binary|std::ios::trunc);
    if(!output){error="EZ3FS commit could not create temporary image";return false;}
    output.write(reinterpret_cast<const char*>(built.bytes.data()),static_cast<std::streamsize>(built.bytes.size()));output.close();
    if(!output){std::error_code ignored;std::filesystem::remove(temporary,ignored);error="EZ3FS commit could not write temporary image";return false;}
    std::error_code rename_error;std::filesystem::rename(temporary,*image_,rename_error);
    if(rename_error){std::error_code ignored;std::filesystem::remove(temporary,ignored);error="EZ3FS commit could not replace image: "+rename_error.message();return false;}
    filesystem_.markClean();error.clear();return true;
}
std::uint64_t VirtualMountBackend::freeBytes() const {
    ArchiveImage built;std::string error;if(!ImageBuilder{}.build(filesystem_.contents(),built,error))return 0;
    return built.bytes.size()<ImageBuilder::cartridge_capacity?ImageBuilder::cartridge_capacity-built.bytes.size():0;
}
} // namespace ez3fs
