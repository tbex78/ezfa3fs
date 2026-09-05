#include "ez3fs/cached_block_device.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
void require(bool condition) { if(!condition)std::abort(); }

class CountingDevice final : public ez3fs::live::BlockDevice {
public:
    CountingDevice() : bytes(ez3fs::live::NorFlash::capacity,0xFF) {}

    bool read(std::size_t offset,std::uint8_t* destination,std::size_t size,
              std::string& error) const override {
        ++reads;
        std::copy_n(bytes.data()+offset,size,destination);
        error.clear();
        return true;
    }
    bool program(std::size_t offset,const std::uint8_t* source,std::size_t size,
                 std::string& error) override {
        ++programs;
        std::copy_n(source,size,bytes.data()+offset);
        error.clear();
        return true;
    }
    bool eraseBlock(std::size_t block,std::string& error) override {
        ++erases;
        std::fill_n(bytes.begin()+static_cast<std::ptrdiff_t>(
                        block*ez3fs::live::NorFlash::block_size),
                    ez3fs::live::NorFlash::block_size,0xFF);
        error.clear();
        return true;
    }

    mutable std::size_t reads=0;
    std::size_t programs=0;
    std::size_t erases=0;
    std::vector<std::uint8_t> bytes;
};
}

int main() {
    using ez3fs::live::NorFlash;
    CountingDevice device;
    device.bytes[4]=0x12;
    ez3fs::live::CachedBlockDevice cache(device);
    std::string error;
    std::uint8_t value=0;
    require(cache.read(4,&value,1,error)&&value==0x12);
    require(cache.read(4,&value,1,error)&&value==0x12);
    require(device.reads==1);

    std::vector<std::uint8_t> replacement(NorFlash::block_size,0x34);
    require(cache.program(0,replacement.data(),replacement.size(),error));
    require(cache.read(4,&value,1,error)&&value==0x34);
    require(device.reads==1);
    require(cache.eraseBlock(0,error));
    require(cache.read(4,&value,1,error)&&value==0xFF);
    require(device.reads==1);
}
