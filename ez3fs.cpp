#include "ez3fs/archive.hpp"
#include "ez3fs/archive_comparison.hpp"
#include "ez3fs/recovery_snapshot.hpp"
#include "ez3fs/byte_storage.hpp"
#include "ez3fs/cartridge_storage.hpp"
#include "ez3fs/cartridge_live_device.hpp"
#include "ez3fs/cartridge_programmer.hpp"
#include "ez3fs/fuse_mount.hpp"
#include "ez3fs/new_image_file.hpp"
#include "ez3fs/live_filesystem.hpp"
#include "ez3fs/live_cartridge_session.hpp"
#include "ez3fs/version.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <set>
#include <stdexcept>
namespace fs=std::filesystem;
namespace {
bool readFile(const fs::path& p,std::vector<std::uint8_t>& b) {
    std::ifstream in(p,std::ios::binary); if(!in)return false;
    b.assign(std::istreambuf_iterator<char>(in),{}); return in.good()||in.eof();
}
bool writeFile(const fs::path& p,const std::uint8_t* d,std::size_t n) {
    std::ofstream out(p,std::ios::binary|std::ios::trunc); if(!out)return false;
    out.write(reinterpret_cast<const char*>(d),static_cast<std::streamsize>(n)); return out.good();
}
std::uint64_t fileModifiedTime(const fs::path& path) {
    std::error_code error;const auto file_time=fs::last_write_time(path,error);
    if(error)return 0;
    const auto system_time=std::chrono::time_point_cast<std::chrono::seconds>(
        file_time-fs::file_time_type::clock::now()+std::chrono::system_clock::now());
    const auto seconds=system_time.time_since_epoch().count();
    return seconds>0?static_cast<std::uint64_t>(seconds):0;
}
void usage() {
#if defined(EZ3FS_LEGACY_CLI)
    std::cerr<<R"(Usage:
  ezfs-legacy create OUTPUT.ez3fs FILE...
  ezfs-legacy list IMAGE.ez3fs
  ezfs-legacy verify IMAGE.ez3fs
  ezfs-legacy extract IMAGE.ez3fs OUTPUT_DIRECTORY
  ezfs-legacy mkdir IMAGE.ez3fs DIRECTORY
  ezfs-legacy add IMAGE.ez3fs SOURCE_FILE DESTINATION
  ezfs-legacy rm IMAGE.ez3fs FILE
  ezfs-legacy card-info
  ezfs-legacy card-list
  ezfs-legacy card-verify
  ezfs-legacy card-extract OUTPUT_DIRECTORY
  ezfs-legacy card-pull OUTPUT.ez3fs
  ezfs-legacy card-write IMAGE.ez3fs
  ezfs-legacy card-status STAGING.ez3fs
  ezfs-legacy card-commit STAGING.ez3fs
  ezfs-legacy card-recover STAGING.ez3fs
  ezfs-legacy card-mount MOUNTPOINT [--foreground]
  ezfs-legacy card-mount MOUNTPOINT --writable STAGING.ez3fs [--foreground]
  ezfs-legacy --version
)";
#else
    std::cerr<<R"(Usage:
  ez3fs format IMAGE.ezfa3fs
  ez3fs list IMAGE.ezfa3fs
  ez3fs verify IMAGE.ezfa3fs
  ez3fs mkdir IMAGE.ezfa3fs DIRECTORY
  ez3fs put IMAGE.ezfa3fs SOURCE_FILE [DESTINATION]
  ez3fs get IMAGE.ezfa3fs FILE OUTPUT_FILE
  ez3fs rm IMAGE.ezfa3fs FILE
  ez3fs rmdir IMAGE.ezfa3fs DIRECTORY
  ez3fs gc IMAGE.ezfa3fs
  ez3fs compact IMAGE.ezfa3fs
  ez3fs space IMAGE.ezfa3fs
  ez3fs mount IMAGE.ezfa3fs MOUNTPOINT [--foreground]
  ez3fs card-mount MOUNTPOINT [--foreground]
  ez3fs card-mount MOUNTPOINT --writable --foreground [--verify]
  ez3fs card-pull IMAGE.ezfa3fs
  ez3fs card-write IMAGE.ezfa3fs
  ez3fs card-gc
  ez3fs card-compact
  ez3fs card-space
  ez3fs card-read-block BLOCK OUTPUT.bin
  ez3fs card-erase-plan BLOCK
  ez3fs card-erase-block BLOCK
  ez3fs card-program-block BLOCK INPUT.bin
  ez3fs --version
)";
#endif
}
bool confirm(const std::string& prompt) { std::cout<<prompt<<" [y/N]: "<<std::flush;std::string answer;std::getline(std::cin,answer);return answer=="y"||answer=="Y"||answer=="yes"||answer=="YES"; }
#if !defined(EZ3FS_LEGACY_CLI)
ez3fs::live::Filesystem::ScanProgress progressReporter(std::string label) {
    return [label=std::move(label),displayed=101u](std::size_t completed,
                                                   std::size_t total) mutable {
        const auto percent=total==0?100u:static_cast<unsigned>(completed*100/total);
        if(percent==displayed)return;
        displayed=percent;
        std::cerr<<'\r'<<label<<": "<<percent<<'%'<<std::flush;
        if(completed==total)std::cerr<<'\n';
    };
}
void reportAutomaticMaintenance(ez3fs::live::MaintenanceAction action) {
    std::cerr<<"EZFA3FS automatic "
             <<(action==ez3fs::live::MaintenanceAction::garbage_collection?
                "garbage collection":"compaction")
             <<" started; the write will resume when it completes.\n";
}
#endif
#if defined(EZ3FS_LEGACY_CLI)
bool loadArchive(const fs::path& p,ez3fs::Archive& a) {
    std::vector<std::uint8_t> b; if(!readFile(p,b)){std::cerr<<"Could not read image: "<<p<<'\n';return false;}
    std::string e; if(!a.open(std::move(b),e)){std::cerr<<e<<'\n';return false;} return true;
}
int createImage(int argc,char** argv) {
    if(argc<3){usage();return 1;} std::vector<ez3fs::InputFile> files; std::set<std::string> names;
    for(int i=3;i<argc;++i){const fs::path p(argv[i]);
        if(!fs::is_regular_file(p)){std::cerr<<"Input is not a regular file: "<<p<<'\n';return 1;}
        const auto name=p.filename().generic_string();
        if(!names.insert(name).second){std::cerr<<"Duplicate archive filename: "<<name<<'\n';return 1;}
        ez3fs::InputFile f{name,{}}; if(!readFile(p,f.bytes)){std::cerr<<"Could not read input: "<<p<<'\n';return 1;}
        f.modified_time=fileModifiedTime(p);
        files.push_back(std::move(f));
    }
    ez3fs::ArchiveImage image; std::string error;
    if(!ez3fs::ImageBuilder{}.build(files,image,error)){std::cerr<<error<<'\n';return 1;}
    if(!writeFile(argv[2],image.bytes.data(),image.bytes.size())){std::cerr<<"Could not write image: "<<argv[2]<<'\n';return 1;}
    std::cout<<"Created "<<argv[2]<<" with "<<image.entries.size()<<" entries, "<<image.bytes.size()<<" programmed bytes.\n"; return 0;
}
void printEntries(const ez3fs::Archive& a) {
    std::cout<<"EZ3FS "<<a.entries().size()<<" entries\n";for(const auto& e:a.entries())
        std::cout<<(e.directory?"directory ":"file      ")<<std::setw(10)<<e.size<<"  "<<e.name<<'\n';
}
int listImage(const char* p) { ez3fs::Archive a;if(!loadArchive(p,a))return 1;printEntries(a);return 0; }
int verifyArchive(const ez3fs::Archive& a) { std::string e;
    if(!a.verify(e)){std::cerr<<e<<'\n';return 1;}std::cout<<"Verified "<<a.entries().size()<<" entries.\n";return 0; }
int verifyImage(const char* p) { ez3fs::Archive a;if(!loadArchive(p,a))return 1;return verifyArchive(a); }
int extractArchive(const ez3fs::Archive& a,const fs::path& destination) { std::string error;
    if(!a.verify(error)){std::cerr<<error<<'\n';return 1;}std::error_code ec;fs::create_directories(destination,ec);
    if(ec){std::cerr<<"Could not create output directory.\n";return 1;}
    for(const auto& e:a.entries()){const auto out=destination/fs::path(e.name);
        if(e.directory){fs::create_directories(out,ec);if(ec){std::cerr<<"Could not create directory: "<<out<<'\n';return 1;}continue;}
        fs::create_directories(out.parent_path(),ec);
        if(ec||fs::exists(out)||!writeFile(out,a.image().data()+e.offset,static_cast<std::size_t>(e.size))){std::cerr<<"Could not safely extract: "<<out<<'\n';return 1;}}
    std::cout<<"Extracted "<<a.entries().size()<<" entries.\n";return 0;
}
int extractImage(const char* p,const fs::path& destination) { ez3fs::Archive a;if(!loadArchive(p,a))return 1;return extractArchive(a,destination); }

bool loadCartridge(ez3fs::CartridgeStorage& storage,ez3fs::Archive& archive) {
    std::string error;
    if(!storage.open(error)){std::cerr<<error<<'\n';return false;}
    if(!ez3fs::ArchiveLoader{}.load(storage,archive,error)){
        std::cerr<<error<<'\n';std::string close_error;
        if(!storage.close(close_error))std::cerr<<"Warning: "<<close_error<<'\n';
        return false;
    }
    return true;
}
bool closeCartridge(ez3fs::CartridgeStorage& storage) {
    std::string error;if(storage.close(error))return true;
    std::cerr<<"Could not finish cartridge read session: "<<error<<'\n';return false;
}
template<typename Action> int withCartridge(Action action) {
    ez3fs::CartridgeStorage storage;ez3fs::Archive archive;
    if(!loadCartridge(storage,archive))return 1;
    const int result=action(storage,archive);
    return closeCartridge(storage)?result:1;
}
int cardInfo() { return withCartridge([](const ez3fs::CartridgeStorage& storage,const ez3fs::Archive& archive){
    const auto id=storage.flashId();
    std::cout<<"EZ3 flash ID "<<std::hex<<std::setfill('0')
             <<std::setw(2)<<static_cast<unsigned>(id[0])<<' '
             <<std::setw(2)<<static_cast<unsigned>(id[1])<<' '
             <<std::setw(2)<<static_cast<unsigned>(id[2])<<' '
             <<std::setw(2)<<static_cast<unsigned>(id[3])<<std::dec<<'\n'
             <<"EZ3FS image bytes "<<archive.image().size()<<'\n'
             <<"Entries "<<archive.entries().size()<<'\n';return 0;
}); }
int cardList() { return withCartridge([](const ez3fs::CartridgeStorage&,const ez3fs::Archive& archive){printEntries(archive);return 0;}); }
int cardVerify() { return withCartridge([](const ez3fs::CartridgeStorage&,const ez3fs::Archive& archive){return verifyArchive(archive);}); }
int cardExtract(const fs::path& destination) { return withCartridge([&](const ez3fs::CartridgeStorage&,const ez3fs::Archive& archive){return extractArchive(archive,destination);}); }
int cardPull(const fs::path& destination) {
    ez3fs::NewImageFile output(destination);std::string error;
    if(!output.available(error)){std::cerr<<error<<'\n';return 1;}
    ez3fs::CartridgeStorage storage;ez3fs::Archive archive;
    if(!loadCartridge(storage,archive))return 1;
    if(!archive.verify(error)){
        std::cerr<<error<<'\n';std::string ignored;storage.close(ignored);return 1;}
    if(!closeCartridge(storage))return 1;
    if(!output.write(archive.image(),error)){std::cerr<<error<<'\n';return 1;}
    std::cout<<"Pulled and verified "<<archive.image().size()<<" bytes to "<<destination<<".\n";
    return 0;
}
int cardWrite(const fs::path& path) {
    ez3fs::Archive archive;if(!loadArchive(path,archive))return 1;std::string error;
    if(!archive.verify(error)){std::cerr<<error<<'\n';return 1;}
    std::cout<<"WARNING: this will erase the complete 32-MiB cartridge and program\n"
             <<archive.image().size()<<" bytes from "<<path<<".\n"
             <<"Confirm cartridge replacement"<<'\n';
    if(!confirm("Proceed")){std::cerr<<"Cancelled; cartridge was not modified.\n";return 1;}
    ez3fs::CartridgeProgrammer programmer;
    if(!programmer.programAndVerify(archive.image(),std::cout,error)){
        std::cerr<<"Cartridge programming failed: "<<error<<'\n';return 1;
    }
    std::cout<<"Programmed and verified the EZ3FS image successfully.\n";return 0;
}
bool compareWithCartridge(const fs::path& staging_path,ez3fs::Archive& staging,
                          ez3fs::ArchiveComparison& comparison) {
    if(!loadArchive(staging_path,staging))return false;std::string error;
    if(!staging.verify(error)){std::cerr<<error<<'\n';return false;}
    ez3fs::CartridgeStorage storage;ez3fs::Archive cartridge;
    if(!loadCartridge(storage,cartridge))return false;
    if(!cartridge.verify(error)){
        std::cerr<<error<<'\n';std::string ignored;storage.close(ignored);return false;}
    if(!closeCartridge(storage))return false;
    comparison=ez3fs::ArchiveComparator{}.compare(cartridge,staging);return true;
}
const char* changeLabel(ez3fs::ChangeKind kind) {
    switch(kind){case ez3fs::ChangeKind::added:return "added   ";
        case ez3fs::ChangeKind::modified:return "modified";
        case ez3fs::ChangeKind::deleted:return "deleted ";
        case ez3fs::ChangeKind::unchanged:return "unchanged";}
    return "unknown ";
}
void printComparison(const ez3fs::ArchiveComparison& comparison) {
    for(const auto& change:comparison.changes)
        if(change.kind!=ez3fs::ChangeKind::unchanged)
            std::cout<<changeLabel(change.kind)<<"  "<<change.path<<'\n';
    std::cout<<"Summary: "<<comparison.count(ez3fs::ChangeKind::added)<<" added, "
             <<comparison.count(ez3fs::ChangeKind::modified)<<" modified, "
             <<comparison.count(ez3fs::ChangeKind::deleted)<<" deleted, "
             <<comparison.count(ez3fs::ChangeKind::unchanged)<<" unchanged.\n"
             <<"Raw image: "<<(comparison.image_identical?"identical":"different")<<".\n";
}
int cardStatus(const fs::path& staging_path) {
    ez3fs::Archive staging;ez3fs::ArchiveComparison comparison;
    if(!compareWithCartridge(staging_path,staging,comparison))return 1;
    printComparison(comparison);return 0;
}
int cardCommit(const fs::path& staging_path) {
    ez3fs::Archive staging;ez3fs::ArchiveComparison comparison;
    if(!compareWithCartridge(staging_path,staging,comparison))return 1;
    printComparison(comparison);
    if(!comparison.requiresCommit()){
        std::cout<<"Cartridge already matches the staging image; nothing to commit.\n";return 0;}
    std::cout<<"WARNING: committing will erase and replace the complete cartridge.\n"
             <<"Confirm cartridge replacement"<<'\n';
    if(!confirm("Proceed")){
        std::cerr<<"Cancelled; cartridge was not modified.\n";return 1;}
    std::string error;ez3fs::CartridgeProgrammer programmer;
    if(!programmer.programAndVerify(staging.image(),std::cout,error)){
        std::cerr<<"Cartridge commit failed: "<<error<<'\n';return 1;}
    std::cout<<"Committed and verified "<<staging_path<<". The staging image was preserved.\n";
    return 0;
}
int cardRecover(const fs::path& staging_path) {
    ez3fs::RecoverySnapshot snapshot(staging_path);std::string error;
    if(!snapshot.exists(error)){
        if(error.empty())error="no recovery snapshot found for "+staging_path.string();
        std::cerr<<error<<'\n';return 1;
    }
    ez3fs::Archive recovery;if(!loadArchive(snapshot.recoveryPath(),recovery))return 1;
    if(!recovery.verify(error)){std::cerr<<error<<'\n';return 1;}
    std::cout<<"WARNING: this will replace the staging image with its recovery snapshot.\n"
             <<"Confirm staging image recovery"<<'\n';
    if(!confirm("Proceed")){
        std::cerr<<"Cancelled; staging image was not modified.\n";return 1;
    }
    if(!snapshot.restore(error)){std::cerr<<error<<'\n';return 1;}
    if(!snapshot.clear(error)){
        std::cerr<<"Recovered staging image, but could not clear snapshot: "<<error<<'\n';return 1;
    }
    std::cout<<"Recovered and verified "<<staging_path<<".\n";return 0;
}
int cardMount(int argc,char** argv) {
    if(argc<3||argc>6){usage();return 1;}bool foreground=false,writable=false;fs::path staging;
    for(int i=3;i<argc;++i){const std::string option(argv[i]);
        if(option=="--foreground")foreground=true;
        else if(option=="--writable"&&i+1<argc&&!writable){writable=true;staging=argv[++i];}
        else{std::cerr<<"Unknown or incomplete card-mount option: "<<option<<'\n';return 1;}}
    std::unique_ptr<ez3fs::NewImageFile> staged_output;
    if(writable){staged_output=std::make_unique<ez3fs::NewImageFile>(staging);std::string error;
        if(!staged_output->available(error)){std::cerr<<error<<'\n';return 1;}}
    ez3fs::CartridgeStorage storage;ez3fs::Archive archive;
    if(!loadCartridge(storage,archive))return 1;
    std::string error;if(!archive.verify(error)){
        std::cerr<<error<<'\n';std::string ignored;storage.close(ignored);return 1;}
    if(!closeCartridge(storage))return 1;
    if(writable){
        ez3fs::RecoverySnapshot recovery(staging);std::string recovery_error;
        if(!recovery.create(archive,recovery_error)){std::cerr<<recovery_error<<'\n';return 1;}
        if(!staged_output->write(archive.image(),error)){std::cerr<<error<<'\n';return 1;}
        std::cout<<"Cartridge changes will be staged in "<<staging<<".\n"
                 <<"The cartridge will not change automatically. After unmounting, run:\n"
                 <<"  ezfs-legacy card-write "<<staging<<'\n';
        const int result=ez3fs::mountImage(staging.string(),argv[2],true,foreground);
        if(result==0){if(!recovery.clear(recovery_error))std::cerr<<"Warning: "<<recovery_error<<'\n';}
        else std::cerr<<"Writable mount did not finish cleanly; recovery snapshot preserved at "<<recovery.recoveryPath()<<'\n';
        return result;
    }
    return ez3fs::mountArchive(archive,argv[2],foreground,"ez3fs-card");
}

bool replaceImage(const fs::path& path,const std::vector<ez3fs::InputFile>& contents) {
    ez3fs::ArchiveImage image;std::string error;
    if(!ez3fs::ImageBuilder{}.build(contents,image,error)){std::cerr<<error<<'\n';return false;}
    fs::path temporary=path;temporary += ".tmp";
    if(fs::exists(temporary)){std::cerr<<"Temporary image already exists: "<<temporary<<'\n';return false;}
    if(!writeFile(temporary,image.bytes.data(),image.bytes.size())){std::cerr<<"Could not write temporary image.\n";return false;}
    std::error_code ec;fs::rename(temporary,path,ec);
    if(ec) {
        fs::path backup=path;backup += ".bak";
        if(fs::exists(backup)){fs::remove(temporary);std::cerr<<"Backup image already exists: "<<backup<<'\n';return false;}
        ec.clear();fs::rename(path,backup,ec);
        if(ec){fs::remove(temporary);std::cerr<<"Could not preserve original image: "<<ec.message()<<'\n';return false;}
        fs::rename(temporary,path,ec);
        if(ec){std::error_code restore_error;fs::rename(backup,path,restore_error);
            std::cerr<<"Could not replace image: "<<ec.message()<<'\n';return false;}
        fs::remove(backup,ec);
        if(ec)std::cerr<<"Warning: could not remove backup image "<<backup<<".\n";
    }
    return true;
}

template<typename Edit> int editImage(const fs::path& path,Edit edit) {
    ez3fs::Archive archive;if(!loadArchive(path,archive))return 1;std::string error;
    if(!archive.verify(error)){std::cerr<<error<<'\n';return 1;}
    ez3fs::ArchiveEditor editor(archive.contents());
    if(!edit(editor,error)){std::cerr<<error<<'\n';return 1;}
    if(!replaceImage(path,editor.contents()))return 1;
    std::cout<<"Updated "<<path<<".\n";return 0;
}

int makeDirectory(const fs::path& image,const std::string& path) {
    return editImage(image,[&](ez3fs::ArchiveEditor& editor,std::string& error){return editor.createDirectory(path,error);});
}
int addFile(const fs::path& image,const fs::path& source,const std::string& destination) {
    if(!fs::is_regular_file(source)){std::cerr<<"Input is not a regular file: "<<source<<'\n';return 1;}
    std::vector<std::uint8_t> bytes;if(!readFile(source,bytes)){std::cerr<<"Could not read input: "<<source<<'\n';return 1;}
    const auto modified_time=fileModifiedTime(source);
    return editImage(image,[&](ez3fs::ArchiveEditor& editor,std::string& error){return editor.putFile(destination,std::move(bytes),error,modified_time);});
}
int removeFile(const fs::path& image,const std::string& path) {
    return editImage(image,[&](ez3fs::ArchiveEditor& editor,std::string& error){return editor.removeFile(path,error);});
}
#else
bool loadLive(const fs::path& path,ez3fs::live::NorFlash& flash,ez3fs::live::Filesystem& filesystem) {
    std::string error;
    if(!flash.load(path.string(),error)||!ez3fs::live::Filesystem::open(flash,filesystem,error)){std::cerr<<error<<'\n';return false;}
    return true;
}
int liveFormat(const fs::path& path) { ez3fs::live::NorFlash flash;std::string error;
    if(!ez3fs::live::Filesystem::format(flash,error)||!flash.save(path.string(),error)){std::cerr<<error<<'\n';return 1;}
    std::cout<<"Formatted EZFA3FS 2.0.0 image "<<path<<".\n";return 0;
}
void printLiveEntries(const ez3fs::live::Filesystem& filesystem) { std::cout<<"EZFA3FS generation "<<filesystem.generation()<<"\n";
    for(const auto& entry:filesystem.entries())std::cout<<(entry.directory?"directory ":"file      ")<<std::setw(10)<<entry.size<<"  "<<entry.name<<'\n';
    std::cout<<"Free blocks: "<<filesystem.freeBlocks()<<'\n'; }
int liveList(const fs::path& path) { ez3fs::live::NorFlash flash;ez3fs::live::Filesystem filesystem(flash);if(!loadLive(path,flash,filesystem))return 1;printLiveEntries(filesystem);return 0; }
int liveVerify(const fs::path& path) { ez3fs::live::NorFlash flash;ez3fs::live::Filesystem filesystem(flash);if(!loadLive(path,flash,filesystem))return 1;std::string error;
    if(!filesystem.verify(error)){std::cerr<<error<<'\n';return 1;}std::cout<<"Verified EZFA3FS generation "<<filesystem.generation()<<" with "<<filesystem.entries().size()<<" entries.\n";return 0; }
std::vector<ez3fs::InputFile> liveContents(const ez3fs::live::Filesystem& filesystem,std::string& error) {
    std::vector<ez3fs::InputFile> contents;
    for(const auto& entry:filesystem.entries()){ez3fs::InputFile file;file.name=entry.name;file.directory=entry.directory;file.modified_time=entry.modified_time;
        if(!entry.directory&&!filesystem.readFile(entry.name,file.bytes,error))return {};
        contents.push_back(std::move(file));}
    return contents;
}
int liveMount(const fs::path& image,const fs::path& mountpoint,bool foreground) {
    ez3fs::live::NorFlash flash;ez3fs::live::Filesystem filesystem(flash);if(!loadLive(image,flash,filesystem))return 1;std::string error;
    if(!filesystem.verify(error)){std::cerr<<error<<'\n';return 1;}auto contents=liveContents(filesystem,error);if(!error.empty()){std::cerr<<error<<'\n';return 1;}
    return ez3fs::mountLiveContents(contents,mountpoint.string(),foreground);
}
int liveCardMount(const fs::path& mountpoint,bool writable,bool foreground,
                  bool verify_referenced_data) {
    if(writable){
        if(!foreground){std::cerr<<"Writable cartridge mounting requires --foreground.\n";return 1;}
        std::cout<<"WARNING: changes made through this mount are written directly to the EZFA3FS cartridge.\n";
        if(!confirm("Proceed")){std::cerr<<"Cancelled; cartridge was not modified.\n";return 1;}
        return ez3fs::mountLiveCartridge(mountpoint.string(),foreground,
                                         verify_referenced_data);
    }
    if(verify_referenced_data){std::cerr<<"--verify is only available for writable live cartridge mounts.\n";return 1;}
    ez3fs::CartridgeStorage storage;std::string error;if(!storage.open(error)){std::cerr<<error<<'\n';return 1;}
    ez3fs::live::NorFlash flash;const bool loaded=flash.load(storage,error);std::string close_error;const bool closed=storage.close(close_error);
    if(!loaded||!closed){if(error.empty())error=close_error;std::cerr<<error<<'\n';return 1;}ez3fs::live::Filesystem filesystem(flash);
    if(!ez3fs::live::Filesystem::open(flash,filesystem,error)||!filesystem.verify(error)){std::cerr<<error<<'\n';return 1;}auto contents=liveContents(filesystem,error);if(!error.empty()){std::cerr<<error<<'\n';return 1;}
    return ez3fs::mountLiveContents(contents,mountpoint.string(),foreground);
}
int liveMkdir(const fs::path& image,const std::string& path) { ez3fs::live::NorFlash flash;ez3fs::live::Filesystem filesystem(flash);if(!loadLive(image,flash,filesystem))return 1;std::string error;
    if(!filesystem.createDirectory(path,error)||!flash.save(image.string(),error)){std::cerr<<error<<'\n';return 1;}return 0; }
int livePut(const fs::path& image,const fs::path& source,const std::string& destination) { if(!fs::is_regular_file(source)){std::cerr<<"Input is not a regular file: "<<source<<'\n';return 1;}
    std::vector<std::uint8_t> bytes;if(!readFile(source,bytes)){std::cerr<<"Could not read input: "<<source<<'\n';return 1;}ez3fs::live::NorFlash flash;ez3fs::live::Filesystem filesystem(flash);if(!loadLive(image,flash,filesystem))return 1;std::string error;
    if(!filesystem.putFile(destination,bytes,fileModifiedTime(source),error,
                           reportAutomaticMaintenance)||
       !flash.save(image.string(),error)){std::cerr<<error<<'\n';return 1;}return 0; }
int liveGet(const fs::path& image,const std::string& source,const fs::path& output) { ez3fs::live::NorFlash flash;ez3fs::live::Filesystem filesystem(flash);if(!loadLive(image,flash,filesystem))return 1;std::string error;std::vector<std::uint8_t> bytes;
    if(!filesystem.readFile(source,bytes,error)||!writeFile(output,bytes.data(),bytes.size())){if(error.empty())error="could not write output file";std::cerr<<error<<'\n';return 1;}return 0; }
int liveRemove(const fs::path& image,const std::string& path,bool directory) { ez3fs::live::NorFlash flash;ez3fs::live::Filesystem filesystem(flash);if(!loadLive(image,flash,filesystem))return 1;std::string error;
    if(!(directory?filesystem.removeDirectory(path,error):filesystem.removeFile(path,error))||!flash.save(image.string(),error)){std::cerr<<error<<'\n';return 1;}return 0; }
int liveGarbageCollect(const fs::path& image) {
    ez3fs::live::NorFlash flash;ez3fs::live::Filesystem filesystem(flash);
    if(!loadLive(image,flash,filesystem))return 1;std::string error;
    if(!filesystem.verify(error)){std::cerr<<error<<'\n';return 1;}
    std::size_t reclaimed=0;
    if(!filesystem.collectGarbage(reclaimed,error)||!flash.save(image.string(),error)){
        std::cerr<<error<<'\n';return 1;
    }
    std::cout<<"Reclaimed "<<reclaimed<<" EZFA3FS block(s) in "<<image<<".\n";return 0;
}
void printLiveCompaction(const ez3fs::live::CompactionReport& report) {
    std::cout<<"Garbage blocks reclaimed: "<<report.garbage_blocks_reclaimed<<'\n'
             <<"Files relocated:          "<<report.files_relocated<<'\n'
             <<"Data blocks relocated:    "<<report.blocks_relocated<<'\n';
}
int liveCompact(const fs::path& image) {
    ez3fs::live::NorFlash flash;ez3fs::live::Filesystem filesystem(flash);
    if(!loadLive(image,flash,filesystem))return 1;
    std::string error;ez3fs::live::CompactionReport report;
    if(!filesystem.verify(error)||!filesystem.compact(report,error)||
       !flash.save(image.string(),error)) {
        std::cerr<<error<<'\n';return 1;
    }
    std::cout<<"Compacted and verified "<<image<<".\n";
    printLiveCompaction(report);return 0;
}
void printLiveSpace(const ez3fs::live::Filesystem& filesystem,
                    const ez3fs::live::SpaceReport& report) {
    constexpr std::size_t kib_per_block=ez3fs::live::NorFlash::block_size/1024;
    const auto available=report.erased_blocks+report.reclaimable_blocks;
    std::cout<<"EZFA3FS generation "<<filesystem.generation()<<" space report\n"
             <<"Active data blocks:             "<<report.active_blocks<<'\n'
             <<"Erased reusable blocks:         "<<report.erased_blocks<<'\n'
             <<"Unreferenced programmed blocks: "<<report.reclaimable_blocks<<'\n'
             <<"Total potentially available:    "<<available<<'\n'
             <<"Largest erased extent:          "<<report.largest_erased_extent
             <<" blocks ("<<report.largest_erased_extent*kib_per_block<<" KiB)\n"
             <<"Largest extent after GC:         "<<report.largest_post_gc_extent
             <<" blocks ("<<report.largest_post_gc_extent*kib_per_block<<" KiB)\n"
             <<"Garbage collection recommended: "<<(report.reclaimable_blocks?"yes":"no")<<'\n'
             <<"Active-data fragmentation:      "
             <<(report.largest_post_gc_extent<available?"yes":"no")<<'\n';
}
int liveSpace(const fs::path& image) {
    ez3fs::live::NorFlash flash;ez3fs::live::Filesystem filesystem(flash);
    if(!loadLive(image,flash,filesystem))return 1;std::string error;ez3fs::live::SpaceReport report;
    if(!filesystem.inspectSpace(report,error)){std::cerr<<error<<'\n';return 1;}
    printLiveSpace(filesystem,report);return 0;
}
int liveCardSpace() {
    ez3fs::CartridgeStorage storage;std::string error;
    if(!storage.open(error)){std::cerr<<error<<'\n';return 1;}
    ez3fs::CartridgeLiveDevice device(storage);ez3fs::live::Filesystem filesystem(device);
    if(!ez3fs::live::Filesystem::open(device,filesystem,error)){
        std::string ignored;storage.close(ignored);std::cerr<<error<<'\n';return 1;
    }
    const auto progress=progressReporter("Inspecting EZFA3FS space");
    ez3fs::live::SpaceReport report;const bool inspected=filesystem.inspectSpace(report,error,progress);
    std::string close_error;const bool closed=storage.close(close_error);
    if(!inspected||!closed){if(error.empty())error=close_error;std::cerr<<error<<'\n';return 1;}
    printLiveSpace(filesystem,report);return 0;
}
int liveCardGarbageCollect() {
    std::cout<<"WARNING: this will erase unreferenced EZFA3FS data blocks on the cartridge.\n"
             <<"Unmount the cartridge before continuing.\n";
    if(!confirm("Proceed")){std::cerr<<"Cancelled; cartridge was not modified.\n";return 1;}
    ez3fs::LiveCartridgeSession cartridge;std::string error;
    if(!cartridge.open(error)){std::cerr<<error<<'\n';return 1;}
    const auto progress=progressReporter("Collecting EZFA3FS garbage");
    std::size_t reclaimed=0;const bool collected=cartridge.filesystem().collectGarbage(reclaimed,error,progress);
    std::string close_error;const bool closed=cartridge.close(close_error);
    if(!collected||!closed){if(error.empty())error=close_error;std::cerr<<error<<'\n';return 1;}
    std::cout<<"Reclaimed and verified "<<reclaimed<<" cartridge block(s).\n";return 0;
}
int liveCardCompact() {
    std::cout<<"WARNING: this will relocate EZFA3FS files and erase their old cartridge blocks.\n"
             <<"Unmount the cartridge before continuing.\n";
    if(!confirm("Proceed")){std::cerr<<"Cancelled; cartridge was not modified.\n";return 1;}
    ez3fs::LiveCartridgeSession cartridge;std::string error;
    if(!cartridge.open(error)){std::cerr<<error<<'\n';return 1;}
    ez3fs::live::CompactionReport report;
    const auto compacted=cartridge.filesystem().compact(
        report,error,progressReporter("Preparing EZFA3FS compaction"));
    std::string close_error;const bool closed=cartridge.close(close_error);
    if(!compacted||!closed){if(error.empty())error=close_error;std::cerr<<error<<'\n';return 1;}
    std::cout<<"Compacted and verified the EZFA3FS cartridge.\n";
    printLiveCompaction(report);return 0;
}
int liveCardPull(const fs::path& image) { ez3fs::CartridgeStorage storage;std::string error;
    if(!storage.open(error)){std::cerr<<error<<'\n';return 1;} ez3fs::live::NorFlash flash;
    const bool loaded=flash.load(storage,error);std::string close_error;const bool closed=storage.close(close_error);
    if(!loaded||!closed){if(error.empty())error=close_error;std::cerr<<error<<'\n';return 1;} ez3fs::live::Filesystem filesystem(flash);
    if(!ez3fs::live::Filesystem::open(flash,filesystem,error)||!filesystem.verify(error)||!flash.save(image.string(),error)){std::cerr<<error<<'\n';return 1;}
    std::cout<<"Pulled and verified EZFA3FS generation "<<filesystem.generation()<<" to "<<image<<".\n";return 0; }
int liveCardReadBlock(const std::string& block_text,const fs::path& output) {
    std::size_t block=0;try { std::size_t parsed=0;block=std::stoull(block_text,&parsed,0);if(parsed!=block_text.size())throw std::invalid_argument("block"); }
    catch(const std::exception&) { std::cerr<<"Invalid cartridge block: "<<block_text<<'\n';return 1; }
    if(block>=ez3fs::live::NorFlash::block_count){std::cerr<<"Cartridge block must be between 0 and 511.\n";return 1;}
    ez3fs::CartridgeStorage storage;std::string error;if(!storage.open(error)){std::cerr<<error<<'\n';return 1;}
    std::vector<std::uint8_t> bytes(ez3fs::live::NorFlash::block_size);const bool read=storage.read(block*bytes.size(),bytes.data(),bytes.size(),error);std::string close_error;const bool closed=storage.close(close_error);
    if(!read||!closed){if(error.empty())error=close_error;std::cerr<<error<<'\n';return 1;}
    if(!writeFile(output,bytes.data(),bytes.size())){std::cerr<<"Could not write block output: "<<output<<'\n';return 1;}
    std::cout<<"Read cartridge block "<<block<<" ("<<bytes.size()<<" bytes) to "<<output<<".\n";return 0;
}
int liveCardErasePlan(const std::string& block_text) {
    std::size_t block=0;try { std::size_t parsed=0;block=std::stoull(block_text,&parsed,0);if(parsed!=block_text.size())throw std::invalid_argument("block"); }
    catch(const std::exception&) { std::cerr<<"Invalid cartridge block: "<<block_text<<'\n';return 1; }
    if(block>=ez3fs::live::NorFlash::block_count){std::cerr<<"Cartridge block must be between 0 and 511.\n";return 1;}
    const std::uint64_t begin=block*ez3fs::live::NorFlash::block_size,end=begin+ez3fs::live::NorFlash::block_size;
    std::cout<<"Dry-run erase plan for logical block "<<block<<" (bytes 0x"<<std::hex<<begin<<"..0x"<<end-1<<std::dec<<")\n";
    for(unsigned window=0;window<4;++window) {
        std::cout<<"window "<<window<<":\n";
        std::vector<std::uint32_t> addresses;
        for(std::uint32_t address=0;address<=0x8000;address+=0x1000)addresses.push_back(address);
        for(std::uint32_t address=0x10000;address<=0x3F8000;address+=0x8000)addresses.push_back(address);
        if(window%2) {addresses.clear();for(std::uint32_t address=0;address<=0x3F8000;address+=0x8000)addresses.push_back(address);for(std::uint32_t address=0x3F9000;address<=0x3FF000;address+=0x1000)addresses.push_back(address);}
        for(const auto address:addresses) { const auto byte_offset=static_cast<std::uint64_t>(window)*0x800000u+static_cast<std::uint64_t>(address)*2u;
            if(byte_offset<end&&byte_offset+0x2000u>begin)std::cout<<"  0x96 word 0x"<<std::hex<<address<<" (byte 0x"<<byte_offset<<")\n"<<std::dec; }
    }
    std::cout<<"No erase command was sent.\n";return 0;
}
int liveCardEraseBlock(const std::string& block_text) {
    std::size_t block=0;try { std::size_t parsed=0;block=std::stoull(block_text,&parsed,0);if(parsed!=block_text.size())throw std::invalid_argument("block"); }
    catch(const std::exception&) { std::cerr<<"Invalid cartridge block: "<<block_text<<'\n';return 1; }
    if(block<2||block>=ez3fs::live::NorFlash::block_count){std::cerr<<"Only blocks 2 through 511 may be erased.\n";return 1;}
    std::cout<<"WARNING: this will erase live cartridge block "<<block<<" (64 KiB).\n";
    if(!confirm("Proceed")){std::cerr<<"Cancelled; cartridge was not modified.\n";return 1;}
    ez3fs::CartridgeProgrammer programmer;std::string error;if(!programmer.eraseLiveBlock(block,std::cout,error)){std::cerr<<error<<'\n';return 1;}return 0;
}
int liveCardProgramBlock(const std::string& block_text,const fs::path& input) {
    std::size_t block=0;try { std::size_t parsed=0;block=std::stoull(block_text,&parsed,0);if(parsed!=block_text.size())throw std::invalid_argument("block"); }
    catch(const std::exception&) { std::cerr<<"Invalid cartridge block: "<<block_text<<'\n';return 1; }
    if(block<2||block>=ez3fs::live::NorFlash::block_count){std::cerr<<"Only blocks 2 through 511 may be programmed.\n";return 1;}
    std::vector<std::uint8_t> bytes;if(!readFile(input,bytes)||bytes.size()!=ez3fs::live::NorFlash::block_size){std::cerr<<"Input must be exactly 64 KiB.\n";return 1;}
    std::cout<<"WARNING: this will program live cartridge block "<<block<<" (64 KiB).\n";
    if(!confirm("Proceed")){std::cerr<<"Cancelled; cartridge was not modified.\n";return 1;}
    ez3fs::CartridgeProgrammer programmer;std::string error;if(!programmer.programLiveBlock(block,bytes,std::cout,error)){std::cerr<<error<<'\n';return 1;}return 0;
}
int liveCardWrite(const fs::path& image) {
    std::vector<std::uint8_t> bytes;
    if(!readFile(image,bytes)){std::cerr<<"Could not read image: "<<image<<'\n';return 1;}
    ez3fs::live::NorFlash flash;std::string error;
    if(bytes.size()!=ez3fs::live::NorFlash::capacity||!flash.load(image.string(),error)){if(error.empty())error="live image must be exactly 32 MiB";std::cerr<<error<<'\n';return 1;}
    ez3fs::live::Filesystem filesystem(flash);
    if(!ez3fs::live::Filesystem::open(flash,filesystem,error)||!filesystem.verify(error)){std::cerr<<error<<'\n';return 1;}
    std::cout<<"WARNING: this will erase the complete 32-MiB cartridge and program\n"
             <<"the verified EZFA3FS image from "<<image<<".\n"
             <<"Confirm cartridge replacement"<<'\n';
    if(!confirm("Proceed")){std::cerr<<"Cancelled; cartridge was not modified.\n";return 1;}
    ez3fs::CartridgeProgrammer programmer;
    if(!programmer.programAndVerify(bytes,std::cout,error)){std::cerr<<"Cartridge programming failed: "<<error<<'\n';return 1;}
    std::cout<<"Programmed and verified the EZFA3FS image successfully.\n";return 0;
}
#endif
}
int main(int argc,char** argv) {
    if(argc==2&&std::string(argv[1])=="--version"){
#if defined(EZ3FS_LEGACY_CLI)
        std::cout<<"ezfs-legacy ";
#else
        std::cout<<"ez3fs ";
#endif
        std::cout<<ez3fs::project_version<<'\n';return 0;
    }
#if defined(EZ3FS_LEGACY_CLI)
    if(argc>=2&&std::string(argv[1])=="create")return createImage(argc,argv);
    if(argc==3&&std::string(argv[1])=="list")return listImage(argv[2]);
    if(argc==3&&std::string(argv[1])=="verify")return verifyImage(argv[2]);
    if(argc==4&&std::string(argv[1])=="extract")return extractImage(argv[2],argv[3]);
    if(argc==4&&std::string(argv[1])=="mkdir")return makeDirectory(argv[2],argv[3]);
    if(argc==5&&std::string(argv[1])=="add")return addFile(argv[2],argv[3],argv[4]);
    if(argc==4&&std::string(argv[1])=="rm")return removeFile(argv[2],argv[3]);
    if(argc==2&&std::string(argv[1])=="card-info")return cardInfo();
    if(argc==2&&std::string(argv[1])=="card-list")return cardList();
    if(argc==2&&std::string(argv[1])=="card-verify")return cardVerify();
    if(argc==3&&std::string(argv[1])=="card-extract")return cardExtract(argv[2]);
    if(argc==3&&std::string(argv[1])=="card-pull")return cardPull(argv[2]);
    if(argc==3&&std::string(argv[1])=="card-write")return cardWrite(argv[2]);
    if(argc==3&&std::string(argv[1])=="card-status")return cardStatus(argv[2]);
    if(argc==3&&std::string(argv[1])=="card-commit")return cardCommit(argv[2]);
    if(argc==3&&std::string(argv[1])=="card-recover")return cardRecover(argv[2]);
    if(argc>=2&&std::string(argv[1])=="card-mount")return cardMount(argc,argv);
#else
    if(argc==3&&std::string(argv[1])=="format")return liveFormat(argv[2]);
    if(argc==3&&std::string(argv[1])=="list")return liveList(argv[2]);
    if(argc==3&&std::string(argv[1])=="verify")return liveVerify(argv[2]);
    if(argc>=4&&std::string(argv[1])=="mount"){bool foreground=false;for(int i=4;i<argc;++i)if(std::string(argv[i])=="--foreground")foreground=true;return liveMount(argv[2],argv[3],foreground);}
    if(argc>=3&&std::string(argv[1])=="card-mount"){bool writable=false,foreground=false,verify_referenced_data=false;for(int i=3;i<argc;++i){const std::string option(argv[i]);if(option=="--writable")writable=true;else if(option=="--foreground")foreground=true;else if(option=="--verify")verify_referenced_data=true;else{std::cerr<<"Unknown card-mount option: "<<option<<'\n';return 1;}}return liveCardMount(argv[2],writable,foreground,verify_referenced_data);}
    if(argc==4&&std::string(argv[1])=="mkdir")return liveMkdir(argv[2],argv[3]);
    if((argc==4||argc==5)&&std::string(argv[1])=="put")return livePut(argv[2],argv[3],argc==5?argv[4]:argv[3]);
    if(argc==5&&std::string(argv[1])=="get")return liveGet(argv[2],argv[3],argv[4]);
    if(argc==4&&std::string(argv[1])=="rm")return liveRemove(argv[2],argv[3],false);
    if(argc==4&&std::string(argv[1])=="rmdir")return liveRemove(argv[2],argv[3],true);
    if(argc==3&&std::string(argv[1])=="gc")return liveGarbageCollect(argv[2]);
    if(argc==3&&std::string(argv[1])=="compact")return liveCompact(argv[2]);
    if(argc==3&&std::string(argv[1])=="space")return liveSpace(argv[2]);
    if(argc==3&&std::string(argv[1])=="card-pull")return liveCardPull(argv[2]);
    if(argc==4&&std::string(argv[1])=="card-read-block")return liveCardReadBlock(argv[2],argv[3]);
    if(argc==3&&std::string(argv[1])=="card-erase-plan")return liveCardErasePlan(argv[2]);
    if(argc==3&&std::string(argv[1])=="card-erase-block")return liveCardEraseBlock(argv[2]);
    if(argc==4&&std::string(argv[1])=="card-program-block")return liveCardProgramBlock(argv[2],argv[3]);
    if(argc==3&&std::string(argv[1])=="card-write")return liveCardWrite(argv[2]);
    if(argc==2&&std::string(argv[1])=="card-gc")return liveCardGarbageCollect();
    if(argc==2&&std::string(argv[1])=="card-compact")return liveCardCompact();
    if(argc==2&&std::string(argv[1])=="card-space")return liveCardSpace();
#endif
    usage();return 1;
}
