#include "ezfa3fs/live_mount_backend.hpp"
#include "ezfa3fs/timestamp.hpp"

#include <algorithm>
#include <iostream>
#include <limits>

namespace ezfa3fs {
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
    const auto name=normalize(path);children.clear();MountNode node;
    if(!name.empty()&&(!lookup(name,node)||!node.directory))return false;
    const auto prefix=name.empty()?std::string{}:name+'/';
    for(const auto& entry:filesystem_.entries())if(entry.name.rfind(prefix,0)==0){const auto rest=entry.name.substr(prefix.size());const auto slash=rest.find('/');if(slash==std::string::npos)children.push_back(rest);}
    for(const auto& [pending_name,pending]:pending_files_) {
        (void)pending;
        if(pending_name.rfind(prefix,0)!=0)continue;
        const auto rest=pending_name.substr(prefix.size());
        if(rest.find('/')==std::string::npos&&
           std::find(children.begin(),children.end(),rest)==children.end())children.push_back(rest);
    }
    return true;
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
bool LiveMountBackend::createFile(const std::string& path,std::string& error) {
    const auto name=normalize(path);
    if(pending_files_.find(name)!=pending_files_.end()) {
        error="invalid or existing live file path";return false;
    }
    if(!filesystem_.canCreateFile(name,error))return false;
    pending_files_.emplace(name,PendingFile{{},currentUnixTimestamp()});

    std::cerr
        <<"Staging file: "<<name
        <<"\n";

    error.clear();return true;
}
bool LiveMountBackend::write(const std::string& path,std::size_t offset,const std::uint8_t* bytes,std::size_t size,std::string& error) {
    const auto name=normalize(path);PendingFile* pending=nullptr;if(!stageFile(name,pending,error))return false;
    if(offset>pending->bytes.size())pending->bytes.resize(offset,0);if(size>std::numeric_limits<std::size_t>::max()-offset){error="live write size overflow";return false;}
    if(offset+size>pending->bytes.size())pending->bytes.resize(offset+size);std::copy(bytes,bytes+size,pending->bytes.begin()+static_cast<std::ptrdiff_t>(offset));pending->modified_time=currentUnixTimestamp();error.clear();return true;
}
bool LiveMountBackend::truncate(const std::string& path,std::size_t size,std::string& error) { const auto name=normalize(path);PendingFile* pending=nullptr;if(!stageFile(name,pending,error))return false;pending->bytes.resize(size);pending->modified_time=currentUnixTimestamp();error.clear();return true; }
bool LiveMountBackend::removeFile(const std::string& path,std::string& error) {
    const auto name=normalize(path);
    const auto committed=std::any_of(filesystem_.entries().begin(),filesystem_.entries().end(),
        [&](const live::Entry& entry){return entry.name==name&&!entry.directory;});
    const bool pending=pending_files_.erase(name)!=0;
    if(committed)return filesystem_.removeFile(name,error);
    if(pending){error.clear();return true;}
    error="live file does not exist";return false;
}
bool LiveMountBackend::removeDirectory(const std::string& path,std::string& error) {
    const auto name=normalize(path);const auto prefix=name+'/';
    if(std::any_of(pending_files_.begin(),pending_files_.end(),
        [&](const auto& pending){return pending.first.rfind(prefix,0)==0;})) {
        error="live directory is not empty";return false;
    }
    return filesystem_.removeDirectory(name,error);
}
bool LiveMountBackend::rename(const std::string& from,const std::string& to,
                              std::string& error) {
    const auto source_name=normalize(from);
    const auto destination_name=normalize(to);

    if(source_name!=destination_name&&
       pending_files_.find(destination_name)!=pending_files_.end()) {
        error="invalid live rename";
        return false;
    }

    MountNode source;
    if(!lookup(from,source)) {
        error="live path does not exist";
        return false;
    }

    if(source.directory) {
        // A directory rename also changes the paths of pending descendants,
        // so retain the existing full-commit behaviour for directories.
        if(!commit(error))return false;
    } else {
        const auto pending_source=pending_files_.find(source_name);

        // An empty staged file intentionally has no persistent filesystem
        // entry. Rename the RAM-only placeholder directly instead of forcing
        // it through commitFile() and then asking Filesystem::rename() to
        // rename an entry that does not exist on flash.
        if(pending_source!=pending_files_.end()&&
           pending_source->second.bytes.empty()) {
            if(source_name==destination_name) {
                error.clear();
                return true;
            }

            if(!filesystem_.canCreateFile(destination_name,error))
                return false;

            auto pending=std::move(pending_source->second);
            pending_files_.erase(pending_source);
            pending_files_.emplace(destination_name,std::move(pending));

            std::cerr
                <<"Renamed staged zero-byte file: "
                <<source_name
                <<" -> "
                <<destination_name
                <<".\n";

            error.clear();
            return true;
        }

        // Finder may have several copy destinations pending simultaneously.
        // Commit only the non-empty file being renamed; never flush unrelated
        // placeholders.
        if(!commitFile(from,error))return false;
    }

    return filesystem_.rename(source_name,destination_name,error);
}

bool LiveMountBackend::stageFile(const std::string& path,PendingFile*& pending,
                                 std::string& error) {
    const auto found=pending_files_.find(path);
    if(found!=pending_files_.end()){pending=&found->second;error.clear();return true;}
    PendingFile staged;if(!filesystem_.readFile(path,staged.bytes,error))return false;
    staged.modified_time=currentUnixTimestamp();
    pending=&pending_files_.emplace(path,std::move(staged)).first->second;error.clear();return true;
}

bool LiveMountBackend::persistFile(const std::string& path,
                                   const PendingFile& pending,
                                   std::string& error) {
    return filesystem_.putFile(path,pending.bytes,pending.modified_time,error,
                               maintenance_observer_);
}

bool LiveMountBackend::commitReady(const PendingFile& pending) const noexcept {
    // Finder and POSIX copy tools commonly create/synchronize destination
    // placeholders before sending their contents. Keep every zero-byte staged
    // file in RAM only. If data later arrives it becomes commit-ready normally;
    // if the mount ends while it is still empty, the placeholder disappears
    // without ever consuming cartridge metadata.
    return !pending.bytes.empty();
}

bool LiveMountBackend::commitFile(const std::string& path,std::string& error) {
    const auto current=pending_files_.find(normalize(path));
    if(current==pending_files_.end()||!commitReady(current->second)) {
        error.clear();
        return true;
    }
    std::cerr
        <<"Finished staging file: "
        <<current->first
        <<" ("<<current->second.bytes.size()/1024
        <<" KiB).\n";

    std::cerr
        <<"Committing file: "
        <<current->first
        <<" ("<<current->second.bytes.size()/1024
        <<" KiB)...\n";

    if(!persistFile(current->first,current->second,error))return false;
    if(persistence_observer_&&!persistence_observer_(error))return false;
    pending_files_.erase(current);
    error.clear();
    return true;
}

bool LiveMountBackend::commit(std::string& error) {
    std::vector<std::string> persisted;
    for(auto current=pending_files_.begin();current!=pending_files_.end();
        ++current) {
        if(!commitReady(current->second)) {
            continue;
        }
        if(!persistFile(current->first,current->second,error))return false;
        persisted.push_back(current->first);
    }
    if(persistence_observer_&&!persistence_observer_(error))return false;
    for(const auto& name:persisted)pending_files_.erase(name);
    error.clear();return true;
}

std::size_t LiveMountBackend::entryCount() const noexcept {
    std::size_t count=filesystem_.entries().size();
    for(const auto& pending:pending_files_) {
        const auto& name=pending.first;
        const auto committed=std::any_of(filesystem_.entries().begin(),filesystem_.entries().end(),
            [&](const live::Entry& entry){return entry.name==name;});
        if(!committed)++count;
    }
    return count;
}
} // namespace ezfa3fs
