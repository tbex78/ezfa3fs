#include "ez3fs/live_mount_backend.hpp"

#include <algorithm>

namespace ez3fs {
bool LiveMountBackend::lookup(const std::string& path,MountNode& node) const {
    for(const auto& entry:filesystem_.entries())if(entry.name==path){node={entry.directory,entry.size,entry.modified_time};return true;}
    if(path.empty()){node={true,0,0};return true;}return false;
}
bool LiveMountBackend::list(const std::string& path,std::vector<std::string>& children) const {
    children.clear();const auto prefix=path.empty()?std::string{}:path+'/';
    for(const auto& entry:filesystem_.entries())if(entry.name.rfind(prefix,0)==0){const auto rest=entry.name.substr(prefix.size());const auto slash=rest.find('/');if(slash==std::string::npos)children.push_back(rest);}
    MountNode node;return path.empty()||lookup(path,node);
}
bool LiveMountBackend::read(const std::string& path,std::size_t offset,std::size_t size,std::vector<std::uint8_t>& bytes) const {
    std::vector<std::uint8_t> all;std::string error;if(!filesystem_.readFile(path,all,error)||offset>all.size()){bytes.clear();return false;}const auto count=std::min(size,all.size()-offset);bytes.assign(all.begin()+static_cast<std::ptrdiff_t>(offset),all.begin()+static_cast<std::ptrdiff_t>(offset+count));return true;
}
bool LiveMountBackend::createFile(const std::string& path,std::string& error) { return filesystem_.putFile(path,{},0,error); }
bool LiveMountBackend::write(const std::string& path,std::size_t offset,const std::uint8_t* bytes,std::size_t size,std::string& error) {
    std::vector<std::uint8_t> all;if(!filesystem_.readFile(path,all,error))return false;if(offset>all.size())all.resize(offset,0);if(offset+size>all.size())all.resize(offset+size);std::copy(bytes,bytes+size,all.begin()+static_cast<std::ptrdiff_t>(offset));return filesystem_.putFile(path,all,0,error);
}
bool LiveMountBackend::truncate(const std::string& path,std::size_t size,std::string& error) { std::vector<std::uint8_t> all;if(!filesystem_.readFile(path,all,error))return false;all.resize(size);return filesystem_.putFile(path,all,0,error); }
} // namespace ez3fs
