#include "ez3fs/live_mount_backend.hpp"
#include "ez3fs/timestamp.hpp"

#include <algorithm>
#include <limits>

namespace ez3fs {
std::string LiveMountBackend::normalize(const std::string& path) {
    const auto first=path.find_first_not_of('/');return first==std::string::npos?std::string{}:path.substr(first);
}
bool LiveMountBackend::lookup(const std::string& path,MountNode& node) const {
    const auto name=normalize(path);const auto pending=pending_files_.find(name);
    if(pending!=pending_files_.end()){node={false,pending->second.bytes.size(),pending->second.modified_time};return true;}
    for(const auto& entry:filesystem_.entries())if(entry.name==name){node={entry.directory,entry.size,entry.modified_time};return true;}
    if(name.empty()){node={true,0,0};return true;}return false;
}
bool LiveMountBackend::list(const std::string& path,std::vector<std::string>& children) const {
    const auto name=normalize(path);children.clear();const auto prefix=name.empty()?std::string{}:name+'/';
    for(const auto& entry:filesystem_.entries())if(entry.name.rfind(prefix,0)==0){const auto rest=entry.name.substr(prefix.size());const auto slash=rest.find('/');if(slash==std::string::npos)children.push_back(rest);}
    MountNode node;return name.empty()||(lookup(name,node)&&node.directory);
}
bool LiveMountBackend::read(const std::string& path,std::size_t offset,std::size_t size,std::vector<std::uint8_t>& bytes) const {
    const auto name=normalize(path);const auto pending=pending_files_.find(name);
    if(pending!=pending_files_.end()) {
        const auto& all=pending->second.bytes;if(offset>all.size()){bytes.clear();return false;}
        const auto count=std::min(size,all.size()-offset);bytes.assign(all.begin()+static_cast<std::ptrdiff_t>(offset),all.begin()+static_cast<std::ptrdiff_t>(offset+count));return true;
    }
    std::string error;return filesystem_.readFileRange(name,offset,size,bytes,error);
}
bool LiveMountBackend::createDirectory(const std::string& path,std::string& error) { return filesystem_.createDirectory(normalize(path),error); }
bool LiveMountBackend::createFile(const std::string& path,std::string& error) { return filesystem_.putFile(normalize(path),{},currentUnixTimestamp(),error); }
bool LiveMountBackend::write(const std::string& path,std::size_t offset,const std::uint8_t* bytes,std::size_t size,std::string& error) {
    const auto name=normalize(path);PendingFile* pending=nullptr;if(!stageFile(name,pending,error))return false;
    if(offset>pending->bytes.size())pending->bytes.resize(offset,0);if(size>std::numeric_limits<std::size_t>::max()-offset){error="live write size overflow";return false;}
    if(offset+size>pending->bytes.size())pending->bytes.resize(offset+size);std::copy(bytes,bytes+size,pending->bytes.begin()+static_cast<std::ptrdiff_t>(offset));pending->modified_time=currentUnixTimestamp();error.clear();return true;
}
bool LiveMountBackend::truncate(const std::string& path,std::size_t size,std::string& error) { const auto name=normalize(path);PendingFile* pending=nullptr;if(!stageFile(name,pending,error))return false;pending->bytes.resize(size);pending->modified_time=currentUnixTimestamp();error.clear();return true; }
bool LiveMountBackend::removeFile(const std::string& path,std::string& error) { pending_files_.erase(normalize(path));return filesystem_.removeFile(normalize(path),error); }
bool LiveMountBackend::removeDirectory(const std::string& path,std::string& error) { return filesystem_.removeDirectory(normalize(path),error); }
bool LiveMountBackend::rename(const std::string& from,const std::string& to,std::string& error) { if(!commit(error))return false;return filesystem_.rename(normalize(from),normalize(to),error); }

bool LiveMountBackend::stageFile(const std::string& path,PendingFile*& pending,
                                 std::string& error) {
    const auto found=pending_files_.find(path);
    if(found!=pending_files_.end()){pending=&found->second;error.clear();return true;}
    PendingFile staged;if(!filesystem_.readFile(path,staged.bytes,error))return false;
    staged.modified_time=currentUnixTimestamp();
    pending=&pending_files_.emplace(path,std::move(staged)).first->second;error.clear();return true;
}

bool LiveMountBackend::commit(std::string& error) {
    while(!pending_files_.empty()) {
        auto current=pending_files_.begin();
        if(!filesystem_.putFile(current->first,current->second.bytes,
                                current->second.modified_time,error))return false;
        pending_files_.erase(current);
    }
    error.clear();return true;
}
} // namespace ez3fs
