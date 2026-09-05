#include "ez3fs/live_filesystem.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <set>

namespace ez3fs::live {
namespace {
constexpr std::uint16_t major=2, minor=0, direct_boot_minor=1;
constexpr std::uint16_t legacy_major=1, legacy_minor=0;
constexpr std::uint32_t commit_marker=0xC0FF17EDu;
constexpr std::size_t superblock_header=32;

template<typename T> void put(std::uint8_t* bytes,std::size_t offset,T value) {
    for(std::size_t i=0;i<sizeof(T);++i)bytes[offset+i]=static_cast<std::uint8_t>(value>>(i*8));
}
template<typename T> T get(const std::uint8_t* bytes,std::size_t offset) {
    T value=0;for(std::size_t i=0;i<sizeof(T);++i)value|=static_cast<T>(bytes[offset+i])<<(i*8);return value;
}
bool validPath(const std::string& path) {
    if(path.empty()||path.front()=='/'||path.find('\\')!=std::string::npos)return false;
    std::size_t begin=0;while(begin<=path.size()) {
        const auto end=path.find('/',begin);const auto part=path.substr(begin,end-begin);
        if(part.empty()||part=="."||part=="..")return false;
        if(end==std::string::npos)break;begin=end+1;
    } return true;
}
std::size_t dataBlockCount(std::uint64_t size) {
    return static_cast<std::size_t>((size+NorFlash::block_size-1)/NorFlash::block_size);
}
bool validDirectBootRomName(const std::string& path) {
    if(!validPath(path)||path.find('/')!=std::string::npos||path.size()<5)return false;
    const auto suffix=path.substr(path.size()-4);
    return std::tolower(static_cast<unsigned char>(suffix[0]))=='.'&&
           std::tolower(static_cast<unsigned char>(suffix[1]))=='g'&&
           std::tolower(static_cast<unsigned char>(suffix[2]))=='b'&&
           std::tolower(static_cast<unsigned char>(suffix[3]))=='a';
}
}

NorFlash::NorFlash():bytes_(capacity,0xFF) {}

bool NorFlash::load(const std::string& path,std::string& error) {
    std::ifstream input(path,std::ios::binary);
    if(!input){error="could not open live image: "+path;return false;}
    std::vector<std::uint8_t> bytes(std::istreambuf_iterator<char>(input),{});
    if(!input.good()&&!input.eof()){error="could not read live image: "+path;return false;}
    if(bytes.size()!=capacity){error="live image must be exactly 32 MiB";return false;}
    bytes_=std::move(bytes);error.clear();return true;
}

bool NorFlash::load(const std::vector<std::uint8_t>& bytes,std::string& error) {
    if(bytes.size()!=capacity){error="live image must be exactly 32 MiB";return false;}
    bytes_=bytes;error.clear();return true;
}

bool NorFlash::load(ByteStorage& storage,std::string& error) {
    return load(storage,std::cerr,error);
}

bool NorFlash::load(ByteStorage& storage,std::ostream& progress,std::string& error) {
    if(storage.capacity()!=capacity){error="storage capacity is not 32 MiB";return false;}
    std::vector<std::uint8_t> bytes(capacity);
    progress<<"Reading EZFA3FS cartridge: 0%"<<std::flush;
    for(std::size_t offset=0;offset<capacity;offset+=block_size) {
        if(!storage.read(offset,bytes.data()+offset,block_size,error))return false;
        progress<<"\rReading EZFA3FS cartridge: "<<((offset+block_size)*100/capacity)<<"%"<<std::flush;
    }
    progress<<"\n";
    bytes_=std::move(bytes);error.clear();return true;
}

bool NorFlash::save(const std::string& path,std::string& error) const {
    std::ofstream output(path,std::ios::binary|std::ios::trunc);
    if(!output){error="could not open live image for writing: "+path;return false;}
    output.write(reinterpret_cast<const char*>(bytes_.data()),static_cast<std::streamsize>(bytes_.size()));
    if(!output){error="could not write live image: "+path;return false;}
    error.clear();return true;
}

bool NorFlash::read(std::size_t offset,std::uint8_t* destination,std::size_t size,
                    std::string& error) const {
    if(offset>bytes_.size()||size>bytes_.size()-offset){error="NOR read out of bounds";return false;}
    std::copy_n(bytes_.data()+offset,size,destination);error.clear();return true;
}

bool NorFlash::program(std::size_t offset,const std::uint8_t* source,std::size_t size,
                       std::string& error) {
    if(offset>bytes_.size()||size>bytes_.size()-offset){error="NOR program out of bounds";return false;}
    std::size_t writable=size;
    if(fault_bytes_){writable=std::min(writable,*fault_bytes_);fault_bytes_.reset();}
    for(std::size_t i=0;i<writable;++i) {
        if((bytes_[offset+i]&source[i])!=source[i]){error="NOR program attempted a 0-to-1 transition";return false;}
    }
    for(std::size_t i=0;i<writable;++i)bytes_[offset+i]&=source[i];
    if(writable!=size){error="simulated interrupted NOR program";return false;}
    error.clear();return true;
}

bool BlockDevice::programBlocks(std::size_t first_block,
                                const std::uint8_t* source,
                                std::size_t block_count,
                                std::size_t& completed_blocks,
                                std::string& error) {
    completed_blocks=0;
    for(std::size_t i=0;i<block_count;++i) {
        if(!program((first_block+i)*NorFlash::block_size,
                    source+i*NorFlash::block_size,
                    NorFlash::block_size,error))return false;
        ++completed_blocks;
    }
    error.clear();return true;
}

bool BlockDevice::replaceMetadataBlock(std::size_t block,
                                       const std::uint8_t* source,
                                       std::size_t size,std::string& error) {
    return eraseBlock(block,error)&&
           program(block*NorFlash::block_size,source,size,error);
}

bool NorFlash::eraseBlock(std::size_t block,std::string& error) {
    if(block>=block_count){error="NOR erase block out of bounds";return false;}
    std::fill(bytes_.begin()+static_cast<std::ptrdiff_t>(block*block_size),
              bytes_.begin()+static_cast<std::ptrdiff_t>((block+1)*block_size),0xFF);
    error.clear();return true;
}

bool Filesystem::format(BlockDevice& flash,std::string& error) {
    for(std::size_t block=0;block<NorFlash::block_count;++block)
        if(!flash.eraseBlock(block,error))return false;
    Filesystem filesystem(flash);return filesystem.commit(error);
}

bool Filesystem::formatDirectBootEmpty(BlockDevice& flash,std::string& error,
                                       std::size_t boot_slot_blocks) {
    if(boot_slot_blocks==0||boot_slot_blocks>NorFlash::block_count-2){error="direct-boot slot must be between 1 and 510 blocks";return false;}
    for(std::size_t block=0;block<NorFlash::block_count;++block)
        if(!flash.eraseBlock(block,error))return false;
    Filesystem filesystem(flash);filesystem.layout_=Layout::direct_boot;filesystem.boot_slot_blocks_=boot_slot_blocks;
    return filesystem.commit(error);
}

bool Filesystem::formatDirectBoot(BlockDevice& flash,const std::string& rom_name,
                                  const std::vector<std::uint8_t>& rom,
                                  std::uint64_t modified_time,std::string& error) {
    if(!formatDirectBootEmpty(flash,error,dataBlockCount(rom.size())))return false;
    Filesystem filesystem(flash);
    return open(flash,filesystem,error)&&filesystem.putFile(rom_name,rom,modified_time,error);
}

bool Filesystem::open(BlockDevice& flash,Filesystem& result,std::string& error,
                      ScanProgress progress) {
    // Retained for source compatibility. Allocation is now checked lazily at
    // the point of use instead of scanning the complete free tail on mount.
    (void)progress;
    bool found=false;std::uint64_t newest=0;std::size_t chosen=0;Layout chosen_layout=Layout::transactional;std::vector<Entry> entries;
    const std::array<std::pair<std::size_t,Layout>,4> candidates{{
        {0,Layout::transactional},{1,Layout::transactional},
        {NorFlash::block_count-2,Layout::direct_boot},{NorFlash::block_count-1,Layout::direct_boot}}};
    for(const auto& candidate:candidates) {const auto block=candidate.first;const auto layout=candidate.second;
        std::vector<std::uint8_t> bytes(NorFlash::block_size);
        if(!flash.read(block*NorFlash::block_size,bytes.data(),bytes.size(),error))return false;
        const bool current_format=std::equal(format_magic.begin(),format_magic.end(),bytes.begin())&&
            get<std::uint16_t>(bytes.data(),8)==major&&get<std::uint16_t>(bytes.data(),10)==minor;
        const bool legacy_format=std::equal(legacy_format_magic.begin(),legacy_format_magic.end(),bytes.begin())&&
            get<std::uint16_t>(bytes.data(),8)==legacy_major&&get<std::uint16_t>(bytes.data(),10)==legacy_minor;
        const auto direct_minor=get<std::uint16_t>(bytes.data(),10);
        const bool direct_format=std::equal(direct_boot_format_magic.begin(),direct_boot_format_magic.end(),bytes.begin())&&
            get<std::uint16_t>(bytes.data(),8)==major&&(direct_minor==minor||direct_minor==direct_boot_minor);
        if((layout==Layout::transactional?(!current_format&&!legacy_format):!direct_format)||get<std::uint32_t>(bytes.data(),28)!=commit_marker)continue;
        const auto length=get<std::uint32_t>(bytes.data(),20);
        if(length>NorFlash::block_size-superblock_header||
           Crc32::calculate(bytes.data()+superblock_header,length)!=get<std::uint32_t>(bytes.data(),24))continue;
        std::vector<Entry> parsed;std::set<std::string> names;
        const bool slotted_direct=layout==Layout::direct_boot&&direct_minor==direct_boot_minor;
        const auto slot_blocks=slotted_direct?get<std::uint32_t>(bytes.data()+superblock_header,0):0;
        std::size_t direct_boot_blocks=slotted_direct?slot_blocks:0;
        const auto count=get<std::uint32_t>(bytes.data()+superblock_header,slotted_direct?4:0);std::size_t offset=slotted_direct?8:4;bool valid= !slotted_direct||(slot_blocks>0&&slot_blocks<=NorFlash::block_count-2);
        for(std::uint32_t i=0;i<count&&valid;++i) {
            if(offset+32>length){valid=false;break;}
            const auto name_length=get<std::uint16_t>(bytes.data()+superblock_header,offset);
            if(name_length==0||offset+32+name_length>length){valid=false;break;}
            const auto* base=bytes.data()+superblock_header+offset;
            Entry entry;entry.name=std::string(reinterpret_cast<const char*>(base+32),name_length);
            entry.directory=(base[2]&1)!=0;entry.size=get<std::uint64_t>(base,4);entry.modified_time=get<std::uint64_t>(base,12);
            entry.crc32=get<std::uint32_t>(base,20);entry.first_block=get<std::uint32_t>(base,24);entry.block_count=get<std::uint32_t>(base,28);
            if(!validPath(entry.name)||!names.insert(entry.name).second||
               (!entry.directory&&!entry.block_count&&entry.size)||
               (entry.directory&&(entry.size||entry.block_count||entry.first_block||entry.crc32))||
               (!entry.directory&&(entry.first_block<(layout==Layout::direct_boot?0:2)||
                   static_cast<std::uint64_t>(entry.first_block)+entry.block_count>(layout==Layout::direct_boot?NorFlash::block_count-2:NorFlash::block_count)||
                   entry.size>static_cast<std::uint64_t>(entry.block_count)*NorFlash::block_size))){valid=false;break;}
            if(layout==Layout::direct_boot&&!slotted_direct&&parsed.empty()&&
               (!validDirectBootRomName(entry.name)||entry.directory||entry.first_block!=0)){valid=false;break;}
            if(layout==Layout::direct_boot&&!parsed.empty()&&!entry.directory&&
               entry.first_block<direct_boot_blocks){valid=false;break;}
            if(layout==Layout::direct_boot&&!slotted_direct&&parsed.empty()&&
               !entry.directory&&entry.first_block==0)direct_boot_blocks=entry.block_count;
            parsed.push_back(std::move(entry));offset+=32+name_length;
        }
        if(!valid||offset!=length)continue;
        const auto generation=get<std::uint64_t>(bytes.data(),12);
        if(!found||generation>newest){found=true;newest=generation;chosen=block;chosen_layout=layout;result.boot_slot_blocks_=slotted_direct?slot_blocks:direct_boot_blocks;entries=std::move(parsed);}
        // Ordinary EZFA3FS images retain their two metadata blocks at the
        // beginning of the cartridge.  Avoid tail probes on their hot mount
        // path; direct-boot metadata is consulted only as a fallback.
        if(block==1&&found)break;
    }
    if(!found){error="no valid EZFA3FS superblock found";return false;}
    result.entries_=std::move(entries);result.generation_=newest;result.active_superblock_=chosen;result.layout_=chosen_layout;result.next_free_block_=result.firstDataBlock();result.unavailable_blocks_.fill(false);
    for(const auto& entry:result.entries_)result.next_free_block_=std::max(result.next_free_block_,static_cast<std::size_t>(entry.first_block+entry.block_count));
    error.clear();return true;
}

Entry* Filesystem::find(const std::string& path){auto it=std::find_if(entries_.begin(),entries_.end(),[&](const Entry& e){return e.name==path;});return it==entries_.end()?nullptr:&*it;}
const Entry* Filesystem::find(const std::string& path) const{auto it=std::find_if(entries_.begin(),entries_.end(),[&](const Entry& e){return e.name==path;});return it==entries_.end()?nullptr:&*it;}
const Entry* Filesystem::directBootRom() const noexcept {
    if(!isDirectBoot()||entries_.empty())return nullptr;
    const auto& entry=entries_.front();
    return isDirectBootRom(entry)?&entry:nullptr;
}
bool Filesystem::isDirectBootRom(const Entry& entry) const noexcept {
    return isDirectBoot()&&!entry.directory&&entry.first_block==0&&validDirectBootRomName(entry.name);
}
bool Filesystem::awaitsDirectBootRom() const noexcept {
    return isDirectBoot()&&directBootRom()==nullptr;
}
bool Filesystem::parentExists(const std::string& path) const {
    const auto slash=path.rfind('/');return slash==std::string::npos||
        (find(path.substr(0,slash))&&find(path.substr(0,slash))->directory);
}
std::size_t Filesystem::firstDataBlock() const noexcept { return layout_==Layout::direct_boot?0:2; }
std::size_t Filesystem::allocationStartBlock() const noexcept { return isDirectBoot()?boot_slot_blocks_:firstDataBlock(); }
std::size_t Filesystem::dataEndBlock() const noexcept { return layout_==Layout::direct_boot?NorFlash::block_count-2:NorFlash::block_count; }
std::size_t Filesystem::alternateSuperblock() const noexcept {
    if(layout_==Layout::direct_boot)return active_superblock_==NorFlash::block_count-2?NorFlash::block_count-1:NorFlash::block_count-2;
    return 1-active_superblock_;
}

bool Filesystem::commit(std::string& error) {
    std::vector<std::uint8_t> manifest;manifest.resize(isDirectBoot()?8:4);
    if(isDirectBoot())put<std::uint32_t>(manifest.data(),0,static_cast<std::uint32_t>(boot_slot_blocks_));
    put<std::uint32_t>(manifest.data(),isDirectBoot()?4:0,static_cast<std::uint32_t>(entries_.size()));
    std::set<std::string> names;
    for(const auto& entry:entries_) {
        if(!names.insert(entry.name).second||entry.name.size()>std::numeric_limits<std::uint16_t>::max()){error="invalid live manifest entry";return false;}
        const auto offset=manifest.size();manifest.resize(offset+32+entry.name.size(),0);auto* base=manifest.data()+offset;
        put<std::uint16_t>(base,0,static_cast<std::uint16_t>(entry.name.size()));base[2]=entry.directory?1:0;
        put<std::uint64_t>(base,4,entry.size);put<std::uint64_t>(base,12,entry.modified_time);put<std::uint32_t>(base,20,entry.crc32);
        put<std::uint32_t>(base,24,entry.first_block);put<std::uint32_t>(base,28,entry.block_count);
        std::copy(entry.name.begin(),entry.name.end(),reinterpret_cast<char*>(base+32));
    }
    if(manifest.size()>NorFlash::block_size-superblock_header){error="live manifest exceeds superblock capacity";return false;}
    const auto target=alternateSuperblock();
    std::vector<std::uint8_t> block(NorFlash::block_size,0xFF);const auto& selected_magic=layout_==Layout::direct_boot?direct_boot_format_magic:format_magic;std::copy(selected_magic.begin(),selected_magic.end(),block.begin());
    put<std::uint16_t>(block.data(),8,major);put<std::uint16_t>(block.data(),10,layout_==Layout::direct_boot?direct_boot_minor:minor);put<std::uint64_t>(block.data(),12,generation_+1);
    put<std::uint32_t>(block.data(),20,static_cast<std::uint32_t>(manifest.size()));
    put<std::uint32_t>(block.data(),24,Crc32::calculate(manifest.data(),manifest.size()));put<std::uint32_t>(block.data(),28,commit_marker);
    std::copy(manifest.begin(),manifest.end(),block.begin()+superblock_header);
    if(!flash_.replaceMetadataBlock(target,block.data(),block.size(),error))return false;
    active_superblock_=target;++generation_;error.clear();return true;
}

bool Filesystem::blockReferenced(std::size_t block) const noexcept {
    if(block<firstDataBlock()||block>=dataEndBlock())return true;
    if(isDirectBoot()&&block<boot_slot_blocks_)return true;
    return std::any_of(entries_.begin(),entries_.end(),[block](const Entry& entry){
        return !entry.directory&&block>=entry.first_block&&
               block<static_cast<std::size_t>(entry.first_block)+entry.block_count;
    });
}

std::size_t Filesystem::freeBlocks() const noexcept {
    std::size_t count=0;
    for(std::size_t block=firstDataBlock();block<dataEndBlock();++block)
        if(!blockReferenced(block)&&!unavailable_blocks_[block])++count;
    return count;
}

bool Filesystem::inspectSpace(SpaceReport& report,std::string& error,
                              ScanProgress progress) const {
    report={};const auto first_data_block=firstDataBlock();
    const auto total=dataEndBlock()-first_data_block;
    std::vector<std::uint8_t> bytes(NorFlash::block_size);
    std::size_t erased_run=0,post_gc_run=0;
    if(progress)progress(0,total);
    for(std::size_t block=first_data_block;block<dataEndBlock();++block) {
        if(blockReferenced(block)) {
            ++report.active_blocks;erased_run=0;post_gc_run=0;
        } else {
            ++post_gc_run;report.largest_post_gc_extent=std::max(report.largest_post_gc_extent,post_gc_run);
            if(!flash_.read(block*NorFlash::block_size,bytes.data(),bytes.size(),error)) {
                error="could not inspect live space block "+std::to_string(block)+": "+error;return false;
            }
            const bool blank=std::all_of(bytes.begin(),bytes.end(),[](std::uint8_t byte){return byte==0xFF;});
            if(blank) {
                ++report.erased_blocks;++erased_run;
                report.largest_erased_extent=std::max(report.largest_erased_extent,erased_run);
            } else {
                ++report.reclaimable_blocks;erased_run=0;
            }
        }
        if(progress)progress(block-first_data_block+1,total);
    }
    error.clear();return true;
}

Filesystem::ExtentSearchResult Filesystem::findBlankExtent(
    std::size_t block_count,std::size_t& first_block,std::string& error) {
    if(block_count==0){first_block=next_free_block_;error.clear();return ExtentSearchResult::found;}
    if(block_count>freeBlocks()){error.clear();return ExtentSearchResult::no_extent;}
    std::vector<std::uint8_t> bytes(NorFlash::block_size);
    const auto inspect=[&](std::size_t begin,std::size_t end)->bool {
        std::size_t run=0,run_start=begin;
        for(std::size_t block=begin;block<end;++block) {
            if(blockReferenced(block)||unavailable_blocks_[block]){run=0;continue;}
            if(!flash_.read(block*NorFlash::block_size,bytes.data(),bytes.size(),error)) {
                error="could not inspect live allocation block "+std::to_string(block)+": "+error;return false;
            }
            const bool blank=std::all_of(bytes.begin(),bytes.end(),[](std::uint8_t byte){return byte==0xFF;});
            if(!blank){unavailable_blocks_[block]=true;run=0;continue;}
            if(run==0)run_start=block;
            if(++run==block_count){first_block=run_start;return true;}
        }
        return false;
    };
    error.clear();
    const auto start=allocationStartBlock();
    if(inspect(std::max(next_free_block_,start),dataEndBlock())){error.clear();return ExtentSearchResult::found;}
    if(!error.empty())return ExtentSearchResult::error;
    if(next_free_block_>start&&inspect(start,std::min(next_free_block_,dataEndBlock()))){error.clear();return ExtentSearchResult::found;}
    if(!error.empty())return ExtentSearchResult::error;
    error.clear();return ExtentSearchResult::no_extent;
}

bool Filesystem::allocateExtent(std::size_t block_count,std::size_t& first_block,
                                std::string& error,
                                const MaintenanceObserver& maintenance) {
    auto result=findBlankExtent(block_count,first_block,error);
    if(result==ExtentSearchResult::found)return true;
    if(result==ExtentSearchResult::error)return false;

    if(maintenance)maintenance(MaintenanceAction::garbage_collection);
    std::size_t reclaimed=0;
    if(!collectGarbage(reclaimed,error))return false;
    result=findBlankExtent(block_count,first_block,error);
    if(result==ExtentSearchResult::found)return true;
    if(result==ExtentSearchResult::error)return false;
    if(block_count>freeBlocks()) {
        error="live filesystem is out of free blocks";return false;
    }

    if(maintenance)maintenance(MaintenanceAction::compaction);
    CompactionReport report;
    if(!compactFiles(report,error))return false;
    result=findBlankExtent(block_count,first_block,error);
    if(result==ExtentSearchResult::found)return true;
    if(result==ExtentSearchResult::error)return false;
    error="live filesystem is out of free blocks: automatic compaction could not create a contiguous extent";
    return false;
}

bool Filesystem::findBlankExtentBefore(std::size_t limit,
                                       std::size_t block_count,
                                       std::size_t& first_block,
                                       std::string& error) {
    const auto start=allocationStartBlock();
    if(block_count==0||limit<=start||block_count>limit-start){error.clear();return false;}
    std::vector<std::uint8_t> bytes(NorFlash::block_size);std::size_t run=0,run_start=start;
    error.clear();
    for(std::size_t block=start;block<limit;++block) {
        if(blockReferenced(block)||unavailable_blocks_[block]){run=0;continue;}
        if(!flash_.read(block*NorFlash::block_size,bytes.data(),bytes.size(),error)) {
            error="could not inspect compaction destination block "+std::to_string(block)+": "+error;return false;
        }
        const bool blank=std::all_of(bytes.begin(),bytes.end(),[](std::uint8_t byte){return byte==0xFF;});
        if(!blank){unavailable_blocks_[block]=true;run=0;continue;}
        if(run==0)run_start=block;
        if(++run==block_count){first_block=run_start;error.clear();return true;}
    }
    error.clear();return false;
}

bool Filesystem::programExtent(std::size_t first_block,
                               const std::vector<std::uint8_t>& bytes,
                               std::string& error) {
    const auto blocks=dataBlockCount(bytes.size());
    std::vector<std::uint8_t> extent(blocks*NorFlash::block_size,0xFF);
    std::copy(bytes.begin(),bytes.end(),extent.begin());
    std::size_t completed=0;
    if(!flash_.programBlocks(first_block,extent.data(),blocks,completed,error)) {
        const auto affected=std::min(blocks,completed+1);
        for(std::size_t leaked=0;leaked<affected;++leaked)
            unavailable_blocks_[first_block+leaked]=true;
        next_free_block_=first_block+affected;return false;
    }
    if(completed!=blocks) {
        const auto affected=std::min(blocks,completed+1);
        for(std::size_t leaked=0;leaked<affected;++leaked)
            unavailable_blocks_[first_block+leaked]=true;
        next_free_block_=first_block+affected;
        error="block device reported an incomplete live extent";return false;
    }
    next_free_block_=first_block+blocks;error.clear();return true;
}

bool Filesystem::ensureDirectBootSlotCapacity(std::size_t block_count,
                                              std::string& error) {
    if(block_count<=boot_slot_blocks_){error.clear();return true;}
    if(block_count>dataEndBlock()) {
        error="direct-boot ROM requires "+std::to_string(block_count)+
              " blocks, but only "+std::to_string(dataEndBlock())+
              " blocks are available before metadata";return false;
    }
    for(std::size_t block=boot_slot_blocks_;block<block_count;++block) {
        const auto occupied=std::find_if(entries_.begin(),entries_.end(),
            [block](const Entry& entry){
                return !entry.directory&&block>=entry.first_block&&
                    block<static_cast<std::size_t>(entry.first_block)+entry.block_count;
            });
        if(occupied!=entries_.end()) {
            error="direct-boot ROM requires "+std::to_string(block_count)+
                  " blocks, but its reserved slot has "+
                  std::to_string(boot_slot_blocks_)+" blocks and cannot grow: block "+
                  std::to_string(block)+" is used by "+occupied->name;return false;
        }
    }
    std::vector<std::uint8_t> bytes(NorFlash::block_size);
    for(std::size_t block=boot_slot_blocks_;block<block_count;++block) {
        if(!flash_.read(block*NorFlash::block_size,bytes.data(),bytes.size(),error)) {
            error="could not inspect direct-boot slot expansion block "+
                  std::to_string(block)+": "+error;return false;
        }
        const bool blank=std::all_of(bytes.begin(),bytes.end(),
            [](std::uint8_t byte){return byte==0xFF;});
        if(!blank&&(!flash_.prepareForErase(error)||!flash_.eraseBlock(block,error))) {
            error="could not erase direct-boot slot expansion block "+
                  std::to_string(block)+": "+error;return false;
        }
    }
    boot_slot_blocks_=block_count;
    next_free_block_=std::max(next_free_block_,boot_slot_blocks_);
    error.clear();return true;
}

bool Filesystem::canCreateFile(const std::string& path,std::string& error) const {
    if(isDirectBoot()) {
        if(!directBootRom()) {
            if(validDirectBootRomName(path)){error.clear();return true;}
            error="direct-boot EZFA3FS requires its first file to be one root-level .gba ROM";return false;
        }
    }
    if(!validPath(path)||!parentExists(path)||find(path)) {
        error="invalid or existing live file path";return false;
    }
    error.clear();return true;
}

bool Filesystem::createDirectory(const std::string& path,std::string& error) {
    if(isDirectBoot()&&!directBootRom()){error="direct-boot EZFA3FS requires its root .gba ROM before directories";return false;}
    if(!validPath(path)||!parentExists(path)||find(path)){error="invalid or existing live directory path";return false;}
    const auto old=entries_;entries_.push_back({path,0,0,0,0,0,true});
    if(commit(error))return true;entries_=old;return false;
}

bool Filesystem::putFile(const std::string& path,const std::vector<std::uint8_t>& bytes,
                         std::uint64_t modified_time,std::string& error,
                         MaintenanceObserver maintenance) {
    if(isDirectBoot()) {
        const auto blocks=dataBlockCount(bytes.size());
        if(!directBootRom()) {
            if(!validDirectBootRomName(path)) {
                error="direct-boot EZFA3FS requires its first file to be one root-level .gba ROM";return false;
            }
            if(bytes.empty()) {error="direct-boot EZFA3FS cannot commit an empty boot ROM";return false;}
            if(!ensureDirectBootSlotCapacity(blocks,error))return false;
            std::vector<std::uint8_t> extent(blocks*NorFlash::block_size,0xFF);
            std::copy(bytes.begin(),bytes.end(),extent.begin());std::size_t completed=0;
            if(!flash_.programBlocks(0,extent.data(),blocks,completed,error)||completed!=blocks) {
                if(error.empty())error="direct-boot ROM programming was incomplete";return false;
            }
            entries_.insert(entries_.begin(),{path,bytes.size(),modified_time,Crc32::calculate(bytes.data(),bytes.size()),0,static_cast<std::uint32_t>(blocks),false});
            next_free_block_=blocks;
            if(commit(error))return true;
            entries_.clear();return false;
        }
        if(const auto* existing=find(path);existing&&isDirectBootRom(*existing)) {
            error="the direct-boot ROM at cartridge offset 0 is immutable";return false;
        }
    }
    if(!validPath(path)||!parentExists(path)){error="invalid live file path";return false;}
    if(const auto* existing=find(path);existing&&existing->directory){error="live path is a directory";return false;}
    const auto blocks=dataBlockCount(bytes.size());
    constexpr unsigned extent_attempts=3;
    const auto old=entries_;std::size_t first_block=0;std::string program_error;
    bool programmed=false;
    for(unsigned attempt=1;attempt<=extent_attempts;++attempt) {
        if(!allocateExtent(blocks,first_block,error,maintenance)) {
            if(!program_error.empty())error=program_error+"; alternate extent unavailable: "+error;
            entries_=old;return false;
        }
        next_free_block_=first_block;
        if(programExtent(first_block,bytes,error)){programmed=true;break;}
        program_error=error;
    }
    if(!programmed){entries_=old;error=program_error;return false;}
    auto* existing=find(path);
    Entry replacement{path,bytes.size(),modified_time,Crc32::calculate(bytes.data(),bytes.size()),static_cast<std::uint32_t>(first_block),static_cast<std::uint32_t>(blocks),false};
    if(existing)*existing=replacement;else entries_.push_back(std::move(replacement));
    if(commit(error))return true;entries_=old;return false;
}

bool Filesystem::collectGarbage(std::size_t& reclaimed_blocks,
                                std::string& error,ScanProgress progress) {
    reclaimed_blocks=0;const auto first_data_block=firstDataBlock();
    const auto total=dataEndBlock()-first_data_block;
    std::vector<std::uint8_t> bytes(NorFlash::block_size);std::vector<std::size_t> garbage;
    if(progress)progress(0,total);
    for(std::size_t block=first_data_block;block<dataEndBlock();++block) {
        if(!blockReferenced(block)) {
            if(!flash_.read(block*NorFlash::block_size,bytes.data(),bytes.size(),error)) {
                error="could not inspect garbage-collection block "+std::to_string(block)+": "+error;return false;
            }
            const bool blank=std::all_of(bytes.begin(),bytes.end(),[](std::uint8_t byte){return byte==0xFF;});
            if(blank)unavailable_blocks_[block]=false;
            else garbage.push_back(block);
        }
        if(progress)progress(block-first_data_block+1,total);
    }
    if(!garbage.empty()) {
        // Write the selected manifest to the alternate superblock before
        // reclaiming anything. Both valid generations then protect the same
        // active extents if a later collection step is interrupted.
        if(!commit(error)){error="could not synchronize live metadata before garbage collection: "+error;return false;}
    }
    for(const auto block:garbage) {
        unavailable_blocks_[block]=true;
        // Real hardware needs a clean writer transition after the inspection
        // read. In-memory devices implement this boundary as a no-op.
        if(!flash_.prepareForErase(error)||!flash_.eraseBlock(block,error)) {
            error="could not reclaim live block "+std::to_string(block)+": "+error;return false;
        }
        unavailable_blocks_[block]=false;++reclaimed_blocks;
    }
    next_free_block_=allocationStartBlock();error.clear();return true;
}

bool Filesystem::compact(CompactionReport& report,std::string& error,
                         ScanProgress progress) {
    report={};
    if(!collectGarbage(report.garbage_blocks_reclaimed,error,progress))return false;
    return compactFiles(report,error);
}

bool Filesystem::compactFiles(CompactionReport& report,std::string& error) {
    for(;;) {
        std::vector<std::size_t> candidates;
        for(std::size_t i=0;i<entries_.size();++i)
            if(!entries_[i].directory&&entries_[i].block_count&&!isDirectBootRom(entries_[i]))candidates.push_back(i);
        std::sort(candidates.begin(),candidates.end(),[this](std::size_t left,std::size_t right){
            return entries_[left].first_block>entries_[right].first_block;
        });
        bool relocated=false;
        for(const auto index:candidates) {
            const Entry original=entries_[index];std::size_t destination=0;
            if(!findBlankExtentBefore(original.first_block,original.block_count,destination,error)) {
                if(!error.empty())return false;
                continue;
            }
            std::vector<std::uint8_t> bytes;
            if(!readFile(original.name,bytes,error))return false;
            if(!programExtent(destination,bytes,error))return false;
            entries_[index].first_block=static_cast<std::uint32_t>(destination);
            if(!commit(error)) {
                entries_[index]=original;
                for(std::size_t i=0;i<original.block_count;++i)unavailable_blocks_[destination+i]=true;
                error="could not commit compacted live extent: "+error;return false;
            }
            // The second identical manifest generation makes both valid
            // superblocks reference the destination before the source is erased.
            if(!commit(error)){error="could not synchronize compacted live metadata: "+error;return false;}
            for(std::size_t i=0;i<original.block_count;++i) {
                const auto block=static_cast<std::size_t>(original.first_block)+i;
                unavailable_blocks_[block]=true;
                if(!flash_.prepareForErase(error)||!flash_.eraseBlock(block,error)) {
                    error="could not erase relocated source block "+std::to_string(block)+": "+error;return false;
                }
                unavailable_blocks_[block]=false;
            }
            ++report.files_relocated;report.blocks_relocated+=original.block_count;
            next_free_block_=allocationStartBlock();relocated=true;break;
        }
        if(!relocated)break;
    }
    error.clear();return true;
}

bool Filesystem::removeFile(const std::string& path,std::string& error) {
    const auto old=entries_;const auto it=std::find_if(entries_.begin(),entries_.end(),[&](const Entry& e){return e.name==path&&!e.directory;});
    if(it==entries_.end()){error="live file does not exist";return false;}
    if(isDirectBootRom(*it)){
        const auto programmed_blocks=static_cast<std::size_t>(it->block_count);
        const auto old=entries_;entries_.erase(it);
        if(!commit(error)){entries_=old;return false;}
        // The boot slot is reserved address space, not allocated ROM data.
        // Erasing its full capacity makes deleting a small ROM needlessly
        // erase hundreds of blocks that are already blank.
        for(std::size_t block=0;block<programmed_blocks;++block)
            if(!flash_.prepareForErase(error)||!flash_.eraseBlock(block,error)){
                error="boot ROM metadata was cleared but block "+std::to_string(block)+" could not be erased: "+error;return false;
            }
        next_free_block_=allocationStartBlock();error.clear();return true;
    }
    entries_.erase(it);if(commit(error))return true;entries_=old;return false;
}
bool Filesystem::removeDirectory(const std::string& path,std::string& error) {
    const auto old=entries_;const auto it=std::find_if(entries_.begin(),entries_.end(),[&](const Entry& e){return e.name==path&&e.directory;});
    if(it==entries_.end()){error="live directory does not exist";return false;}const auto prefix=path+'/';
    if(std::any_of(entries_.begin(),entries_.end(),[&](const Entry& e){return e.name.rfind(prefix,0)==0;})){error="live directory is not empty";return false;}
    entries_.erase(it);if(commit(error))return true;entries_=old;return false;
}
bool Filesystem::rename(const std::string& from,const std::string& to,std::string& error) {
    auto* source=find(from);if(!source||find(to)||!validPath(to)||!parentExists(to)){error="invalid live rename";return false;}
    if(isDirectBootRom(*source)){error="the direct-boot ROM at cartridge offset 0 is immutable";return false;}
    const auto old=entries_;
    if(source->directory){const auto prefix=from+'/';for(auto& entry:entries_)if(entry.name==from||entry.name.rfind(prefix,0)==0)entry.name=to+entry.name.substr(from.size());}
    else source->name=to;
    if(commit(error))return true;entries_=old;return false;
}

bool Filesystem::readFile(const std::string& path,std::vector<std::uint8_t>& bytes,
                          std::string& error) const {
    const auto* entry=find(path);if(!entry||entry->directory){error="live file does not exist";return false;}
    if(!readEntryRange(*entry,0,static_cast<std::size_t>(entry->size),bytes,error))return false;
    if(Crc32::calculate(bytes.data(),bytes.size())!=entry->crc32){error="live file checksum mismatch";return false;}
    error.clear();return true;
}

bool Filesystem::readFileRange(const std::string& path,std::size_t offset,
                               std::size_t size,std::vector<std::uint8_t>& bytes,
                               std::string& error) const {
    const auto* entry=find(path);
    if(!entry||entry->directory){error="live file does not exist";return false;}
    return readEntryRange(*entry,offset,size,bytes,error);
}

bool Filesystem::readEntryRange(const Entry& entry,std::size_t offset,
                                std::size_t size,std::vector<std::uint8_t>& bytes,
                                std::string& error,
                                const std::function<void()>& block_read) const {
    const auto file_size=static_cast<std::size_t>(entry.size);
    if(offset>file_size){error="live file read offset is out of bounds";return false;}
    const auto count=std::min(size,file_size-offset);bytes.clear();bytes.reserve(count);
    if(count==0){error.clear();return true;}
    const auto first=offset/NorFlash::block_size;
    const auto last=(offset+count-1)/NorFlash::block_size;
    std::vector<std::uint8_t> block(NorFlash::block_size);
    for(std::size_t index=first;index<=last;++index) {
        if(!flash_.read((entry.first_block+index)*NorFlash::block_size,
                        block.data(),block.size(),error))return false;
        const auto begin=index==first?offset%NorFlash::block_size:0;
        const auto end=index==last?((offset+count-1)%NorFlash::block_size)+1:
                                  NorFlash::block_size;
        bytes.insert(bytes.end(),block.begin()+static_cast<std::ptrdiff_t>(begin),
                     block.begin()+static_cast<std::ptrdiff_t>(end));
        if(block_read)block_read();
    }
    error.clear();return true;
}

bool Filesystem::verify(std::string& error,ScanProgress progress) const {
    std::size_t total=0;
    for(const auto& entry:entries_)
        if(!entry.directory)total+=dataBlockCount(entry.size);
    std::size_t completed=0;
    if(progress)progress(completed,total);
    for(const auto& entry:entries_) {
        if(entry.directory) continue;
        std::vector<std::uint8_t> bytes;
        if(!readEntryRange(entry,0,static_cast<std::size_t>(entry.size),bytes,error,
            [&]{if(progress)progress(++completed,total);}))return false;
        if(Crc32::calculate(bytes.data(),bytes.size())!=entry.crc32) {
            error="live file checksum mismatch";return false;
        }
    }
    error.clear();return true;
}

} // namespace ez3fs::live
