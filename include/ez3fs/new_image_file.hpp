#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ez3fs {

class NewImageFile final {
public:
    explicit NewImageFile(std::filesystem::path destination);

    bool available(std::string& error) const;
    bool write(const std::vector<std::uint8_t>& image,std::string& error) const;
    const std::filesystem::path& path() const noexcept { return destination_; }

private:
    bool occupied(const std::filesystem::path& path,std::string& error) const;
    std::filesystem::path destination_;
};

} // namespace ez3fs
