#pragma once

#include "ez3fs/archive.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace ez3fs {

enum class ChangeKind { added, modified, deleted, unchanged };

struct ArchiveChange {
    ChangeKind kind;
    std::string path;
};

struct ArchiveComparison {
    std::vector<ArchiveChange> changes;
    bool image_identical = false;

    std::size_t count(ChangeKind kind) const noexcept;
    bool requiresCommit() const noexcept { return !image_identical; }
};

class ArchiveComparator final {
public:
    ArchiveComparison compare(const Archive& cartridge,
                              const Archive& staging) const;
};

} // namespace ez3fs
