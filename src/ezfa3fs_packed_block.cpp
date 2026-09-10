#include "ezfa3fs/packed_block.hpp"

#include "ezfa3fs/crc32.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <utility>

namespace ezfa3fs::live {
namespace {

constexpr std::array<std::uint8_t,8> packed_magic{
    {'E','Z','3','P','A','C','K',0}};
constexpr std::uint32_t packed_version=1;
constexpr std::size_t alignment=8;

std::size_t aligned(std::size_t size) noexcept {
    return (size+alignment-1)&~(alignment-1);
}

template<typename T>
void put(std::uint8_t* bytes,std::size_t offset,T value) {
    for(std::size_t index=0;index<sizeof(T);++index)
        bytes[offset+index]=
            static_cast<std::uint8_t>(value>>(index*8));
}

template<typename T>
T get(const std::uint8_t* bytes,std::size_t offset) {
    T value=0;
    for(std::size_t index=0;index<sizeof(T);++index)
        value|=static_cast<T>(bytes[offset+index])<<(index*8);
    return value;
}

}

std::size_t PackedBlock::encodedRecordSize(std::size_t byte_count) noexcept {
    if(byte_count>block_size)return block_size+1;
    return record_header_size+aligned(byte_count);
}

bool PackedBlock::canEncode(
    const std::vector<PackedRecord>& records) noexcept {

    std::size_t used=header_size;
    for(const auto& record:records) {
        const auto encoded=encodedRecordSize(record.bytes.size());
        if(used>block_size||record.id==0||record.bytes.empty()||
           encoded>block_size-used)
            return false;
        used+=encoded;
    }
    return !records.empty();
}

bool PackedBlock::encode(
    std::uint64_t generation,
    const std::vector<PackedRecord>& records,
    std::vector<std::uint8_t>& block,
    std::vector<PackedRecordLocation>& locations,
    std::string& error) {

    if(generation==0||records.size()>std::numeric_limits<std::uint32_t>::max()||
       !canEncode(records)) {
        error="packed block records do not fit";
        return false;
    }

    std::set<std::uint32_t> identifiers;
    block.assign(block_size,0xFF);
    std::fill_n(block.begin(),header_size,0);
    std::copy(packed_magic.begin(),packed_magic.end(),block.begin());
    put<std::uint32_t>(block.data(),8,packed_version);
    put<std::uint64_t>(block.data(),12,generation);
    put<std::uint32_t>(
        block.data(),20,static_cast<std::uint32_t>(records.size()));

    locations.clear();
    locations.reserve(records.size());
    std::size_t offset=header_size;

    for(const auto& record:records) {
        if(!identifiers.insert(record.id).second||
           record.bytes.size()>std::numeric_limits<std::uint32_t>::max()) {
            error="packed block contains an invalid record identifier or size";
            return false;
        }

        locations.push_back({record.id,static_cast<std::uint32_t>(offset)});
        put<std::uint32_t>(block.data(),offset,record.id);
        put<std::uint32_t>(
            block.data(),offset+4,
            static_cast<std::uint32_t>(record.bytes.size()));
        put<std::uint32_t>(
            block.data(),offset+8,
            Crc32::calculate(record.bytes.data(),record.bytes.size()));
        put<std::uint64_t>(block.data(),offset+16,record.modified_time);
        std::copy(
            record.bytes.begin(),record.bytes.end(),
            block.begin()+static_cast<std::ptrdiff_t>(
                offset+record_header_size));
        offset+=encodedRecordSize(record.bytes.size());
    }

    put<std::uint32_t>(
        block.data(),24,static_cast<std::uint32_t>(offset));
    put<std::uint32_t>(
        block.data(),28,
        Crc32::calculate(block.data()+header_size,offset-header_size));
    put<std::uint32_t>(block.data(),32,0);
    put<std::uint32_t>(
        block.data(),32,Crc32::calculate(block.data(),header_size));

    error.clear();
    return true;
}

bool PackedBlock::decode(
    const std::vector<std::uint8_t>& block,
    std::uint64_t& generation,
    std::vector<PackedRecord>& records,
    std::vector<PackedRecordLocation>& locations,
    std::string& error) {

    if(block.size()!=block_size||
       !std::equal(packed_magic.begin(),packed_magic.end(),block.begin())||
       get<std::uint32_t>(block.data(),8)!=packed_version) {
        error="invalid packed block header";
        return false;
    }

    std::vector<std::uint8_t> header(block.begin(),block.begin()+header_size);
    const auto header_crc=get<std::uint32_t>(header.data(),32);
    put<std::uint32_t>(header.data(),32,0);
    if(Crc32::calculate(header.data(),header.size())!=header_crc||
       !std::all_of(header.begin()+36,header.end(),
                    [](std::uint8_t byte){return byte==0;})) {
        error="packed block header checksum or reserved bytes are invalid";
        return false;
    }

    generation=get<std::uint64_t>(block.data(),12);
    const auto count=get<std::uint32_t>(block.data(),20);
    const auto used=get<std::uint32_t>(block.data(),24);
    if(generation==0||count==0||used<header_size||used>block_size||
       count>(used-header_size)/record_header_size||
       Crc32::calculate(block.data()+header_size,used-header_size)!=
           get<std::uint32_t>(block.data(),28)) {
        error="packed block bounds or payload checksum are invalid";
        return false;
    }

    records.clear();
    locations.clear();
    records.reserve(count);
    locations.reserve(count);
    std::set<std::uint32_t> identifiers;
    std::size_t offset=header_size;

    for(std::uint32_t index=0;index<count;++index) {
        if(offset>used||record_header_size>used-offset) {
            error="packed block record header is truncated";
            return false;
        }
        const auto id=get<std::uint32_t>(block.data(),offset);
        const auto size=get<std::uint32_t>(block.data(),offset+4);
        const auto crc=get<std::uint32_t>(block.data(),offset+8);
        const auto flags=get<std::uint32_t>(block.data(),offset+12);
        const auto modified=get<std::uint64_t>(block.data(),offset+16);
        const auto reserved=get<std::uint64_t>(block.data(),offset+24);
        const auto encoded=encodedRecordSize(size);
        if(id==0||!identifiers.insert(id).second||size==0||flags!=0||
           reserved!=0||encoded>used-offset) {
            error="packed block record is invalid";
            return false;
        }

        const auto begin=block.begin()+static_cast<std::ptrdiff_t>(
            offset+record_header_size);
        std::vector<std::uint8_t> bytes(begin,begin+size);
        if(Crc32::calculate(bytes.data(),bytes.size())!=crc) {
            error="packed block record checksum is invalid";
            return false;
        }
        records.push_back({id,modified,std::move(bytes)});
        locations.push_back({id,static_cast<std::uint32_t>(offset)});
        offset+=encoded;
    }

    if(offset!=used) {
        error="packed block used length does not match its records";
        return false;
    }

    error.clear();
    return true;
}

bool PackedBlockAllocator::buildPlan(
    const std::vector<PackedRecord>& records,
    std::vector<Bin>& bins,
    std::string& error) {

    bins.clear();
    std::vector<std::size_t> used_lengths;
    std::set<std::uint32_t> identifiers;
    for(std::size_t index=0;index<records.size();++index) {
        if(records[index].id==0||
           !identifiers.insert(records[index].id).second||
           records[index].bytes.empty()||
           records[index].bytes.size()>SmallFileAllocationPolicy::threshold) {
            error="packed allocation plan contains an ineligible record";
            return false;
        }

        const auto encoded=PackedBlock::encodedRecordSize(
            records[index].bytes.size());
        bool placed=false;
        for(std::size_t bin_index=0;bin_index<bins.size();++bin_index) {
            if(encoded<=PackedBlock::block_size-used_lengths[bin_index]) {
                bins[bin_index].push_back(index);
                used_lengths[bin_index]+=encoded;
                placed=true;
                break;
            }
        }
        if(!placed) {
            if(encoded>PackedBlock::block_size-PackedBlock::header_size) {
                error="packed record does not fit in one block";
                return false;
            }
            bins.push_back({index});
            used_lengths.push_back(PackedBlock::header_size+encoded);
        }
    }

    error.clear();
    return true;
}

} // namespace ezfa3fs::live
