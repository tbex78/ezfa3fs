#pragma once

#include "ez3fs/archive.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace ez3fs {

class ByteStorage {
public:
    virtual ~ByteStorage() = default;
    virtual std::uint64_t capacity() const noexcept = 0;
    virtual bool read(std::uint64_t offset, std::uint8_t* destination,
                      std::size_t size, std::string& error) = 0;
};

class ArchiveLoader final {
public:
    bool load(ByteStorage& storage, Archive& archive, std::string& error) const;
};

} // namespace ez3fs
