#include "ez3fs/archive_comparison.hpp"

#include <algorithm>
#include <map>

namespace ez3fs {
namespace {
bool sameEntry(const ArchiveEntry& left,const ArchiveEntry& right) noexcept
{
    return left.directory==right.directory && left.size==right.size &&
           left.crc32==right.crc32 &&
           left.modified_time==right.modified_time;
}
}

std::size_t ArchiveComparison::count(ChangeKind kind) const noexcept
{
    return static_cast<std::size_t>(std::count_if(
        changes.begin(),changes.end(),
        [kind](const ArchiveChange& change){return change.kind==kind;}));
}

ArchiveComparison ArchiveComparator::compare(const Archive& cartridge,
                                              const Archive& staging) const
{
    std::map<std::string,const ArchiveEntry*> current;
    std::map<std::string,const ArchiveEntry*> desired;
    for(const auto& entry:cartridge.entries())current.emplace(entry.name,&entry);
    for(const auto& entry:staging.entries())desired.emplace(entry.name,&entry);

    ArchiveComparison result;
    result.image_identical=cartridge.image()==staging.image();
    for(const auto& item:current) {
        const auto found=desired.find(item.first);
        if(found==desired.end())result.changes.push_back({ChangeKind::deleted,item.first});
        else result.changes.push_back({sameEntry(*item.second,*found->second)?
            ChangeKind::unchanged:ChangeKind::modified,item.first});
    }
    for(const auto& item:desired)
        if(current.find(item.first)==current.end())
            result.changes.push_back({ChangeKind::added,item.first});
    std::sort(result.changes.begin(),result.changes.end(),
        [](const ArchiveChange& left,const ArchiveChange& right){return left.path<right.path;});
    return result;
}

} // namespace ez3fs
