#include "ez3fs/archive.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <set>

namespace ez3fs {
namespace {
constexpr std::array<std::uint8_t, 8> magic{{'E','Z','3','F','S','\r','\n',0x1A}};
constexpr std::array<std::uint8_t, 8> legacy_magic{{'E','Z','F','S','\r','\n',0x1A,'\n'}};
constexpr std::size_t header_size = 64;
constexpr std::size_t entry_size = 288;
constexpr std::size_t name_size = 256;
constexpr std::uint32_t directory_flag = 1;
constexpr std::uint16_t current_format_minor = 1;

template<typename T> void writeLe(std::vector<std::uint8_t>& b, std::size_t o, T v) {
    for (std::size_t i=0; i<sizeof(T); ++i) b[o+i]=static_cast<std::uint8_t>(v>>(i*8));
}
template<typename T> T readLe(const std::vector<std::uint8_t>& b, std::size_t o) {
    T v=0; for (std::size_t i=0; i<sizeof(T); ++i) v|=static_cast<T>(b[o+i])<<(i*8); return v;
}
std::uint64_t alignUp(std::uint64_t v, std::uint64_t a) { return ((v+a-1)/a)*a; }
bool validName(const std::string& name) {
    if (name.empty() || name.size()>=name_size || name.front()=='/' ||
        name.find('\\')!=std::string::npos || name.find('\0')!=std::string::npos) return false;
    std::size_t start=0;
    while (start<=name.size()) {
        const auto end=name.find('/',start);
        const auto part=name.substr(start,end-start);
        if (part.empty() || part=="." || part=="..") return false;
        if (end==std::string::npos) break;
        start=end+1;
    }
    return true;
}
} // namespace

std::uint32_t Crc32::calculate(const std::uint8_t* data, std::size_t size) noexcept {
    std::uint32_t crc=0xFFFFFFFFu;
    for (std::size_t i=0;i<size;++i) {
        crc^=data[i];
        for (unsigned bit=0;bit<8;++bit) crc=(crc>>1)^(0xEDB88320u&(0u-(crc&1u)));
    }
    return ~crc;
}

bool ImageBuilder::build(const std::vector<InputFile>& files, ArchiveImage& result,
                         std::string& error) const {
    result={}; error.clear();
    if (files.size()>std::numeric_limits<std::uint32_t>::max()) { error="too many files"; return false; }
    std::set<std::string> names;
    for (const auto& f:files) {
        if (!validName(f.name)) { error="invalid archive path: "+f.name; return false; }
        if (!names.insert(f.name).second) { error="duplicate archive path: "+f.name; return false; }
        if (f.directory && !f.bytes.empty()) { error="directory contains file data: "+f.name; return false; }
    }
    const std::uint64_t index_bytes=files.size()*entry_size;
    const std::uint64_t data_offset=alignUp(header_size+index_bytes,program_block_size);
    std::uint64_t data_end=data_offset;
    for (const auto& f:files) {
        if (f.directory) continue;
        if (data_end>cartridge_capacity || f.bytes.size()>cartridge_capacity-data_end) {
            error="archive contents exceed the 32-MiB cartridge capacity"; return false;
        }
        data_end+=f.bytes.size();
    }
    const auto image_size=alignUp(data_end,program_block_size);
    if (image_size>cartridge_capacity) { error="archive image exceeds the 32-MiB cartridge capacity"; return false; }
    result.bytes.assign(static_cast<std::size_t>(image_size),0xFF);
    std::copy(magic.begin(),magic.end(),result.bytes.begin());
    writeLe<std::uint16_t>(result.bytes,8,1); writeLe<std::uint16_t>(result.bytes,10,current_format_minor);
    writeLe<std::uint32_t>(result.bytes,12,header_size); writeLe<std::uint32_t>(result.bytes,16,entry_size);
    writeLe<std::uint32_t>(result.bytes,20,static_cast<std::uint32_t>(files.size()));
    writeLe<std::uint64_t>(result.bytes,24,header_size); writeLe<std::uint64_t>(result.bytes,32,data_offset);
    writeLe<std::uint64_t>(result.bytes,40,image_size);
    std::uint64_t file_offset=data_offset;
    for (std::size_t i=0;i<files.size();++i) {
        const auto& f=files[i]; const std::size_t base=header_size+i*entry_size;
        std::copy(f.name.begin(),f.name.end(),result.bytes.begin()+base); result.bytes[base+f.name.size()]=0;
        const auto offset=f.directory?0:file_offset;
        const auto crc=f.directory?0:Crc32::calculate(f.bytes.data(),f.bytes.size());
        writeLe<std::uint64_t>(result.bytes,base+256,offset);
        writeLe<std::uint64_t>(result.bytes,base+264,f.bytes.size());
        writeLe<std::uint32_t>(result.bytes,base+272,crc);
        writeLe<std::uint32_t>(result.bytes,base+276,f.directory?directory_flag:0u);
        if (!f.directory) {
            std::copy(f.bytes.begin(),f.bytes.end(),result.bytes.begin()+static_cast<std::ptrdiff_t>(file_offset));
            file_offset+=f.bytes.size();
        }
        result.entries.push_back({f.name,offset,f.bytes.size(),crc,f.directory});
    }
    writeLe<std::uint32_t>(result.bytes,48,Crc32::calculate(result.bytes.data()+header_size,index_bytes));
    writeLe<std::uint32_t>(result.bytes,52,0u);
    writeLe<std::uint32_t>(result.bytes,52,Crc32::calculate(result.bytes.data(),header_size));
    return true;
}

bool Archive::open(std::vector<std::uint8_t> image, std::string& error) {
    image_.clear(); entries_.clear(); error.clear();
    const bool recognized_magic=image.size()>=header_size&&
        (std::equal(magic.begin(),magic.end(),image.begin())||
         std::equal(legacy_magic.begin(),legacy_magic.end(),image.begin()));
    if (!recognized_magic) { error="not an EZ3FS image"; return false; }
    if (readLe<std::uint16_t>(image,8)!=1) { error="unsupported EZ3FS major format version"; return false; }
    const auto format_minor=readLe<std::uint16_t>(image,10);
    if (format_minor>current_format_minor) { error="unsupported EZ3FS minor format version"; return false; }
    if (readLe<std::uint32_t>(image,12)!=header_size || readLe<std::uint32_t>(image,16)!=entry_size) { error="unsupported EZ3FS structure size"; return false; }
    const auto stored_header_crc=readLe<std::uint32_t>(image,52);
    writeLe<std::uint32_t>(image,52,0u); const auto actual_header_crc=Crc32::calculate(image.data(),header_size);
    writeLe<std::uint32_t>(image,52,stored_header_crc);
    if (stored_header_crc!=actual_header_crc) { error="EZ3FS header checksum mismatch"; return false; }
    const auto count=readLe<std::uint32_t>(image,20); const auto index_offset=readLe<std::uint64_t>(image,24);
    const auto data_offset=readLe<std::uint64_t>(image,32); const auto image_size=readLe<std::uint64_t>(image,40);
    const auto index_crc=readLe<std::uint32_t>(image,48); const std::uint64_t index_bytes=static_cast<std::uint64_t>(count)*entry_size;
    if (index_offset!=header_size || index_bytes>image.size()-header_size || data_offset<index_offset+index_bytes ||
        data_offset>image.size() || data_offset%ImageBuilder::program_block_size!=0 ||
        image_size!=image.size() || image.size()<ImageBuilder::program_block_size ||
        image.size()%ImageBuilder::program_block_size!=0 ||
        image.size()>ImageBuilder::cartridge_capacity) { error="invalid EZ3FS image bounds"; return false; }
    if (Crc32::calculate(image.data()+header_size,index_bytes)!=index_crc) { error="EZ3FS index checksum mismatch"; return false; }
    std::set<std::string> names; std::uint64_t previous_end=data_offset;
    for (std::uint32_t i=0;i<count;++i) {
        const std::size_t base=header_size+static_cast<std::size_t>(i)*entry_size;
        const auto terminator=std::find(image.begin()+base,image.begin()+base+name_size,0);
        if (terminator==image.begin()+base+name_size) { error="unterminated EZ3FS entry name"; return false; }
        const std::string name(image.begin()+base,terminator); const auto offset=readLe<std::uint64_t>(image,base+256);
        const auto size=readLe<std::uint64_t>(image,base+264); const auto crc=readLe<std::uint32_t>(image,base+272);
        const auto flags=readLe<std::uint32_t>(image,base+276); const bool directory=(flags&directory_flag)!=0;
        if (!validName(name)||!names.insert(name).second||flags>directory_flag||
            (format_minor==0&&flags!=0)||
            (directory&&(offset!=0||size!=0||crc!=0))||
            (!directory&&(offset<previous_end||offset>image_size||size>image_size-offset))) {
            error="invalid EZ3FS file entry"; return false;
        }
        entries_.push_back({name,offset,size,crc,directory});
        if (!directory) previous_end=offset+size;
    }
    image_=std::move(image); return true;
}

bool Archive::verify(std::string& error) const {
    for (const auto& e:entries_) {
        if (e.directory) continue;
        if (Crc32::calculate(image_.data()+static_cast<std::size_t>(e.offset),static_cast<std::size_t>(e.size))!=e.crc32) {
            error="file checksum mismatch: "+e.name; return false;
        }
    }
    error.clear(); return true;
}

std::vector<InputFile> Archive::contents() const {
    std::vector<InputFile> result;
    result.reserve(entries_.size());
    for (const auto& entry:entries_) {
        std::vector<std::uint8_t> bytes;
        if (!entry.directory)
            bytes.assign(image_.begin()+static_cast<std::ptrdiff_t>(entry.offset),
                         image_.begin()+static_cast<std::ptrdiff_t>(entry.offset+entry.size));
        result.push_back({entry.name,std::move(bytes),entry.directory});
    }
    return result;
}

ArchiveEditor::ArchiveEditor(std::vector<InputFile> contents):contents_(std::move(contents)) {}

bool ArchiveEditor::parentExists(const std::string& path) const {
    const auto slash=path.rfind('/');
    if (slash==std::string::npos) return true;
    const auto parent=path.substr(0,slash);
    return std::any_of(contents_.begin(),contents_.end(),[&](const InputFile& item){
        return item.directory&&item.name==parent;
    });
}

bool ArchiveEditor::createDirectory(const std::string& path,std::string& error) {
    if (!validName(path)) { error="invalid directory path: "+path; return false; }
    if (!parentExists(path)) { error="parent directory does not exist: "+path; return false; }
    if (std::any_of(contents_.begin(),contents_.end(),[&](const InputFile& item){return item.name==path;})) {
        error="archive path already exists: "+path; return false;
    }
    contents_.push_back({path,{},true}); error.clear(); return true;
}

bool ArchiveEditor::putFile(const std::string& path,std::vector<std::uint8_t> bytes,std::string& error) {
    if (!validName(path)) { error="invalid file path: "+path; return false; }
    if (!parentExists(path)) { error="parent directory does not exist: "+path; return false; }
    auto found=std::find_if(contents_.begin(),contents_.end(),[&](const InputFile& item){return item.name==path;});
    if (found!=contents_.end()) {
        if (found->directory) { error="path is a directory: "+path; return false; }
        found->bytes=std::move(bytes); error.clear(); return true;
    }
    contents_.push_back({path,std::move(bytes),false}); error.clear(); return true;
}

bool ArchiveEditor::removeFile(const std::string& path,std::string& error) {
    const auto found=std::find_if(contents_.begin(),contents_.end(),[&](const InputFile& item){return item.name==path;});
    if (found==contents_.end()||found->directory) { error="file does not exist: "+path; return false; }
    contents_.erase(found); error.clear(); return true;
}

bool ArchiveEditor::removeDirectory(const std::string& path,std::string& error) {
    const auto found=std::find_if(contents_.begin(),contents_.end(),[&](const InputFile& item){return item.name==path;});
    if (found==contents_.end()||!found->directory) { error="directory does not exist: "+path; return false; }
    const auto prefix=path+'/';
    if (std::any_of(contents_.begin(),contents_.end(),[&](const InputFile& item){return item.name.rfind(prefix,0)==0;})) {
        error="directory is not empty: "+path; return false;
    }
    contents_.erase(found); error.clear(); return true;
}
} // namespace ez3fs
