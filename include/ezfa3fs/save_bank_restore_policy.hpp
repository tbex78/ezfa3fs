#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace ezfa3fs {

class SaveBankRestorePolicy final {
public:
    enum class Decision {
        skip,
        restore_writer_marker,
        restore_zeroed_cartridge
    };

    static Decision evaluate(const std::vector<std::uint8_t>& snapshot,
                             const std::vector<std::uint8_t>& current) noexcept
    {
        const bool same=snapshot==current;
        const bool first_bank_has_writer_marker=current.size()>=2&&
            current[0]==0x00&&current[1]==0x04;
        if(!same&&first_bank_has_writer_marker)
            return Decision::restore_writer_marker;

        const bool snapshot_has_nonzero=std::any_of(
            snapshot.begin(),snapshot.end(),
            [](std::uint8_t byte){return byte!=0x00;});
        const bool current_is_all_zero=std::all_of(
            current.begin(),current.end(),
            [](std::uint8_t byte){return byte==0x00;});
        if(snapshot_has_nonzero&&current_is_all_zero)
            return Decision::restore_zeroed_cartridge;
        return Decision::skip;
    }
};

} // namespace ezfa3fs
