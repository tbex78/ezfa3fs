#pragma once

#include "ezfa3fs/cartridge_storage.hpp"

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace ezfa3fs {

class CartridgeProgrammer final {
public:
    bool programAndVerify(const std::vector<std::uint8_t>& image,
                          std::ostream& progress, std::string& error);
    bool eraseLiveBlock(std::size_t block,std::ostream& progress,std::string& error);
    bool programLiveBlock(std::size_t block,const std::vector<std::uint8_t>& bytes,
                          std::ostream& progress,std::string& error);

private:
    CartridgeStorage storage_;
};

} // namespace ezfa3fs
