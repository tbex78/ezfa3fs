#include "ez3fs/fuse_mount.hpp"
#include "ez3fs/archive.hpp"
#include "ez3fs/live_cartridge_session.hpp"
#include "ez3fs/live_mount_backend.hpp"
#include "ez3fs/mount_backend.hpp"
#include "ez3fs/mount_session.hpp"
#include "ez3fs/virtual_mount_backend.hpp"
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#if defined(EZ3FS_HAS_FUSE3)
#define FUSE_USE_VERSION 31
#define FUSE_DARWIN_ENABLE_EXTENSIONS 0
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdollar-in-identifier-extension"
#pragma clang diagnostic ignored "-Wgnu-anonymous-struct"
#pragma clang diagnostic ignored "-Wnested-anon-types"
#endif
#include <fuse3/fuse.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#include <unistd.h>
#endif

namespace ez3fs {
#if defined(EZ3FS_HAS_FUSE3)
namespace {
namespace fs=std::filesystem;

bool readImage(const fs::path& path,std::vector<std::uint8_t>& bytes) {
    std::ifstream input(path,std::ios::binary);if(!input)return false;
    bytes.assign(std::istreambuf_iterator<char>(input),{});return input.good()||input.eof();
}
MountSession& session(){return *static_cast<MountSession*>(fuse_get_context()->private_data);}
int mutationFailure(MountSession& value,const std::string& error) {
    if(value.shouldReportFailure(error))std::cerr<<"EZ3FS mutation failed: "<<error<<'\n';
    if(!value.backend().writable())return -EROFS;
    if(error.find("out of free blocks")!=std::string::npos)return -ENOSPC;
    if(error.find("not empty")!=std::string::npos)return -ENOTEMPTY;
    if(error.find("does not exist")!=std::string::npos)return -ENOENT;
    if(error.find("existing")!=std::string::npos)return -EEXIST;
    if(error.find("USB ")!=std::string::npos||error.find("cartridge")!=std::string::npos||
       error.find("readback")!=std::string::npos)return -EIO;
    return -EINVAL;
}
int beginMutation(MountSession& value) {
    std::string error;
    if(value.mutationAllowed(error))return 0;
    return mutationFailure(value,error);
}
int commitSession(MountSession& value) {
    std::string error;
    if(value.commit(error))return 0;
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

int ez3fsGetattr(const char* path,struct stat* status,struct fuse_file_info*) {
    std::lock_guard<std::mutex> lock(session().mutex());MountNode info;if(!session().backend().lookup(path,info))return -ENOENT;
    std::memset(status,0,sizeof(*status));status->st_mode=(info.directory?S_IFDIR|0755:S_IFREG|0644);
    status->st_nlink=info.directory?2:1;status->st_size=static_cast<off_t>(info.size);status->st_uid=getuid();status->st_gid=getgid();
    const auto timestamp=static_cast<time_t>(info.modified_time?info.modified_time:session().mountedAt());
    status->st_atime=timestamp;status->st_mtime=timestamp;status->st_ctime=timestamp;return 0;
}
int ez3fsReaddir(const char* path,void* buffer,fuse_fill_dir_t filler,off_t,struct fuse_file_info*,enum fuse_readdir_flags) {
    std::lock_guard<std::mutex> lock(session().mutex());std::vector<std::string> children;
    if(!session().backend().list(path,children))return -ENOENT;
    filler(buffer,".",nullptr,0,FUSE_FILL_DIR_DEFAULTS);filler(buffer,"..",nullptr,0,FUSE_FILL_DIR_DEFAULTS);
    for(const auto& child:children)if(filler(buffer,child.c_str(),nullptr,0,FUSE_FILL_DIR_DEFAULTS)!=0)break;return 0;
}
int ez3fsOpen(const char* path,struct fuse_file_info* info) {
    std::lock_guard<std::mutex> lock(session().mutex());MountNode node;if(!session().backend().lookup(path,node)||node.directory)return -ENOENT;
    if((info->flags&O_ACCMODE)!=O_RDONLY) {
        if(!session().backend().writable())return -EROFS;
        if(const int failure=beginMutation(session());failure!=0)return failure;
    }
    return 0;
}
int ez3fsChmod(const char* path,mode_t,struct fuse_file_info*) {
    std::lock_guard<std::mutex> lock(session().mutex());
    // EZ3FS formats do not store Unix permission bits. Accept chmod on a
    // writable mount so standard copy tools can finish, while getattr keeps
    // exposing the filesystem's fixed 0644/0755 policy.
    return metadataMutation(path);
}
int ez3fsChown(const char* path,uid_t,gid_t,struct fuse_file_info*) {
    std::lock_guard<std::mutex> lock(session().mutex());return metadataMutation(path);
}
int ez3fsUtimens(const char* path,const struct timespec[2],struct fuse_file_info*) {
    std::lock_guard<std::mutex> lock(session().mutex());return metadataMutation(path);
}
int ez3fsAccess(const char* path,int) {
    std::lock_guard<std::mutex> lock(session().mutex());MountNode node;
    return session().backend().lookup(path,node)?0:-ENOENT;
}
int ez3fsRead(const char* path,char* buffer,size_t size,off_t offset,struct fuse_file_info*) {
    if(offset<0)return -EINVAL;std::lock_guard<std::mutex> lock(session().mutex());std::vector<std::uint8_t> bytes;
    if(!session().backend().read(path,static_cast<std::size_t>(offset),size,bytes))return -ENOENT;
    std::memcpy(buffer,bytes.data(),bytes.size());return static_cast<int>(bytes.size());
}
int ez3fsMkdir(const char* path,mode_t) {std::lock_guard<std::mutex> lock(session().mutex());if(const int failure=beginMutation(session());failure!=0)return failure;std::string error;
    const bool changed=session().backend().createDirectory(path,error);return finishMutation(changed,error);}
int ez3fsCreate(const char* path,mode_t,struct fuse_file_info*) {std::lock_guard<std::mutex> lock(session().mutex());if(const int failure=beginMutation(session());failure!=0)return failure;std::string error;
    const bool changed=session().backend().createFile(path,error);return changed?0:mutationFailure(session(),error);}
int ez3fsWrite(const char* path,const char* buffer,size_t size,off_t offset,struct fuse_file_info*) {
    if(offset<0)return -EINVAL;std::lock_guard<std::mutex> lock(session().mutex());if(const int failure=beginMutation(session());failure!=0)return failure;std::string error;
    return session().backend().write(path,static_cast<std::size_t>(offset),reinterpret_cast<const std::uint8_t*>(buffer),size,error)?static_cast<int>(size):mutationFailure(session(),error);
}
int ez3fsTruncate(const char* path,off_t size,struct fuse_file_info*) {if(size<0)return -EINVAL;
    std::lock_guard<std::mutex> lock(session().mutex());if(const int failure=beginMutation(session());failure!=0)return failure;std::string error;
    const bool changed=session().backend().truncate(path,static_cast<std::size_t>(size),error);return changed?0:mutationFailure(session(),error);}
int ez3fsUnlink(const char* path) {std::lock_guard<std::mutex> lock(session().mutex());if(const int failure=beginMutation(session());failure!=0)return failure;std::string error;
    const bool changed=session().backend().removeFile(path,error);return finishMutation(changed,error);}
int ez3fsRmdir(const char* path) {std::lock_guard<std::mutex> lock(session().mutex());if(const int failure=beginMutation(session());failure!=0)return failure;std::string error;
    const bool changed=session().backend().removeDirectory(path,error);return finishMutation(changed,error);}
int ez3fsRename(const char* from,const char* to,unsigned flags) {if(flags!=0)return -EINVAL;
    std::lock_guard<std::mutex> lock(session().mutex());if(const int failure=beginMutation(session());failure!=0)return failure;std::string error;
    const bool changed=session().backend().rename(from,to,error);return finishMutation(changed,error);}
int ez3fsFlush(const char*,struct fuse_file_info*) {std::lock_guard<std::mutex> lock(session().mutex());return commitSession(session());}
int ez3fsFsync(const char*,int,struct fuse_file_info*) {std::lock_guard<std::mutex> lock(session().mutex());return commitSession(session());}
int ez3fsRelease(const char*,struct fuse_file_info*) {std::lock_guard<std::mutex> lock(session().mutex());return commitSession(session());}
int ez3fsSetxattr(const char* path,const char*,const char*,size_t,int) {
    std::lock_guard<std::mutex> lock(session().mutex());MountNode node;
    if(!session().backend().lookup(path,node))return -ENOENT;
    if(!session().backend().writable())return -EROFS;
    // EZ3FS does not persist extended attributes. Accept and discard them so
    // macOS copy tools can complete after the file data has been committed.
    return beginMutation(session());
}
int ez3fsGetxattr(const char* path,const char*,char*,size_t) {
    std::lock_guard<std::mutex> lock(session().mutex());MountNode node;
    return session().backend().lookup(path,node)?-ENODATA:-ENOENT;
}
int ez3fsListxattr(const char* path,char*,size_t) {
    std::lock_guard<std::mutex> lock(session().mutex());MountNode node;
    return session().backend().lookup(path,node)?0:-ENOENT;
}
int ez3fsRemovexattr(const char* path,const char*) {
    std::lock_guard<std::mutex> lock(session().mutex());MountNode node;
    if(!session().backend().lookup(path,node))return -ENOENT;
    if(!session().backend().writable())return -EROFS;
    return beginMutation(session());
}
void ez3fsDestroy(void* private_data) {auto* mounted=static_cast<MountSession*>(private_data);std::lock_guard<std::mutex> lock(mounted->mutex());if(mounted->commitFailed())return;std::string error;if(!mounted->commit(error))std::cerr<<"EZ3FS commit failed: "<<error<<'\n';}
int ez3fsStatfs(const char*,struct statvfs* status) {std::lock_guard<std::mutex> lock(session().mutex());
    std::memset(status,0,sizeof(*status));status->f_bsize=4096;status->f_frsize=4096;
    status->f_blocks=session().backend().capacityBytes()/4096;status->f_bfree=session().backend().freeBytes()/4096;
    status->f_bavail=status->f_bfree;status->f_files=session().backend().entryCount()+1024;
    status->f_ffree=1024;status->f_favail=1024;status->f_namemax=255;return 0;}

fuse_operations operations() {fuse_operations value{};value.getattr=ez3fsGetattr;value.readdir=ez3fsReaddir;value.open=ez3fsOpen;
    value.read=ez3fsRead;value.chmod=ez3fsChmod;value.chown=ez3fsChown;value.utimens=ez3fsUtimens;value.access=ez3fsAccess;
    value.mkdir=ez3fsMkdir;value.create=ez3fsCreate;value.write=ez3fsWrite;value.truncate=ez3fsTruncate;
    value.unlink=ez3fsUnlink;value.rmdir=ez3fsRmdir;value.rename=ez3fsRename;value.flush=ez3fsFlush;value.fsync=ez3fsFsync;
    value.release=ez3fsRelease;value.setxattr=ez3fsSetxattr;value.getxattr=ez3fsGetxattr;value.listxattr=ez3fsListxattr;
    value.removexattr=ez3fsRemovexattr;value.destroy=ez3fsDestroy;value.statfs=ez3fsStatfs;return value;}
int runMount(MountSession& mounted,const std::string& mountpoint,
             bool foreground,const std::string& filesystem_name) {
    auto callbacks=operations();
    std::vector<std::string> arguments{"ez3fs","-o","fsname="+filesystem_name};
#if defined(__APPLE__)
    if(filesystem_name=="ez3fs-card") {
        arguments.push_back("-o");arguments.push_back("volname=EZ3FS Cartridge");
    } else if(filesystem_name=="ezfa3fs-card") {
        arguments.push_back("-o");arguments.push_back("volname=EZFA3FS Cartridge");
        arguments.push_back("-o");arguments.push_back("daemon_timeout=600");
        arguments.push_back("-o");arguments.push_back("noappledouble");
        arguments.push_back("-o");arguments.push_back("noapplexattr");
    }
#endif
    if(foreground)arguments.push_back("-f");arguments.push_back(mountpoint);
    std::vector<char*> argv;for(auto& argument:arguments)argv.push_back(argument.data());
    return fuse_main(static_cast<int>(argv.size()),argv.data(),&callbacks,&mounted);
}
} // namespace

int mountImage(const std::string& image,const std::string& mountpoint,bool writable,bool foreground) {
    std::error_code path_error;const auto absolute_image=fs::absolute(image,path_error);
    if(path_error){std::cerr<<"Could not resolve image path: "<<path_error.message()<<'\n';return 1;}
    std::vector<std::uint8_t> bytes;if(!readImage(absolute_image,bytes)){std::cerr<<"Could not read image: "<<absolute_image<<'\n';return 1;}
    Archive archive;std::string error;if(!archive.open(std::move(bytes),error)||!archive.verify(error)){std::cerr<<error<<'\n';return 1;}
    std::optional<fs::path> commit_path;if(writable)commit_path=absolute_image;
    MountSession mounted(std::make_unique<VirtualMountBackend>(archive.contents(),writable,commit_path));
    return runMount(mounted,mountpoint,foreground,"ez3fs");
}
int mountArchive(const Archive& archive,const std::string& mountpoint,
                 bool foreground,const std::string& filesystem_name) {
    std::string error;if(!archive.verify(error)){std::cerr<<error<<'\n';return 1;}
    MountSession mounted(std::make_unique<VirtualMountBackend>(archive.contents(),false));
    return runMount(mounted,mountpoint,foreground,filesystem_name);
}
int mountLiveContents(const std::vector<InputFile>& contents,const std::string& mountpoint,bool foreground) {
    MountSession mounted(std::make_unique<VirtualMountBackend>(contents,false));
    return runMount(mounted,mountpoint,foreground,"ezfa3fs");
}
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
    MountSession mounted(std::make_unique<LiveMountBackend>(cartridge.filesystem(),maintenance));
    const int result=runMount(mounted,mountpoint,foreground,"ezfa3fs-card");
    if(!cartridge.close(error)){std::cerr<<"Could not close live cartridge session: "<<error<<'\n';return 1;}
    return result;
}
#else
int mountImage(const std::string&,const std::string&,bool,bool) {
    std::cerr<<"FUSE 3 support was not available when ez3fs was built.\n";return 1;
}
int mountArchive(const Archive&,const std::string&,bool,const std::string&) {
    std::cerr<<"FUSE 3 support was not available when ez3fs was built.\n";return 1;
}
int mountLiveContents(const std::vector<InputFile>&,const std::string&,bool) {
    std::cerr<<"FUSE 3 support was not available when ez3fs was built.\n";return 1;
}
int mountLiveCartridge(const std::string&,bool,bool) {
    std::cerr<<"FUSE 3 support was not available when ez3fs was built.\n";return 1;
}
#endif
} // namespace ez3fs
