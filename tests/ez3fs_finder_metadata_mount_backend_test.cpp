#include "ez3fs/finder_metadata_mount_backend.hpp"
#include "ez3fs/virtual_mount_backend.hpp"

#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace { void require(bool condition){if(!condition)std::abort();} }

int main() {
    std::string error;
    auto persistent=std::make_unique<ez3fs::VirtualMountBackend>(
        std::vector<ez3fs::InputFile>{},true);
    ez3fs::FinderMetadataMountBackend backend(std::move(persistent));
    const auto free_before=backend.freeBytes();

    require(ez3fs::FinderMetadataMountBackend::isFinderMetadata("/.DS_Store"));
    require(ez3fs::FinderMetadataMountBackend::isFinderMetadata("/dir/._file"));
    require(!ez3fs::FinderMetadataMountBackend::isFinderMetadata("/file.txt"));

    require(backend.createFile("/.DS_Store",error));
    const std::uint8_t data[]={'m','a','c'};
    require(backend.write("/.DS_Store",0,data,sizeof(data),error));
    require(backend.commitFile("/.DS_Store",error));
    ez3fs::MountNode node;
    require(backend.lookup("/.DS_Store",node)&&node.size==sizeof(data));
    std::vector<std::uint8_t> read;
    require(backend.read("/.DS_Store",0,sizeof(data),read));
    require(read==std::vector<std::uint8_t>(data,data+sizeof(data)));
    require(backend.freeBytes()==free_before);
    require(backend.removeFile("/.DS_Store",error));
    require(!backend.lookup("/.DS_Store",node));

    require(backend.createDirectory("/folder",error));
    require(backend.createFile("/folder/.DS_Store",error));
    require(!backend.removeDirectory("/folder",error));
    require(backend.rename("/folder","/renamed",error));
    require(!backend.lookup("/folder/.DS_Store",node));
    require(backend.lookup("/renamed/.DS_Store",node));
    require(backend.removeFile("/renamed/.DS_Store",error));
    require(backend.removeDirectory("/renamed",error));
}
