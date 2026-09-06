#pragma once

#include <chrono>
#include <cstdint>

namespace ezfa3fs {

inline std::uint64_t currentUnixTimestamp() noexcept
{
    const auto seconds=std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return seconds>0?static_cast<std::uint64_t>(seconds):1;
}

} // namespace ezfa3fs
