#pragma once
#include <cstddef>
#include <cstdint>
namespace ezfa3fs {
class Crc32 final {
public:
    static std::uint32_t calculate(const std::uint8_t*,std::size_t) noexcept;
};
}
