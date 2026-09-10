#pragma once

#include "ezfa3fs/crc32.hpp"
#include "ezfa3fs/byte_storage.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <iosfwd>

namespace ezfa3fs::live {

struct PackedRecord;
struct PackedRecordLocation;

inline constexpr std::string_view format_version = "0.3.0";
inline constexpr std::string_view direct_boot_format_version = "0.4.0";
inline constexpr std::array<std::uint8_t,8> format_magic{
    {'E','Z','F','A','3','F','S',0}};
inline constexpr std::array<std::uint8_t,8> direct_boot_format_magic{
    {'E','Z','F','A','3','D','B',0}};

inline bool hasFormatMagic(const std::uint8_t* bytes) noexcept
{
    return std::equal(format_magic.begin(),format_magic.end(),bytes) ||
           std::equal(direct_boot_format_magic.begin(),direct_boot_format_magic.end(),bytes);
}

class BlockDevice {
public:
    virtual ~BlockDevice() = default;
    virtual bool read(std::size_t offset,std::uint8_t* destination,std::size_t size,std::string& error) const = 0;
    virtual bool program(std::size_t offset,const std::uint8_t* source,std::size_t size,std::string& error) = 0;
    virtual bool programBlocks(std::size_t first_block,const std::uint8_t* source,
                               std::size_t block_count,
                               std::size_t& completed_blocks,std::string& error);
    virtual bool eraseBlock(std::size_t block,std::string& error) = 0;
    virtual bool eraseBlocks(const std::vector<std::size_t>& blocks,
                             std::string& error);
    virtual bool replaceBlocks(std::size_t first_block,
                               const std::uint8_t* source,
                               std::size_t block_count,
                               const std::vector<std::size_t>& erase_blocks,
                               std::size_t& completed_blocks,
                               std::string& error);
    virtual bool replaceMetadataBlock(std::size_t block,
                                      const std::uint8_t* source,
                                      std::size_t size,std::string& error);
    virtual bool prepareForErase(std::string& error) { error.clear();return true; }
    virtual bool prepareForProgram(std::string& error) { error.clear();return true; }
};

class NorFlash final : public BlockDevice {
public:
    static constexpr std::size_t block_size = 0x10000;
    static constexpr std::size_t block_count = 0x200;
    static constexpr std::size_t capacity = block_size*block_count;

    NorFlash();
    bool load(const std::string& path,std::string& error);
    bool load(const std::vector<std::uint8_t>& bytes,std::string& error);
    bool load(ByteStorage& storage,std::string& error);
    bool load(ByteStorage& storage,std::ostream& progress,std::string& error);
    bool save(const std::string& path,std::string& error) const;
    bool read(std::size_t offset,std::uint8_t* destination,std::size_t size,std::string& error) const override;
    bool program(std::size_t offset,const std::uint8_t* source,std::size_t size,std::string& error) override;
    bool eraseBlock(std::size_t block,std::string& error) override;
    void failNextProgramAfter(std::size_t bytes) noexcept { fault_bytes_=bytes; }

private:
    std::vector<std::uint8_t> bytes_;
    std::optional<std::size_t> fault_bytes_;
};

enum class StorageType : std::uint8_t {
    dedicated,
    packed
};

struct Entry final {
    std::string name;
    std::uint64_t size = 0;
    std::uint64_t modified_time = 0;
    std::uint32_t crc32 = 0;
    std::uint32_t first_block = 0;
    std::uint32_t block_count = 0;
    bool directory = false;
    StorageType storage = StorageType::dedicated;
    std::uint64_t packed_generation = 0;
    std::uint32_t packed_record_id = 0;
    std::uint32_t packed_record_offset = 0;
};

struct FileWrite final {
    // Batch destinations are filesystem paths. The filesystem owns no input
    // references after putFiles() returns.
    std::string path;
    std::vector<std::uint8_t> bytes;
    std::uint64_t modified_time = 0;
};

struct SpaceReport final {
    std::size_t active_blocks = 0;
    std::size_t erased_blocks = 0;
    std::size_t reclaimable_blocks = 0;
    std::size_t largest_erased_extent = 0;
    std::size_t largest_post_gc_extent = 0;
};

struct CompactionReport final {
    std::size_t garbage_blocks_reclaimed = 0;
    std::size_t files_relocated = 0;
    std::size_t blocks_relocated = 0;
};

struct GarbageCollectionState final {
    std::size_t next_block = 0;
    std::size_t reclaimed_blocks = 0;

    // Idle GC scans one block at a time but erases stale blocks in small
    // batches so the expensive cartridge writer transition is amortized.
    std::vector<std::size_t> pending_blocks;
    std::vector<std::size_t> last_reclaimed_blocks;

    bool metadata_synchronized = false;
    bool initialized = false;
};

enum class MaintenanceAction {
    garbage_collection,
    compaction
};

enum class Layout {
    transactional,
    direct_boot
};

class Filesystem final {
public:
    using ScanProgress = std::function<void(std::size_t,std::size_t)>;
    using MaintenanceObserver = std::function<void(MaintenanceAction)>;

    explicit Filesystem(BlockDevice& flash) : flash_(flash) {}
    static bool format(BlockDevice& flash,std::string& error);
    static bool formatDirectBootEmpty(BlockDevice& flash,std::string& error,
                                      std::size_t boot_slot_blocks=1);
    static bool formatDirectBoot(BlockDevice& flash,const std::string& rom_name,
                                 const std::vector<std::uint8_t>& rom,
                                 std::uint64_t modified_time,std::string& error);
    static bool open(BlockDevice& flash,Filesystem& filesystem,std::string& error,
                     ScanProgress progress = {});

    bool canCreateFile(const std::string& path,std::string& error) const;
    bool createDirectory(const std::string& path,std::string& error);
    bool putFile(const std::string& path,const std::vector<std::uint8_t>& bytes,
                 std::uint64_t modified_time,std::string& error,
                 MaintenanceObserver maintenance = {});
    bool putFiles(const std::vector<FileWrite>& files,std::string& error,
                  MaintenanceObserver maintenance = {});
    bool removeFile(const std::string& path,std::string& error);
    bool removeDirectory(const std::string& path,std::string& error);
    bool rename(const std::string& from,const std::string& to,std::string& error);
    bool readFile(const std::string& path,std::vector<std::uint8_t>& bytes,
                  std::string& error) const;
    bool readFileRange(const std::string& path,std::size_t offset,std::size_t size,
                       std::vector<std::uint8_t>& bytes,std::string& error) const;
    bool verify(std::string& error,ScanProgress progress = {}) const;

    // Writable cartridge mounts remove zero-byte files that were left behind
    // by an interrupted/failed previous copy before exposing the filesystem.
    bool removeEmptyFiles(std::size_t& removed_files,std::string& error);

    bool collectGarbage(std::size_t& reclaimed_blocks,std::string& error,
                        ScanProgress progress = {});

    bool collectGarbageStep(GarbageCollectionState& state,
                            bool resynchronize_metadata,
                            bool& complete,
                            std::string& error);
    bool inspectSpace(SpaceReport& report,std::string& error,
                      ScanProgress progress = {}) const;
    bool compact(CompactionReport& report,std::string& error,
                 ScanProgress progress = {});
    const std::vector<Entry>& entries() const noexcept { return entries_; }
    std::uint64_t generation() const noexcept { return generation_; }
    std::size_t activeSuperblock() const noexcept { return active_superblock_; }
    std::size_t freeBlocks() const noexcept;
    Layout layout() const noexcept { return layout_; }
    bool isDirectBoot() const noexcept { return layout_==Layout::direct_boot; }
    bool awaitsDirectBootRom() const noexcept;
    bool packedStorageEnabled() const noexcept {
        return packed_storage_enabled_;
    }

private:
    struct FileWriteView final {
        const std::string* path = nullptr;
        const std::vector<std::uint8_t>* bytes = nullptr;
        std::uint64_t modified_time = 0;
    };

    enum class ExtentSearchResult { found,no_extent,error };
    bool commit(std::string& error);
    ExtentSearchResult findBlankExtent(std::size_t block_count,
                                       std::size_t& first_block,
                                       std::string& error);
    bool allocateExtent(std::size_t block_count,std::size_t& first_block,
                        std::string& error,const MaintenanceObserver& maintenance);
    bool findBlankExtentBefore(std::size_t limit,std::size_t block_count,
                               std::size_t& first_block,std::string& error);
    bool programExtent(std::size_t first_block,
                       const std::vector<std::uint8_t>& bytes,std::string& error);
    bool programPreparedExtent(std::size_t first_block,
                               const std::vector<std::uint8_t>& extent,
                               std::string& error);
    bool putFileViews(const std::vector<FileWriteView>& files,
                      std::string& error,
                      const MaintenanceObserver& maintenance);
    bool resizeDirectBootSlot(std::size_t block_count,std::string& error);
    bool findStaleDirectBootRomBlocks(std::size_t block_count,
                                      std::vector<std::size_t>& stale_blocks,
                                      std::string& error);
    bool readEntryRange(const Entry& entry,std::size_t offset,std::size_t size,
                        std::vector<std::uint8_t>& bytes,std::string& error,
                        const std::function<void()>& block_read = {}) const;
    bool compactPackedBlocks(CompactionReport& report,std::string& error);
    bool compactFiles(CompactionReport& report,std::string& error);
    bool blockReferenced(std::size_t block) const noexcept;
    bool parentExists(const std::string& path) const;
    Entry* find(const std::string& path);
    const Entry* find(const std::string& path) const;
    const Entry* directBootRom() const noexcept;
    bool isDirectBootRom(const Entry& entry) const noexcept;
    bool readPackedBlock(
        std::uint32_t block,std::uint64_t expected_generation,
        std::vector<PackedRecord>& records,
        std::vector<PackedRecordLocation>& locations,
        std::string& error) const;
    bool readPackedEntry(const Entry& entry,std::vector<std::uint8_t>& bytes,
                         std::string& error) const;
    std::size_t firstDataBlock() const noexcept;
    std::size_t allocationStartBlock() const noexcept;
    std::size_t dataEndBlock() const noexcept;
    std::size_t alternateSuperblock() const noexcept;
    BlockDevice& flash_;
    std::vector<Entry> entries_;
    std::uint64_t generation_ = 0;
    std::size_t active_superblock_ = 0;
    std::size_t next_free_block_ = 2;
    Layout layout_ = Layout::transactional;
    std::size_t boot_slot_blocks_ = 0;
    bool packed_storage_enabled_ = false;
    std::array<bool,NorFlash::block_count> unavailable_blocks_{};
};

} // namespace ezfa3fs::live
