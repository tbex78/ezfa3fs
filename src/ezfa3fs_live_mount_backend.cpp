#include "ezfa3fs/live_mount_backend.hpp"
#include "ezfa3fs/timestamp.hpp"

#include <algorithm>
#include <iostream>
#include <limits>

namespace ezfa3fs {
std::string LiveMountBackend::normalize(const std::string& path) {
    const auto first=path.find_first_not_of('/');
    return first==std::string::npos
        ? std::string{}
        : path.substr(first);
}

void LiveMountBackend::publishPendingFile(
    const std::string& name,
    const PendingFile& pending) {

    visible_nodes_[name]={
        false,
        pending.bytes.size(),
        pending.modified_time
    };
}

void LiveMountBackend::publishFilesystemEntry(
    const std::string& name) {

    const auto found=
        std::find_if(
            filesystem_.entries().begin(),
            filesystem_.entries().end(),
            [&](const live::Entry& entry) {
                return entry.name==name;
            });

    if(found==filesystem_.entries().end())
        return;

    visible_nodes_[name]={
        found->directory,
        found->size,
        found->modified_time
    };
}

void LiveMountBackend::renameVisibleTree(
    const std::string& from,
    const std::string& to) {

    const auto prefix=from+'/';

    std::vector<std::pair<std::string,MountNode>> moved;

    for(auto current=visible_nodes_.begin();
        current!=visible_nodes_.end();) {

        if(current->first!=from &&
           current->first.rfind(prefix,0)!=0) {
            ++current;
            continue;
        }

        moved.emplace_back(
            to+current->first.substr(from.size()),
            current->second);

        current=visible_nodes_.erase(current);
    }

    for(auto& item:moved)
        visible_nodes_[std::move(item.first)]=item.second;
}

bool LiveMountBackend::lookup(
    const std::string& path,
    MountNode& node) const {

    const auto name=normalize(path);

    if(name.empty()) {
        node={true,0,0};
        return true;
    }

    const auto found=visible_nodes_.find(name);

    if(found==visible_nodes_.end())
        return false;

    node=found->second;
    return true;
}

bool LiveMountBackend::list(
    const std::string& path,
    std::vector<std::string>& children) const {

    const auto name=normalize(path);

    children.clear();

    MountNode node;

    if(!name.empty() &&
       (!lookup(name,node)||!node.directory))
        return false;

    const auto prefix=
        name.empty()
            ? std::string{}
            : name+'/';

    for(const auto& item:visible_nodes_) {
        const auto& entry_name=item.first;

        if(entry_name.rfind(prefix,0)!=0)
            continue;

        const auto rest=
            entry_name.substr(prefix.size());

        if(rest.empty())
            continue;

        const auto slash=rest.find('/');

        const auto child=
            slash==std::string::npos
                ? rest
                : rest.substr(0,slash);

        if(std::find(
                children.begin(),
                children.end(),
                child)==children.end()) {
            children.push_back(child);
        }
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
bool LiveMountBackend::createDirectory(
    const std::string& path,
    std::string& error) {

    const auto name=normalize(path);

    if(!filesystem_.createDirectory(name,error))
        return false;

    publishFilesystemEntry(name);
    return true;
}
bool LiveMountBackend::createFile(const std::string& path,std::string& error) {
    const auto name=normalize(path);
    if(pending_files_.find(name)!=pending_files_.end()) {
        error="invalid or existing live file path";return false;
    }
    if(!filesystem_.canCreateFile(name,error))return false;
    const auto modified_time=currentUnixTimestamp();

    pending_files_.emplace(
        name,
        PendingFile{{},modified_time});

    visible_nodes_[name]={
        false,
        0,
        modified_time
    };

    std::cerr
        <<"Staging file: "<<name
        <<"\n";

    error.clear();return true;
}
bool LiveMountBackend::write(const std::string& path,std::size_t offset,const std::uint8_t* bytes,std::size_t size,std::string& error) {
    const auto name=normalize(path);PendingFile* pending=nullptr;if(!stageFile(name,pending,error))return false;
    if(offset>pending->bytes.size())pending->bytes.resize(offset,0);if(size>std::numeric_limits<std::size_t>::max()-offset){error="live write size overflow";return false;}
    if(offset+size>pending->bytes.size())pending->bytes.resize(offset+size);std::copy(bytes,bytes+size,pending->bytes.begin()+static_cast<std::ptrdiff_t>(offset));pending->modified_time=currentUnixTimestamp();publishPendingFile(name,*pending);error.clear();return true;
}
bool LiveMountBackend::truncate(const std::string& path,std::size_t size,std::string& error) { const auto name=normalize(path);PendingFile* pending=nullptr;if(!stageFile(name,pending,error))return false;pending->bytes.resize(size);pending->modified_time=currentUnixTimestamp();publishPendingFile(name,*pending);error.clear();return true; }
bool LiveMountBackend::removeFile(
    const std::string& path,
    std::string& error) {

    const auto name=normalize(path);

    const auto committed=
        std::any_of(
            filesystem_.entries().begin(),
            filesystem_.entries().end(),
            [&](const live::Entry& entry) {
                return entry.name==name&&!entry.directory;
            });

    const auto pending=pending_files_.find(name);

    if(committed) {
        if(!filesystem_.removeFile(name,error))
            return false;

        if(pending!=pending_files_.end())
            pending_files_.erase(pending);

        visible_nodes_.erase(name);
        return true;
    }

    if(pending!=pending_files_.end()) {
        pending_files_.erase(pending);
        visible_nodes_.erase(name);
        error.clear();
        return true;
    }

    error="live file does not exist";
    return false;
}

bool LiveMountBackend::removeDirectory(
    const std::string& path,
    std::string& error) {

    const auto name=normalize(path);
    const auto prefix=name+'/';

    if(std::any_of(
            pending_files_.begin(),
            pending_files_.end(),
            [&](const auto& pending) {
                return pending.first.rfind(prefix,0)==0;
            })) {
        error="live directory is not empty";
        return false;
    }

    if(!filesystem_.removeDirectory(name,error))
        return false;

    visible_nodes_.erase(name);
    return true;
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
            renameVisibleTree(source_name,destination_name);

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

    if(!filesystem_.rename(
            source_name,
            destination_name,
            error))
        return false;

    renameVisibleTree(
        source_name,
        destination_name);

    error.clear();
    return true;
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
    return visible_nodes_.size();
}

} // namespace ezfa3fs
