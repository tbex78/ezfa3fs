#include "ez3fs/byte_storage.hpp"

#include <algorithm>
#include <array>
#include <vector>

namespace ez3fs {
namespace {
constexpr std::size_t header_size = 64;
constexpr std::array<std::uint8_t, 8> magic{{'E','Z','3','F','S','\r','\n',0x1A}};
constexpr std::array<std::uint8_t, 8> legacy_magic{{'E','Z','F','S','\r','\n',0x1A,'\n'}};

std::uint64_t readLe64(const std::array<std::uint8_t, header_size>& bytes,
                       std::size_t offset) noexcept
{
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < sizeof(value); ++i)
        value |= static_cast<std::uint64_t>(bytes[offset + i]) << (i * 8);
    return value;
}
} // namespace

bool ArchiveLoader::load(ByteStorage& storage, Archive& archive,
                         std::string& error) const
{
    std::array<std::uint8_t, header_size> header{};
    if (!storage.read(0, header.data(), header.size(), error)) return false;
    if (!std::equal(magic.begin(), magic.end(), header.begin()) &&
        !std::equal(legacy_magic.begin(), legacy_magic.end(), header.begin())) {
        error = "cartridge does not contain an EZ3FS image at offset 0";
        return false;
    }

    const auto image_size = readLe64(header, 40);
    if (image_size < ImageBuilder::program_block_size ||
        image_size > storage.capacity() ||
        image_size % ImageBuilder::program_block_size != 0) {
        error = "invalid EZ3FS image size in storage header";
        return false;
    }

    std::vector<std::uint8_t> image(static_cast<std::size_t>(image_size));
    std::copy(header.begin(), header.end(), image.begin());
    if (image.size() > header.size() &&
        !storage.read(header.size(), image.data() + header.size(),
                      image.size() - header.size(), error)) return false;
    return archive.open(std::move(image), error);
}

} // namespace ez3fs
