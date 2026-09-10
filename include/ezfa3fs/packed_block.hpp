#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ezfa3fs::live {

class SmallFileAllocationPolicy final {
public:
    static constexpr std::size_t threshold = 16*1024;

    bool shouldPack(std::size_t size) const noexcept {
        return size>0&&size<=threshold;
    }
};

struct PackedRecord final {
    std::uint32_t id = 0;
    std::uint64_t modified_time = 0;
    std::vector<std::uint8_t> bytes;
};

struct PackedRecordLocation final {
    std::uint32_t id = 0;
    std::uint32_t offset = 0;
};

class PackedBlock final {
public:
    static constexpr std::size_t block_size = 0x10000;
    static constexpr std::size_t header_size = 64;
    static constexpr std::size_t record_header_size = 32;

    static std::size_t encodedRecordSize(std::size_t byte_count) noexcept;
    static bool canEncode(const std::vector<PackedRecord>& records) noexcept;

    static bool encode(
        std::uint64_t generation,
        const std::vector<PackedRecord>& records,
        std::vector<std::uint8_t>& block,
        std::vector<PackedRecordLocation>& locations,
        std::string& error);

    static bool decode(
        const std::vector<std::uint8_t>& block,
        std::uint64_t& generation,
        std::vector<PackedRecord>& records,
        std::vector<PackedRecordLocation>& locations,
        std::string& error);
};

class PackedBlockAllocator final {
public:
    using Bin = std::vector<std::size_t>;

    static bool buildPlan(
        const std::vector<PackedRecord>& records,
        std::vector<Bin>& bins,
        std::string& error);
};

} // namespace ezfa3fs::live
