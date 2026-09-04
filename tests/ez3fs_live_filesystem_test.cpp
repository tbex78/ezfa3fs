#include "ez3fs/live_filesystem.hpp"

#include <algorithm>
#include <cstdlib>

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
        return flash_.program(offset,source,size,error);
    }
    bool eraseBlock(std::size_t block,std::string& error) override {
        return flash_.eraseBlock(block,error);
    }
    mutable std::size_t read_count = 0;
private:
    ez3fs::live::NorFlash& flash_;
};
}

int main()
{
    ez3fs::live::NorFlash flash;std::string error;
    require(ez3fs::live::Filesystem::format(flash,error));
    CountingDevice device(flash);ez3fs::live::Filesystem filesystem(device);
    require(ez3fs::live::Filesystem::open(device,filesystem,error));
    require(device.read_count==2);
    require(filesystem.generation()==1);
    require(filesystem.createDirectory("docs",error));
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
    flash.failNextProgramAfter(8);
    require(!filesystem.putFile("docs/partial.txt",{'x'},1236,error));
    require(filesystem.freeBlocks()==free_before_data_failure-1);

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
    require(recovered!=reopened.entries().end()&&recovered->first_block==7);
    std::size_t reclaimed=0,progress_completed=0,progress_total=0;
    require(reopened.collectGarbage(reclaimed,error,
        [&](std::size_t completed,std::size_t total){progress_completed=completed;progress_total=total;}));
    require(reclaimed==2);
    require(progress_completed==510&&progress_total==510);
    require(reopened.verify(error));
    require(reopened.putFile("docs/recycled.txt",{'z'},1238,error));
    const auto recycled=std::find_if(reopened.entries().begin(),reopened.entries().end(),
        [](const ez3fs::live::Entry& entry){return entry.name=="docs/recycled.txt";});
    require(recycled!=reopened.entries().end()&&recycled->first_block==2);
    require(!reopened.createDirectory("docs/readme.txt",error));
    require(reopened.removeFile("docs/readme.txt",error));
    require(reopened.removeFile("docs/large.bin",error));
    require(reopened.removeFile("docs/recovered.txt",error));
    require(reopened.removeFile("docs/recycled.txt",error));
    require(reopened.removeDirectory("docs",error));
}
