#include "ezfa3fs/live_filesystem.hpp"
#include "ezfa3fs/cartridge_flash_geometry.hpp"
#include "ezfa3fs/packed_block.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <set>

namespace {
void requireAt(bool condition,int line) {
    if(!condition) {
        std::cerr<<"requirement failed at line "<<line<<'\n';
        std::abort();
    }
}
#define require(...) requireAt((__VA_ARGS__),__LINE__)

class CountingDevice final : public ezfa3fs::live::BlockDevice {
public:
    explicit CountingDevice(ezfa3fs::live::NorFlash& flash):flash_(flash) {}
    bool read(std::size_t offset,std::uint8_t* destination,std::size_t size,
              std::string& error) const override {
        ++read_count;return flash_.read(offset,destination,size,error);
    }
    bool program(std::size_t offset,const std::uint8_t* source,std::size_t size,
                 std::string& error) override {
        ++program_count;
        if(failed_program_calls.count(program_count))flash_.failNextProgramAfter(8);
        return flash_.program(offset,source,size,error);
    }
    bool programBlocks(std::size_t first_block,const std::uint8_t* source,
                       std::size_t block_count,std::size_t& completed_blocks,
                       std::string& error) override {
        ++extent_program_count;
        return BlockDevice::programBlocks(first_block,source,block_count,
                                          completed_blocks,error);
    }
    bool eraseBlock(std::size_t block,std::string& error) override {
        erased_blocks.push_back(block);
        return flash_.eraseBlock(block,error);
    }
    bool eraseBlocks(const std::vector<std::size_t>& blocks,
                     std::string& error) override {
        ++erase_batch_count;
        return BlockDevice::eraseBlocks(blocks,error);
    }
    bool replaceBlocks(std::size_t first_block,const std::uint8_t* source,
                       std::size_t block_count,
                       const std::vector<std::size_t>& erase_blocks,
                       std::size_t& completed_blocks,
                       std::string& error) override {
        ++extent_replace_count;
        return BlockDevice::replaceBlocks(first_block,source,block_count,
                                          erase_blocks,completed_blocks,error);
    }
    bool replaceMetadataBlock(std::size_t block,const std::uint8_t* source,
                              std::size_t size,std::string& error) override {
        ++replace_count;
        return BlockDevice::replaceMetadataBlock(block,source,size,error);
    }
    bool prepareForErase(std::string& error) override {
        ++prepare_erase_count;error.clear();return true;
    }
    bool prepareForProgram(std::string& error) override {
        ++prepare_program_count;error.clear();return true;
    }
    void failProgramCall(std::size_t call) { failed_program_calls.insert(call); }
    void failProgramCalls(std::size_t first,std::size_t count) {
        for(std::size_t i=0;i<count;++i)failed_program_calls.insert(first+i);
    }
    mutable std::size_t read_count = 0;
    std::size_t program_count = 0;
    std::size_t extent_program_count = 0;
    std::size_t replace_count = 0;
    std::size_t prepare_erase_count = 0;
    std::size_t prepare_program_count = 0;
    std::size_t erase_batch_count = 0;
    std::size_t extent_replace_count = 0;
    std::vector<std::size_t> erased_blocks;
private:
    ezfa3fs::live::NorFlash& flash_;
    std::set<std::size_t> failed_program_calls;
};

void putLittle(std::vector<std::uint8_t>& bytes,std::size_t offset,
               std::uint64_t value,std::size_t size) {
    for(std::size_t index=0;index<size;++index)
        bytes[offset+index]=static_cast<std::uint8_t>(value>>(index*8));
}

std::vector<std::uint8_t> blockData(std::size_t blocks,std::uint8_t value) {
    return std::vector<std::uint8_t>(blocks*ezfa3fs::live::NorFlash::block_size,value);
}

void verifyFormatIdentity() {
    constexpr const auto& current_magic=ezfa3fs::live::format_magic;
    ezfa3fs::live::NorFlash formatted;std::string error;
    require(ezfa3fs::live::Filesystem::format(formatted,error));
    std::vector<std::uint8_t> block(ezfa3fs::live::NorFlash::block_size);
    require(formatted.read(ezfa3fs::live::NorFlash::block_size,block.data(),block.size(),error));
    require(std::equal(current_magic.begin(),current_magic.end(),block.begin()));
    require(block[8]==0&&block[9]==0&&block[10]==3&&block[11]==0);
    ezfa3fs::live::Filesystem filesystem(formatted);
    require(ezfa3fs::live::Filesystem::open(
        formatted,filesystem,error));
    const auto identity=filesystem.formatIdentity();
    require(identity.layout==ezfa3fs::live::Layout::transactional&&
            identity.version==ezfa3fs::live::format_version&&
            identity.packed_storage);
}

void rewriteFormatMinor(ezfa3fs::live::NorFlash& flash,
                        std::size_t block_index,std::uint16_t minor) {
    std::string error;
    std::vector<std::uint8_t> block(ezfa3fs::live::NorFlash::block_size);
    require(flash.read(block_index*ezfa3fs::live::NorFlash::block_size,
                       block.data(),block.size(),error));
    putLittle(block,10,minor,2);
    require(flash.eraseBlock(block_index,error));
    require(flash.program(block_index*ezfa3fs::live::NorFlash::block_size,
                          block.data(),block.size(),error));
}

void verifyLegacyFormatsAreRejected() {
    ezfa3fs::live::NorFlash flash;std::string error;
    require(ezfa3fs::live::Filesystem::format(flash,error));
    rewriteFormatMinor(flash,1,1);
    ezfa3fs::live::Filesystem filesystem(flash);
    require(!ezfa3fs::live::Filesystem::open(flash,filesystem,error));
    require(error=="no valid EZFA3FS superblock found");

    ezfa3fs::live::NorFlash direct_boot;
    require(ezfa3fs::live::Filesystem::formatDirectBootEmpty(
        direct_boot,error));
    rewriteFormatMinor(direct_boot,
                       ezfa3fs::live::NorFlash::block_count-2,2);
    ezfa3fs::live::Filesystem direct_filesystem(direct_boot);
    require(!ezfa3fs::live::Filesystem::open(
        direct_boot,direct_filesystem,error));
    require(error=="no valid EZFA3FS superblock found");
}

void verifyPackedBlockCodecRejectsCorruption() {
    std::string error;
    const ezfa3fs::live::SmallFileAllocationPolicy policy;
    require(!policy.shouldPack(0)&&
            policy.shouldPack(policy.threshold)&&
            !policy.shouldPack(policy.threshold+1));
    const std::vector<ezfa3fs::live::PackedRecord> records{
        {1,10,{'a','b','c'}},
        {2,11,{'d','e'}}
    };
    std::vector<std::uint8_t> block;
    std::vector<ezfa3fs::live::PackedRecordLocation> locations;
    require(ezfa3fs::live::PackedBlock::encode(
        7,records,block,locations,error));
    std::uint64_t generation=0;
    std::vector<ezfa3fs::live::PackedRecord> decoded;
    std::vector<ezfa3fs::live::PackedRecordLocation> decoded_locations;
    require(ezfa3fs::live::PackedBlock::decode(
        block,generation,decoded,decoded_locations,error));
    require(generation==7&&decoded.size()==2&&
            decoded[0].bytes==records[0].bytes&&
            decoded[1].bytes==records[1].bytes&&
            decoded_locations.size()==locations.size()&&
            decoded_locations[0].id==locations[0].id&&
            decoded_locations[0].offset==locations[0].offset&&
            decoded_locations[1].id==locations[1].id&&
            decoded_locations[1].offset==locations[1].offset);

    block[locations.front().offset+
          ezfa3fs::live::PackedBlock::record_header_size]^=0x01;
    require(!ezfa3fs::live::PackedBlock::decode(
        block,generation,decoded,decoded_locations,error));
}

void verifyPhysicalEraseGeometry() {
    const auto bottom=ezfa3fs::CartridgeFlashGeometry::sectorsForLogicalBlock(0);
    require(bottom.size()==8&&bottom.front().window==0&&
            bottom.front().word_address==0&&bottom.back().word_address==0x7000);
    const auto ordinary=ezfa3fs::CartridgeFlashGeometry::sectorsForLogicalBlock(2);
    require(ordinary.size()==1&&ordinary.front().window==0&&
            ordinary.front().word_address==0x10000);
    const auto middle_top=
        ezfa3fs::CartridgeFlashGeometry::sectorsForLogicalBlock(255);
    require(middle_top.size()==8&&middle_top.front().window==1&&
            middle_top.front().word_address==0x3F8000&&
            middle_top.back().word_address==0x3FF000);

    const auto middle_bottom=
        ezfa3fs::CartridgeFlashGeometry::sectorsForLogicalBlock(256);
    require(middle_bottom.size()==8&&middle_bottom.front().window==2&&
            middle_bottom.front().word_address==0&&
            middle_bottom.back().word_address==0x7000);

    const auto top=ezfa3fs::CartridgeFlashGeometry::sectorsForLogicalBlock(511);
    require(top.size()==8&&top.front().window==3&&
            top.front().word_address==0x3F8000&&top.back().word_address==0x3FF000);
    const auto top_prefix=ezfa3fs::CartridgeFlashGeometry::sectorsCoveringBlockPrefix(
        511,ezfa3fs::CartridgeFlashGeometry::boot_sector_size);
    require(top_prefix.size()==1&&top_prefix.front().window==3&&
            top_prefix.front().word_address==0x3F8000);
}

void verifyDirectBootLayout() {
    ezfa3fs::live::NorFlash flash;std::string error;
    const std::vector<std::uint8_t> rom{0x18,0x00,0x00,0xEA,0x42,0x47,0x41};
    require(ezfa3fs::live::Filesystem::formatDirectBoot(flash,"direct.gba",rom,1234,error));
    std::vector<std::uint8_t> bytes(rom.size());
    require(flash.read(0,bytes.data(),bytes.size(),error));
    require(bytes==rom);
    std::vector<std::uint8_t> superblock(ezfa3fs::live::NorFlash::block_size);
    require(flash.read((ezfa3fs::live::NorFlash::block_count-2)*ezfa3fs::live::NorFlash::block_size,
                       superblock.data(),superblock.size(),error));
    require(std::equal(ezfa3fs::live::direct_boot_format_magic.begin(),
                       ezfa3fs::live::direct_boot_format_magic.end(),superblock.begin()));
    require(superblock[8]==0&&superblock[9]==0&&
            superblock[10]==4&&superblock[11]==0);
    ezfa3fs::live::Filesystem filesystem(flash);
    require(ezfa3fs::live::Filesystem::open(flash,filesystem,error));
    const auto identity=filesystem.formatIdentity();
    require(identity.layout==ezfa3fs::live::Layout::direct_boot&&
            identity.version==ezfa3fs::live::direct_boot_format_version&&
            identity.packed_storage);
    require(filesystem.isDirectBoot()&&filesystem.entries().size()==1&&
            filesystem.entries().front().first_block==0);
    require(filesystem.readFile("direct.gba",bytes,error)&&bytes==rom);
    require(filesystem.createDirectory("extras",error));
    require(filesystem.putFile("extras/readme.txt",{'o','k'},1235,error));
    const auto extra=std::find_if(filesystem.entries().begin(),filesystem.entries().end(),
        [](const ezfa3fs::live::Entry& entry){return entry.name=="extras/readme.txt";});
    require(extra!=filesystem.entries().end()&&extra->first_block==1);
    require(filesystem.readFile("direct.gba",bytes,error)&&bytes==rom);
    const auto batch_generation=filesystem.generation();
    require(filesystem.putFiles({
        {"batch-a.txt",{'a'},1236},
        {"batch-b.txt",{'b'},1237}
    },error));
    require(filesystem.generation()==batch_generation+1);
    ezfa3fs::live::Filesystem reopened(flash);
    require(ezfa3fs::live::Filesystem::open(flash,reopened,error));
    require(reopened.generation()==filesystem.generation());
    require(reopened.entries().size()==5);
    require(reopened.readFile("extras/readme.txt",bytes,error));
    require(bytes==std::vector<std::uint8_t>({'o','k'}));
    require(reopened.readFile("batch-a.txt",bytes,error)&&
            bytes==std::vector<std::uint8_t>({'a'}));
    require(reopened.readFile("batch-b.txt",bytes,error)&&
            bytes==std::vector<std::uint8_t>({'b'}));
    require(!filesystem.putFile("direct.gba",rom,1236,error));
    require(error.find("immutable")!=std::string::npos);
    require(filesystem.removeFile("extras/readme.txt",error));
    std::size_t reclaimed=0;require(filesystem.collectGarbage(reclaimed,error)&&reclaimed==1);
    require(filesystem.readFile("direct.gba",bytes,error)&&bytes==rom);
    require(filesystem.removeFile("direct.gba",error));
    require(!filesystem.canCreateFile(".DS_Store",error));
    require(filesystem.putFile("replacement.gba",rom,1237,error));
    require(filesystem.entries().front().name=="replacement.gba");
    require(filesystem.readFile("replacement.gba",bytes,error)&&bytes==rom);
    require(filesystem.verify(error));
}

void verifyEmptyDirectBootLayout() {
    ezfa3fs::live::NorFlash flash;std::string error;
    require(ezfa3fs::live::Filesystem::formatDirectBootEmpty(flash,error));
    ezfa3fs::live::Filesystem filesystem(flash);
    require(ezfa3fs::live::Filesystem::open(flash,filesystem,error));
    require(filesystem.activeSuperblock()==
            ezfa3fs::live::NorFlash::block_count-2);
    require(filesystem.isDirectBoot()&&filesystem.entries().empty());
    require(filesystem.freeBlocks()==ezfa3fs::live::NorFlash::block_count-3);
    require(!filesystem.canCreateFile(".DS_Store",error));
    require(error.find("root-level .gba")!=std::string::npos);
    const std::vector<std::uint8_t> rom{0x18,0x00,0x00,0xEA,0x44};
    require(filesystem.putFile("first.GBA",rom,1234,error));
    std::vector<std::uint8_t> bytes(rom.size());
    require(flash.read(0,bytes.data(),bytes.size(),error)&&bytes==rom);
    require(filesystem.entries().size()==1&&filesystem.entries().front().first_block==0);
    require(filesystem.freeBlocks()==ezfa3fs::live::NorFlash::block_count-3);
    require(filesystem.putFile("second.gba",rom,1235,error));
    const auto second=std::find_if(
        filesystem.entries().begin(),
        filesystem.entries().end(),
        [](const ezfa3fs::live::Entry& entry) {
            return entry.name=="second.gba";
        });
    require(second!=filesystem.entries().end()&&second->first_block==1);
    require(!filesystem.putFile("first.GBA",rom,1236,error));
    require(error.find("immutable")!=std::string::npos);

    ezfa3fs::live::NorFlash growing_flash;
    require(ezfa3fs::live::Filesystem::formatDirectBootEmpty(growing_flash,error,1));
    ezfa3fs::live::Filesystem growing(growing_flash);
    require(ezfa3fs::live::Filesystem::open(growing_flash,growing,error));
    const auto larger_rom=blockData(2,0x5A);
    require(growing.putFile("larger.gba",larger_rom,1236,error));
    require(growing.entries().front().block_count==2);
    require(growing.removeFile("larger.gba",error));
    require(growing.putFile("another.gba",blockData(3,0xA5),1237,error));
    require(growing.entries().front().block_count==3);
    require(growing.readFile("another.gba",bytes,error));
    require(bytes==blockData(3,0xA5));
}

void verifyDirectBootDeleteInvalidatesOnlyFirstBlock() {
    ezfa3fs::live::NorFlash flash;std::string error;
    require(ezfa3fs::live::Filesystem::formatDirectBootEmpty(flash,error,256));
    CountingDevice device(flash);
    ezfa3fs::live::Filesystem filesystem(device);
    require(ezfa3fs::live::Filesystem::open(device,filesystem,error));
    const auto old_rom=blockData(3,0x00);
    require(filesystem.putFile("old.gba",old_rom,1234,error));
    require(filesystem.entries().front().block_count==3);
    require(filesystem.freeBlocks()==ezfa3fs::live::NorFlash::block_count-5);
    const auto prepared_before_delete=device.prepare_erase_count;
    const auto block0_erases=std::count(device.erased_blocks.begin(),
                                        device.erased_blocks.end(),0);
    const auto block1_erases=std::count(device.erased_blocks.begin(),
                                        device.erased_blocks.end(),1);
    const auto block2_erases=std::count(device.erased_blocks.begin(),
                                        device.erased_blocks.end(),2);
    require(filesystem.removeFile("old.gba",error));
    require(device.prepare_erase_count==prepared_before_delete+1);
    require(std::count(device.erased_blocks.begin(),device.erased_blocks.end(),0)==
            block0_erases+1);
    require(std::count(device.erased_blocks.begin(),device.erased_blocks.end(),1)==
            block1_erases);
    require(std::count(device.erased_blocks.begin(),device.erased_blocks.end(),2)==
            block2_erases);
    require(filesystem.awaitsDirectBootRom());

    // Replacement preflight erases stale blocks that it will overwrite, but
    // does not spend time erasing an old tail beyond the replacement extent.
    const auto replacement=blockData(2,0xF0);
    const auto prepared_programs=device.prepare_program_count;
    const auto replacements=device.extent_replace_count;
    const auto erase_batches=device.erase_batch_count;
    require(filesystem.putFile("new.gba",replacement,1235,error));
    require(device.extent_replace_count==replacements+1);
    require(device.prepare_program_count==prepared_programs+1);
    require(device.erase_batch_count==erase_batches+1);
    require(std::count(device.erased_blocks.begin(),device.erased_blocks.end(),1)==
            block1_erases+1);
    require(std::count(device.erased_blocks.begin(),device.erased_blocks.end(),2)==
            block2_erases);
    std::vector<std::uint8_t> bytes;
    require(filesystem.readFile("new.gba",bytes,error)&&bytes==replacement);
    require(flash.read(2*ezfa3fs::live::NorFlash::block_size,bytes.data(),
                       ezfa3fs::live::NorFlash::block_size,error));
    require(bytes.front()==0x00);
}

void verifyInterruptedCompaction(std::size_t failure_offset,
                                 std::uint32_t recovered_block,
                                 std::uint64_t generation_advance) {
    ezfa3fs::live::NorFlash flash;std::string error;
    require(ezfa3fs::live::Filesystem::format(flash,error));
    CountingDevice device(flash);ezfa3fs::live::Filesystem filesystem(device);
    require(ezfa3fs::live::Filesystem::open(device,filesystem,error));
    require(filesystem.putFile("movable",blockData(1,0x11),1,error));
    require(filesystem.putFile("fixed",blockData(1,0x22),1,error));
    require(filesystem.putFile("movable",blockData(1,0x33),2,error));
    std::size_t reclaimed=0;require(filesystem.collectGarbage(reclaimed,error));
    require(reclaimed==1);
    const auto generation=filesystem.generation();
    device.failProgramCall(device.program_count+failure_offset);
    ezfa3fs::live::CompactionReport report;
    require(!filesystem.compact(report,error));

    CountingDevice recovered_device(flash);ezfa3fs::live::Filesystem recovered(recovered_device);
    require(ezfa3fs::live::Filesystem::open(recovered_device,recovered,error));
    require(recovered.generation()==generation+generation_advance);
    const auto entry=std::find_if(recovered.entries().begin(),recovered.entries().end(),
        [](const ezfa3fs::live::Entry& candidate){return candidate.name=="movable";});
    require(entry!=recovered.entries().end()&&entry->first_block==recovered_block);
    std::vector<std::uint8_t> bytes;
    require(recovered.readFile("movable",bytes,error));
    require(bytes==blockData(1,0x33));
    require(recovered.verify(error));
}

void verifyPackedSmallFileCopyOnWriteAndCompaction() {
    ezfa3fs::live::NorFlash flash;std::string error;
    require(ezfa3fs::live::Filesystem::format(flash,error));
    ezfa3fs::live::Filesystem filesystem(flash);
    require(ezfa3fs::live::Filesystem::open(flash,filesystem,error));

    const auto small=[](std::uint8_t value) {
        return std::vector<std::uint8_t>(
            ezfa3fs::live::SmallFileAllocationPolicy::threshold,value);
    };
    require(filesystem.putFiles({
        {"alpha",small(0x11),1},
        {"beta",small(0x22),2},
        {"gamma",small(0x33),3}
    },error));
    const auto full_block_generation=filesystem.entries().front().packed_generation;
    require(filesystem.putFile("delta",small(0x44),4,error));
    require(filesystem.entries().size()==4);
    require(std::all_of(
        filesystem.entries().begin(),filesystem.entries().end(),
        [](const ezfa3fs::live::Entry& entry) {
            return entry.storage==ezfa3fs::live::StorageType::packed&&
                   entry.block_count==1&&entry.packed_generation!=0&&
                   entry.packed_record_id!=0;
        }));
    require(filesystem.entries()[0].first_block==2&&
            filesystem.entries()[1].first_block==2&&
            filesystem.entries()[2].first_block==2&&
            filesystem.entries()[3].first_block==3&&
            filesystem.entries()[0].packed_generation==full_block_generation);

    require(filesystem.putFile("gamma",small(0x55),5,error));
    require(filesystem.entries()[0].first_block==4&&
            filesystem.entries()[1].first_block==4&&
            filesystem.entries()[2].first_block==4&&
            filesystem.entries()[0].packed_generation>
                full_block_generation);

    ezfa3fs::live::SpaceReport before;
    require(filesystem.inspectSpace(before,error));
    require(before.active_blocks==2);
    require(filesystem.removeFile("alpha",error));
    require(filesystem.removeFile("beta",error));
    require(filesystem.rename("gamma","renamed",error));

    ezfa3fs::live::CompactionReport report;
    require(filesystem.compact(report,error));
    require(report.files_relocated==2&&report.blocks_relocated==2);
    require(filesystem.entries().size()==2&&
            filesystem.entries()[0].first_block==
                filesystem.entries()[1].first_block);

    std::vector<std::uint8_t> bytes;
    require(filesystem.readFileRange("renamed",100,10,bytes,error));
    require(bytes==std::vector<std::uint8_t>(10,0x55));
    require(filesystem.readFile("delta",bytes,error)&&bytes==small(0x44));

    ezfa3fs::live::Filesystem reopened(flash);
    require(ezfa3fs::live::Filesystem::open(flash,reopened,error));
    require(reopened.packedStorageEnabled());
    require(reopened.verify(error));
}

void verifyAlternateExtentRetry() {
    ezfa3fs::live::NorFlash flash;std::string error;
    require(ezfa3fs::live::Filesystem::format(flash,error));
    CountingDevice device(flash);ezfa3fs::live::Filesystem filesystem(device);
    require(ezfa3fs::live::Filesystem::open(device,filesystem,error));
    device.failProgramCall(device.program_count+1);
    const auto expected=blockData(2,0x5A);
    require(filesystem.putFile("retried",expected,1,error));
    require(device.extent_program_count==2);
    const auto entry=std::find_if(filesystem.entries().begin(),filesystem.entries().end(),
        [](const ezfa3fs::live::Entry& candidate){return candidate.name=="retried";});
    require(entry!=filesystem.entries().end()&&entry->first_block==3);
    std::vector<std::uint8_t> bytes;
    require(filesystem.readFile("retried",bytes,error));
    require(bytes==expected);
    require(filesystem.verify(error));
}

void verifyBatchProgramming() {
    ezfa3fs::live::NorFlash flash;
    std::string error;
    require(ezfa3fs::live::Filesystem::format(flash,error));
    CountingDevice device(flash);
    ezfa3fs::live::Filesystem filesystem(device);
    require(ezfa3fs::live::Filesystem::open(device,filesystem,error));

    const auto generation=filesystem.generation();
    const auto prepared=device.prepare_program_count;
    const auto programs=device.extent_program_count;
    const auto metadata=device.replace_count;
    const auto large=blockData(2,0x5A);
    const std::vector<ezfa3fs::live::FileWrite> files{
        {"first.bin",{'a','b','c'},10},
        {"empty.txt",{},11},
        {"large.bin",large,12}
    };

    require(filesystem.putFiles(files,error));
    require(filesystem.generation()==generation+1);
    require(device.prepare_program_count==prepared+1);
    require(device.extent_program_count==programs+1);
    require(device.replace_count==metadata+1);

    const auto find=[&](const std::string& name) {
        return std::find_if(filesystem.entries().begin(),
                            filesystem.entries().end(),
            [&](const ezfa3fs::live::Entry& entry) {
                return entry.name==name;
            });
    };
    const auto first=find("first.bin");
    const auto empty=find("empty.txt");
    const auto large_entry=find("large.bin");
    require(first!=filesystem.entries().end()&&first->first_block==2&&
            first->block_count==1&&
            first->storage==ezfa3fs::live::StorageType::packed);
    require(empty!=filesystem.entries().end()&&empty->first_block==3&&
            empty->block_count==0);
    require(large_entry!=filesystem.entries().end()&&
            large_entry->first_block==3&&large_entry->block_count==2);

    std::vector<std::uint8_t> bytes;
    require(filesystem.readFile("first.bin",bytes,error)&&
            bytes==std::vector<std::uint8_t>({'a','b','c'}));
    require(filesystem.readFile("empty.txt",bytes,error)&&bytes.empty());
    require(filesystem.readFile("large.bin",bytes,error)&&bytes==large);
    require(filesystem.verify(error));

    const auto unchanged_generation=filesystem.generation();
    require(!filesystem.putFiles({
        {"duplicate",{'a'},20},
        {"duplicate",{'b'},21}
    },error));
    require(error.find("duplicate")!=std::string::npos);
    require(filesystem.generation()==unchanged_generation);
}

void verifyFailedBatchIsNotPublished() {
    ezfa3fs::live::NorFlash flash;
    std::string error;
    require(ezfa3fs::live::Filesystem::format(flash,error));
    CountingDevice device(flash);
    ezfa3fs::live::Filesystem filesystem(device);
    require(ezfa3fs::live::Filesystem::open(device,filesystem,error));

    const auto generation=filesystem.generation();
    device.failProgramCalls(device.program_count+1,3);
    require(!filesystem.putFiles({
        {"first",{'a'},1},
        {"second",{'b'},2}
    },error));
    require(filesystem.generation()==generation);
    require(filesystem.entries().empty());

    ezfa3fs::live::Filesystem reopened(device);
    require(ezfa3fs::live::Filesystem::open(device,reopened,error));
    require(reopened.generation()==generation);
    require(reopened.entries().empty());
}

void verifyAutomaticGarbageCollection() {
    ezfa3fs::live::NorFlash flash;std::string error;
    require(ezfa3fs::live::Filesystem::format(flash,error));
    CountingDevice device(flash);ezfa3fs::live::Filesystem filesystem(device);
    require(ezfa3fs::live::Filesystem::open(device,filesystem,error));
    require(filesystem.putFile("replaceable",blockData(255,0x11),1,error));
    require(filesystem.putFile("replaceable",blockData(255,0x22),2,error));
    std::vector<ezfa3fs::live::MaintenanceAction> actions;
    require(filesystem.putFile("replaceable",blockData(255,0x33),3,error,
        [&](ezfa3fs::live::MaintenanceAction action){actions.push_back(action);}));
    require(actions==std::vector<ezfa3fs::live::MaintenanceAction>{
        ezfa3fs::live::MaintenanceAction::garbage_collection});
    const auto entry=std::find_if(filesystem.entries().begin(),filesystem.entries().end(),
        [](const ezfa3fs::live::Entry& candidate){return candidate.name=="replaceable";});
    require(entry!=filesystem.entries().end()&&entry->first_block==2);
    actions.clear();
    require(!filesystem.putFile("too-large",blockData(256,0x44),4,error,
        [&](ezfa3fs::live::MaintenanceAction action){actions.push_back(action);}));
    require(error.find("out of free blocks")!=std::string::npos);
    require(actions==std::vector<ezfa3fs::live::MaintenanceAction>{
        ezfa3fs::live::MaintenanceAction::garbage_collection});
    require(filesystem.verify(error));
}

void verifyCooperativeGarbageCollectionBatching() {
    ezfa3fs::live::NorFlash flash;
    std::string error;
    require(ezfa3fs::live::Filesystem::format(flash,error));
    CountingDevice device(flash);
    ezfa3fs::live::Filesystem filesystem(device);
    require(ezfa3fs::live::Filesystem::open(device,filesystem,error));
    require(filesystem.putFile("replaceable",blockData(3,0x11),1,error));
    require(filesystem.putFile("replaceable",blockData(3,0x22),2,error));

    const auto erase_batches=device.erase_batch_count;
    ezfa3fs::live::GarbageCollectionState state;
    bool complete=false;

    // Accumulate the first stale extent, then simulate a mutation while the
    // cooperative scan is in progress. Resynchronization must discard those
    // decisions, rescan from the beginning, and include both stale extents in
    // the one final batch.
    for(std::size_t steps=0;steps<10;++steps)
        require(filesystem.collectGarbageStep(
            state,
            false,
            complete,
            error));

    require(!complete);
    require(filesystem.putFile("replaceable",blockData(3,0x33),3,error));
    require(filesystem.collectGarbageStep(
        state,
        true,
        complete,
        error));

    for(std::size_t steps=0;
        !complete&&steps<=ezfa3fs::live::NorFlash::block_count*2;
        ++steps) {

        require(filesystem.collectGarbageStep(
            state,
            false,
            complete,
            error));
    }

    require(complete);
    require(state.reclaimed_blocks==6);
    require(state.last_reclaimed_blocks==
            std::vector<std::size_t>{2,3,4,5,6,7});
    require(device.erase_batch_count==erase_batches+1);
}

void verifyAutomaticCompaction() {
    ezfa3fs::live::NorFlash flash;std::string error;
    require(ezfa3fs::live::Filesystem::format(flash,error));
    CountingDevice device(flash);ezfa3fs::live::Filesystem filesystem(device);
    require(ezfa3fs::live::Filesystem::open(device,filesystem,error));
    const auto extent=blockData(100,0x44);
    require(filesystem.putFile("first",extent,1,error));
    require(filesystem.putFile("hole",extent,1,error));
    require(filesystem.putFile("movable",extent,1,error));
    require(filesystem.removeFile("hole",error));
    std::size_t reclaimed=0;require(filesystem.collectGarbage(reclaimed,error));
    require(reclaimed==100);

    std::vector<ezfa3fs::live::MaintenanceAction> actions;
    require(filesystem.putFile("large",blockData(220,0x55),2,error,
        [&](ezfa3fs::live::MaintenanceAction action){actions.push_back(action);}));
    require(actions==std::vector<ezfa3fs::live::MaintenanceAction>{
        ezfa3fs::live::MaintenanceAction::garbage_collection,
        ezfa3fs::live::MaintenanceAction::compaction});
    const auto moved=std::find_if(filesystem.entries().begin(),filesystem.entries().end(),
        [](const ezfa3fs::live::Entry& candidate){return candidate.name=="movable";});
    const auto large=std::find_if(filesystem.entries().begin(),filesystem.entries().end(),
        [](const ezfa3fs::live::Entry& candidate){return candidate.name=="large";});
    require(moved!=filesystem.entries().end()&&moved->first_block==102);
    require(large!=filesystem.entries().end()&&large->first_block==202);
    require(filesystem.verify(error));
}
}

int main()
{
    verifyFormatIdentity();
    verifyLegacyFormatsAreRejected();
    verifyPackedBlockCodecRejectsCorruption();
    verifyPhysicalEraseGeometry();
    verifyDirectBootLayout();
    verifyEmptyDirectBootLayout();
    verifyDirectBootDeleteInvalidatesOnlyFirstBlock();
    // Losing power while either metadata generation is being updated leaves a
    // complete source or destination extent referenced by the newest valid one.
    verifyInterruptedCompaction(2,4,0);
    verifyInterruptedCompaction(3,2,1);
    verifyAlternateExtentRetry();
    verifyPackedSmallFileCopyOnWriteAndCompaction();
    verifyBatchProgramming();
    verifyFailedBatchIsNotPublished();
    verifyAutomaticGarbageCollection();
    verifyCooperativeGarbageCollectionBatching();
    verifyAutomaticCompaction();

    ezfa3fs::live::NorFlash flash;std::string error;
    require(ezfa3fs::live::Filesystem::format(flash,error));
    CountingDevice device(flash);ezfa3fs::live::Filesystem filesystem(device);
    require(ezfa3fs::live::Filesystem::open(device,filesystem,error));
    require(device.read_count==2);
    require(filesystem.generation()==1);
    const auto replacements_before_create=device.replace_count;
    require(filesystem.createDirectory("docs",error));
    require(device.replace_count==replacements_before_create+1);
    require(filesystem.putFile("docs/readme.txt",{'o','k'},1234,error));
    std::vector<std::uint8_t> bytes;
    require(filesystem.readFile("docs/readme.txt",bytes,error));
    require(bytes==std::vector<std::uint8_t>({'o','k'}));
    std::vector<std::uint8_t> large(ezfa3fs::live::NorFlash::block_size+4);
    for(std::size_t i=0;i<large.size();++i)large[i]=static_cast<std::uint8_t>(i);
    require(filesystem.putFile("docs/large.bin",large,1234,error));
    require(filesystem.readFileRange("docs/large.bin",
        ezfa3fs::live::NorFlash::block_size-2,6,bytes,error));
    require(bytes==std::vector<std::uint8_t>({0xFE,0xFF,0x00,0x01,0x02,0x03}));
    const auto generation=filesystem.generation();
    require(filesystem.putFile("docs/readme.txt",{'n','e','w'},1235,error));
    require(filesystem.generation()>generation);

    const auto free_before_data_failure=filesystem.freeBlocks();
    device.failProgramCalls(device.program_count+1,3);
    require(!filesystem.putFile("docs/partial.txt",{'x'},1236,error));
    require(filesystem.freeBlocks()==free_before_data_failure-3);

    const auto before=filesystem.generation();
    const auto free_before=filesystem.freeBlocks();
    flash.failNextProgramAfter(8);
    require(!filesystem.createDirectory("interrupted",error));
    require(filesystem.freeBlocks()==free_before);
    CountingDevice reopened_device(flash);ezfa3fs::live::Filesystem reopened(reopened_device);
    require(ezfa3fs::live::Filesystem::open(reopened_device,reopened,error));
    require(reopened_device.read_count==2);
    require(reopened.generation()==before);
    require(reopened.entries().size()==3);
    require(reopened.freeBlocks()==free_before_data_failure);
    require(reopened.putFile("docs/recovered.txt",{'y'},1237,error));
    const auto recovered=std::find_if(reopened.entries().begin(),reopened.entries().end(),
        [](const ezfa3fs::live::Entry& entry){return entry.name=="docs/recovered.txt";});
    require(recovered!=reopened.entries().end()&&recovered->first_block==9);
    ezfa3fs::live::SpaceReport space;std::size_t inspected=0,inspection_total=0;
    require(reopened.inspectSpace(space,error,
        [&](std::size_t completed,std::size_t total){inspected=completed;inspection_total=total;}));
    require(space.active_blocks==3);
    require(space.erased_blocks==502);
    require(space.reclaimable_blocks==5);
    require(space.largest_erased_extent==502);
    require(space.largest_post_gc_extent==502);
    require(inspected==510&&inspection_total==510);
    std::size_t reclaimed=0,progress_completed=0,progress_total=0;
    const auto prepares_before_gc=reopened_device.prepare_erase_count;
    const auto batches_before_gc=reopened_device.erase_batch_count;

    require(reopened.collectGarbage(reclaimed,error,
        [&](std::size_t completed,std::size_t total){
            progress_completed=completed;
            progress_total=total;
        }));

    require(reclaimed==5);

    // Garbage collection now amortizes the cartridge writer transition across
    // the complete stale-block batch instead of restarting it once per block.
    require(reopened_device.prepare_erase_count==prepares_before_gc+1);
    require(reopened_device.erase_batch_count==batches_before_gc+1);

    require(progress_completed==510&&progress_total==510);
    std::vector<std::pair<std::size_t,std::size_t>> verification_progress;
    require(reopened.verify(error,[&](std::size_t completed,std::size_t total) {
        verification_progress.emplace_back(completed,total);
    }));
    require(verification_progress.front()==std::pair<std::size_t,std::size_t>{0,3});
    require(verification_progress.back()==std::pair<std::size_t,std::size_t>{3,3});
    require(verification_progress.size()==4);
    require(reopened.putFile("docs/recycled.txt",{'z'},1238,error));
    const auto recycled=std::find_if(reopened.entries().begin(),reopened.entries().end(),
        [](const ezfa3fs::live::Entry& entry){return entry.name=="docs/recycled.txt";});
    // GC preserves the hot allocation cursor, so newly written data keeps
    // moving forward instead of immediately jumping back into reclaimed space.
    require(recycled!=reopened.entries().end()&&recycled->first_block==10);
    const auto generation_before_compaction=reopened.generation();
    const auto prepares_before_compaction=
        reopened_device.prepare_erase_count;
    const auto batches_before_compaction=
        reopened_device.erase_batch_count;

    ezfa3fs::live::CompactionReport compaction;
    require(reopened.compact(compaction,error));
    require(compaction.garbage_blocks_reclaimed==1);
    // The one packed block containing all three small files moves 10 -> 2.
    require(compaction.files_relocated==3);
    require(compaction.blocks_relocated==1);

    // GC synchronizes metadata once, then packed relocation publishes two
    // generations before its old source block is erased.
    require(reopened.generation()==generation_before_compaction+3);
    const auto compacted_recycled=std::find_if(
        reopened.entries().begin(),reopened.entries().end(),
        [](const ezfa3fs::live::Entry& entry){
            return entry.name=="docs/recycled.txt";
        });
    require(compacted_recycled!=reopened.entries().end()&&
            compacted_recycled->first_block==2);

    const auto compacted_recovered=std::find_if(
        reopened.entries().begin(),reopened.entries().end(),
        [](const ezfa3fs::live::Entry& entry){
            return entry.name=="docs/recovered.txt";
        });
    require(compacted_recovered!=reopened.entries().end()&&
            compacted_recovered->first_block==2);

    // Garbage collection and packed relocation each erase one batch.
    require(reopened_device.prepare_erase_count==
            prepares_before_compaction+2);
    require(reopened_device.erase_batch_count==
            batches_before_compaction+2);

    require(reopened.verify(error));
    ezfa3fs::live::SpaceReport compacted_space;
    require(reopened.inspectSpace(compacted_space,error));
    require(compacted_space.active_blocks==3);
    require(compacted_space.reclaimable_blocks==0);
    require(compacted_space.largest_erased_extent==507);
    require(compacted_space.largest_post_gc_extent==507);
    require(!reopened.createDirectory("docs/readme.txt",error));
    require(reopened.removeFile("docs/readme.txt",error));
    require(reopened.removeFile("docs/large.bin",error));
    require(reopened.removeFile("docs/recovered.txt",error));
    require(reopened.removeFile("docs/recycled.txt",error));
    require(reopened.removeDirectory("docs",error));
}
