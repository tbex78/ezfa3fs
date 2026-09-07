#pragma once

#include <cstdint>
#include <vector>

namespace ezfa3fs {

class SaveBankRestorePolicy final {
public:
    enum class Decision {
        skip,
        restore
    };

    // The caller establishes the causal and safety conditions: the session
    // must have executed a save-touching writer control, the snapshot must
    // have been captured reproducibly from startup-only state, and current
    // banks must have been read after re-entering that same state.  Once those
    // conditions hold, any difference is session-induced and must be repaired;
    // no corruption-pattern guess (00/04, all-zero, etc.) is required.
    static Decision evaluate(const std::vector<std::uint8_t>& snapshot,
                             const std::vector<std::uint8_t>& current) noexcept
    {
        return snapshot==current?Decision::skip:Decision::restore;
    }
};

} // namespace ezfa3fs
