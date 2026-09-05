#include "ez3fs/finder_metadata_mount_backend.hpp"

#include "ez3fs/timestamp.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace ez3fs {

FinderMetadataMountBackend::FinderMetadataMountBackend(
    std::unique_ptr<MountBackend> backend) : backend_(std::move(backend)) {}

std::string FinderMetadataMountBackend::normalize(const std::string& path) {
    const auto first=path.find_first_not_of('/');
    return first==std::string::npos?std::string{}:path.substr(first);
}

bool FinderMetadataMountBackend::isFinderMetadata(
    const std::string& path) noexcept {
    const auto name=normalize(path);
    const auto slash=name.rfind('/');
    const auto base=slash==std::string::npos?name:name.substr(slash+1);
    return base.rfind(".DS_Store",0)==0||base.rfind("._",0)==0;
}

bool FinderMetadataMountBackend::lookup(const std::string& path,
                                        MountNode& node) const {
    const auto found=transient_files_.find(normalize(path));
    if(found!=transient_files_.end()) {
        node={false,found->second.bytes.size(),found->second.modified_time};
        return true;
    }
    return backend_->lookup(path,node);
}

bool FinderMetadataMountBackend::list(
    const std::string& path,std::vector<std::string>& children) const {
    if(!backend_->list(path,children))return false;
    const auto directory=normalize(path);
    const auto prefix=directory.empty()?std::string{}:directory+'/';
    for(const auto& item:transient_files_) {
        if(item.first.rfind(prefix,0)!=0)continue;
        const auto child=item.first.substr(prefix.size());
        if(child.find('/')==std::string::npos&&
           std::find(children.begin(),children.end(),child)==children.end())
            children.push_back(child);
    }
    return true;
}

bool FinderMetadataMountBackend::read(
    const std::string& path,std::size_t offset,std::size_t size,
    std::vector<std::uint8_t>& bytes) const {
    const auto found=transient_files_.find(normalize(path));
    if(found==transient_files_.end())return backend_->read(path,offset,size,bytes);
    if(offset>found->second.bytes.size())return false;
    const auto count=std::min(size,found->second.bytes.size()-offset);
    bytes.assign(found->second.bytes.begin()+static_cast<std::ptrdiff_t>(offset),
                 found->second.bytes.begin()+
                    static_cast<std::ptrdiff_t>(offset+count));
    return true;
}

bool FinderMetadataMountBackend::writable() const noexcept {
    return backend_->writable();
}

bool FinderMetadataMountBackend::createDirectory(
    const std::string& path,std::string& error) {
    return backend_->createDirectory(path,error);
}

bool FinderMetadataMountBackend::createFile(
    const std::string& path,std::string& error) {
    if(!isFinderMetadata(path))return backend_->createFile(path,error);
    MountNode existing;
    const auto name=normalize(path);
    if(transient_files_.find(name)!=transient_files_.end()||
       backend_->lookup(path,existing)) {
        error="existing Finder metadata file";
        return false;
    }
    transient_files_.emplace(name,TransientFile{{},currentUnixTimestamp(),false});
    error.clear();
    return true;
}

bool FinderMetadataMountBackend::stage(
    const std::string& path,TransientFile*& file,std::string& error) {
    const auto name=normalize(path);
    const auto existing=transient_files_.find(name);
    if(existing!=transient_files_.end()) {
        file=&existing->second;
        error.clear();
        return true;
    }
    MountNode node;
    TransientFile staged;
    if(backend_->lookup(path,node)) {
        if(node.directory||node.size>std::numeric_limits<std::size_t>::max()||
           !backend_->read(path,0,static_cast<std::size_t>(node.size),staged.bytes)) {
            error="could not stage Finder metadata file";
            return false;
        }
        staged.modified_time=node.modified_time;
        staged.shadows_persisted_file=true;
    } else {
        staged.modified_time=currentUnixTimestamp();
    }
    file=&transient_files_.emplace(name,std::move(staged)).first->second;
    error.clear();
    return true;
}

bool FinderMetadataMountBackend::write(
    const std::string& path,std::size_t offset,const std::uint8_t* bytes,
    std::size_t size,std::string& error) {
    if(!isFinderMetadata(path))return backend_->write(path,offset,bytes,size,error);
    if(size>std::numeric_limits<std::size_t>::max()-offset) {
        error="Finder metadata write size overflow";
        return false;
    }
    TransientFile* file=nullptr;
    if(!stage(path,file,error))return false;
    if(offset>file->bytes.size())file->bytes.resize(offset,0);
    if(offset+size>file->bytes.size())file->bytes.resize(offset+size);
    std::copy(bytes,bytes+size,file->bytes.begin()+static_cast<std::ptrdiff_t>(offset));
    file->modified_time=currentUnixTimestamp();
    error.clear();
    return true;
}

bool FinderMetadataMountBackend::truncate(
    const std::string& path,std::size_t size,std::string& error) {
    if(!isFinderMetadata(path))return backend_->truncate(path,size,error);
    TransientFile* file=nullptr;
    if(!stage(path,file,error))return false;
    file->bytes.resize(size);
    file->modified_time=currentUnixTimestamp();
    error.clear();
    return true;
}

bool FinderMetadataMountBackend::removeFile(
    const std::string& path,std::string& error) {
    if(!isFinderMetadata(path))return backend_->removeFile(path,error);
    const auto found=transient_files_.find(normalize(path));
    if(found==transient_files_.end())return backend_->removeFile(path,error);
    const bool persisted=found->second.shadows_persisted_file;
    if(persisted&&!backend_->removeFile(path,error))return false;
    transient_files_.erase(found);
    error.clear();
    return true;
}

bool FinderMetadataMountBackend::removeDirectory(
    const std::string& path,std::string& error) {
    const auto name=normalize(path);
    const auto prefix=name.empty()?std::string{}:name+'/';
    if(std::any_of(transient_files_.begin(),transient_files_.end(),
        [&](const auto& file){return file.first.rfind(prefix,0)==0;})) {
        error="directory contains transient Finder metadata";
        return false;
    }
    return backend_->removeDirectory(path,error);
}

bool FinderMetadataMountBackend::rename(
    const std::string& from,const std::string& to,std::string& error) {
    const auto source=transient_files_.find(normalize(from));
    if(source==transient_files_.end()) {
        if(!backend_->rename(from,to,error))return false;
        const auto old_name=normalize(from);
        const auto new_name=normalize(to);
        const auto prefix=old_name+'/';
        std::vector<std::pair<std::string,TransientFile>> moved;
        for(auto current=transient_files_.begin();current!=transient_files_.end();) {
            if(current->first.rfind(prefix,0)!=0) {
                ++current;
                continue;
            }
            moved.emplace_back(new_name+current->first.substr(old_name.size()),
                               std::move(current->second));
            current=transient_files_.erase(current);
        }
        for(auto& file:moved)
            transient_files_.emplace(std::move(file.first),std::move(file.second));
        error.clear();
        return true;
    }
    if(!isFinderMetadata(to)) {
        error="cannot persist transient Finder metadata through rename";
        return false;
    }
    MountNode existing;
    const auto destination=normalize(to);
    if(transient_files_.find(destination)!=transient_files_.end()||
       backend_->lookup(to,existing)) {
        error="invalid transient Finder metadata rename";
        return false;
    }
    if(source->second.shadows_persisted_file&&
       !backend_->removeFile(from,error))return false;
    auto file=std::move(source->second);
    file.shadows_persisted_file=false;
    transient_files_.erase(source);
    transient_files_.emplace(destination,std::move(file));
    error.clear();
    return true;
}

bool FinderMetadataMountBackend::commitFile(
    const std::string& path,std::string& error) {
    if(isFinderMetadata(path)) {
        error.clear();
        return true;
    }
    return backend_->commitFile(path,error);
}

bool FinderMetadataMountBackend::commit(std::string& error) {
    return backend_->commit(error);
}

std::uint64_t FinderMetadataMountBackend::capacityBytes() const noexcept {
    return backend_->capacityBytes();
}

std::uint64_t FinderMetadataMountBackend::freeBytes() const {
    return backend_->freeBytes();
}

std::size_t FinderMetadataMountBackend::entryCount() const noexcept {
    std::size_t extra=0;
    for(const auto& file:transient_files_)
        if(!file.second.shadows_persisted_file)++extra;
    return backend_->entryCount()+extra;
}

} // namespace ez3fs
