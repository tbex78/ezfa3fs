#include "ez3fs/live_filesystem.hpp"
#include "ez3fs/cartridge_flash_geometry.hpp"

#include <algorithm>
#include <cstdlib>
#include <set>

namespace {
void require(bool condition) { if(!condition)std::abort(); }

class CountingDevice final : public ez3fs::live::BlockDevice {
public:
    explicit CountingDevice(ez3fs::live::NorFlash& flash):flash_(flash) {}
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
    bool replaceMetadataBlock(std::size_t block,const std::uint8_t* source,
                              std::size_t size,std::string& error) override {
        ++replace_count;
        return BlockDevice::replaceMetadataBlock(block,source,size,error);
    }
    bool prepareForErase(std::string& error) override {
        ++prepare_erase_count;error.clear();return true;
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
    std::vector<std::size_t> erased_blocks;
private:
    ez3fs::live::NorFlash& flash_;
    std::set<std::size_t> failed_program_calls;
};

void putLittle(std::vector<std::uint8_t>& bytes,std::size_t offset,
               std::uint64_t value,std::size_t size) {
    for(std::size_t index=0;index<size;++index)
        bytes[offset+index]=static_cast<std::uint8_t>(value>>(index*8));
}

std::vector<std::uint8_t> blockData(std::size_t blocks,std::uint8_t value) {
    return std::vector<std::uint8_t>(blocks*ez3fs::live::NorFlash::block_size,value);
}

void verifyFormatIdentityAndLegacyCompatibility() {
    constexpr const auto& current_magic=ez3fs::live::format_magic;
    constexpr const auto& old_magic=ez3fs::live::legacy_format_magic;
    ez3fs::live::NorFlash formatted;std::string error;
    require(ez3fs::live::Filesystem::format(formatted,error));
    std::vector<std::uint8_t> block(ez3fs::live::NorFlash::block_size);
    require(formatted.read(ez3fs::live::NorFlash::block_size,block.data(),block.size(),error));
    require(std::equal(current_magic.begin(),current_magic.end(),block.begin()));
    require(block[8]==2&&block[9]==0&&block[10]==0&&block[11]==0);

    ez3fs::live::NorFlash legacy;
    std::fill(block.begin(),block.end(),0xFF);
    std::copy(old_magic.begin(),old_magic.end(),block.begin());
    std::fill(block.begin()+32,block.begin()+36,0);
    putLittle(block,8,1,2);putLittle(block,10,0,2);putLittle(block,12,7,8);
    putLittle(block,20,4,4);putLittle(block,24,ez3fs::Crc32::calculate(block.data()+32,4),4);
    putLittle(block,28,0xC0FF17EDu,4);
    require(legacy.program(0,block.data(),block.size(),error));
    ez3fs::live::Filesystem filesystem(legacy);
    require(ez3fs::live::Filesystem::open(legacy,filesystem,error));
    require(filesystem.generation()==7&&filesystem.entries().empty());
    require(filesystem.createDirectory("migrated",error));
    require(legacy.read(ez3fs::live::NorFlash::block_size,block.data(),block.size(),error));
    require(std::equal(current_magic.begin(),current_magic.end(),block.begin()));
    require(block[8]==2&&block[9]==0&&block[10]==0&&block[11]==0);
}

void verifyPhysicalEraseGeometry() {
    const auto bottom=ez3fs::CartridgeFlashGeometry::sectorsForLogicalBlock(0);
    require(bottom.size()==8&&bottom.front().window==0&&
            bottom.front().word_address==0&&bottom.back().word_address==0x7000);
    const auto ordinary=ez3fs::CartridgeFlashGeometry::sectorsForLogicalBlock(2);
    require(ordinary.size()==1&&ordinary.front().window==0&&
            ordinary.front().word_address==0x10000);
    const auto top=ez3fs::CartridgeFlashGeometry::sectorsForLogicalBlock(511);
    require(top.size()==8&&top.front().window==3&&
            top.front().word_address==0x3F8000&&top.back().word_address==0x3FF000);
    const auto top_prefix=ez3fs::CartridgeFlashGeometry::sectorsCoveringBlockPrefix(
        511,ez3fs::CartridgeFlashGeometry::boot_sector_size);
    require(top_prefix.size()==1&&top_prefix.front().window==3&&
            top_prefix.front().word_address==0x3F8000);
}

void verifyDirectBootLayout() {
    ez3fs::live::NorFlash flash;std::string error;
    const std::vector<std::uint8_t> rom{0x18,0x00,0x00,0xEA,0x42,0x47,0x41};
    require(ez3fs::live::Filesystem::formatDirectBoot(flash,"direct.gba",rom,1234,error));
    std::vector<std::uint8_t> bytes(rom.size());
    require(flash.read(0,bytes.data(),bytes.size(),error));
    require(bytes==rom);
    std::vector<std::uint8_t> superblock(ez3fs::live::NorFlash::block_size);
    require(flash.read((ez3fs::live::NorFlash::block_count-2)*ez3fs::live::NorFlash::block_size,
                       superblock.data(),superblock.size(),error));
    require(std::equal(ez3fs::live::direct_boot_format_magic.begin(),
                       ez3fs::live::direct_boot_format_magic.end(),superblock.begin()));
    ez3fs::live::Filesystem filesystem(flash);
    require(ez3fs::live::Filesystem::open(flash,filesystem,error));
    require(filesystem.isDirectBoot()&&filesystem.entries().size()==1&&
            filesystem.entries().front().first_block==0);
    require(filesystem.readFile("direct.gba",bytes,error)&&bytes==rom);
    require(filesystem.createDirectory("extras",error));
    require(filesystem.putFile("extras/readme.txt",{'o','k'},1235,error));
    const auto extra=std::find_if(filesystem.entries().begin(),filesystem.entries().end(),
        [](const ez3fs::live::Entry& entry){return entry.name=="extras/readme.txt";});
    require(extra!=filesystem.entries().end()&&extra->first_block==1);
    require(filesystem.readFile("direct.gba",bytes,error)&&bytes==rom);
    ez3fs::live::Filesystem reopened(flash);
    require(ez3fs::live::Filesystem::open(flash,reopened,error));
    require(reopened.generation()==filesystem.generation());
    require(reopened.entries().size()==3);
    require(reopened.readFile("extras/readme.txt",bytes,error));
    require(bytes==std::vector<std::uint8_t>({'o','k'}));
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
    ez3fs::live::NorFlash flash;std::string error;
    require(ez3fs::live::Filesystem::formatDirectBootEmpty(flash,error));
    ez3fs::live::Filesystem filesystem(flash);
    require(ez3fs::live::Filesystem::open(flash,filesystem,error));
    require(filesystem.isDirectBoot()&&filesystem.entries().empty());
    require(!filesystem.canCreateFile(".DS_Store",error));
    require(error.find("root-level .gba")!=std::string::npos);
    const std::vector<std::uint8_t> rom{0x18,0x00,0x00,0xEA,0x44};
    require(filesystem.putFile("first.GBA",rom,1234,error));
    std::vector<std::uint8_t> bytes(rom.size());
    require(flash.read(0,bytes.data(),bytes.size(),error)&&bytes==rom);
    require(filesystem.entries().size()==1&&filesystem.entries().front().first_block==0);
    require(filesystem.putFile("second.gba",rom,1235,error));
    require(!filesystem.putFile("first.GBA",rom,1236,error));
    require(error.find("immutable")!=std::string::npos);

    ez3fs::live::NorFlash growing_flash;
    require(ez3fs::live::Filesystem::formatDirectBootEmpty(growing_flash,error,1));
    ez3fs::live::Filesystem growing(growing_flash);
    require(ez3fs::live::Filesystem::open(growing_flash,growing,error));
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
    ez3fs::live::NorFlash flash;std::string error;
    require(ez3fs::live::Filesystem::formatDirectBootEmpty(flash,error,256));
    CountingDevice device(flash);
    ez3fs::live::Filesystem filesystem(device);
    require(ez3fs::live::Filesystem::open(device,filesystem,error));
    const auto old_rom=blockData(3,0x00);
    require(filesystem.putFile("old.gba",old_rom,1234,error));
    require(filesystem.entries().front().block_count==3);
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
    require(filesystem.putFile("new.gba",replacement,1235,error));
    require(std::count(device.erased_blocks.begin(),device.erased_blocks.end(),1)==
            block1_erases+1);
    require(std::count(device.erased_blocks.begin(),device.erased_blocks.end(),2)==
            block2_erases);
    std::vector<std::uint8_t> bytes;
    require(filesystem.readFile("new.gba",bytes,error)&&bytes==replacement);
    require(flash.read(2*ez3fs::live::NorFlash::block_size,bytes.data(),
                       ez3fs::live::NorFlash::block_size,error));
    require(bytes.front()==0x00);
}

void verifyInterruptedCompaction(std::size_t failure_offset,
                                 std::uint32_t recovered_block,
                                 std::uint64_t generation_advance) {
    ez3fs::live::NorFlash flash;std::string error;
    require(ez3fs::live::Filesystem::format(flash,error));
    CountingDevice device(flash);ez3fs::live::Filesystem filesystem(device);
    require(ez3fs::live::Filesystem::open(device,filesystem,error));
    require(filesystem.putFile("movable",{'a'},1,error));
    require(filesystem.putFile("fixed",{'b'},1,error));
    require(filesystem.putFile("movable",{'c'},2,error));
    std::size_t reclaimed=0;require(filesystem.collectGarbage(reclaimed,error));
    require(reclaimed==1);
    const auto generation=filesystem.generation();
    device.failProgramCall(device.program_count+failure_offset);
    ez3fs::live::CompactionReport report;
    require(!filesystem.compact(report,error));

    CountingDevice recovered_device(flash);ez3fs::live::Filesystem recovered(recovered_device);
    require(ez3fs::live::Filesystem::open(recovered_device,recovered,error));
    require(recovered.generation()==generation+generation_advance);
    const auto entry=std::find_if(recovered.entries().begin(),recovered.entries().end(),
        [](const ez3fs::live::Entry& candidate){return candidate.name=="movable";});
    require(entry!=recovered.entries().end()&&entry->first_block==recovered_block);
    std::vector<std::uint8_t> bytes;
    require(recovered.readFile("movable",bytes,error));
    require(bytes==std::vector<std::uint8_t>({'c'}));
    require(recovered.verify(error));
}

void verifyAlternateExtentRetry() {
    ez3fs::live::NorFlash flash;std::string error;
    require(ez3fs::live::Filesystem::format(flash,error));
    CountingDevice device(flash);ez3fs::live::Filesystem filesystem(device);
    require(ez3fs::live::Filesystem::open(device,filesystem,error));
    device.failProgramCall(device.program_count+1);
    const auto expected=blockData(2,0x5A);
    require(filesystem.putFile("retried",expected,1,error));
    require(device.extent_program_count==2);
    const auto entry=std::find_if(filesystem.entries().begin(),filesystem.entries().end(),
        [](const ez3fs::live::Entry& candidate){return candidate.name=="retried";});
    require(entry!=filesystem.entries().end()&&entry->first_block==3);
    std::vector<std::uint8_t> bytes;
    require(filesystem.readFile("retried",bytes,error));
    require(bytes==expected);
    require(filesystem.verify(error));
}

void verifyAutomaticGarbageCollection() {
    ez3fs::live::NorFlash flash;std::string error;
    require(ez3fs::live::Filesystem::format(flash,error));
    CountingDevice device(flash);ez3fs::live::Filesystem filesystem(device);
    require(ez3fs::live::Filesystem::open(device,filesystem,error));
    require(filesystem.putFile("replaceable",blockData(255,0x11),1,error));
    require(filesystem.putFile("replaceable",blockData(255,0x22),2,error));
    std::vector<ez3fs::live::MaintenanceAction> actions;
    require(filesystem.putFile("replaceable",blockData(255,0x33),3,error,
        [&](ez3fs::live::MaintenanceAction action){actions.push_back(action);}));
    require(actions==std::vector<ez3fs::live::MaintenanceAction>{
        ez3fs::live::MaintenanceAction::garbage_collection});
    const auto entry=std::find_if(filesystem.entries().begin(),filesystem.entries().end(),
        [](const ez3fs::live::Entry& candidate){return candidate.name=="replaceable";});
    require(entry!=filesystem.entries().end()&&entry->first_block==2);
    actions.clear();
    require(!filesystem.putFile("too-large",blockData(256,0x44),4,error,
        [&](ez3fs::live::MaintenanceAction action){actions.push_back(action);}));
    require(error.find("out of free blocks")!=std::string::npos);
    require(actions==std::vector<ez3fs::live::MaintenanceAction>{
        ez3fs::live::MaintenanceAction::garbage_collection});
    require(filesystem.verify(error));
}

void verifyAutomaticCompaction() {
    ez3fs::live::NorFlash flash;std::string error;
    require(ez3fs::live::Filesystem::format(flash,error));
    CountingDevice device(flash);ez3fs::live::Filesystem filesystem(device);
    require(ez3fs::live::Filesystem::open(device,filesystem,error));
    const auto extent=blockData(100,0x44);
    require(filesystem.putFile("first",extent,1,error));
    require(filesystem.putFile("hole",extent,1,error));
    require(filesystem.putFile("movable",extent,1,error));
    require(filesystem.removeFile("hole",error));
    std::size_t reclaimed=0;require(filesystem.collectGarbage(reclaimed,error));
    require(reclaimed==100);

    std::vector<ez3fs::live::MaintenanceAction> actions;
    require(filesystem.putFile("large",blockData(220,0x55),2,error,
        [&](ez3fs::live::MaintenanceAction action){actions.push_back(action);}));
    require(actions==std::vector<ez3fs::live::MaintenanceAction>{
        ez3fs::live::MaintenanceAction::garbage_collection,
        ez3fs::live::MaintenanceAction::compaction});
    const auto moved=std::find_if(filesystem.entries().begin(),filesystem.entries().end(),
        [](const ez3fs::live::Entry& candidate){return candidate.name=="movable";});
    const auto large=std::find_if(filesystem.entries().begin(),filesystem.entries().end(),
        [](const ez3fs::live::Entry& candidate){return candidate.name=="large";});
    require(moved!=filesystem.entries().end()&&moved->first_block==102);
    require(large!=filesystem.entries().end()&&large->first_block==202);
    require(filesystem.verify(error));
}
}

int main()
{
    verifyFormatIdentityAndLegacyCompatibility();
    verifyPhysicalEraseGeometry();
    verifyDirectBootLayout();
    verifyEmptyDirectBootLayout();
    verifyDirectBootDeleteInvalidatesOnlyFirstBlock();
    // Losing power while either metadata generation is being updated leaves a
    // complete source or destination extent referenced by the newest valid one.
    verifyInterruptedCompaction(2,4,0);
    verifyInterruptedCompaction(3,2,1);
    verifyAlternateExtentRetry();
    verifyAutomaticGarbageCollection();
    verifyAutomaticCompaction();

    ez3fs::live::NorFlash flash;std::string error;
    require(ez3fs::live::Filesystem::format(flash,error));
    CountingDevice device(flash);ez3fs::live::Filesystem filesystem(device);
    require(ez3fs::live::Filesystem::open(device,filesystem,error));
    require(device.read_count==2);
    require(filesystem.generation()==1);
    const auto replacements_before_create=device.replace_count;
    require(filesystem.createDirectory("docs",error));
    require(device.replace_count==replacements_before_create+1);
    require(filesystem.putFile("docs/readme.txt",{'o','k'},1234,error));
    std::vector<std::uint8_t> bytes;
    require(filesystem.readFile("docs/readme.txt",bytes,error));
    require(bytes==std::vector<std::uint8_t>({'o','k'}));
    std::vector<std::uint8_t> large(ez3fs::live::NorFlash::block_size+4);
    for(std::size_t i=0;i<large.size();++i)large[i]=static_cast<std::uint8_t>(i);
    require(filesystem.putFile("docs/large.bin",large,1234,error));
    require(filesystem.readFileRange("docs/large.bin",
        ez3fs::live::NorFlash::block_size-2,6,bytes,error));
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
    CountingDevice reopened_device(flash);ez3fs::live::Filesystem reopened(reopened_device);
    require(ez3fs::live::Filesystem::open(reopened_device,reopened,error));
    require(reopened_device.read_count==2);
    require(reopened.generation()==before);
    require(reopened.entries().size()==3);
    require(reopened.freeBlocks()==free_before_data_failure);
    require(reopened.putFile("docs/recovered.txt",{'y'},1237,error));
    const auto recovered=std::find_if(reopened.entries().begin(),reopened.entries().end(),
        [](const ez3fs::live::Entry& entry){return entry.name=="docs/recovered.txt";});
    require(recovered!=reopened.entries().end()&&recovered->first_block==9);
    ez3fs::live::SpaceReport space;std::size_t inspected=0,inspection_total=0;
    require(reopened.inspectSpace(space,error,
        [&](std::size_t completed,std::size_t total){inspected=completed;inspection_total=total;}));
    require(space.active_blocks==4);
    require(space.erased_blocks==502);
    require(space.reclaimable_blocks==4);
    require(space.largest_erased_extent==502);
    require(space.largest_post_gc_extent==502);
    require(inspected==510&&inspection_total==510);
    std::size_t reclaimed=0,progress_completed=0,progress_total=0;
    require(reopened.collectGarbage(reclaimed,error,
        [&](std::size_t completed,std::size_t total){progress_completed=completed;progress_total=total;}));
    require(reclaimed==4);
    require(reopened_device.prepare_erase_count==reclaimed);
    require(progress_completed==510&&progress_total==510);
    std::vector<std::pair<std::size_t,std::size_t>> verification_progress;
    require(reopened.verify(error,[&](std::size_t completed,std::size_t total) {
        verification_progress.emplace_back(completed,total);
    }));
    require(verification_progress.front()==std::pair<std::size_t,std::size_t>{0,4});
    require(verification_progress.back()==std::pair<std::size_t,std::size_t>{4,4});
    require(verification_progress.size()==5);
    require(reopened.putFile("docs/recycled.txt",{'z'},1238,error));
    const auto recycled=std::find_if(reopened.entries().begin(),reopened.entries().end(),
        [](const ez3fs::live::Entry& entry){return entry.name=="docs/recycled.txt";});
    require(recycled!=reopened.entries().end()&&recycled->first_block==2);
    const auto generation_before_compaction=reopened.generation();
    ez3fs::live::CompactionReport compaction;
    require(reopened.compact(compaction,error));
    require(compaction.garbage_blocks_reclaimed==0);
    require(compaction.files_relocated==1);
    require(compaction.blocks_relocated==1);
    require(reopened.generation()==generation_before_compaction+2);
    const auto compacted_recovered=std::find_if(reopened.entries().begin(),reopened.entries().end(),
        [](const ez3fs::live::Entry& entry){return entry.name=="docs/recovered.txt";});
    require(compacted_recovered!=reopened.entries().end()&&compacted_recovered->first_block==6);
    require(reopened_device.prepare_erase_count==reclaimed+1);
    require(reopened.verify(error));
    ez3fs::live::SpaceReport compacted_space;
    require(reopened.inspectSpace(compacted_space,error));
    require(compacted_space.active_blocks==5);
    require(compacted_space.reclaimable_blocks==0);
    require(compacted_space.largest_erased_extent==505);
    require(compacted_space.largest_post_gc_extent==505);
    require(!reopened.createDirectory("docs/readme.txt",error));
    require(reopened.removeFile("docs/readme.txt",error));
    require(reopened.removeFile("docs/large.bin",error));
    require(reopened.removeFile("docs/recovered.txt",error));
    require(reopened.removeFile("docs/recycled.txt",error));
    require(reopened.removeDirectory("docs",error));
}
