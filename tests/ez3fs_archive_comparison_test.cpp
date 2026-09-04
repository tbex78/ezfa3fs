#include "ez3fs/archive_comparison.hpp"

#include <cstdlib>

namespace { void require(bool condition) { if(!condition)std::abort(); } }

ez3fs::Archive makeArchive(const std::vector<ez3fs::InputFile>& files)
{
    ez3fs::ArchiveImage image;std::string error;
    require(ez3fs::ImageBuilder{}.build(files,image,error));
    ez3fs::Archive archive;require(archive.open(std::move(image.bytes),error));
    return archive;
}

int main()
{
    const auto current=makeArchive({
        {"deleted.txt",{1},false,100},
        {"modified.txt",{2},false,100},
        {"same.txt",{3},false,100}});
    const auto desired=makeArchive({
        {"added.txt",{4},false,100},
        {"modified.txt",{9},false,101},
        {"same.txt",{3},false,100}});
    const auto comparison=ez3fs::ArchiveComparator{}.compare(current,desired);
    require(comparison.count(ez3fs::ChangeKind::added)==1);
    require(comparison.count(ez3fs::ChangeKind::modified)==1);
    require(comparison.count(ez3fs::ChangeKind::deleted)==1);
    require(comparison.count(ez3fs::ChangeKind::unchanged)==1);
    require(comparison.requiresCommit());
    const auto identical=ez3fs::ArchiveComparator{}.compare(current,current);
    require(identical.image_identical);require(!identical.requiresCommit());
}
