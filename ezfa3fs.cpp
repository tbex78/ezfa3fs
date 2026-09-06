#include "ezfa3fs/byte_storage.hpp"
#include "ezfa3fs/cartridge_storage.hpp"
#include "ezfa3fs/cartridge_live_device.hpp"
#include "ezfa3fs/cartridge_programmer.hpp"
#include "ezfa3fs/cartridge_flash_geometry.hpp"
#include "ezfa3fs/fuse_mount.hpp"
#include "ezfa3fs/live_filesystem.hpp"
#include "ezfa3fs/live_mount_backend.hpp"
#include "ezfa3fs/live_cartridge_session.hpp"
#include "ezfa3fs/version.hpp"
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
    std::cerr<<R"(Usage:
  ezfa3fs format IMAGE.ezfa3fs
  ezfa3fs format --direct-boot IMAGE.ezfa3fs ROM.gba
  ezfa3fs list IMAGE.ezfa3fs
  ezfa3fs verify IMAGE.ezfa3fs
  ezfa3fs mkdir IMAGE.ezfa3fs DIRECTORY
  ezfa3fs put IMAGE.ezfa3fs SOURCE_FILE [DESTINATION]
  ezfa3fs get IMAGE.ezfa3fs FILE OUTPUT_FILE
  ezfa3fs rm IMAGE.ezfa3fs FILE
  ezfa3fs rmdir IMAGE.ezfa3fs DIRECTORY
  ezfa3fs gc IMAGE.ezfa3fs
  ezfa3fs compact IMAGE.ezfa3fs
  ezfa3fs space IMAGE.ezfa3fs
  ezfa3fs mount IMAGE.ezfa3fs MOUNTPOINT [--writable] [--foreground]
  ezfa3fs card-mount MOUNTPOINT [--foreground]
  ezfa3fs card-mount MOUNTPOINT --writable --foreground [--verify]
  ezfa3fs card-pull IMAGE.ezfa3fs
  ezfa3fs card-write IMAGE.ezfa3fs
  ezfa3fs card-gc
  ezfa3fs card-compact
  ezfa3fs card-space
  ezfa3fs card-read-block BLOCK OUTPUT.bin
  ezfa3fs card-erase-plan BLOCK
  ezfa3fs card-erase-block BLOCK
  ezfa3fs card-program-block BLOCK INPUT.bin
  ezfa3fs --version
)";

}
bool confirm(const std::string& prompt) { std::cout<<prompt<<" [y/N]: "<<std::flush;std::string answer;std::getline(std::cin,answer);return answer=="y"||answer=="Y"||answer=="yes"||answer=="YES"; }
ezfa3fs::live::Filesystem::ScanProgress progressReporter(std::string label) {
    return [label=std::move(label),displayed=101u](std::size_t completed,
                                                   std::size_t total) mutable {
        const auto percent=total==0?100u:static_cast<unsigned>(completed*100/total);
        if(percent==displayed)return;
        displayed=percent;
        std::cerr<<'\r'<<label<<": "<<percent<<'%'<<std::flush;
        if(completed==total)std::cerr<<'\n';
    };
}
void reportAutomaticMaintenance(ezfa3fs::live::MaintenanceAction action) {
    std::cerr<<"EZFA3FS automatic "
             <<(action==ezfa3fs::live::MaintenanceAction::garbage_collection?
                "garbage collection":"compaction")
             <<" started; the write will resume when it completes.\n";
}
bool loadLive(const fs::path& path,ezfa3fs::live::NorFlash& flash,ezfa3fs::live::Filesystem& filesystem) {
    std::string error;
    if(!flash.load(path.string(),error)||!ezfa3fs::live::Filesystem::open(flash,filesystem,error)){std::cerr<<error<<'\n';return false;}
    return true;
}
int liveFormat(const fs::path& path) { ezfa3fs::live::NorFlash flash;std::string error;
    if(!ezfa3fs::live::Filesystem::format(flash,error)||!flash.save(path.string(),error)){std::cerr<<error<<'\n';return 1;}
    std::cout<<"Formatted EZFA3FS "<<ezfa3fs::live::format_version
             <<" image "<<path<<".\n";return 0;
}
int liveFormatDirectBoot(const fs::path& path,const fs::path& rom_path) {
    if(!fs::is_regular_file(rom_path)){std::cerr<<"Input is not a regular file: "<<rom_path<<'\n';return 1;}
    std::vector<std::uint8_t> rom;if(!readFile(rom_path,rom)){std::cerr<<"Could not read input ROM: "<<rom_path<<'\n';return 1;}
    ezfa3fs::live::NorFlash flash;std::string error;
    if(!ezfa3fs::live::Filesystem::formatDirectBoot(flash,rom_path.filename().string(),rom,fileModifiedTime(rom_path),error)||
       !flash.save(path.string(),error)){std::cerr<<error<<'\n';return 1;}
    std::cout<<"Formatted direct-boot EZFA3FS "
             <<ezfa3fs::live::direct_boot_format_version<<" image "<<path
             <<" with "<<rom_path.filename()<<" at cartridge offset 0.\n";return 0;
}
int liveFormatDirectBootEmpty(const fs::path& path) {
    ezfa3fs::live::NorFlash flash;std::string error;
    if(!ezfa3fs::live::Filesystem::formatDirectBootEmpty(flash,error)||!flash.save(path.string(),error)){
        std::cerr<<error<<'\n';return 1;
    }
    std::cout<<"Formatted empty direct-boot EZFA3FS "
             <<ezfa3fs::live::direct_boot_format_version<<" image "<<path
             <<". Add exactly one root-level .gba ROM with put or a writable cartridge mount.\n";return 0;
}
void printLiveEntries(const ezfa3fs::live::Filesystem& filesystem) { std::cout<<"EZFA3FS generation "<<filesystem.generation()<<"\n";
    for(const auto& entry:filesystem.entries())std::cout<<(entry.directory?"directory ":"file      ")<<std::setw(10)<<entry.size<<"  "<<entry.name<<'\n';
    std::cout<<"Free blocks: "<<filesystem.freeBlocks()<<'\n'; }
int liveList(const fs::path& path) { ezfa3fs::live::NorFlash flash;ezfa3fs::live::Filesystem filesystem(flash);if(!loadLive(path,flash,filesystem))return 1;printLiveEntries(filesystem);return 0; }
int liveVerify(const fs::path& path) { ezfa3fs::live::NorFlash flash;ezfa3fs::live::Filesystem filesystem(flash);if(!loadLive(path,flash,filesystem))return 1;std::string error;
    if(!filesystem.verify(error)){std::cerr<<error<<'\n';return 1;}std::cout<<"Verified EZFA3FS generation "<<filesystem.generation()<<" with "<<filesystem.entries().size()<<" entries.\n";return 0; }
int liveMount(const fs::path& image,const fs::path& mountpoint,bool writable,
              bool foreground) {
    ezfa3fs::live::NorFlash flash;ezfa3fs::live::Filesystem filesystem(flash);if(!loadLive(image,flash,filesystem))return 1;std::string error;
    if(!filesystem.verify(error)){std::cerr<<error<<'\n';return 1;}
    ezfa3fs::LiveMountBackend::PersistenceObserver persist;
    if(writable) {
        persist=[&flash,&image](std::string& save_error) {
            return flash.save(image.string(),save_error);
        };
    }
    auto backend=std::make_unique<ezfa3fs::LiveMountBackend>(
        filesystem,ezfa3fs::live::Filesystem::MaintenanceObserver{},persist,writable);
    return ezfa3fs::mountBackend(std::move(backend),mountpoint.string(),foreground,
                               "ezfa3fs-image");
}
int liveCardMount(const fs::path& mountpoint,bool writable,bool foreground,
                  bool verify_referenced_data) {
    if(writable){
        if(!foreground){std::cerr<<"Writable cartridge mounting requires --foreground.\n";return 1;}
        std::cout<<"WARNING: changes made through this mount are written directly to the EZFA3FS cartridge.\n";
        if(!confirm("Proceed")){std::cerr<<"Cancelled; cartridge was not modified.\n";return 1;}
        return ezfa3fs::mountLiveCartridge(mountpoint.string(),foreground,
                                         verify_referenced_data);
    }
    if(verify_referenced_data){std::cerr<<"--verify is only available for writable live cartridge mounts.\n";return 1;}
    ezfa3fs::CartridgeStorage storage;std::string error;if(!storage.open(error)){std::cerr<<error<<'\n';return 1;}
    ezfa3fs::live::NorFlash flash;const bool loaded=flash.load(storage,error);std::string close_error;const bool closed=storage.close(close_error);
    if(!loaded||!closed){if(error.empty())error=close_error;std::cerr<<error<<'\n';return 1;}ezfa3fs::live::Filesystem filesystem(flash);
    if(!ezfa3fs::live::Filesystem::open(flash,filesystem,error)||!filesystem.verify(error)){std::cerr<<error<<'\n';return 1;}
    auto backend=std::make_unique<ezfa3fs::LiveMountBackend>(
        filesystem,ezfa3fs::live::Filesystem::MaintenanceObserver{},
        ezfa3fs::LiveMountBackend::PersistenceObserver{},false);
    return ezfa3fs::mountBackend(std::move(backend),mountpoint.string(),foreground,
                               "ezfa3fs-card");
}
int liveMkdir(const fs::path& image,const std::string& path) { ezfa3fs::live::NorFlash flash;ezfa3fs::live::Filesystem filesystem(flash);if(!loadLive(image,flash,filesystem))return 1;std::string error;
    if(!filesystem.createDirectory(path,error)||!flash.save(image.string(),error)){std::cerr<<error<<'\n';return 1;}return 0; }
int livePut(const fs::path& image,const fs::path& source,const std::string& destination) { if(!fs::is_regular_file(source)){std::cerr<<"Input is not a regular file: "<<source<<'\n';return 1;}
    std::vector<std::uint8_t> bytes;if(!readFile(source,bytes)){std::cerr<<"Could not read input: "<<source<<'\n';return 1;}ezfa3fs::live::NorFlash flash;ezfa3fs::live::Filesystem filesystem(flash);if(!loadLive(image,flash,filesystem))return 1;std::string error;
    if(!filesystem.putFile(destination,bytes,fileModifiedTime(source),error,
                           reportAutomaticMaintenance)||
       !flash.save(image.string(),error)){std::cerr<<error<<'\n';return 1;}return 0; }
int liveGet(const fs::path& image,const std::string& source,const fs::path& output) { ezfa3fs::live::NorFlash flash;ezfa3fs::live::Filesystem filesystem(flash);if(!loadLive(image,flash,filesystem))return 1;std::string error;std::vector<std::uint8_t> bytes;
    if(!filesystem.readFile(source,bytes,error)||!writeFile(output,bytes.data(),bytes.size())){if(error.empty())error="could not write output file";std::cerr<<error<<'\n';return 1;}return 0; }
int liveRemove(const fs::path& image,const std::string& path,bool directory) { ezfa3fs::live::NorFlash flash;ezfa3fs::live::Filesystem filesystem(flash);if(!loadLive(image,flash,filesystem))return 1;std::string error;
    if(!(directory?filesystem.removeDirectory(path,error):filesystem.removeFile(path,error))||!flash.save(image.string(),error)){std::cerr<<error<<'\n';return 1;}return 0; }
int liveGarbageCollect(const fs::path& image) {
    ezfa3fs::live::NorFlash flash;ezfa3fs::live::Filesystem filesystem(flash);
    if(!loadLive(image,flash,filesystem))return 1;std::string error;
    if(!filesystem.verify(error)){std::cerr<<error<<'\n';return 1;}
    std::size_t reclaimed=0;
    if(!filesystem.collectGarbage(reclaimed,error)||!flash.save(image.string(),error)){
        std::cerr<<error<<'\n';return 1;
    }
    std::cout<<"Reclaimed "<<reclaimed<<" EZFA3FS block(s) in "<<image<<".\n";return 0;
}
void printLiveCompaction(const ezfa3fs::live::CompactionReport& report) {
    std::cout<<"Garbage blocks reclaimed: "<<report.garbage_blocks_reclaimed<<'\n'
             <<"Files relocated:          "<<report.files_relocated<<'\n'
             <<"Data blocks relocated:    "<<report.blocks_relocated<<'\n';
}
int liveCompact(const fs::path& image) {
    ezfa3fs::live::NorFlash flash;ezfa3fs::live::Filesystem filesystem(flash);
    if(!loadLive(image,flash,filesystem))return 1;
    std::string error;ezfa3fs::live::CompactionReport report;
    if(!filesystem.verify(error)||!filesystem.compact(report,error)||
       !flash.save(image.string(),error)) {
        std::cerr<<error<<'\n';return 1;
    }
    std::cout<<"Compacted and verified "<<image<<".\n";
    printLiveCompaction(report);return 0;
}
void printLiveSpace(const ezfa3fs::live::Filesystem& filesystem,
                    const ezfa3fs::live::SpaceReport& report) {
    constexpr std::size_t kib_per_block=ezfa3fs::live::NorFlash::block_size/1024;
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
    ezfa3fs::live::NorFlash flash;ezfa3fs::live::Filesystem filesystem(flash);
    if(!loadLive(image,flash,filesystem))return 1;std::string error;ezfa3fs::live::SpaceReport report;
    if(!filesystem.inspectSpace(report,error)){std::cerr<<error<<'\n';return 1;}
    printLiveSpace(filesystem,report);return 0;
}
int liveCardSpace() {
    ezfa3fs::CartridgeStorage storage;std::string error;
    if(!storage.open(error)){std::cerr<<error<<'\n';return 1;}
    ezfa3fs::CartridgeLiveDevice device(storage);ezfa3fs::live::Filesystem filesystem(device);
    if(!ezfa3fs::live::Filesystem::open(device,filesystem,error)){
        std::string ignored;storage.close(ignored);std::cerr<<error<<'\n';return 1;
    }
    const auto progress=progressReporter("Inspecting EZFA3FS space");
    ezfa3fs::live::SpaceReport report;const bool inspected=filesystem.inspectSpace(report,error,progress);
    std::string close_error;const bool closed=storage.close(close_error);
    if(!inspected||!closed){if(error.empty())error=close_error;std::cerr<<error<<'\n';return 1;}
    printLiveSpace(filesystem,report);return 0;
}
int liveCardGarbageCollect() {
    std::cout<<"WARNING: this will erase unreferenced EZFA3FS data blocks on the cartridge.\n"
             <<"Unmount the cartridge before continuing.\n";
    if(!confirm("Proceed")){std::cerr<<"Cancelled; cartridge was not modified.\n";return 1;}
    ezfa3fs::LiveCartridgeSession cartridge;std::string error;
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
    ezfa3fs::LiveCartridgeSession cartridge;std::string error;
    if(!cartridge.open(error)){std::cerr<<error<<'\n';return 1;}
    ezfa3fs::live::CompactionReport report;
    const auto compacted=cartridge.filesystem().compact(
        report,error,progressReporter("Preparing EZFA3FS compaction"));
    std::string close_error;const bool closed=cartridge.close(close_error);
    if(!compacted||!closed){if(error.empty())error=close_error;std::cerr<<error<<'\n';return 1;}
    std::cout<<"Compacted and verified the EZFA3FS cartridge.\n";
    printLiveCompaction(report);return 0;
}
int liveCardPull(const fs::path& image) { ezfa3fs::CartridgeStorage storage;std::string error;
    if(!storage.open(error)){std::cerr<<error<<'\n';return 1;} ezfa3fs::live::NorFlash flash;
    const bool loaded=flash.load(storage,error);std::string close_error;const bool closed=storage.close(close_error);
    if(!loaded||!closed){if(error.empty())error=close_error;std::cerr<<error<<'\n';return 1;} ezfa3fs::live::Filesystem filesystem(flash);
    if(!ezfa3fs::live::Filesystem::open(flash,filesystem,error)||!filesystem.verify(error)||!flash.save(image.string(),error)){std::cerr<<error<<'\n';return 1;}
    std::cout<<"Pulled and verified EZFA3FS generation "<<filesystem.generation()<<" to "<<image<<".\n";return 0; }
int liveCardReadBlock(const std::string& block_text,const fs::path& output) {
    std::size_t block=0;try { std::size_t parsed=0;block=std::stoull(block_text,&parsed,0);if(parsed!=block_text.size())throw std::invalid_argument("block"); }
    catch(const std::exception&) { std::cerr<<"Invalid cartridge block: "<<block_text<<'\n';return 1; }
    if(block>=ezfa3fs::live::NorFlash::block_count){std::cerr<<"Cartridge block must be between 0 and 511.\n";return 1;}
    ezfa3fs::CartridgeStorage storage;std::string error;if(!storage.open(error)){std::cerr<<error<<'\n';return 1;}
    std::vector<std::uint8_t> bytes(ezfa3fs::live::NorFlash::block_size);const bool read=storage.read(block*bytes.size(),bytes.data(),bytes.size(),error);std::string close_error;const bool closed=storage.close(close_error);
    if(!read||!closed){if(error.empty())error=close_error;std::cerr<<error<<'\n';return 1;}
    if(!writeFile(output,bytes.data(),bytes.size())){std::cerr<<"Could not write block output: "<<output<<'\n';return 1;}
    std::cout<<"Read cartridge block "<<block<<" ("<<bytes.size()<<" bytes) to "<<output<<".\n";return 0;
}
int liveCardErasePlan(const std::string& block_text) {
    std::size_t block=0;try { std::size_t parsed=0;block=std::stoull(block_text,&parsed,0);if(parsed!=block_text.size())throw std::invalid_argument("block"); }
    catch(const std::exception&) { std::cerr<<"Invalid cartridge block: "<<block_text<<'\n';return 1; }
    if(block>=ezfa3fs::live::NorFlash::block_count){std::cerr<<"Cartridge block must be between 0 and 511.\n";return 1;}
    const std::uint64_t begin=block*ezfa3fs::live::NorFlash::block_size,end=begin+ezfa3fs::live::NorFlash::block_size;
    std::cout<<"Dry-run erase plan for logical block "<<block<<" (bytes 0x"<<std::hex<<begin<<"..0x"<<end-1<<std::dec<<")\n";
    const auto sectors=ezfa3fs::CartridgeFlashGeometry::sectorsForLogicalBlock(block);
    for(unsigned window=0;window<4;++window) {
        std::cout<<"window "<<window<<":\n";
        for(const auto& sector:sectors)if(sector.window==window) {
            const auto byte_offset=static_cast<std::uint64_t>(window)*0x800000u+
                                   static_cast<std::uint64_t>(sector.word_address)*2u;
            std::cout<<"  0x96 word 0x"<<std::hex<<sector.word_address
                     <<" (byte 0x"<<byte_offset<<")\n"<<std::dec;
        }
    }
    std::cout<<"No erase command was sent.\n";return 0;
}
int liveCardEraseBlock(const std::string& block_text) {
    std::size_t block=0;try { std::size_t parsed=0;block=std::stoull(block_text,&parsed,0);if(parsed!=block_text.size())throw std::invalid_argument("block"); }
    catch(const std::exception&) { std::cerr<<"Invalid cartridge block: "<<block_text<<'\n';return 1; }
    if(block<2||block>=ezfa3fs::live::NorFlash::block_count){std::cerr<<"Only blocks 2 through 511 may be erased.\n";return 1;}
    std::cout<<"WARNING: this will erase live cartridge block "<<block<<" (64 KiB).\n";
    if(!confirm("Proceed")){std::cerr<<"Cancelled; cartridge was not modified.\n";return 1;}
    ezfa3fs::CartridgeProgrammer programmer;std::string error;if(!programmer.eraseLiveBlock(block,std::cout,error)){std::cerr<<error<<'\n';return 1;}return 0;
}
int liveCardProgramBlock(const std::string& block_text,const fs::path& input) {
    std::size_t block=0;try { std::size_t parsed=0;block=std::stoull(block_text,&parsed,0);if(parsed!=block_text.size())throw std::invalid_argument("block"); }
    catch(const std::exception&) { std::cerr<<"Invalid cartridge block: "<<block_text<<'\n';return 1; }
    if(block<2||block>=ezfa3fs::live::NorFlash::block_count){std::cerr<<"Only blocks 2 through 511 may be programmed.\n";return 1;}
    std::vector<std::uint8_t> bytes;if(!readFile(input,bytes)||bytes.size()!=ezfa3fs::live::NorFlash::block_size){std::cerr<<"Input must be exactly 64 KiB.\n";return 1;}
    std::cout<<"WARNING: this will program live cartridge block "<<block<<" (64 KiB).\n";
    if(!confirm("Proceed")){std::cerr<<"Cancelled; cartridge was not modified.\n";return 1;}
    ezfa3fs::CartridgeProgrammer programmer;std::string error;if(!programmer.programLiveBlock(block,bytes,std::cout,error)){std::cerr<<error<<'\n';return 1;}return 0;
}
int liveCardWrite(const fs::path& image) {
    std::vector<std::uint8_t> bytes;
    if(!readFile(image,bytes)){std::cerr<<"Could not read image: "<<image<<'\n';return 1;}
    ezfa3fs::live::NorFlash flash;std::string error;
    if(bytes.size()!=ezfa3fs::live::NorFlash::capacity||!flash.load(image.string(),error)){if(error.empty())error="live image must be exactly 32 MiB";std::cerr<<error<<'\n';return 1;}
    ezfa3fs::live::Filesystem filesystem(flash);
    if(!ezfa3fs::live::Filesystem::open(flash,filesystem,error)||!filesystem.verify(error)){std::cerr<<error<<'\n';return 1;}
    std::cout<<"WARNING: this will erase the complete 32-MiB cartridge and program\n"
             <<"the verified EZFA3FS image from "<<image<<".\n"
             <<"Confirm cartridge replacement"<<'\n';
    if(!confirm("Proceed")){std::cerr<<"Cancelled; cartridge was not modified.\n";return 1;}
    ezfa3fs::CartridgeProgrammer programmer;
    if(!programmer.programAndVerify(bytes,std::cout,error)){std::cerr<<"Cartridge programming failed: "<<error<<'\n';return 1;}
    std::cout<<"Programmed and verified the EZFA3FS image successfully.\n";return 0;
}

}
int main(int argc,char** argv) {
    if(argc==2&&std::string(argv[1])=="--version"){
        std::cout<<"ezfa3fs ";

        std::cout<<ezfa3fs::project_version<<'\n';return 0;
    }
    if(argc==3&&std::string(argv[1])=="format")return liveFormat(argv[2]);
    if(argc==4&&std::string(argv[1])=="format"&&std::string(argv[2])=="--direct-boot")return liveFormatDirectBootEmpty(argv[3]);
    if(argc==5&&std::string(argv[1])=="format"&&std::string(argv[2])=="--direct-boot")return liveFormatDirectBoot(argv[3],argv[4]);
    if(argc==3&&std::string(argv[1])=="list")return liveList(argv[2]);
    if(argc==3&&std::string(argv[1])=="verify")return liveVerify(argv[2]);
    if(argc>=4&&std::string(argv[1])=="mount"){
        bool writable=false,foreground=false;
        for(int i=4;i<argc;++i){const std::string option(argv[i]);
            if(option=="--writable")writable=true;
            else if(option=="--foreground")foreground=true;
            else{std::cerr<<"Unknown mount option: "<<option<<'\n';return 1;}}
        return liveMount(argv[2],argv[3],writable,foreground);
    }
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

    usage();return 1;
}
