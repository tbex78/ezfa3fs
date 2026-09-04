#include "ez3fs/fuse_mount.hpp"
#include "ez3fs/archive.hpp"
#include "ez3fs/virtual_filesystem.hpp"
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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
bool writeImage(const fs::path& path,const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path,std::ios::binary|std::ios::trunc);if(!output)return false;
    output.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));return output.good();
}

class MountSession final {
public:
    MountSession(fs::path image,std::vector<InputFile> contents,bool writable)
        :image_(std::move(image)),filesystem_(std::move(contents),writable) {}
    explicit MountSession(std::vector<InputFile> contents)
        :filesystem_(std::move(contents),false) {}
    VirtualFilesystem& filesystem() noexcept{return filesystem_;}
    std::mutex& mutex() noexcept{return mutex_;}
    bool commit() {
        if(!filesystem_.dirty())return true;
        if(!image_){std::cerr<<"EZ3FS in-memory mount cannot be committed.\n";return false;}
        ArchiveImage built;std::string error;
        if(!ImageBuilder{}.build(filesystem_.contents(),built,error)){std::cerr<<"EZ3FS commit failed: "<<error<<'\n';return false;}
        fs::path temporary=*image_;temporary += ".fuse.tmp";
        if(fs::exists(temporary)||!writeImage(temporary,built.bytes)){std::cerr<<"EZ3FS commit could not create "<<temporary<<'\n';return false;}
        std::error_code ec;fs::rename(temporary,*image_,ec);
        if(ec){fs::remove(temporary);std::cerr<<"EZ3FS commit could not replace image: "<<ec.message()<<'\n';return false;}
        filesystem_.markClean();return true;
    }
private:
    std::optional<fs::path> image_;
    VirtualFilesystem filesystem_;
    std::mutex mutex_;
};

MountSession& session(){return *static_cast<MountSession*>(fuse_get_context()->private_data);}
int mutationFailure(MountSession& value){return value.filesystem().writable()?-EINVAL:-EROFS;}
int finishMutation(bool changed) {
    if(!changed)return mutationFailure(session());
    return session().commit()?0:-EIO;
}

int ez3fsGetattr(const char* path,struct stat* status,struct fuse_file_info*) {
    std::lock_guard<std::mutex> lock(session().mutex());NodeInfo info;if(!session().filesystem().lookup(path,info))return -ENOENT;
    std::memset(status,0,sizeof(*status));status->st_mode=(info.directory?S_IFDIR|0755:S_IFREG|0644);
    status->st_nlink=info.directory?2:1;status->st_size=static_cast<off_t>(info.size);status->st_uid=getuid();status->st_gid=getgid();return 0;
}
int ez3fsReaddir(const char* path,void* buffer,fuse_fill_dir_t filler,off_t,struct fuse_file_info*,enum fuse_readdir_flags) {
    std::lock_guard<std::mutex> lock(session().mutex());std::vector<std::string> children;
    if(!session().filesystem().list(path,children))return -ENOENT;
    filler(buffer,".",nullptr,0,FUSE_FILL_DIR_DEFAULTS);filler(buffer,"..",nullptr,0,FUSE_FILL_DIR_DEFAULTS);
    for(const auto& child:children)if(filler(buffer,child.c_str(),nullptr,0,FUSE_FILL_DIR_DEFAULTS)!=0)break;return 0;
}
int ez3fsOpen(const char* path,struct fuse_file_info* info) {
    std::lock_guard<std::mutex> lock(session().mutex());NodeInfo node;if(!session().filesystem().lookup(path,node)||node.directory)return -ENOENT;
    if((info->flags&O_ACCMODE)!=O_RDONLY&&!session().filesystem().writable())return -EROFS;return 0;
}
int ez3fsRead(const char* path,char* buffer,size_t size,off_t offset,struct fuse_file_info*) {
    if(offset<0)return -EINVAL;std::lock_guard<std::mutex> lock(session().mutex());std::vector<std::uint8_t> bytes;
    if(!session().filesystem().read(path,static_cast<std::size_t>(offset),size,bytes))return -ENOENT;
    std::memcpy(buffer,bytes.data(),bytes.size());return static_cast<int>(bytes.size());
}
int ez3fsMkdir(const char* path,mode_t) {std::lock_guard<std::mutex> lock(session().mutex());std::string error;
    return finishMutation(session().filesystem().createDirectory(path,error));}
int ez3fsCreate(const char* path,mode_t,struct fuse_file_info*) {std::lock_guard<std::mutex> lock(session().mutex());std::string error;
    return finishMutation(session().filesystem().createFile(path,error));}
int ez3fsWrite(const char* path,const char* buffer,size_t size,off_t offset,struct fuse_file_info*) {
    if(offset<0)return -EINVAL;std::lock_guard<std::mutex> lock(session().mutex());std::string error;
    return session().filesystem().write(path,static_cast<std::size_t>(offset),reinterpret_cast<const std::uint8_t*>(buffer),size,error)?static_cast<int>(size):mutationFailure(session());
}
int ez3fsTruncate(const char* path,off_t size,struct fuse_file_info*) {if(size<0)return -EINVAL;
    std::lock_guard<std::mutex> lock(session().mutex());std::string error;
    return finishMutation(session().filesystem().truncate(path,static_cast<std::size_t>(size),error));}
int ez3fsUnlink(const char* path) {std::lock_guard<std::mutex> lock(session().mutex());std::string error;
    return finishMutation(session().filesystem().removeFile(path,error));}
int ez3fsRmdir(const char* path) {std::lock_guard<std::mutex> lock(session().mutex());std::string error;
    return finishMutation(session().filesystem().removeDirectory(path,error));}
int ez3fsRename(const char* from,const char* to,unsigned flags) {if(flags!=0)return -EINVAL;
    std::lock_guard<std::mutex> lock(session().mutex());std::string error;
    return finishMutation(session().filesystem().rename(from,to,error));}
int ez3fsFlush(const char*,struct fuse_file_info*) {std::lock_guard<std::mutex> lock(session().mutex());return session().commit()?0:-EIO;}
int ez3fsFsync(const char*,int,struct fuse_file_info*) {std::lock_guard<std::mutex> lock(session().mutex());return session().commit()?0:-EIO;}
void ez3fsDestroy(void* private_data) {auto* mounted=static_cast<MountSession*>(private_data);std::lock_guard<std::mutex> lock(mounted->mutex());mounted->commit();}
int ez3fsStatfs(const char*,struct statvfs* status) {std::lock_guard<std::mutex> lock(session().mutex());
    ArchiveImage built;std::string error;if(!ImageBuilder{}.build(session().filesystem().contents(),built,error))return -EIO;
    std::memset(status,0,sizeof(*status));status->f_bsize=4096;status->f_frsize=4096;
    status->f_blocks=ImageBuilder::cartridge_capacity/4096;status->f_bfree=(ImageBuilder::cartridge_capacity-built.bytes.size())/4096;
    status->f_bavail=status->f_bfree;status->f_files=session().filesystem().contents().size()+1024;
    status->f_ffree=1024;status->f_favail=1024;status->f_namemax=255;return 0;}

fuse_operations operations() {fuse_operations value{};value.getattr=ez3fsGetattr;value.readdir=ez3fsReaddir;value.open=ez3fsOpen;
    value.read=ez3fsRead;value.mkdir=ez3fsMkdir;value.create=ez3fsCreate;value.write=ez3fsWrite;value.truncate=ez3fsTruncate;
    value.unlink=ez3fsUnlink;value.rmdir=ez3fsRmdir;value.rename=ez3fsRename;value.flush=ez3fsFlush;value.fsync=ez3fsFsync;
    value.destroy=ez3fsDestroy;value.statfs=ez3fsStatfs;return value;}
int runMount(MountSession& mounted,const std::string& mountpoint,
             bool foreground,const std::string& filesystem_name) {
    auto callbacks=operations();
    std::vector<std::string> arguments{"ez3fs","-o","fsname="+filesystem_name};
#if defined(__APPLE__)
    if(filesystem_name=="ez3fs-card") {
        arguments.push_back("-o");arguments.push_back("volname=EZ3FS Cartridge");
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
    MountSession mounted(absolute_image,archive.contents(),writable);
    return runMount(mounted,mountpoint,foreground,"ez3fs");
}
int mountArchive(const Archive& archive,const std::string& mountpoint,
                 bool foreground,const std::string& filesystem_name) {
    std::string error;if(!archive.verify(error)){std::cerr<<error<<'\n';return 1;}
    MountSession mounted(archive.contents());
    return runMount(mounted,mountpoint,foreground,filesystem_name);
}
#else
int mountImage(const std::string&,const std::string&,bool,bool) {
    std::cerr<<"FUSE 3 support was not available when ez3fs was built.\n";return 1;
}
int mountArchive(const Archive&,const std::string&,bool,const std::string&) {
    std::cerr<<"FUSE 3 support was not available when ez3fs was built.\n";return 1;
}
#endif
} // namespace ez3fs
