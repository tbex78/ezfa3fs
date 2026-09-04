#include "ez3fs/live_filesystem.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <set>

namespace ez3fs::live {
namespace {
constexpr std::array<std::uint8_t,8> magic{{'E','Z','3','L','I','V','E',0}};
constexpr std::uint16_t major=1, minor=0;
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
    progress<<"Reading EZ3FS-LIVE cartridge: 0%"<<std::flush;
    for(std::size_t offset=0;offset<capacity;offset+=block_size) {
        if(!storage.read(offset,bytes.data()+offset,block_size,error))return false;
        progress<<"\rReading EZ3FS-LIVE cartridge: "<<((offset+block_size)*100/capacity)<<"%"<<std::flush;
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

bool Filesystem::open(BlockDevice& flash,Filesystem& result,std::string& error) {
    bool found=false;std::uint64_t newest=0;std::size_t chosen=0;std::vector<Entry> entries;
    for(std::size_t block=0;block<2;++block) {
        std::vector<std::uint8_t> bytes(NorFlash::block_size);
        if(!flash.read(block*NorFlash::block_size,bytes.data(),bytes.size(),error))return false;
        if(!std::equal(magic.begin(),magic.end(),bytes.begin())||get<std::uint16_t>(bytes.data(),8)!=major||
           get<std::uint16_t>(bytes.data(),10)!=minor||get<std::uint32_t>(bytes.data(),28)!=commit_marker)continue;
        const auto length=get<std::uint32_t>(bytes.data(),20);
        if(length>NorFlash::block_size-superblock_header||
           Crc32::calculate(bytes.data()+superblock_header,length)!=get<std::uint32_t>(bytes.data(),24))continue;
        std::vector<Entry> parsed;std::set<std::string> names;
        const auto count=get<std::uint32_t>(bytes.data()+superblock_header,0);std::size_t offset=4;bool valid=true;
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
               (!entry.directory&&(entry.first_block<2||
                   static_cast<std::uint64_t>(entry.first_block)+entry.block_count>NorFlash::block_count||
                   entry.size>static_cast<std::uint64_t>(entry.block_count)*NorFlash::block_size))){valid=false;break;}
            parsed.push_back(std::move(entry));offset+=32+name_length;
        }
        if(!valid||offset!=length)continue;
        const auto generation=get<std::uint64_t>(bytes.data(),12);
        if(!found||generation>newest){found=true;newest=generation;chosen=block;entries=std::move(parsed);}
    }
    if(!found){error="no valid EZ3FS-LIVE superblock found";return false;}
    result.entries_=std::move(entries);result.generation_=newest;result.active_superblock_=chosen;result.next_free_block_=2;
    for(const auto& entry:result.entries_)result.next_free_block_=std::max(result.next_free_block_,static_cast<std::size_t>(entry.first_block+entry.block_count));
    error.clear();return true;
}

Entry* Filesystem::find(const std::string& path){auto it=std::find_if(entries_.begin(),entries_.end(),[&](const Entry& e){return e.name==path;});return it==entries_.end()?nullptr:&*it;}
const Entry* Filesystem::find(const std::string& path) const{auto it=std::find_if(entries_.begin(),entries_.end(),[&](const Entry& e){return e.name==path;});return it==entries_.end()?nullptr:&*it;}
bool Filesystem::parentExists(const std::string& path) const {
    const auto slash=path.rfind('/');return slash==std::string::npos||
        (find(path.substr(0,slash))&&find(path.substr(0,slash))->directory);
}

bool Filesystem::commit(std::string& error) {
    std::vector<std::uint8_t> manifest;manifest.resize(4);put<std::uint32_t>(manifest.data(),0,static_cast<std::uint32_t>(entries_.size()));
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
    const auto target=1-active_superblock_;if(!flash_.eraseBlock(target,error))return false;
    std::vector<std::uint8_t> block(NorFlash::block_size,0xFF);std::copy(magic.begin(),magic.end(),block.begin());
    put<std::uint16_t>(block.data(),8,major);put<std::uint16_t>(block.data(),10,minor);put<std::uint64_t>(block.data(),12,generation_+1);
    put<std::uint32_t>(block.data(),20,static_cast<std::uint32_t>(manifest.size()));
    put<std::uint32_t>(block.data(),24,Crc32::calculate(manifest.data(),manifest.size()));put<std::uint32_t>(block.data(),28,commit_marker);
    std::copy(manifest.begin(),manifest.end(),block.begin()+superblock_header);
    if(!flash_.program(target*NorFlash::block_size,block.data(),block.size(),error))return false;
    active_superblock_=target;++generation_;error.clear();return true;
}

std::size_t Filesystem::freeBlocks() const noexcept{return NorFlash::block_count-next_free_block_;}

bool Filesystem::createDirectory(const std::string& path,std::string& error) {
    if(!validPath(path)||!parentExists(path)||find(path)){error="invalid or existing live directory path";return false;}
    const auto old=entries_;entries_.push_back({path,0,0,0,0,0,true});
    if(commit(error))return true;entries_=old;return false;
}

bool Filesystem::putFile(const std::string& path,const std::vector<std::uint8_t>& bytes,
                         std::uint64_t modified_time,std::string& error) {
    if(!validPath(path)||!parentExists(path)){error="invalid live file path";return false;}
    const auto blocks=(bytes.size()+NorFlash::block_size-1)/NorFlash::block_size;
    if(blocks>freeBlocks()){error="live filesystem is out of free blocks; garbage collection is required";return false;}
    const auto old=entries_;const auto old_next=next_free_block_;auto* existing=find(path);
    if(existing&&existing->directory){error="live path is a directory";return false;}
    for(std::size_t i=0;i<blocks;++i){
        std::vector<std::uint8_t> block(NorFlash::block_size,0xFF);const auto begin=i*NorFlash::block_size;const auto count=std::min(NorFlash::block_size,bytes.size()-begin);
        std::copy_n(bytes.data()+begin,count,block.data());
        if(!flash_.program(next_free_block_*NorFlash::block_size,block.data(),block.size(),error)){
            // A failed NOR transaction may have programmed a prefix. Never
            // reuse that physical block, even though the manifest is rolled back.
            ++next_free_block_;entries_=old;return false;
        }
        ++next_free_block_;
    }
    Entry replacement{path,bytes.size(),modified_time,Crc32::calculate(bytes.data(),bytes.size()),static_cast<std::uint32_t>(old_next),static_cast<std::uint32_t>(blocks),false};
    if(existing)*existing=replacement;else entries_.push_back(std::move(replacement));
    if(commit(error))return true;entries_=old;return false;
}

bool Filesystem::removeFile(const std::string& path,std::string& error) {
    const auto old=entries_;const auto it=std::find_if(entries_.begin(),entries_.end(),[&](const Entry& e){return e.name==path&&!e.directory;});
    if(it==entries_.end()){error="live file does not exist";return false;}entries_.erase(it);if(commit(error))return true;entries_=old;return false;
}
bool Filesystem::removeDirectory(const std::string& path,std::string& error) {
    const auto old=entries_;const auto it=std::find_if(entries_.begin(),entries_.end(),[&](const Entry& e){return e.name==path&&e.directory;});
    if(it==entries_.end()){error="live directory does not exist";return false;}const auto prefix=path+'/';
    if(std::any_of(entries_.begin(),entries_.end(),[&](const Entry& e){return e.name.rfind(prefix,0)==0;})){error="live directory is not empty";return false;}
    entries_.erase(it);if(commit(error))return true;entries_=old;return false;
}

bool Filesystem::readFile(const std::string& path,std::vector<std::uint8_t>& bytes,
                          std::string& error) const {
    const auto* entry=find(path);if(!entry||entry->directory){error="live file does not exist";return false;}
    bytes.resize(static_cast<std::size_t>(entry->size));std::size_t done=0;
    for(std::uint32_t i=0;i<entry->block_count&&done<bytes.size();++i){
        std::vector<std::uint8_t> block(NorFlash::block_size);if(!flash_.read((entry->first_block+i)*NorFlash::block_size,block.data(),block.size(),error))return false;
        const auto count=std::min(block.size(),bytes.size()-done);std::copy_n(block.data(),count,bytes.data()+done);done+=count;
    }
    if(Crc32::calculate(bytes.data(),bytes.size())!=entry->crc32){error="live file checksum mismatch";return false;}
    error.clear();return true;
}

bool Filesystem::verify(std::string& error) const {
    for(const auto& entry:entries_) {
        if(entry.directory) continue;
        std::vector<std::uint8_t> bytes;
        if(!readFile(entry.name,bytes,error)) return false;
    }
    error.clear();return true;
}

} // namespace ez3fs::live
