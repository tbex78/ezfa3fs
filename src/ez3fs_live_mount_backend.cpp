#include "ez3fs/live_mount_backend.hpp"
#include "ez3fs/timestamp.hpp"

#include <algorithm>
#include <limits>

namespace ez3fs {
std::string LiveMountBackend::normalize(const std::string& path) {
    const auto first=path.find_first_not_of('/');return first==std::string::npos?std::string{}:path.substr(first);
}
bool LiveMountBackend::lookup(const std::string& path,MountNode& node) const {
    const auto name=normalize(path);for(const auto& entry:filesystem_.entries())if(entry.name==name){node={entry.directory,entry.size,entry.modified_time};return true;}
    if(name.empty()){node={true,0,0};return true;}return false;
}
bool LiveMountBackend::list(const std::string& path,std::vector<std::string>& children) const {
    const auto name=normalize(path);children.clear();const auto prefix=name.empty()?std::string{}:name+'/';
    for(const auto& entry:filesystem_.entries())if(entry.name.rfind(prefix,0)==0){const auto rest=entry.name.substr(prefix.size());const auto slash=rest.find('/');if(slash==std::string::npos)children.push_back(rest);}
    MountNode node;return name.empty()||(lookup(name,node)&&node.directory);
}
bool LiveMountBackend::read(const std::string& path,std::size_t offset,std::size_t size,std::vector<std::uint8_t>& bytes) const {
    std::vector<std::uint8_t> all;std::string error;if(!filesystem_.readFile(normalize(path),all,error)||offset>all.size()){bytes.clear();return false;}const auto count=std::min(size,all.size()-offset);bytes.assign(all.begin()+static_cast<std::ptrdiff_t>(offset),all.begin()+static_cast<std::ptrdiff_t>(offset+count));return true;
}
bool LiveMountBackend::createDirectory(const std::string& path,std::string& error) { return filesystem_.createDirectory(normalize(path),error); }
bool LiveMountBackend::createFile(const std::string& path,std::string& error) { return filesystem_.putFile(normalize(path),{},currentUnixTimestamp(),error); }
bool LiveMountBackend::write(const std::string& path,std::size_t offset,const std::uint8_t* bytes,std::size_t size,std::string& error) {
    const auto name=normalize(path);std::vector<std::uint8_t> all;if(!filesystem_.readFile(name,all,error))return false;if(offset>all.size())all.resize(offset,0);if(size>std::numeric_limits<std::size_t>::max()-offset){error="live write size overflow";return false;}if(offset+size>all.size())all.resize(offset+size);std::copy(bytes,bytes+size,all.begin()+static_cast<std::ptrdiff_t>(offset));return filesystem_.putFile(name,all,currentUnixTimestamp(),error);
}
bool LiveMountBackend::truncate(const std::string& path,std::size_t size,std::string& error) { const auto name=normalize(path);std::vector<std::uint8_t> all;if(!filesystem_.readFile(name,all,error))return false;all.resize(size);return filesystem_.putFile(name,all,currentUnixTimestamp(),error); }
bool LiveMountBackend::removeFile(const std::string& path,std::string& error) { return filesystem_.removeFile(normalize(path),error); }
bool LiveMountBackend::removeDirectory(const std::string& path,std::string& error) { return filesystem_.removeDirectory(normalize(path),error); }
bool LiveMountBackend::rename(const std::string& from,const std::string& to,std::string& error) { return filesystem_.rename(normalize(from),normalize(to),error); }
} // namespace ez3fs
