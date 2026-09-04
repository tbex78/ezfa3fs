#pragma once

#include "ez3fs/cartridge_storage.hpp"

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace ez3fs {

class CartridgeProgrammer final {
public:
    bool programAndVerify(const std::vector<std::uint8_t>& image,
                          std::ostream& progress, std::string& error);
    bool eraseLiveBlock(std::size_t block,std::ostream& progress,std::string& error);

private:
    CartridgeStorage storage_;
};

} // namespace ez3fs
