#include "ezfa3fs/fuse_mount.hpp"
#include "ezfa3fs/finder_metadata_mount_backend.hpp"
#include "ezfa3fs/live_cartridge_session.hpp"
#include "ezfa3fs/live_mount_backend.hpp"
#include "ezfa3fs/mount_backend.hpp"
#include "ezfa3fs/mount_session.hpp"
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <vector>

#if defined(EZFA3FS_HAS_FUSE3)
#define FUSE_USE_VERSION 31
#define FUSE_DARWIN_ENABLE_EXTENSIONS 0
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdollar-in-identifier-extension"
#pragma clang diagnostic ignored "-Wgnu-anonymous-struct"
#pragma clang diagnostic ignored "-Wnested-anon-types"
#endif
#include <fuse3/fuse.h>
#if defined(__APPLE__)
#include <fuse3/fuse_lowlevel.h>
#endif
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#include <unistd.h>
#endif

namespace ezfa3fs {
#if defined(EZFA3FS_HAS_FUSE3)
namespace {
struct FuseFileHandle final {
    bool writable = false;
    bool dirty = false;
};

FuseFileHandle* fileHandle(struct fuse_file_info* info) {
    return info&&info->fh!=0?
        reinterpret_cast<FuseFileHandle*>(static_cast<std::uintptr_t>(info->fh)):
        nullptr;
}
void installFileHandle(struct fuse_file_info* info,bool writable,bool dirty) {
    if(!info)return;
    auto handle=std::make_unique<FuseFileHandle>();
    handle->writable=writable;handle->dirty=dirty;
    info->fh=static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(handle.release()));
}
void markFileHandleDirty(struct fuse_file_info* info) {
    if(auto* handle=fileHandle(info);handle&&handle->writable)handle->dirty=true;
}
std::unique_ptr<FuseFileHandle> takeFileHandle(struct fuse_file_info* info) {
    auto* handle=fileHandle(info);
    if(info)info->fh=0;
    return std::unique_ptr<FuseFileHandle>(handle);
}

MountSession& session(){return *static_cast<MountSession*>(fuse_get_context()->private_data);}
int mutationFailure(MountSession& value,const std::string& error) {
    if(value.shouldReportFailure(error))std::cerr<<"EZFA3FS mutation failed: "<<error<<'\n';
    if(!value.backend().writable())return -EROFS;
    if(error.find("out of free blocks")!=std::string::npos)return -ENOSPC;
    if(error.find("not empty")!=std::string::npos)return -ENOTEMPTY;
    if(error.find("does not exist")!=std::string::npos)return -ENOENT;
    if(error.find("existing")!=std::string::npos)return -EEXIST;
    if(error.find("USB ")!=std::string::npos||error.find("cartridge")!=std::string::npos||
       error.find("readback")!=std::string::npos||
       error.find("live image")!=std::string::npos)return -EIO;
    return -EINVAL;
}
int beginMutation(MountSession& value) {
    if(!value.backend().writable())return -EROFS;
    std::string error;
    if(value.mutationAllowed(error))return 0;
    return mutationFailure(value,error);
}
int commitSession(MountSession& value) {
    std::string error;
    if(value.commit(error))return 0;
    return mutationFailure(value,error);
}
int commitSessionFile(MountSession& value,const char* path) {
    std::string error;
    if(value.commitFile(path,error))return 0;
    return mutationFailure(value,error);
}
int finishMutation(bool changed,const std::string& error) {
    if(!changed)return mutationFailure(session(),error);
    return commitSession(session());
}
int metadataMutation(const char* path) {
    MountNode node;
    if(!session().backend().lookup(path,node))return -ENOENT;
    // EZFA3FS persists file data, directory structure, and modification
    // times only. Unix permissions and ownership are deliberately a fixed
    // mount policy, so acknowledge metadata updates Finder requires without
    // turning them into filesystem transactions.
    if(!session().backend().writable())return -EROFS;
    return beginMutation(session());
}

int resizeFile(const char* path,off_t size,struct fuse_file_info* info) {
    if(size<0)return -EINVAL;
    MountNode node;
    if(!session().backend().lookup(path,node))return -ENOENT;
    if(node.directory)return -EISDIR;
    if(!session().backend().writable())return -EROFS;
    if(const int failure=beginMutation(session());failure!=0)return failure;
    if(static_cast<std::uint64_t>(size)==node.size)return 0;
    std::string error;
    if(!session().backend().truncate(path,static_cast<std::size_t>(size),error))
        return mutationFailure(session(),error);
    if(fileHandle(info)) {
        markFileHandleDirty(info);
        return 0;
    }
    return commitSessionFile(session(),path);
}

int ezfa3fsGetattr(const char* path,struct stat* status,struct fuse_file_info*) {
    std::lock_guard<std::mutex> lock(session().mutex());MountNode info;if(!session().backend().lookup(path,info))return -ENOENT;
    std::memset(status,0,sizeof(*status));status->st_mode=(info.directory?S_IFDIR|0755:S_IFREG|0644);
    status->st_nlink=info.directory?2:1;status->st_size=static_cast<off_t>(info.size);status->st_uid=getuid();status->st_gid=getgid();
    const auto timestamp=static_cast<time_t>(info.modified_time?info.modified_time:session().mountedAt());
    status->st_atime=timestamp;status->st_mtime=timestamp;status->st_ctime=timestamp;return 0;
}
int ezfa3fsReaddir(const char* path,void* buffer,fuse_fill_dir_t filler,off_t,struct fuse_file_info*,enum fuse_readdir_flags) {
    std::lock_guard<std::mutex> lock(session().mutex());std::vector<std::string> children;
    if(!session().backend().list(path,children))return -ENOENT;
    filler(buffer,".",nullptr,0,FUSE_FILL_DIR_DEFAULTS);filler(buffer,"..",nullptr,0,FUSE_FILL_DIR_DEFAULTS);
    for(const auto& child:children)if(filler(buffer,child.c_str(),nullptr,0,FUSE_FILL_DIR_DEFAULTS)!=0)break;return 0;
}
int ezfa3fsOpen(const char* path,struct fuse_file_info* info) {
    std::lock_guard<std::mutex> lock(session().mutex());MountNode node;if(!session().backend().lookup(path,node)||node.directory)return -ENOENT;
    const bool writable=(info->flags&O_ACCMODE)!=O_RDONLY;
    if(writable) {
        if(!session().backend().writable())return -EROFS;
        if(const int failure=beginMutation(session());failure!=0)return failure;
    }
    installFileHandle(info,writable,false);
    return 0;
}
int ezfa3fsChmod(const char* path,mode_t,struct fuse_file_info*) {
    std::lock_guard<std::mutex> lock(session().mutex());
    // EZFA3FS formats do not store Unix permission bits. Accept chmod on a
    // writable mount so standard copy tools can finish, while getattr keeps
    // exposing the filesystem's fixed 0644/0755 policy.
    return metadataMutation(path);
}
int ezfa3fsChown(const char* path,uid_t,gid_t,struct fuse_file_info*) {
    std::lock_guard<std::mutex> lock(session().mutex());return metadataMutation(path);
}
int ezfa3fsUtimens(const char* path,const struct timespec[2],struct fuse_file_info*) {
    std::lock_guard<std::mutex> lock(session().mutex());return metadataMutation(path);
}
#if defined(__APPLE__)
int ezfa3fsSetattr(const char* path,struct fuse_darwin_attr* attributes,
                 int to_set,struct fuse_file_info* info) {
    std::lock_guard<std::mutex> lock(session().mutex());
    if((to_set&FUSE_SET_ATTR_SIZE)!=0) {
        if(!attributes)return -EINVAL;
        return resizeFile(path,attributes->size,info);
    }
    // macFUSE combines chmod, chown, timestamps, and BSD flags in this
    // Darwin-specific callback. EZFA3FS exposes those as fixed metadata, so
    // acknowledge them without creating another cartridge transaction.
    return metadataMutation(path);
}
int ezfa3fsChflags(const char* path,struct fuse_file_info*,unsigned int) {
    std::lock_guard<std::mutex> lock(session().mutex());
    return metadataMutation(path);
}
#endif
int ezfa3fsAccess(const char* path,int) {
    std::lock_guard<std::mutex> lock(session().mutex());MountNode node;
    return session().backend().lookup(path,node)?0:-ENOENT;
}
int ezfa3fsRead(const char* path,char* buffer,size_t size,off_t offset,struct fuse_file_info*) {
    if(offset<0)return -EINVAL;std::lock_guard<std::mutex> lock(session().mutex());std::vector<std::uint8_t> bytes;
    if(!session().backend().read(path,static_cast<std::size_t>(offset),size,bytes))return -ENOENT;
    std::memcpy(buffer,bytes.data(),bytes.size());return static_cast<int>(bytes.size());
}
int ezfa3fsMkdir(const char* path,mode_t) {std::lock_guard<std::mutex> lock(session().mutex());if(const int failure=beginMutation(session());failure!=0)return failure;std::string error;
    const bool changed=session().backend().createDirectory(path,error);return finishMutation(changed,error);}
int ezfa3fsCreate(const char* path,mode_t,struct fuse_file_info* info) {std::lock_guard<std::mutex> lock(session().mutex());if(const int failure=beginMutation(session());failure!=0)return failure;std::string error;
    const bool changed=session().backend().createFile(path,error);if(changed)installFileHandle(info,true,false);return changed?0:mutationFailure(session(),error);}
int ezfa3fsWrite(const char* path,const char* buffer,size_t size,off_t offset,struct fuse_file_info* info) {
    if(offset<0)return -EINVAL;std::lock_guard<std::mutex> lock(session().mutex());if(const int failure=beginMutation(session());failure!=0)return failure;std::string error;
    if(!session().backend().write(path,static_cast<std::size_t>(offset),reinterpret_cast<const std::uint8_t*>(buffer),size,error))return mutationFailure(session(),error);
    markFileHandleDirty(info);return static_cast<int>(size);
}
int ezfa3fsTruncate(const char* path,off_t size,struct fuse_file_info* info) {
    std::lock_guard<std::mutex> lock(session().mutex());
    return resizeFile(path,size,info);
}
int ezfa3fsUnlink(const char* path) {std::lock_guard<std::mutex> lock(session().mutex());if(const int failure=beginMutation(session());failure!=0)return failure;std::string error;
    const bool changed=session().backend().removeFile(path,error);return finishMutation(changed,error);}
int ezfa3fsRmdir(const char* path) {std::lock_guard<std::mutex> lock(session().mutex());if(const int failure=beginMutation(session());failure!=0)return failure;std::string error;
    const bool changed=session().backend().removeDirectory(path,error);return finishMutation(changed,error);}
int ezfa3fsRename(const char* from,const char* to,unsigned flags) {
#if defined(RENAME_NOREPLACE)
    if((flags&~static_cast<unsigned>(RENAME_NOREPLACE))!=0)return -ENOTSUP;
#else
    if(flags!=0)return -ENOTSUP;
#endif
    std::lock_guard<std::mutex> lock(session().mutex());if(const int failure=beginMutation(session());failure!=0)return failure;std::string error;
    const bool changed=session().backend().rename(from,to,error);return finishMutation(changed,error);}
int ezfa3fsFlush(const char*,struct fuse_file_info*) {
    std::lock_guard<std::mutex> lock(session().mutex());
    // macFUSE may flush an open file repeatedly while a copy is still
    // growing. Committing here rewrites the complete copy-on-write extent and
    // both metadata generations for every partial size. Keep flush as a
    // health check; the final release remains the file durability boundary.
    return beginMutation(session());
}
int ezfa3fsFsync(const char*,int,struct fuse_file_info*) {std::lock_guard<std::mutex> lock(session().mutex());return beginMutation(session());}
int ezfa3fsRelease(const char* path,struct fuse_file_info* info) {std::lock_guard<std::mutex> lock(session().mutex());
    const auto handle=takeFileHandle(info);
    if(handle&&!handle->dirty)return beginMutation(session());
    return commitSessionFile(session(),path);
}
int ezfa3fsSetxattr(const char* path,const char*,const char*,size_t,int) {
    std::lock_guard<std::mutex> lock(session().mutex());MountNode node;
    if(!session().backend().lookup(path,node))return -ENOENT;
    if(!session().backend().writable())return -EROFS;
    // EZFA3FS does not persist extended attributes. Accept and discard them so
    // macOS copy tools can complete after the file data has been committed.
    return beginMutation(session());
}
int ezfa3fsGetxattr(const char* path,const char*,char*,size_t) {
    std::lock_guard<std::mutex> lock(session().mutex());MountNode node;
    return session().backend().lookup(path,node)?-ENODATA:-ENOENT;
}
int ezfa3fsListxattr(const char* path,char*,size_t) {
    std::lock_guard<std::mutex> lock(session().mutex());MountNode node;
    return session().backend().lookup(path,node)?0:-ENOENT;
}
int ezfa3fsRemovexattr(const char* path,const char*) {
    std::lock_guard<std::mutex> lock(session().mutex());MountNode node;
    if(!session().backend().lookup(path,node))return -ENOENT;
    if(!session().backend().writable())return -EROFS;
    return beginMutation(session());
}
void ezfa3fsDestroy(void* private_data) {auto* mounted=static_cast<MountSession*>(private_data);std::lock_guard<std::mutex> lock(mounted->mutex());if(!mounted->backend().writable())return;if(mounted->commitFailed())return;std::string error;if(!mounted->commit(error))std::cerr<<"EZFA3FS commit failed: "<<error<<'\n';}
int ezfa3fsStatfs(const char*,struct statvfs* status) {std::lock_guard<std::mutex> lock(session().mutex());
    std::memset(status,0,sizeof(*status));status->f_bsize=4096;status->f_frsize=4096;
    status->f_blocks=session().backend().capacityBytes()/4096;status->f_bfree=session().backend().freeBytes()/4096;
    status->f_bavail=status->f_bfree;status->f_files=session().backend().entryCount()+1024;
    status->f_ffree=1024;status->f_favail=1024;status->f_namemax=255;return 0;}

fuse_operations operations() {fuse_operations value{};value.getattr=ezfa3fsGetattr;value.readdir=ezfa3fsReaddir;value.open=ezfa3fsOpen;
    value.read=ezfa3fsRead;value.chmod=ezfa3fsChmod;value.chown=ezfa3fsChown;value.utimens=ezfa3fsUtimens;value.access=ezfa3fsAccess;
#if defined(__APPLE__)
    value.setattr=ezfa3fsSetattr;value.chflags=ezfa3fsChflags;
#endif
    value.mkdir=ezfa3fsMkdir;value.create=ezfa3fsCreate;value.write=ezfa3fsWrite;value.truncate=ezfa3fsTruncate;
    value.unlink=ezfa3fsUnlink;value.rmdir=ezfa3fsRmdir;value.rename=ezfa3fsRename;value.flush=ezfa3fsFlush;value.fsync=ezfa3fsFsync;
    value.release=ezfa3fsRelease;value.setxattr=ezfa3fsSetxattr;value.getxattr=ezfa3fsGetxattr;value.listxattr=ezfa3fsListxattr;
    value.removexattr=ezfa3fsRemovexattr;value.destroy=ezfa3fsDestroy;value.statfs=ezfa3fsStatfs;return value;}
int runMount(MountSession& mounted,const std::string& mountpoint,
             bool foreground,const std::string& filesystem_name) {
    auto callbacks=operations();
    std::vector<std::string> arguments{"ezfa3fs","-o","fsname="+filesystem_name};
    if(!mounted.backend().writable()) {
        arguments.push_back("-o");
        arguments.push_back("ro");
    }
#if defined(__APPLE__)
    if(filesystem_name=="ezfa3fs-card") {
        arguments.push_back("-o");arguments.push_back("volname=EZFA3FS Cartridge");
    } else if(filesystem_name=="ezfa3fs-card") {
        arguments.push_back("-o");arguments.push_back("volname=EZFA3FS Cartridge");
        arguments.push_back("-o");arguments.push_back("daemon_timeout=600");
    }
#endif
    if(foreground)arguments.push_back("-f");arguments.push_back(mountpoint);
    std::vector<char*> argv;for(auto& argument:arguments)argv.push_back(argument.data());
    return fuse_main(static_cast<int>(argv.size()),argv.data(),&callbacks,&mounted);
}
} // namespace

int mountLiveCartridge(const std::string& mountpoint,bool foreground,
                       bool verify_referenced_data) {
    if(!foreground) {
        std::cerr<<"A writable live cartridge mount requires --foreground so the USB session is not inherited across FUSE daemonization.\n";
        return 1;
    }
    LiveCartridgeSession cartridge;std::string error;
    const auto progress=[displayed=101u](std::size_t completed,
                                         std::size_t total) mutable {
        const auto percent=total==0?100u:
            static_cast<unsigned>(completed*100/total);
        if(percent==displayed)return;
        displayed=percent;
        std::cerr<<'\r'<<"Verifying EZFA3FS cartridge: "
                 <<percent<<'%'<<std::flush;
        if(completed==total)std::cerr<<'\n';
    };
    if(!cartridge.open(error,verify_referenced_data,progress)){
        std::cerr<<error<<'\n';return 1;
    }
    const auto maintenance=[](live::MaintenanceAction action) {
        std::cerr<<"EZFA3FS automatic "
                 <<(action==live::MaintenanceAction::garbage_collection?
                    "garbage collection":"compaction")
                 <<" started; the current write will resume when it completes.\n";
    };
    std::unique_ptr<MountBackend> backend=
        std::make_unique<LiveMountBackend>(cartridge.filesystem(),maintenance);
#if defined(__APPLE__)
    backend=std::make_unique<FinderMetadataMountBackend>(std::move(backend));
#endif
    MountSession mounted(std::move(backend));
    const int result=runMount(mounted,mountpoint,foreground,"ezfa3fs-card");
    if(!cartridge.close(error)){
        std::cerr<<"Could not close live cartridge session: "<<error<<'\n';
        return 1;
    }
    return result;
}

int mountBackend(std::unique_ptr<MountBackend> backend,
                 const std::string& mountpoint,bool foreground,
                 const std::string& filesystem_name) {
    MountSession mounted(std::move(backend));
    return runMount(mounted,mountpoint,foreground,filesystem_name);
}
#else
int mountLiveCartridge(const std::string&,bool,bool) {
    std::cerr<<"FUSE 3 support was not available when ezfa3fs was built.\n";return 1;
}
int mountBackend(std::unique_ptr<MountBackend>,const std::string&,bool,
                 const std::string&) {
    std::cerr<<"FUSE 3 support was not available when ezfa3fs was built.\n";
    return 1;
}
#endif
} // namespace ezfa3fs
