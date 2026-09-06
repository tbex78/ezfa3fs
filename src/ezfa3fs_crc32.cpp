#include "ezfa3fs/crc32.hpp"
namespace ezfa3fs {
std::uint32_t Crc32::calculate(const std::uint8_t* data,std::size_t size) noexcept {
    std::uint32_t crc=0xFFFFFFFFu;
    for(std::size_t i=0;i<size;++i){crc^=data[i];for(unsigned bit=0;bit<8;++bit)
        crc=(crc>>1)^(0xEDB88320u&(0u-(crc&1u)));}
    return ~crc;
}
}
