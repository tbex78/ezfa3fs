#include "ez3fs/archive.hpp"
#include "ez3fs/byte_storage.hpp"
#include "ez3fs/version.hpp"
#include <algorithm>
#include <cstdlib>
#include <string>
namespace {
void require(bool condition) { if(!condition) std::abort(); }
class MemoryStorage final : public ez3fs::ByteStorage {
public:
    explicit MemoryStorage(const std::vector<std::uint8_t>& bytes):bytes_(bytes) {}
    std::uint64_t capacity() const noexcept override { return bytes_.size(); }
    bool read(std::uint64_t offset,std::uint8_t* destination,std::size_t size,
              std::string& error) override {
        if(offset>bytes_.size()||size>bytes_.size()-offset){error="out of bounds";return false;}
        std::copy_n(bytes_.data()+static_cast<std::size_t>(offset),size,destination);
        error.clear();return true;
    }
private:
    const std::vector<std::uint8_t>& bytes_;
};
}
int main() {
    require(ez3fs::project_version=="0.45.5");
    const std::vector<ez3fs::InputFile> files{
        {"hello.txt",{'h','e','l','l','o'},false,1700000000},
        {"folder/data.bin",{0,0x7F,0xFF},false,1700000001}};
    ez3fs::ArchiveImage image;std::string error;require(ez3fs::ImageBuilder{}.build(files,image,error));
    require(error.empty());require(image.bytes.size()==0x20000);
    ez3fs::Archive archive;require(archive.open(image.bytes,error));require(archive.entries().size()==2);require(archive.verify(error));
    require(archive.entries()[0].modified_time==1700000000);
    require(archive.entries()[1].modified_time==1700000001);
    MemoryStorage storage(image.bytes);ez3fs::Archive loaded;
    require(ez3fs::ArchiveLoader{}.load(storage,loaded,error));require(loaded.verify(error));
    const std::vector<std::uint8_t> expected_magic{'E','Z','3','F','S','\r','\n',0x1A};
    require(std::equal(expected_magic.begin(),expected_magic.end(),image.bytes.begin()));
    auto legacy=image.bytes;
    const std::vector<std::uint8_t> legacy_magic{'E','Z','F','S','\r','\n',0x1A,'\n'};
    std::copy(legacy_magic.begin(),legacy_magic.end(),legacy.begin());
    std::fill(legacy.begin()+52,legacy.begin()+56,0);
    const auto legacy_header_crc=ez3fs::Crc32::calculate(legacy.data(),64);
    for(std::size_t i=0;i<4;++i)legacy[52+i]=static_cast<std::uint8_t>(legacy_header_crc>>(i*8));
    ez3fs::Archive legacy_archive;require(legacy_archive.open(std::move(legacy),error));require(legacy_archive.verify(error));
    auto corrupt=image.bytes;corrupt[archive.entries()[0].offset]^=1;ez3fs::Archive corrupt_archive;
    require(corrupt_archive.open(std::move(corrupt),error));require(!corrupt_archive.verify(error));
    ez3fs::ArchiveImage invalid;require(!ez3fs::ImageBuilder{}.build({{"../escape",{1}}},invalid,error));
    require(!ez3fs::ImageBuilder{}.build({{"same",{1}},{"same",{2}}},invalid,error));
    ez3fs::ArchiveEditor editor(archive.contents());
    require(editor.createDirectory("documents",error));
    require(editor.createDirectory("documents/manuals",error));
    require(editor.putFile("documents/manuals/readme.txt",{'o','k'},error));
    require(!editor.removeDirectory("documents/manuals",error));
    require(editor.removeFile("documents/manuals/readme.txt",error));
    require(editor.removeDirectory("documents/manuals",error));
    require(editor.removeDirectory("documents",error));
    require(!editor.putFile("missing/file",{1},error));
    std::vector<std::uint8_t> oversized(0x02000000,0);
    require(!ez3fs::ImageBuilder{}.build({{"large",std::move(oversized)}},invalid,error));
}
