#include "ezfa3fs/save_bank_restore_policy.hpp"

#include <cstdlib>
#include <vector>

namespace {
void require(bool condition) { if(!condition)std::abort(); }
}

int main()
{
    using Decision=ezfa3fs::SaveBankRestorePolicy::Decision;
    using Policy=ezfa3fs::SaveBankRestorePolicy;

    const std::vector<std::uint8_t> snapshot{0x12,0x34,0x56,0x78};
    require(Policy::evaluate(snapshot,{0x00,0x04,0x56,0x78})==
            Decision::restore_writer_marker);
    require(Policy::evaluate({0x00,0x00,0x00,0x00},{0x00,0x04,0x00,0x00})==
            Decision::restore_writer_marker);
    require(Policy::evaluate(snapshot,{0x00,0x00,0x00,0x00})==
            Decision::restore_zeroed_cartridge);
    require(Policy::evaluate(snapshot,snapshot)==Decision::skip);
    require(Policy::evaluate(snapshot,{0x01,0x00,0x00,0x00})==Decision::skip);
    require(Policy::evaluate({0x00,0x00,0x00,0x00},
                             {0x00,0x00,0x00,0x00})==Decision::skip);
}
