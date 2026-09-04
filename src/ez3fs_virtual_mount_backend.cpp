#include "ez3fs/virtual_mount_backend.hpp"

namespace ez3fs {
bool VirtualMountBackend::lookup(const std::string& path,MountNode& node) const { NodeInfo info;if(!filesystem_.lookup(path,info))return false;node={info.directory,info.size,info.modified_time};return true; }
bool VirtualMountBackend::list(const std::string& path,std::vector<std::string>& children) const { return filesystem_.list(path,children); }
bool VirtualMountBackend::read(const std::string& path,std::size_t offset,std::size_t size,std::vector<std::uint8_t>& bytes) const { return filesystem_.read(path,offset,size,bytes); }
} // namespace ez3fs
