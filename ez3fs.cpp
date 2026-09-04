#include "ez3fs/archive.hpp"
#include "ez3fs/byte_storage.hpp"
#include "ez3fs/cartridge_storage.hpp"
#include "ez3fs/cartridge_programmer.hpp"
#include "ez3fs/fuse_mount.hpp"
#include "ez3fs/version.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <set>
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
bool pathOccupied(const fs::path& path,std::error_code& error) {
    const auto status=fs::symlink_status(path,error);
    return !error&&status.type()!=fs::file_type::not_found;
}
std::uint64_t fileModifiedTime(const fs::path& path) {
    std::error_code error;const auto file_time=fs::last_write_time(path,error);
    if(error)return 0;
    const auto system_time=std::chrono::time_point_cast<std::chrono::seconds>(
        file_time-fs::file_time_type::clock::now()+std::chrono::system_clock::now());
    const auto seconds=system_time.time_since_epoch().count();
    return seconds>0?static_cast<std::uint64_t>(seconds):0;
}
void usage() { std::cerr<<"Usage:\n  ez3fs create OUTPUT.ez3fs FILE...\n  ez3fs list IMAGE.ez3fs\n  ez3fs verify IMAGE.ez3fs\n  ez3fs extract IMAGE.ez3fs OUTPUT_DIRECTORY\n  ez3fs mkdir IMAGE.ez3fs DIRECTORY\n  ez3fs add IMAGE.ez3fs SOURCE_FILE DESTINATION\n  ez3fs rm IMAGE.ez3fs FILE\n  ez3fs rmdir IMAGE.ez3fs DIRECTORY\n  ez3fs mount IMAGE.ez3fs MOUNTPOINT [--writable] [--foreground]\n  ez3fs card-info\n  ez3fs card-list\n  ez3fs card-verify\n  ez3fs card-extract OUTPUT_DIRECTORY\n  ez3fs card-pull OUTPUT.ez3fs\n  ez3fs card-write IMAGE.ez3fs\n  ez3fs card-mount MOUNTPOINT [--foreground]\n  ez3fs --version\n"; }
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
    std::error_code path_error;
    if(pathOccupied(destination,path_error)||path_error){
        std::cerr<<"Refusing to overwrite destination: "<<destination<<'\n';return 1;}
    ez3fs::CartridgeStorage storage;ez3fs::Archive archive;
    if(!loadCartridge(storage,archive))return 1;
    std::string error;if(!archive.verify(error)){
        std::cerr<<error<<'\n';std::string ignored;storage.close(ignored);return 1;}
    if(!closeCartridge(storage))return 1;

    fs::path temporary=destination;temporary += ".pull.tmp";
    path_error.clear();
    if(pathOccupied(destination,path_error)||path_error){
        std::cerr<<"Destination or temporary output already exists; nothing was written.\n";return 1;}
    path_error.clear();
    if(pathOccupied(temporary,path_error)||path_error){
        std::cerr<<"Destination or temporary output already exists; nothing was written.\n";return 1;}
    if(!writeFile(temporary,archive.image().data(),archive.image().size())){
        std::error_code cleanup_error;fs::remove(temporary,cleanup_error);
        std::cerr<<"Could not write pulled image: "<<temporary<<'\n';return 1;}
    fs::rename(temporary,destination,path_error);
    if(path_error){
        std::error_code cleanup_error;fs::remove(temporary,cleanup_error);
        std::cerr<<"Could not finalize pulled image: "<<path_error.message()<<'\n';return 1;}
    std::cout<<"Pulled and verified "<<archive.image().size()<<" bytes to "<<destination<<".\n";
    return 0;
}
int cardWrite(const fs::path& path) {
    ez3fs::Archive archive;if(!loadArchive(path,archive))return 1;std::string error;
    if(!archive.verify(error)){std::cerr<<error<<'\n';return 1;}
    std::cout<<"WARNING: this will erase the complete 32-MiB cartridge and program\n"
             <<archive.image().size()<<" bytes from "<<path<<".\n"
             <<"Type WRITE EZ3FS to continue: "<<std::flush;
    std::string confirmation;std::getline(std::cin,confirmation);
    if(confirmation!="WRITE EZ3FS"){std::cerr<<"Cancelled; cartridge was not modified.\n";return 1;}
    ez3fs::CartridgeProgrammer programmer;
    if(!programmer.programAndVerify(archive.image(),std::cout,error)){
        std::cerr<<"Cartridge programming failed: "<<error<<'\n';return 1;
    }
    std::cout<<"Programmed and verified the EZ3FS image successfully.\n";return 0;
}
int cardMount(int argc,char** argv) {
    if(argc<3||argc>4){usage();return 1;}bool foreground=false;
    if(argc==4){if(std::string(argv[3])!="--foreground"){
        std::cerr<<"Unknown card-mount option: "<<argv[3]<<'\n';return 1;}foreground=true;}
    ez3fs::CartridgeStorage storage;ez3fs::Archive archive;
    if(!loadCartridge(storage,archive))return 1;
    std::string error;if(!archive.verify(error)){
        std::cerr<<error<<'\n';std::string ignored;storage.close(ignored);return 1;}
    if(!closeCartridge(storage))return 1;
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
int removeDirectory(const fs::path& image,const std::string& path) {
    return editImage(image,[&](ez3fs::ArchiveEditor& editor,std::string& error){return editor.removeDirectory(path,error);});
}
int mountFilesystem(int argc,char** argv) {
    if(argc<4||argc>6){usage();return 1;}bool writable=false,foreground=false;
    for(int i=4;i<argc;++i){const std::string option(argv[i]);
        if(option=="--writable")writable=true;else if(option=="--foreground")foreground=true;
        else{std::cerr<<"Unknown mount option: "<<option<<'\n';return 1;}}
    return ez3fs::mountImage(argv[2],argv[3],writable,foreground);
}
}
int main(int argc,char** argv) {
    if(argc==2&&std::string(argv[1])=="--version"){std::cout<<"ez3fs "<<ez3fs::project_version<<'\n';return 0;}
    if(argc>=2&&std::string(argv[1])=="create")return createImage(argc,argv);
    if(argc==3&&std::string(argv[1])=="list")return listImage(argv[2]);
    if(argc==3&&std::string(argv[1])=="verify")return verifyImage(argv[2]);
    if(argc==4&&std::string(argv[1])=="extract")return extractImage(argv[2],argv[3]);
    if(argc==4&&std::string(argv[1])=="mkdir")return makeDirectory(argv[2],argv[3]);
    if(argc==5&&std::string(argv[1])=="add")return addFile(argv[2],argv[3],argv[4]);
    if(argc==4&&std::string(argv[1])=="rm")return removeFile(argv[2],argv[3]);
    if(argc==4&&std::string(argv[1])=="rmdir")return removeDirectory(argv[2],argv[3]);
    if(argc>=2&&std::string(argv[1])=="mount")return mountFilesystem(argc,argv);
    if(argc==2&&std::string(argv[1])=="card-info")return cardInfo();
    if(argc==2&&std::string(argv[1])=="card-list")return cardList();
    if(argc==2&&std::string(argv[1])=="card-verify")return cardVerify();
    if(argc==3&&std::string(argv[1])=="card-extract")return cardExtract(argv[2]);
    if(argc==3&&std::string(argv[1])=="card-pull")return cardPull(argv[2]);
    if(argc==3&&std::string(argv[1])=="card-write")return cardWrite(argv[2]);
    if(argc>=2&&std::string(argv[1])=="card-mount")return cardMount(argc,argv);
    usage();return 1;
}
