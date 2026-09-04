#include "ez3fs/virtual_filesystem.hpp"
#include <algorithm>
#include <cstring>
#include <set>

namespace ez3fs {
VirtualFilesystem::VirtualFilesystem(std::vector<InputFile> contents,bool writable)
    :contents_(std::move(contents)),writable_(writable) {}
std::string VirtualFilesystem::normalize(const std::string& path) {
    if(path=="/")return {}; return !path.empty()&&path.front()=='/'?path.substr(1):path;
}
auto VirtualFilesystem::find(const std::string& path)->std::vector<InputFile>::iterator {
    const auto key=normalize(path);return std::find_if(contents_.begin(),contents_.end(),[&](const InputFile& n){return n.name==key;});
}
auto VirtualFilesystem::find(const std::string& path) const->std::vector<InputFile>::const_iterator {
    const auto key=normalize(path);return std::find_if(contents_.begin(),contents_.end(),[&](const InputFile& n){return n.name==key;});
}
bool VirtualFilesystem::parentExists(const std::string& path) const {
    const auto key=normalize(path);const auto slash=key.rfind('/');if(slash==std::string::npos)return true;
    const auto parent=find(key.substr(0,slash));return parent!=contents_.end()&&parent->directory;
}
bool VirtualFilesystem::lookup(const std::string& path,NodeInfo& info) const {
    const auto key=normalize(path);if(key.empty()){info={true,0};return true;}const auto node=find(path);
    if(node==contents_.end()) {
        const auto prefix=key+'/';
        if(std::any_of(contents_.begin(),contents_.end(),[&](const InputFile& item){return item.name.rfind(prefix,0)==0;})) {
            info={true,0};return true;
        }
        return false;
    }
    info={node->directory,node->bytes.size()};return true;
}
bool VirtualFilesystem::list(const std::string& path,std::vector<std::string>& children) const {
    NodeInfo info;if(!lookup(path,info)||!info.directory)return false;children.clear();std::set<std::string> unique;
    const auto key=normalize(path);const auto prefix=key.empty()?std::string{}:key+'/';
    for(const auto& node:contents_)if(node.name.rfind(prefix,0)==0){const auto rest=node.name.substr(prefix.size());
        const auto slash=rest.find('/');unique.insert(rest.substr(0,slash));}
    children.assign(unique.begin(),unique.end());return true;
}
bool VirtualFilesystem::read(const std::string& path,std::size_t offset,std::size_t size,std::vector<std::uint8_t>& output) const {
    const auto node=find(path);if(node==contents_.end()||node->directory)return false;
    const auto count=offset>=node->bytes.size()?0:std::min(size,node->bytes.size()-offset);
    output.assign(node->bytes.begin()+static_cast<std::ptrdiff_t>(std::min(offset,node->bytes.size())),
                  node->bytes.begin()+static_cast<std::ptrdiff_t>(std::min(offset,node->bytes.size())+count));return true;
}
bool VirtualFilesystem::createDirectory(const std::string& path,std::string& error) {
    if(!writable_){error="filesystem is read-only";return false;}ArchiveEditor editor(contents_);
    if(!editor.createDirectory(normalize(path),error))return false;contents_=editor.contents();dirty_=true;return true;
}
bool VirtualFilesystem::createFile(const std::string& path,std::string& error) {
    if(!writable_){error="filesystem is read-only";return false;}const auto key=normalize(path);
    if(key.empty()||find(key)!=contents_.end()){error="path already exists";return false;}
    ArchiveEditor editor(contents_);if(!editor.putFile(key,{},error))return false;contents_=editor.contents();dirty_=true;return true;
}
bool VirtualFilesystem::write(const std::string& path,std::size_t offset,const std::uint8_t* data,std::size_t size,std::string& error) {
    if(!writable_){error="filesystem is read-only";return false;}auto node=find(path);if(node==contents_.end()||node->directory){error="file does not exist";return false;}
    if(offset>ImageBuilder::cartridge_capacity||size>ImageBuilder::cartridge_capacity-offset){error="write exceeds capacity";return false;}
    if(node->bytes.size()<offset+size)node->bytes.resize(offset+size,0);std::memcpy(node->bytes.data()+offset,data,size);dirty_=true;error.clear();return true;
}
bool VirtualFilesystem::truncate(const std::string& path,std::size_t size,std::string& error) {
    if(!writable_){error="filesystem is read-only";return false;}auto node=find(path);if(node==contents_.end()||node->directory){error="file does not exist";return false;}
    if(size>ImageBuilder::cartridge_capacity){error="size exceeds capacity";return false;}node->bytes.resize(size,0);dirty_=true;error.clear();return true;
}
bool VirtualFilesystem::removeFile(const std::string& path,std::string& error) {
    if(!writable_){error="filesystem is read-only";return false;}ArchiveEditor editor(contents_);
    if(!editor.removeFile(normalize(path),error))return false;contents_=editor.contents();dirty_=true;return true;
}
bool VirtualFilesystem::removeDirectory(const std::string& path,std::string& error) {
    if(!writable_){error="filesystem is read-only";return false;}ArchiveEditor editor(contents_);
    if(!editor.removeDirectory(normalize(path),error))return false;contents_=editor.contents();dirty_=true;return true;
}
bool VirtualFilesystem::rename(const std::string& from,const std::string& to,std::string& error) {
    if(!writable_){error="filesystem is read-only";return false;}const auto source=normalize(from),destination=normalize(to);
    auto node=find(source);if(source.empty()||destination.empty()||node==contents_.end()){error="source does not exist";return false;}
    if(find(destination)!=contents_.end()||!parentExists(destination)||destination.rfind(source+'/',0)==0){error="invalid rename destination";return false;}
    const auto prefix=source+'/';for(auto& item:contents_)if(item.name==source)item.name=destination;
    else if(item.name.rfind(prefix,0)==0)item.name=destination+'/'+item.name.substr(prefix.size());
    dirty_=true;error.clear();return true;
}
} // namespace ez3fs
