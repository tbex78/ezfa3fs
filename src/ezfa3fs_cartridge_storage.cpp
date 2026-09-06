#include "ezfa3fs/cartridge_storage.hpp"
#include "ezfa3fs/cartridge_flash_geometry.hpp"
#include "ezfa3fs/cartridge_programmer.hpp"
#include "ezfa3fs/live_filesystem.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <thread>
#include <vector>

#if defined(EZFA3FS_HAS_LIBUSB)
#  if __has_include(<libusb-1.0/libusb.h>)
#    include <libusb-1.0/libusb.h>
#  else
#    include <libusb.h>
#  endif
#endif

namespace ezfa3fs {

class CartridgeStorage::Impl final {
public:
    friend class CartridgeProgrammer;
    ~Impl() {
#if defined(EZFA3FS_HAS_LIBUSB)
        std::string ignored;
        if(handle)restoreSaveBanks(ignored);
#endif
        shutdown();
    }

    bool open(std::string& error);
    bool openForProgramming(std::string& error,bool trust_validated_format=false,
                            bool preserve_save_banks=true);
    bool close(std::string& error,bool settle_before_release=true);
    void shutdown() noexcept;
    bool read(std::uint64_t offset, std::uint8_t* destination,
              std::size_t size, std::string& error);
    bool eraseLiveBlock(std::size_t block,std::ostream& progress,std::string& error,bool allow_metadata=false);
    bool eraseLiveBlocks(const std::vector<std::size_t>& blocks,
                         std::ostream& progress,std::string& error,
                         bool allow_metadata=false,
                         bool wait_until_ready=true);
    bool eraseLiveBlockPrefix(std::size_t block,std::size_t byte_count,
                              std::ostream& progress,std::string& error,
                              bool allow_metadata=false);
    bool programLiveBlock(std::size_t block,const std::vector<std::uint8_t>& bytes,
                          std::ostream& progress,std::string& error,bool allow_metadata=false);
    bool programLiveBlockPrefix(std::size_t block,
                                const std::vector<std::uint8_t>& bytes,
                                std::ostream& progress,std::string& error,
                                bool allow_metadata=false);
    bool programLiveExtent(std::size_t first_block,
                           const std::vector<std::uint8_t>& bytes,
                           std::size_t& completed_blocks,
                           std::ostream& progress,std::string& error,
                           bool allow_metadata=false);
    bool is_open = false;
    std::array<std::uint8_t, 4> flash_id{};

private:
#if defined(EZFA3FS_HAS_LIBUSB)
    static constexpr unsigned timeout_ms = 15000;
    libusb_context* context = nullptr;
    libusb_device_handle* handle = nullptr;
    bool claimed = false;
    std::uint32_t mapped_limit = 0x00800000u;
    static constexpr std::size_t save_bank_size = 0x8000;
    static constexpr std::size_t save_bank_count = 4;
    std::vector<std::uint8_t> save_backup;
    bool save_dirty = false;
    bool save_preservation_enabled = true;

    bool out(const std::vector<std::uint8_t>& bytes, std::string& error);
    bool in(std::vector<std::uint8_t>& bytes, std::size_t size,
            std::string& error);
    bool commandEcho(const std::vector<std::uint8_t>& command,
                     const std::vector<std::uint8_t>& data,
                     std::string& error);
    bool tx92(std::uint8_t first, std::uint8_t second, std::string& error,
              std::uint32_t word_address = 0);
    bool selectSaveBank(std::uint16_t selector,std::string& error);
    bool readSaveBank(std::uint16_t selector,
                      std::vector<std::uint8_t>& bytes,std::string& error);
    bool writeSaveBank(std::uint16_t selector,
                       const std::vector<std::uint8_t>& bytes,
                       std::string& error);
    bool readSaveBanks(std::vector<std::uint8_t>& bytes,std::string& error);
    bool captureSaveBanks(std::string& error);
    bool restoreSaveBanks(std::string& error);
    bool waitReady(unsigned attempts, std::string& error);
    bool activateWriter(std::string& error);
    bool initialize(std::string& error,bool allow_erased,
                    bool trust_validated_format=false);
    bool probePrefix(std::uint8_t a0,std::uint8_t a1,
                     std::uint8_t b0,std::uint8_t b1,
                     std::uint8_t c0,std::uint8_t c1,
                     bool include_tail,std::string& error,bool writer_probe=false);
    bool unlockWindow(std::string& error,bool writer);
    bool resetAfterFlashId(bool use_f0,std::string& error,bool writer);
    bool probeFlashWindow(std::uint8_t a0,std::uint8_t a1,
                          std::uint8_t b0,std::uint8_t b1,
                          std::uint8_t c0,std::uint8_t c1,
                          std::array<std::uint8_t,4>& id,
                          std::string& error,bool writer_probe=false);
    bool readFlashId(std::array<std::uint8_t, 4>& id, std::string& error);
    bool prepareMapping(std::uint64_t end, std::string& error);
    bool mappingBody(std::uint32_t limit, std::string& error);
    bool rawRead(std::uint32_t offset, std::uint8_t* destination,
                 std::size_t size, std::string& error);
    bool tx92One(std::uint8_t selector, std::uint8_t value,
                 std::string& error);
    bool selectWriteWindow(unsigned window, std::string& error);
    bool programTransaction(std::uint32_t word_address,
                            const std::vector<std::uint8_t>& data,
                            const char* operation,std::string& error);
    bool finishWriteOperation(std::string& error);
    bool finishLiveWriteOperation(std::string& error);
#endif
    bool clearSaveBanks(std::string& error);
    bool eraseAll(std::ostream& progress, std::string& error);
    bool programImage(const std::vector<std::uint8_t>& image,
                      std::ostream& progress, std::string& error);
    bool programImageRange(std::size_t offset,
                           const std::vector<std::uint8_t>& bytes,
                           std::ostream& progress,std::string& error);
};

namespace {
std::size_t metadataTransferSize(const std::vector<std::uint8_t>& bytes)
{
    constexpr std::size_t transfer_granularity=
        CartridgeFlashGeometry::boot_sector_size;
    const auto last=std::find_if(bytes.rbegin(),bytes.rend(),
        [](std::uint8_t byte){return byte!=0xFF;});
    const auto meaningful=std::max<std::size_t>(1,static_cast<std::size_t>(
        std::distance(bytes.begin(),last.base())));
    return std::min(bytes.size(),
        ((meaningful+transfer_granularity-1)/transfer_granularity)*
        transfer_granularity);
}
} // namespace

#if defined(EZFA3FS_HAS_LIBUSB)
namespace {
void preciseCommandDataDelay() {
    // The legacy DLL busy-waits for exactly 0x2EE microseconds. A scheduler
    // sleep can overshoot on macOS and cause the bridge to acknowledge the
    // command while ignoring the following large data payload.
    const auto deadline=std::chrono::steady_clock::now()+
        std::chrono::microseconds(750);
    while(std::chrono::steady_clock::now()<deadline) {}
}

void putLe32(std::vector<std::uint8_t>& bytes, std::size_t offset,
             std::uint32_t value)
{
    for (std::size_t i = 0; i < 4; ++i)
        bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
}

std::string usbError(const char* operation, int result)
{
    return std::string(operation) + ": " + libusb_error_name(result);
}

std::string formatFlashId(const std::array<std::uint8_t,4>& id)
{
    std::ostringstream output;
    output << std::hex << std::uppercase << std::setfill('0');
    for (std::size_t i=0;i<id.size();++i) {
        if (i) output << ' ';
        output << std::setw(2) << static_cast<unsigned>(id[i]);
    }
    return output.str();
}

} // namespace

bool CartridgeStorage::Impl::out(const std::vector<std::uint8_t>& bytes,
                                 std::string& error)
{
    if(!handle){error="USB OUT attempted without an open cartridge session";return false;}
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        int transferred = 0;
        const int result = libusb_bulk_transfer(handle, 0x02,
            const_cast<unsigned char*>(bytes.data()), static_cast<int>(bytes.size()),
            &transferred, timeout_ms);
        if (result == 0) {
            if (transferred != static_cast<int>(bytes.size())) {
                error = "USB OUT returned a short transfer"; return false;
            }
            return true;
        }
        // A stalled endpoint normally transfers no bytes. Clear the halt and
        // retry the same transaction once; this avoids replaying a partially
        // accepted flash command while recovering transient USB stalls.
        if (result == LIBUSB_ERROR_PIPE && transferred == 0 && attempt == 0 &&
            libusb_clear_halt(handle, 0x02) == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        error = usbError("USB OUT failed", result); return false;
    }
    return false;
}

bool CartridgeStorage::Impl::in(std::vector<std::uint8_t>& bytes,
                                std::size_t size, std::string& error)
{
    if(!handle){error="USB IN attempted without an open cartridge session";return false;}
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        bytes.assign(size, 0);
        int transferred = 0;
        const int result = libusb_bulk_transfer(handle, 0x81, bytes.data(),
            static_cast<int>(size), &transferred, timeout_ms);
        if (result == 0) {
            if (transferred != static_cast<int>(size)) {
                error = "USB IN returned a short transfer"; return false;
            }
            return true;
        }
        if (result == LIBUSB_ERROR_PIPE && attempt == 0 &&
            libusb_clear_halt(handle, 0x81) == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        error = usbError("USB IN failed", result); return false;
    }
    return false;
}

bool CartridgeStorage::Impl::commandEcho(
    const std::vector<std::uint8_t>& command,
    const std::vector<std::uint8_t>& data, std::string& error)
{
    if (!out(command, error)) return false;
    preciseCommandDataDelay();
    if (!out(data, error)) return false;
    std::vector<std::uint8_t> echo;
    if (!in(echo, command.size(), error)) return false;
    if (echo != command) { error = "EZ3 command echo mismatch"; return false; }
    return true;
}

bool CartridgeStorage::Impl::tx92(std::uint8_t first, std::uint8_t second,
                                  std::string& error,
                                  std::uint32_t word_address)
{
    std::vector<std::uint8_t> command =
        {0x5A,0xA5,0x92,0x02,0,0,0,0,0x02,0,0,0,0};
    putLe32(command, 4, word_address);
    return commandEcho(command, {first, second}, error);
}

bool CartridgeStorage::Impl::selectSaveBank(std::uint16_t selector,
                                             std::string& error)
{
    const auto low=static_cast<std::uint8_t>(selector&0xFFu);
    const auto high=static_cast<std::uint8_t>(selector>>8);
    return tx92(0x55,0xAA,error)&&tx92(0,0,error)&&tx92(0,0,error)&&
           tx92(low,high,error)&&tx92(0,0,error)&&tx92(0,0,error)&&
           tx92(0,0,error)&&tx92(0,0,error);
}

bool CartridgeStorage::Impl::readSaveBank(
    std::uint16_t selector,std::vector<std::uint8_t>& bytes,
    std::string& error)
{
    const std::vector<std::uint8_t> command=
        {0x5A,0xA5,0x91,0x01,0,0,0,0,0,0x80,0,0,0};
    return selectSaveBank(selector,error)&&out(command,error)&&
           in(bytes,save_bank_size,error);
}

bool CartridgeStorage::Impl::writeSaveBank(
    std::uint16_t selector,const std::vector<std::uint8_t>& bytes,
    std::string& error)
{
    if(bytes.size()!=save_bank_size) {
        error="save-bank restore requires exactly 32 KiB";
        return false;
    }
    const std::vector<std::uint8_t> command=
        {0x5A,0xA5,0x92,0x01,0,0,0,0,0,0x80,0,0,0};
    if(!selectSaveBank(selector,error)||!out(command,error)||
       !out(bytes,error))return false;
    std::vector<std::uint8_t> echo;
    if(!in(echo,command.size(),error))return false;
    if(echo!=command) {
        error="cartridge save-bank restore command echo mismatch";
        return false;
    }
    return true;
}

bool CartridgeStorage::Impl::readSaveBanks(std::vector<std::uint8_t>& bytes,
                                            std::string& error)
{
    bytes.clear();
    bytes.reserve(save_bank_count*save_bank_size);
    for(std::size_t bank=0;bank<save_bank_count;++bank) {
        const auto selector=static_cast<std::uint16_t>(0x0900u+bank*0x10u);
        std::vector<std::uint8_t> contents;
        if(!readSaveBank(selector,contents,error))return false;
        bytes.insert(bytes.end(),contents.begin(),contents.end());
    }
    return true;
}

bool CartridgeStorage::Impl::captureSaveBanks(std::string& error)
{
    if(!save_backup.empty())return true;
    if(!readSaveBanks(save_backup,error)) {
        save_backup.clear();
        if(error.empty())error="could not preserve cartridge save banks";
        return false;
    }
    return true;
}

bool CartridgeStorage::Impl::restoreSaveBanks(std::string& error)
{
    if(!save_dirty)return true;
    if(save_backup.size()!=save_bank_count*save_bank_size) {
        error="cartridge save banks were modified without a valid backup";
        return false;
    }
    for(std::size_t bank=0;bank<save_bank_count;++bank) {
        const auto selector=static_cast<std::uint16_t>(0x0900u+bank*0x10u);
        const auto first=save_backup.begin()+
            static_cast<std::ptrdiff_t>(bank*save_bank_size);
        const std::vector<std::uint8_t> contents(first,first+save_bank_size);
        bool restored=false;
        std::string last_error;
        for(unsigned attempt=1;attempt<=3&&!restored;++attempt) {
            std::vector<std::uint8_t> readback;
            restored=writeSaveBank(selector,contents,last_error)&&
                     readSaveBank(selector,readback,last_error)&&
                     readback==contents;
            if(!restored&&readback.size()==contents.size()) {
                const auto mismatch=std::mismatch(readback.begin(),
                                                  readback.end(),
                                                  contents.begin());
                if(mismatch.first!=readback.end()) {
                    std::ostringstream detail;
                    detail<<"save-bank readback differs at byte 0x"<<std::hex
                          <<static_cast<std::size_t>(mismatch.first-
                                                    readback.begin())
                          <<" (read 0x"<<static_cast<unsigned>(*mismatch.first)
                          <<", expected 0x"
                          <<static_cast<unsigned>(*mismatch.second)<<')';
                    last_error=detail.str();
                }
            }
            if(!restored&&attempt<3)
                std::cerr<<"Retrying save-bank restore for bank "<<(bank+1)
                         <<" (attempt "<<(attempt+1)<<"/3): "
                         <<last_error<<'\n';
        }
        if(!restored) {
            error="could not restore save bank "+std::to_string(bank+1)+
                  ": "+last_error;
            return false;
        }
    }
    save_dirty=false;
    save_backup.clear();
    return true;
}

bool CartridgeStorage::Impl::clearSaveBanks(std::string& error)
{
    const std::vector<std::uint8_t> blank(save_bank_size,0xFF);
    for(std::size_t bank=0;bank<save_bank_count;++bank) {
        const auto selector=static_cast<std::uint16_t>(0x0900u+bank*0x10u);
        std::vector<std::uint8_t> readback;
        if(!writeSaveBank(selector,blank,error)||
           !readSaveBank(selector,readback,error)||readback!=blank) {
            if(error.empty())error="save-bank clear verification failed for bank "+
                                   std::to_string(bank+1);
            return false;
        }
    }
    save_dirty=false;
    save_backup.clear();
    error.clear();return true;
}

bool CartridgeStorage::Impl::waitReady(unsigned attempts, std::string& error)
{
    const std::vector<std::uint8_t> command =
        {0x5A,0xA5,0x98,0,0,0,0,0,0,0,0,0,0};
    for (unsigned attempt = 0; attempt < attempts; ++attempt) {
        std::vector<std::uint8_t> response;
        if (!out(command, error) || !in(response, 1, error)) return false;
        if (response[0] == 1) return true;
        if (response[0] != 0) { error = "unexpected cartridge readiness response"; return false; }
        if (attempt + 1 < attempts)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    error = "cartridge was not ready after bounded polling";
    return false;
}

bool CartridgeStorage::Impl::activateWriter(std::string& error)
{
    // The captured Windows workflow performs this transition after the full
    // manager probe and before erase/program traffic.  The three polls are
    // intentionally not folded into waitReady(): all three successful replies
    // and their one-second quiet intervals are part of the proven sequence.
    const std::vector<std::uint8_t> command =
        {0x5A,0xA5,0x98,0,0,0,0,0,0,0,0,0,0};
    for(unsigned poll=0;poll<3;++poll) {
        std::vector<std::uint8_t> response;
        if(!out(command,error)||!in(response,1,error))return false;
        if(response[0]!=1) {
            std::ostringstream detail;
            detail<<"unexpected cartridge writer activation response 0x"
                  <<std::hex<<static_cast<unsigned>(response[0])<<std::dec
                  <<" at poll "<<(poll+1)<<"/3";
            error=detail.str();
            return false;
        }
        if(poll+1<3)
            std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
    error.clear();
    return true;
}

bool CartridgeStorage::Impl::probePrefix(
    std::uint8_t a0,std::uint8_t a1,std::uint8_t b0,std::uint8_t b1,
    std::uint8_t c0,std::uint8_t c1,bool include_tail,std::string& error,
    bool writer_probe)
{
    if (!tx92(0x55,0xAA,error) || !tx92(a0,a1,error) ||
        !tx92(b0,b1,error) || !tx92(c0,c1,error)) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(125));
    if(!include_tail)return true;
    return unlockWindow(error,writer_probe);
}

bool CartridgeStorage::Impl::unlockWindow(std::string& error,bool writer)
{
    if(!tx92(0xAA,0x55,error)||!tx92(0,0,error)||
       !tx92(0,0,error)||!tx92(0,0,error))return false;
    // The companion manager writer includes these selectors in every probe,
    // before issuing the flash-ID command. Read-only probing keeps its
    // historical sequence.
    return !writer||(tx92One(0,0xAA,error)&&tx92One(0,0x55,error)&&
                     tx92One(1,0x06,error));
}

bool CartridgeStorage::Impl::resetAfterFlashId(bool use_f0,std::string& error,
                                                bool writer)
{
    if(!tx92(use_f0?0xF0:0xFF,use_f0?0:0xFF,error))return false;
    return !writer||(tx92One(1,0x04,error)&&tx92One(0,0,error)&&
                     tx92One(0,0,error));
}

bool CartridgeStorage::Impl::probeFlashWindow(
    std::uint8_t a0,std::uint8_t a1,std::uint8_t b0,std::uint8_t b1,
    std::uint8_t c0,std::uint8_t c1,std::array<std::uint8_t,4>& id,
    std::string& error,bool writer_probe)
{
    return probePrefix(a0,a1,b0,b1,c0,c1,true,error,writer_probe)&&
           tx92(0x90,0,error)&&readFlashId(id,error)&&
           resetAfterFlashId(false,error,writer_probe);
}

bool CartridgeStorage::Impl::readFlashId(
    std::array<std::uint8_t, 4>& id, std::string& error)
{
    const std::vector<std::uint8_t> command =
        {0x5A,0xA5,0x91,0x02,0,0,0,0,4,0,0,0,0};
    std::vector<std::uint8_t> response;
    if (!out(command,error) || !in(response,4,error)) return false;
    std::copy(response.begin(), response.end(), id.begin());
    return true;
}

bool CartridgeStorage::Impl::initialize(std::string& error,bool allow_erased,
                                        bool trust_validated_format)
{
    const std::vector<std::uint8_t> c97 =
        {0x5A,0xA5,0x97,0,0,0,0,0,0,0,0,0,0};
    const std::vector<std::uint8_t> c99 =
        {0x5A,0xA5,0x99,0,1,0,0,0,0,0,0,0,0};
    std::vector<std::uint8_t> response;
    if (!out(c97,error) || !in(response,1,error) || response[0] != 0) {
        if (error.empty()) error = "unexpected cartridge startup response";
        return false;
    }
    if (!waitReady(5,error) || !out(c99,error) ||
        !in(response,c99.size(),error) || response != c99) {
        if (error.empty()) error = "cartridge startup echo mismatch";
        return false;
    }

    std::array<std::uint8_t,4> ignored{};
    if (!probePrefix(0,0,0,0,0,0,true,error,allow_erased) || !tx92(0xAA,0,error,0x555) ||
        !tx92(0x55,0,error,0x2AA) || !tx92(0x90,0,error,0x555) ||
        !readFlashId(ignored,error) || !tx92(0x90,0,error) ||
        !resetAfterFlashId(true,error,allow_erased) ||
        !probeFlashWindow(0,0,0,0,0,0,flash_id,error,allow_erased))return false;

    if(allow_erased) {
        // The original manager primes all four 8-MiB windows before writing.
        // Live reconnects must repeat this sequence; jumping from a shortened
        // window-0 probe directly to a higher write window is rejected by
        // real hardware with completion status 0x01.
        const std::array<std::array<std::uint8_t,6>,3> upper{{
            {{2,0,0,0x40,0,0}},{{2,0,0,0x80,0,0}},{{2,0,0,0xC0,0,0}}}};
        for(const auto& probe:upper)
            if(!probeFlashWindow(probe[0],probe[1],probe[2],probe[3],
                                 probe[4],probe[5],ignored,error,true))return false;
        if(!probeFlashWindow(0,0,0,0,2,0,ignored,error,true)||
           !probePrefix(0,0,0,0,0,0,false,error))return false;
    }

    const std::vector<std::uint8_t> c95 =
        {0x5A,0xA5,0x95,0,0x80,0,0,0,0,0,0,0,0};
    if (!out(c95,error) || !in(response,c95.size(),error) || response != c95) {
        if (error.empty()) error = "cartridge read-prime echo mismatch";
        return false;
    }
    std::array<std::uint8_t,0xAC> prime{};
    if(!rawRead(0,prime.data(),prime.size(),error))return false;

    const std::array<std::uint8_t,4> b8{{0x1C,0,0xB8,0}};
    const std::array<std::uint8_t,4> b9{{0x1C,0,0xB9,0}};
    if (flash_id != b8 && flash_id != b9) {
        // Initial live mounting must prove that this is an EZ3 filesystem.
        // Internal writer reconnects already have that proof. In direct-boot
        // mode, repeating discovery can mistake the GBA entry instruction at
        // offset zero for a flash ID and can leave the bridge in the 32-MiB
        // metadata-read mapping before low-block verification.
        if(trust_validated_format){error.clear();return true;}
        // Some genuine EZ3 units do not return either captured ID reliably.
        // The historical reader therefore used content as a read-only fallback.
        // Accept only an EZFA3FS signature at offset zero; arbitrary cartridges
        // still cannot enter the EZ3-specific mapping path.
        std::array<std::uint8_t,8> header{};
        std::copy_n(prime.begin(),header.size(),header.begin());
        const bool erased = std::all_of(header.begin(),header.end(),
            [](std::uint8_t byte){return byte==0xFF;});
        std::array<std::uint8_t,8> live_header{};
        if (!live::hasFormatMagic(header.data()) && !rawRead(0x10000u,live_header.data(),live_header.size(),error)) return false;
        std::array<std::uint8_t,8> direct_boot_header{};
        if (!live::hasFormatMagic(header.data()) && !live::hasFormatMagic(live_header.data())) {
            if(!mappingBody(0x02000000u,error))return false;
            mapped_limit=0x02000000u;
            if(!rawRead(0x01FE0000u,direct_boot_header.data(),direct_boot_header.size(),error))return false;
        }
        std::array<std::uint8_t,8> alternate_direct_boot_header{};
        if (!live::hasFormatMagic(header.data()) && !live::hasFormatMagic(live_header.data()) &&
            !live::hasFormatMagic(direct_boot_header.data()) &&
            !rawRead(0x01FF0000u,alternate_direct_boot_header.data(),alternate_direct_boot_header.size(),error)) return false;
        if (!live::hasFormatMagic(header.data()) &&
            !live::hasFormatMagic(live_header.data()) && !live::hasFormatMagic(direct_boot_header.data()) &&
            !live::hasFormatMagic(alternate_direct_boot_header.data()) && !(allow_erased && erased)) {
            error = "unsupported cartridge flash identifier: " +
                    formatFlashId(flash_id) +
                    "; no EZFA3FS metadata found";
            return false;
        }
    }

    error.clear();return true;
}

bool CartridgeStorage::Impl::mappingBody(std::uint32_t limit,
                                         std::string& error)
{
    // Capture-derived standalone read transition. Unlike verification that
    // begins inside an active full-image program session, a reopened and
    // 0x95-primed reader must not send the one-byte writer selectors here.
    if (!tx92(0xFF,0xFF,error) || !tx92(0x55,0xAA,error)) return false;
    if (limit == 0x01000000u) {
        if (!tx92(2,0,error) || !tx92(0,0x80,error) || !tx92(0,0,error)) return false;
    } else if (limit == 0x01800000u) {
        if (!tx92(2,0,error) || !tx92(0,0xC0,error) || !tx92(0,0,error)) return false;
    } else {
        if (!tx92(0,0,error) || !tx92(0,0,error) || !tx92(2,0,error)) return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(125));
    return tx92(0xAA,0x55,error) && tx92(0,0,error) && tx92(0,0,error) &&
           tx92(0,0,error) && tx92(0xFF,0xFF,error) &&
           tx92(0x55,0xAA,error) && tx92(0,0,error) &&
           tx92(0,0,error) && tx92(0,0,error);
}

bool CartridgeStorage::Impl::prepareMapping(std::uint64_t end,
                                            std::string& error)
{
    if (end <= mapped_limit) return true;
    std::uint32_t required = 0;
    if (end <= 0x01000000u) required = 0x01000000u;
    else if (end <= 0x01800000u) required = 0x01800000u;
    else if (end <= 0x02000000u) required = 0x02000000u;
    else { error = "read exceeds the 32-MiB cartridge capacity"; return false; }
    if (!mappingBody(required,error)) return false;
    mapped_limit = required;
    return true;
}

bool CartridgeStorage::Impl::rawRead(std::uint32_t offset,
                                     std::uint8_t* destination,
                                     std::size_t size, std::string& error)
{
    std::size_t done = 0;
    while (done < size) {
        const auto piece = std::min<std::size_t>(0x10000, size - done);
        std::vector<std::uint8_t> command =
            {0x5A,0xA5,0x91,0,0,0,0,0,0,0,0,0,0};
        putLe32(command,4,(offset + static_cast<std::uint32_t>(done))/2u);
        putLe32(command,8,static_cast<std::uint32_t>(piece));
        std::vector<std::uint8_t> response;
        if (!out(command,error) || !in(response,piece,error)) return false;
        std::copy(response.begin(),response.end(),destination + done);
        done += piece;
    }
    return true;
}

bool CartridgeStorage::Impl::tx92One(std::uint8_t selector,
                                     std::uint8_t value,
                                     std::string& error)
{
    // Hardware isolation in the companion project proved that these legacy
    // writer-control transfers address bytes in the selected save bank.
    // Mark the snapshot dirty before sending so partial USB failures are also
    // covered by session-close restoration.
    if(save_preservation_enabled)save_dirty=true;
    const std::vector<std::uint8_t> command =
        {0x5A,0xA5,0x92,0x01,selector,0,0,0,0x01,0,0,0,0};
    return commandEcho(command,{value},error);
}

bool CartridgeStorage::Impl::selectWriteWindow(unsigned window,
                                                std::string& error)
{
    if (window>3) { error="invalid cartridge flash window"; return false; }
    const auto mode=static_cast<std::uint8_t>(window==0?0:2);
    const auto high=static_cast<std::uint8_t>(window*0x40);
    if (!tx92(0x55,0xAA,error) || !tx92(mode,0,error) ||
        !tx92(0,high,error) || !tx92(0,0,error)) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(125));
    return unlockWindow(error,true);
}

bool CartridgeStorage::Impl::programTransaction(
    std::uint32_t word_address,const std::vector<std::uint8_t>& data,
    const char* operation,std::string& error) {
    std::vector<std::uint8_t> command=
        {0x5A,0xA5,0x92,0,0,0,0,0,0,0,0,0,0x41};
    putLe32(command,4,word_address);
    putLe32(command,8,static_cast<std::uint32_t>(data.size()));
    if(!out(command,error))return false;
    preciseCommandDataDelay();
    if(!out(data,error))return false;
    std::vector<std::uint8_t> response;
    if(!in(response,command.size(),error))return false;
    command[12]=0;
    if(response==command){error.clear();return true;}
    const auto mismatch=std::mismatch(response.begin(),response.end(),command.begin());
    std::ostringstream detail;detail<<operation<<" completion mismatch";
    if(mismatch.first!=response.end())
        detail<<" at byte 0x"<<std::hex
              <<static_cast<std::size_t>(mismatch.first-response.begin())
              <<" (received 0x"<<static_cast<unsigned>(*mismatch.first)
              <<", expected 0x"<<static_cast<unsigned>(*mismatch.second)
              <<')'<<std::dec;
    error=detail.str();return false;
}

bool CartridgeStorage::Impl::finishWriteOperation(std::string& error)
{
    const bool finished=tx92(0xFF,0xFF,error) && tx92One(1,0x04,error) &&
                        tx92One(0,0,error) && tx92One(0,0,error);
    // The captured post-write status tail restores the default linear first
    // 8-MiB read view.  Higher offsets still trigger prepareMapping().
    if(finished)mapped_limit=0x00800000u;
    return finished;
}

bool CartridgeStorage::Impl::finishLiveWriteOperation(std::string& error)
{
    // A command completion echo only confirms that the USB bridge accepted
    // the operation.  Live metadata is read immediately afterward, so wait
    // until the cartridge reports ready before exposing that state to the
    // verifier.  Full-image programming retains its historical fast path.
    return finishWriteOperation(error) && waitReady(10,error);
}

bool CartridgeStorage::Impl::eraseAll(std::ostream& progress,
                                      std::string& error)
{
    std::vector<std::uint32_t> even;
    for (std::uint32_t address=0;address<=0x8000;address+=0x1000)
        even.push_back(address);
    for (std::uint32_t address=0x10000;address<=0x3F8000;address+=0x8000)
        even.push_back(address);
    std::vector<std::uint32_t> odd;
    for (std::uint32_t address=0;address<=0x3F8000;address+=0x8000)
        odd.push_back(address);
    for (std::uint32_t address=0x3F9000;address<=0x3FF000;address+=0x1000)
        odd.push_back(address);

    const std::size_t total=2*(even.size()+odd.size());
    std::size_t completed=0;
    for (unsigned window=0;window<4;++window) {
        if (window!=0 && !finishWriteOperation(error)) return false;
        if (!selectWriteWindow(window,error)) return false;
        const auto& addresses=(window%2==0)?even:odd;
        for (const auto address:addresses) {
            std::vector<std::uint8_t> command =
                {0x5A,0xA5,0x96,0,
                 static_cast<std::uint8_t>(address),
                 static_cast<std::uint8_t>(address>>8),
                 static_cast<std::uint8_t>(address>>16),
                 static_cast<std::uint8_t>(address>>24),0,0,0,0,0};
            std::vector<std::uint8_t> response;
            if (!out(command,error) || !in(response,command.size(),error)) return false;
            if (!std::equal(command.begin(),command.begin()+12,response.begin()) ||
                response[12]!=0) {
                error="cartridge erase command failed in window "+
                      std::to_string(window);return false;
            }
            ++completed;
            if (completed%16==0 || completed==total)
                progress << "\rErasing " << completed << '/' << total << std::flush;
        }
    }
    progress << '\n';
    return finishWriteOperation(error);
}

bool CartridgeStorage::Impl::programImage(
    const std::vector<std::uint8_t>& image,std::ostream& progress,
    std::string& error)
{
    constexpr std::size_t window_size=0x800000;
    constexpr std::size_t block_size=0x10000;
    if (!selectWriteWindow(0,error)) return false;
    for (std::size_t offset=0;offset<image.size();offset+=block_size) {
        if (offset!=0 && offset%window_size==0) {
            if (!finishWriteOperation(error) ||
                !selectWriteWindow(static_cast<unsigned>(offset/window_size),error))
                return false;
        }
        const auto size=std::min(block_size,image.size()-offset);
        const auto local=static_cast<std::uint32_t>(offset%window_size);
        std::vector<std::uint8_t> data(image.begin()+static_cast<std::ptrdiff_t>(offset),
                                       image.begin()+static_cast<std::ptrdiff_t>(offset+size));
        if(!programTransaction(local/2,data,"cartridge program",error))return false;
        progress << "\rProgramming " << offset+size << '/' << image.size() << std::flush;
    }
    progress << '\n';
    return finishWriteOperation(error);
}

bool CartridgeStorage::Impl::programImageRange(
    std::size_t offset,const std::vector<std::uint8_t>& bytes,
    std::ostream& progress,std::string& error)
{
    constexpr std::size_t window_size=0x800000;
    if(bytes.empty()||offset>live::NorFlash::capacity||
       bytes.size()>live::NorFlash::capacity-offset||
       offset/window_size!=(offset+bytes.size()-1)/window_size) {
        error="invalid cartridge image programming range";return false;
    }
    const auto window=static_cast<unsigned>(offset/window_size);
    if(!selectWriteWindow(window,error))return false;
    const auto local=static_cast<std::uint32_t>(offset%window_size);
    if(!programTransaction(local/2,bytes,"cartridge format metadata",error))
        return false;
    progress<<"Programmed EZFA3FS metadata ("<<bytes.size()/1024<<" KiB).\n";
    return finishWriteOperation(error);
}
bool CartridgeStorage::Impl::eraseLiveBlock(std::size_t block,std::ostream& progress,std::string& error,bool allow_metadata)
{
    return eraseLiveBlockPrefix(block,live::NorFlash::block_size,progress,
                                error,allow_metadata);
}
bool CartridgeStorage::Impl::eraseLiveBlocks(
    const std::vector<std::size_t>& blocks,std::ostream& progress,
    std::string& error,bool allow_metadata,bool wait_until_ready) {
    if(blocks.empty()){error.clear();return true;}
    if(!std::is_sorted(blocks.begin(),blocks.end())||
       std::adjacent_find(blocks.begin(),blocks.end())!=blocks.end()) {
        error="live erase batch must contain sorted unique blocks";return false;
    }
    if(std::any_of(blocks.begin(),blocks.end(),[allow_metadata](std::size_t block) {
           return (!allow_metadata&&block<2)||block>=live::NorFlash::block_count;
       })) {
        error="live erase batch contains a block outside the permitted range";
        return false;
    }
    std::size_t sector_count=0;
    for(const auto block:blocks)
        sector_count+=CartridgeFlashGeometry::sectorsForLogicalBlock(block).size();
    std::size_t completed_sectors=0;
    unsigned displayed_percent=101;
    const auto report_progress=[&] {
        const auto percent=static_cast<unsigned>(
            completed_sectors*100/sector_count);
        if(percent==displayed_percent)return;
        displayed_percent=percent;
        progress<<"\rErasing EZFA3FS extent: "<<percent<<'%'<<std::flush;
    };
    report_progress();
    unsigned selected_window=4;
    for(const auto block:blocks) {
        const auto sectors=CartridgeFlashGeometry::sectorsForLogicalBlock(block);
        const auto window=sectors.front().window;
        if(window!=selected_window) {
            if(selected_window!=4&&!finishLiveWriteOperation(error))return false;
            if(!selectWriteWindow(window,error))return false;
            selected_window=window;
        }
        for(const auto& sector:sectors) {
            const auto address=sector.word_address;
            std::vector<std::uint8_t> command={0x5A,0xA5,0x96,0,
                static_cast<std::uint8_t>(address),
                static_cast<std::uint8_t>(address>>8),
                static_cast<std::uint8_t>(address>>16),
                static_cast<std::uint8_t>(address>>24),0,0,0,0,0};
            std::vector<std::uint8_t> response;
            if(!out(command,error)||!in(response,command.size(),error))return false;
            if(response.size()!=command.size()||
               !std::equal(command.begin(),command.begin()+12,response.begin())||
               response[12]!=0) {
                std::ostringstream detail;
                detail<<"cartridge live batch erase response mismatch";
                if(response.size()==command.size())
                    detail<<" (status 0x"<<std::hex
                          <<static_cast<unsigned>(response[12])<<')'<<std::dec;
                error=detail.str();return false;
            }
            ++completed_sectors;
            report_progress();
        }
    }
    const bool finished=wait_until_ready?finishLiveWriteOperation(error):
                                         finishWriteOperation(error);
    if(!finished)return false;
    progress<<'\n';error.clear();return true;
}
bool CartridgeStorage::Impl::eraseLiveBlockPrefix(
    std::size_t block,std::size_t byte_count,std::ostream& progress,
    std::string& error,bool allow_metadata)
{
    if((!allow_metadata&&block<2)||block>=0x200||byte_count==0||
       byte_count>live::NorFlash::block_size) {
        error="live erase block prefix is outside the permitted range";return false;
    }
    const auto sectors=CartridgeFlashGeometry::sectorsCoveringBlockPrefix(
        block,byte_count);
    if(!selectWriteWindow(sectors.front().window,error))return false;
    for(const auto& sector:sectors){const auto address=sector.word_address;
        std::vector<std::uint8_t> command={0x5A,0xA5,0x96,0,static_cast<std::uint8_t>(address),static_cast<std::uint8_t>(address>>8),static_cast<std::uint8_t>(address>>16),static_cast<std::uint8_t>(address>>24),0,0,0,0,0};
        std::vector<std::uint8_t> response;
        if(!out(command,error)||!in(response,command.size(),error))return false;
        if(response.size()!=command.size()||!std::equal(command.begin(),command.begin()+12,response.begin())||response[12]!=0){
            std::ostringstream detail;detail<<"cartridge live block erase response mismatch";
            if(response.size()==command.size())detail<<" (status 0x"<<std::hex<<static_cast<unsigned>(response[12])<<')'<<std::dec;
            else detail<<" (received "<<response.size()<<" bytes, expected "<<command.size()<<')';
            error=detail.str();return false;
        }
        progress<<"Erase response status: 0x"<<std::hex<<static_cast<unsigned>(response[12])<<std::dec<<"\n";
    }
    if(!finishLiveWriteOperation(error))return false;progress<<"Erased cartridge block "<<block<<".\n";return true;
}
bool CartridgeStorage::Impl::programLiveBlock(std::size_t block,const std::vector<std::uint8_t>& bytes,std::ostream& progress,std::string& error,bool allow_metadata)
{
    std::size_t completed=0;
    return programLiveExtent(block,bytes,completed,progress,error,allow_metadata);
}
bool CartridgeStorage::Impl::programLiveBlockPrefix(
    std::size_t block,const std::vector<std::uint8_t>& bytes,
    std::ostream& progress,std::string& error,bool allow_metadata)
{
    constexpr std::size_t blocks_per_window=0x80;
    if(bytes.empty()||bytes.size()>live::NorFlash::block_size||block>=0x200||
       (!allow_metadata&&block<2)) {
        error="live block-prefix programming is outside the permitted range";
        return false;
    }
    const auto window=static_cast<unsigned>(block/blocks_per_window);
    const auto local=static_cast<std::uint32_t>(
        (block%blocks_per_window)*0x8000u);
    if(!selectWriteWindow(window,error)||
       !programTransaction(local,bytes,"cartridge live metadata program",error)||
       !finishLiveWriteOperation(error))return false;
    progress<<"Programmed cartridge block "<<block<<" metadata prefix ("
            <<bytes.size()/1024<<" KiB).\n";
    error.clear();return true;
}
bool CartridgeStorage::Impl::programLiveExtent(
    std::size_t first_block,const std::vector<std::uint8_t>& bytes,
    std::size_t& completed_blocks,std::ostream& progress,std::string& error,
    bool allow_metadata) {
    constexpr std::size_t block_size=live::NorFlash::block_size;
    constexpr std::size_t blocks_per_window=0x80;
    completed_blocks=0;
    const auto block_count=bytes.size()/block_size;
    if(bytes.empty()||bytes.size()%block_size||first_block>=0x200||
       block_count>0x200-first_block||(!allow_metadata&&first_block<2)) {
        error="live extent programming is outside the permitted range";return false;
    }
    unsigned selected_window=4;
    for(std::size_t i=0;i<block_count;++i) {
        const auto block=first_block+i;
        const auto window=static_cast<unsigned>(block/blocks_per_window);
        if(window!=selected_window) {
            if(selected_window!=4&&!finishLiveWriteOperation(error))return false;
            if(!selectWriteWindow(window,error))return false;
            selected_window=window;
        }
        const auto local=static_cast<std::uint32_t>(
            (block%blocks_per_window)*0x8000u);
        std::vector<std::uint8_t> data(
            bytes.begin()+static_cast<std::ptrdiff_t>(i*block_size),
            bytes.begin()+static_cast<std::ptrdiff_t>((i+1)*block_size));
        if(!programTransaction(local,data,"cartridge live block program",error)) {
            if(block_count>1)progress<<'\n';
            return false;
        }
        ++completed_blocks;
        if(block_count>1)
            progress<<"\rProgramming EZFA3FS extent: "
                    <<(completed_blocks*100/block_count)<<'%'<<std::flush;
    }
    if(!finishLiveWriteOperation(error))return false;
    if(block_count>1)progress<<'\n';
    else progress<<"Programmed cartridge block "<<first_block<<".\n";
    error.clear();return true;
}
#else
bool CartridgeStorage::Impl::eraseAll(std::ostream&,std::string& error)
{
    error="EZFA3FS was built without libusb support";return false;
}

bool CartridgeStorage::Impl::programImage(
    const std::vector<std::uint8_t>&,std::ostream&,std::string& error)
{
    error="EZFA3FS was built without libusb support";return false;
}
bool CartridgeStorage::Impl::programImageRange(
    std::size_t,const std::vector<std::uint8_t>&,std::ostream&,
    std::string& error)
{ error="EZFA3FS was built without libusb support";return false; }
bool CartridgeStorage::Impl::clearSaveBanks(std::string& error)
{ error="EZFA3FS was built without libusb support";return false; }
bool CartridgeStorage::Impl::eraseLiveBlock(std::size_t,std::ostream&,std::string& error,bool)
{ error="EZFA3FS was built without libusb support";return false; }
bool CartridgeStorage::Impl::eraseLiveBlocks(const std::vector<std::size_t>&,
                                             std::ostream&,std::string& error,
                                             bool,bool)
{ error="EZFA3FS was built without libusb support";return false; }
bool CartridgeStorage::Impl::eraseLiveBlockPrefix(std::size_t,std::size_t,
                                                  std::ostream&,std::string& error,bool)
{ error="EZFA3FS was built without libusb support";return false; }
bool CartridgeStorage::Impl::programLiveBlock(std::size_t,const std::vector<std::uint8_t>&,std::ostream&,std::string& error,bool)
{ error="EZFA3FS was built without libusb support";return false; }
bool CartridgeStorage::Impl::programLiveBlockPrefix(std::size_t,
                                                    const std::vector<std::uint8_t>&,
                                                    std::ostream&,std::string& error,bool)
{ error="EZFA3FS was built without libusb support";return false; }
bool CartridgeStorage::Impl::programLiveExtent(std::size_t,const std::vector<std::uint8_t>&,
                                               std::size_t& completed,std::ostream&,
                                               std::string& error,bool)
{ completed=0;error="EZFA3FS was built without libusb support";return false; }
#endif

bool CartridgeStorage::Impl::open(std::string& error)
{
#if !defined(EZFA3FS_HAS_LIBUSB)
    error = "EZFA3FS was built without libusb support";
    return false;
#else
    error.clear(); shutdown();
    int result = libusb_init(&context);
    if (result != 0) { error = usbError("libusb initialization failed",result); shutdown(); return false; }
    handle = libusb_open_device_with_vid_pid(context,0x0E6A,0x5088);
    if (!handle) { error = "EZ-Flash Advance III USB device not found"; shutdown(); return false; }
#if defined(__linux__)
    libusb_set_auto_detach_kernel_driver(handle,1);
#endif
    result = libusb_claim_interface(handle,0);
    if (result != 0) { error = usbError("could not claim USB interface 0",result); shutdown(); return false; }
    claimed = true;
    if (!initialize(error,false)) { shutdown(); return false; }
    is_open = true;
    return true;
#endif
}

bool CartridgeStorage::Impl::openForProgramming(std::string& error,
                                                bool trust_validated_format,
                                                bool preserve_save_banks)
{
#if !defined(EZFA3FS_HAS_LIBUSB)
    (void)trust_validated_format;
    (void)preserve_save_banks;
    error="EZFA3FS was built without libusb support";return false;
#else
    error.clear();shutdown();
    int result=libusb_init(&context);
    if(result!=0){error=usbError("libusb initialization failed",result);shutdown();return false;}
    handle=libusb_open_device_with_vid_pid(context,0x0E6A,0x5088);
    if(!handle){error="EZ-Flash Advance III USB device not found";shutdown();return false;}
#if defined(__linux__)
    libusb_set_auto_detach_kernel_driver(handle,1);
#endif
    result=libusb_claim_interface(handle,0);
    if(result!=0){error=usbError("could not claim USB interface 0",result);shutdown();return false;}
    claimed=true;
    save_preservation_enabled=preserve_save_banks;
    if(preserve_save_banks&&!captureSaveBanks(error)){shutdown();return false;}
    if(!initialize(error,true,trust_validated_format)||!activateWriter(error)) {
        const auto writer_error=error;
        std::string restore_error;
        if(preserve_save_banks&&!restoreSaveBanks(restore_error))
            error=writer_error+"; could not restore cartridge save banks: "+
                  restore_error;
        else
            error=writer_error;
        shutdown();return false;
    }
    is_open=true;return true;
#endif
}

bool CartridgeStorage::Impl::close(std::string& error,bool settle_before_release)
{
#if defined(EZFA3FS_HAS_LIBUSB)
    bool finished = true;
    if (is_open) {
        for (unsigned i=0; i<3 && finished; ++i) finished = waitReady(1,error);
        if (finished&&settle_before_release)
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    std::string restore_error;
    const bool restored=!settle_before_release||!handle||
                        restoreSaveBanks(restore_error);
    shutdown();
    if(!restored) {
        if(!error.empty())error+="; ";
        error+="could not restore cartridge save banks: "+restore_error;
    }
    return finished&&restored;
#else
    (void)settle_before_release;
    error.clear(); return true;
#endif
}

void CartridgeStorage::Impl::shutdown() noexcept
{
#if defined(EZFA3FS_HAS_LIBUSB)
    is_open = false;
    if (handle && claimed) libusb_release_interface(handle,0);
    claimed = false;
    if (handle) libusb_close(handle);
    handle = nullptr;
    if (context) libusb_exit(context);
    context = nullptr;
    // Initialization and the captured post-write status sequence leave the
    // first 8 MiB directly readable.  Expanding this mapping for a low read
    // selects the wrong cartridge view on real hardware.
    mapped_limit = 0x00800000u;
#else
    is_open = false;
#endif
}

bool CartridgeStorage::Impl::read(std::uint64_t offset,
                                  std::uint8_t* destination,
                                  std::size_t size, std::string& error)
{
#if !defined(EZFA3FS_HAS_LIBUSB)
    (void)offset; (void)destination; (void)size;
    error = "EZFA3FS was built without libusb support"; return false;
#else
    if (!is_open) { error = "cartridge is not open"; return false; }
    if ((offset & 1u) != 0) { error = "cartridge read offset must be word-aligned"; return false; }
    if (offset > CartridgeStorage::cartridge_capacity ||
        size > CartridgeStorage::cartridge_capacity - offset) {
        error = "read exceeds the 32-MiB cartridge capacity"; return false;
    }
    if (!prepareMapping(offset + size,error)) return false;
    return rawRead(static_cast<std::uint32_t>(offset),destination,size,error);
#endif
}

CartridgeStorage::CartridgeStorage() : impl_(new Impl) {}
CartridgeStorage::~CartridgeStorage() = default;
bool CartridgeStorage::open(std::string& error) { return impl_->open(error); }
bool CartridgeStorage::close(std::string& error) { return impl_->close(error); }
bool CartridgeStorage::openForLiveWrite(std::string& error) {
    return openLiveWriteSessionWithRetry(error,false);
}
bool CartridgeStorage::openLiveWriteSessionWithRetry(
    std::string& error,bool trust_validated_format) {
    constexpr unsigned attempts=3;
    for(unsigned attempt=1;attempt<=attempts;++attempt) {
        if(impl_->openForProgramming(error,trust_validated_format))return true;
        if(attempt<attempts) {
            std::cerr<<"Retrying cartridge writer initialization (attempt "
                     <<(attempt+1)<<'/'<<attempts<<"): "<<error<<'\n';
            std::this_thread::sleep_for(std::chrono::milliseconds(250*attempt));
        }
    }
    return false;
}
bool CartridgeStorage::restartLiveWriteSession(std::string& error) {
    std::string close_error;const bool closed=impl_->close(close_error,false);
    if(openLiveWriteSessionWithRetry(error,true))return true;
    if(!closed&&!close_error.empty())error+="; close also failed: "+close_error;
    return false;
}
bool CartridgeStorage::readLiveFilesystem(
    std::uint64_t offset,std::uint8_t* destination,std::size_t size,
    std::string& error) {
    constexpr unsigned attempts=3;
    for(unsigned attempt=1;attempt<=attempts;++attempt) {
        if(read(offset,destination,size,error))return true;
        if(attempt==attempts)return false;
        const auto read_error=error;
        std::cerr<<"Retrying live cartridge read (attempt "<<(attempt+1)
                 <<'/'<<attempts<<"): "<<read_error<<'\n';
        std::string restart_error;
        if(!restartLiveWriteSession(restart_error)) {
            error=read_error+"; live read restart failed: "+restart_error;
            return false;
        }
    }
    return false;
}
bool CartridgeStorage::eraseLiveBlock(std::size_t block,std::string& error) { return impl_->eraseLiveBlock(block,std::cerr,error); }
bool CartridgeStorage::programLiveBlock(std::size_t block,const std::vector<std::uint8_t>& bytes,std::string& error) { return impl_->programLiveBlock(block,bytes,std::cerr,error); }
bool CartridgeStorage::readLiveBlockAfterWrite(std::size_t block,std::size_t size,
                                               std::vector<std::uint8_t>& bytes,
                                               std::string& error,
                                               bool reopen_first) {
    bytes.resize(size);
    if(!reopen_first&&read(block*live::NorFlash::block_size,bytes.data(),bytes.size(),error)) {
        error.clear();return true;
    }
    const auto direct_error=error;std::string close_error;const bool closed=close(close_error);
    std::string reopen_error;
    if(!openLiveWriteSessionWithRetry(reopen_error,true)) {
        error="could not reopen cartridge after live write: "+reopen_error;
        if(!closed&&!close_error.empty())error+="; close also failed: "+close_error;
        return false;
    }
    if(!read(block*live::NorFlash::block_size,bytes.data(),bytes.size(),error)) {
        error="could not verify live cartridge block "+std::to_string(block)+": "+error;
        if(!direct_error.empty())error+="; direct verification also failed: "+direct_error;
        return false;
    }
    error.clear();return true;
}
bool CartridgeStorage::verifyLiveBlockAfterWrite(
    std::size_t block,const std::vector<std::uint8_t>& expected,
    const char* operation,const std::string& operation_error,
    bool reopen_first,std::string& error) {
    constexpr unsigned verification_attempts=3;
    std::string verification_error,last_mismatch;
    for(unsigned attempt=1;attempt<=verification_attempts;++attempt) {
        std::vector<std::uint8_t> readback;
        std::string read_error;
        // Successful data writes normally need only a short settling reread.
        // Reserve the more invasive USB reinitialization for the final check;
        // metadata and failed completion responses still reopen immediately.
        const bool reopen=reopen_first||attempt==verification_attempts;
        if(readLiveBlockAfterWrite(block,expected.size(),readback,read_error,reopen)) {
            const auto mismatch=std::mismatch(readback.begin(),readback.end(),
                                              expected.begin());
            if(mismatch.first==readback.end()){error.clear();return true;}
            std::ostringstream detail;
            detail<<operation<<" readback block "<<block
                  <<" differs at byte 0x"<<std::hex
                  <<static_cast<std::size_t>(mismatch.first-readback.begin())
                  <<" (read 0x"<<static_cast<unsigned>(*mismatch.first)
                  <<", expected 0x"<<static_cast<unsigned>(*mismatch.second)
                  <<')'<<std::dec;
            last_mismatch=detail.str();verification_error=last_mismatch;
        } else {
            verification_error=read_error;
            if(!last_mismatch.empty())
                verification_error+="; last readback result: "+last_mismatch;
        }
        if(attempt<verification_attempts)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    error=operation_error;
    if(!error.empty()&&!verification_error.empty())error+="; ";
    error+=verification_error;
    return false;
}
bool CartridgeStorage::eraseLiveFilesystemBlock(std::size_t block,std::string& error) {
    constexpr unsigned attempts=3;
    const bool metadata_block=block<2||block>=live::NorFlash::block_count-2;
    const std::vector<std::uint8_t> erased(live::NorFlash::block_size,0xFF);
    for(unsigned attempt=1;attempt<=attempts;++attempt) {
        std::string operation_error;
        if(!isOpen()&&!openLiveWriteSessionWithRetry(operation_error,true)) {
            error="could not restore cartridge writer before erase retry: "+operation_error;
            return false;
        }
        const bool completed=impl_->eraseLiveBlock(block,std::cerr,operation_error,true);
        // Metadata occupies the flash's boot sectors.  The cartridge can keep
        // returning the pre-write/erased view there until the USB session is
        // reopened, even after a successful write-window completion.
        if(verifyLiveBlockAfterWrite(block,erased,"erase",operation_error,
                                     !completed||metadata_block,error))return true;
        if(attempt<attempts)std::cerr<<"Retrying live block erase (attempt "<<(attempt+1)<<'/'<<attempts<<"): "<<error<<'\n';
    }
    return false;
}
bool CartridgeStorage::eraseLiveFilesystemBlocks(
    const std::vector<std::size_t>& blocks,std::string& error) {
    if(blocks.empty()){error.clear();return true;}
    std::string operation_error;
    if(!isOpen()&&!openLiveWriteSessionWithRetry(operation_error,true)) {
        error="could not restore cartridge writer before batch erase: "+
              operation_error;return false;
    }
    const bool completed=impl_->eraseLiveBlocks(
        blocks,std::cerr,operation_error,true);
    const std::vector<std::uint8_t> blank(live::NorFlash::block_size,0xFF);
    std::vector<std::size_t> failed_blocks;
    for(std::size_t index=0;index<blocks.size();++index) {
        std::vector<std::uint8_t> readback;
        std::string read_error;
        if(!readLiveBlockAfterWrite(blocks[index],blank.size(),readback,
                                    read_error,index==0)||readback!=blank)
            failed_blocks.push_back(blocks[index]);
    }
    for(const auto block:failed_blocks) {
        std::string retry_error;
        if(!eraseLiveFilesystemBlock(block,retry_error)) {
            error="batch erase could not recover block "+
                  std::to_string(block)+": "+retry_error;
            if(!completed&&!operation_error.empty())
                error+="; initial batch also failed: "+operation_error;
            return false;
        }
    }
    std::cerr<<"Verified "<<blocks.size()
             <<" erased cartridge block(s)";
    if(!failed_blocks.empty())
        std::cerr<<" ("<<failed_blocks.size()<<" repaired individually)";
    std::cerr<<".\n";
    error.clear();return true;
}
bool CartridgeStorage::programLiveFilesystemBlock(std::size_t block,const std::vector<std::uint8_t>& bytes,std::string& error) {
    constexpr unsigned attempts=3;
    const bool metadata_block=block<2||block>=live::NorFlash::block_count-2;
    for(unsigned attempt=1;attempt<=attempts;++attempt) {
        std::string operation_error;
        if(!isOpen()&&!openLiveWriteSessionWithRetry(operation_error,true)) {
            error="could not restore cartridge writer before program retry: "+operation_error;
            return false;
        }
        const bool completed=impl_->programLiveBlock(block,bytes,std::cerr,operation_error,true);
        if(verifyLiveBlockAfterWrite(block,bytes,"program",operation_error,
                                     !completed||metadata_block,error))return true;
        if(attempt==attempts)return false;
        std::cerr<<"Retrying live block program (attempt "<<(attempt+1)<<'/'<<attempts<<"): "<<error<<'\n';
        std::string erase_error;
        if(!eraseLiveFilesystemBlock(block,erase_error)) {
            error+="; retry erase failed: "+erase_error;return false;
        }
        // Erase verification leaves the bridge in its read mapping. A fresh
        // writer session is required before replaying the program command;
        // otherwise real hardware can acknowledge neither command nor data
        // and leave the block completely blank.
        std::string restart_error;
        if(!restartLiveWriteSession(restart_error)) {
            error+="; retry writer restart failed: "+restart_error;return false;
        }
    }
    return false;
}
bool CartridgeStorage::replaceLiveFilesystemMetadata(
    std::size_t block,const std::vector<std::uint8_t>& bytes,
    std::string& error) {
    constexpr unsigned attempts=3;
    if(block>=live::NorFlash::block_count||
       bytes.size()!=live::NorFlash::block_size) {
        error="live filesystem block replacement is out of range";return false;
    }
    // The final logical block is the flash's split top boot block. The only
    // capture-proven program operation at that boundary is one complete 64-KiB
    // transaction after erasing all eight physical sectors. Ordinary metadata
    // blocks retain the smaller 8-KiB-rounded transfer.
    const auto transfer_size=block==live::NorFlash::block_count-1?
        live::NorFlash::block_size:metadataTransferSize(bytes);
    const std::vector<std::uint8_t> prefix(bytes.begin(),
        bytes.begin()+static_cast<std::ptrdiff_t>(transfer_size));
    for(unsigned attempt=1;attempt<=attempts;++attempt) {
        std::string restart_error;
        if(!restartLiveWriteSession(restart_error)) {
            error="could not prepare cartridge writer for metadata replacement: "+
                  restart_error;
            return false;
        }
        std::string operation_error;
        const bool erased=impl_->eraseLiveBlockPrefix(
            block,transfer_size,std::cerr,operation_error,true);
        if(!erased) {
            error=operation_error;
        } else {
            // Do not inspect the erased sector here. A read switches the
            // bridge away from the capture-proven erase -> program state and
            // caused block 511 to remain blank despite a successful command
            // response. Final payload verification proves the resulting
            // contents while the other superblock remains the recovery point.
            const bool programmed=impl_->programLiveBlockPrefix(
                block,prefix,std::cerr,operation_error,true);
            if(verifyLiveBlockAfterWrite(
                    block,prefix,"metadata replacement",operation_error,
                    true,error))return true;
            if(!programmed&&!operation_error.empty()&&
               error.find(operation_error)==std::string::npos)
                error=operation_error+"; "+error;
        }
        if(attempt==attempts)return false;
        std::cerr<<"Retrying live block replacement (attempt "
                 <<(attempt+1)<<'/'<<attempts<<"): "<<error<<'\n';
    }
    return false;
}
bool CartridgeStorage::programLiveFilesystemExtent(
    std::size_t first_block,const std::vector<std::uint8_t>& bytes,
    std::size_t& completed_blocks,std::string& error) {
    constexpr std::size_t block_size=live::NorFlash::block_size;
    completed_blocks=0;
    std::string operation_error;
    if(!isOpen()&&!openLiveWriteSessionWithRetry(operation_error,true)) {
        error="could not restore cartridge writer before extent programming: "+operation_error;
        return false;
    }
    const bool transferred=impl_->programLiveExtent(first_block,bytes,
        completed_blocks,std::cerr,operation_error,true);
    const auto block_count=bytes.size()/block_size;
    const auto verify_count=transferred?block_count:
        std::min(block_count,completed_blocks+1);
    for(std::size_t i=0;i<verify_count;++i) {
        std::vector<std::uint8_t> expected(
            bytes.begin()+static_cast<std::ptrdiff_t>(i*block_size),
            bytes.begin()+static_cast<std::ptrdiff_t>((i+1)*block_size));
        const auto verification_error=(transferred||i<completed_blocks)?
            std::string{}:operation_error;
        if(!verifyLiveBlockAfterWrite(first_block+i,expected,"program",
                                      verification_error,
                                      !transferred&&i==0,error)) {
            // An extent transfer shares one USB write session.  A transient
            // failure can therefore leave exactly one completed transaction
            // unreadable while its neighbours are sound. Do not discard an
            // otherwise valid large-file transfer merely because its bulk
            // verification encountered that recoverable condition.
            const auto verification_failure=error;
            std::string recovery_error;
            // A failed extent transaction may have programmed only a subset
            // of the expected zero bits. Replaying the program command over
            // that data cannot perform the 0-to-1 transitions needed to
            // repair it. Start recovery from a verified erased block, then
            // reopen the writer because erase verification leaves the bridge
            // in its read mapping.
            if(!eraseLiveFilesystemBlock(first_block+i,recovery_error)) {
                error=verification_failure+"; block recovery erase failed: "+
                      recovery_error;
                completed_blocks=i;return false;
            }
            if(!restartLiveWriteSession(recovery_error)) {
                error=verification_failure+"; block recovery writer restart failed: "+
                      recovery_error;
                completed_blocks=i;return false;
            }
            if(!programLiveFilesystemBlock(first_block+i,expected,recovery_error)) {
                error=verification_failure+"; block recovery failed: "+
                      recovery_error;
                completed_blocks=i;return false;
            }
        }
    }
    if(!transferred) {
        if(verify_count==block_count){completed_blocks=block_count;error.clear();return true;}
        error=operation_error;return false;
    }
    completed_blocks=block_count;error.clear();return true;
}

bool CartridgeStorage::replaceLiveFilesystemExtent(
    std::size_t first_block,const std::vector<std::uint8_t>& bytes,
    const std::vector<std::size_t>& erase_blocks,
    std::size_t& completed_blocks,std::string& error) {
    constexpr std::size_t block_size=live::NorFlash::block_size;
    constexpr unsigned attempts=3;
    completed_blocks=0;
    if(bytes.empty()||bytes.size()%block_size||
       first_block>=live::NorFlash::block_count||
       bytes.size()/block_size>live::NorFlash::block_count-first_block||
       !std::is_sorted(erase_blocks.begin(),erase_blocks.end())||
       std::adjacent_find(erase_blocks.begin(),erase_blocks.end())!=
           erase_blocks.end()) {
        error="live filesystem replacement extent is invalid";
        return false;
    }
    const auto block_count=bytes.size()/block_size;
    const auto extent_end=first_block+block_count;
    if(std::any_of(erase_blocks.begin(),erase_blocks.end(),
                   [first_block,extent_end](std::size_t block) {
                       return block<first_block||block>=extent_end;
                   })) {
        error="live filesystem replacement erase is outside its extent";
        return false;
    }

    std::size_t next=0;
    std::string last_error;
    for(unsigned attempt=1;attempt<=attempts&&next<block_count;++attempt) {
        std::string operation_error;
        if(!restartLiveWriteSession(operation_error)) {
            last_error="could not prepare cartridge writer for extent replacement: "+
                       operation_error;
        } else {
            std::vector<std::size_t> blocks_to_erase;
            if(attempt==1) {
                std::copy_if(erase_blocks.begin(),erase_blocks.end(),
                             std::back_inserter(blocks_to_erase),
                             [first_block,next](std::size_t block) {
                                 return block>=first_block+next;
                             });
            } else {
                for(std::size_t index=next;index<block_count;++index)
                    blocks_to_erase.push_back(first_block+index);
            }

            // Keep the capture-proven erase -> finish -> select -> program
            // transition in one initialized USB session. Reading the blank
            // extent or reopening the device here leaves block zero in the
            // linear read mapping and the bridge accepts, but drops, its data.
            const bool erased=blocks_to_erase.empty()||
                impl_->eraseLiveBlocks(blocks_to_erase,std::cerr,
                                       operation_error,true,false);
            if(!erased) {
                last_error=operation_error;
            } else {
                const std::vector<std::uint8_t> remaining(
                    bytes.begin()+static_cast<std::ptrdiff_t>(next*block_size),
                    bytes.end());
                std::size_t transferred_blocks=0;
                const bool transferred=impl_->programLiveExtent(
                    first_block+next,remaining,transferred_blocks,std::cerr,
                    operation_error,true);
                const auto remaining_count=block_count-next;
                const auto verify_count=transferred?remaining_count:
                    std::min(remaining_count,transferred_blocks+1);
                std::size_t verified=0;
                for(;verified<verify_count;++verified) {
                    const auto index=next+verified;
                    const std::vector<std::uint8_t> expected(
                        bytes.begin()+static_cast<std::ptrdiff_t>(index*block_size),
                        bytes.begin()+static_cast<std::ptrdiff_t>((index+1)*block_size));
                    const auto verification_error=
                        (transferred||verified<transferred_blocks)?
                            std::string{}:operation_error;
                    if(!verifyLiveBlockAfterWrite(
                            first_block+index,expected,"replacement program",
                            verification_error,
                            first_block+index<2||(!transferred&&verified==0),
                            last_error))break;
                }
                next+=verified;
                completed_blocks=next;
                if(next==block_count) {
                    error.clear();
                    return true;
                }
                if(verified==verify_count&&last_error.empty())
                    last_error=operation_error.empty()?
                        "cartridge replacement transfer was incomplete":
                        operation_error;
            }
        }
        if(attempt<attempts) {
            std::cerr<<"Retrying live extent replacement (attempt "
                     <<(attempt+1)<<'/'<<attempts<<"): "<<last_error<<'\n';
        }
    }
    error=last_error.empty()?"cartridge replacement extent was incomplete":
                             last_error;
    return false;
}
bool CartridgeStorage::isOpen() const noexcept { return impl_->is_open; }
std::array<std::uint8_t,4> CartridgeStorage::flashId() const noexcept { return impl_->flash_id; }
std::uint64_t CartridgeStorage::capacity() const noexcept { return cartridge_capacity; }
bool CartridgeStorage::read(std::uint64_t offset,std::uint8_t* destination,
                            std::size_t size,std::string& error)
{ return impl_->read(offset,destination,size,error); }

bool CartridgeProgrammer::program(
    const std::vector<std::uint8_t>& image,const ProgramOptions& options,
    std::ostream& progress,std::string& error)
{
    live::NorFlash live_flash;live::Filesystem live_filesystem(live_flash);
    if(!live_flash.load(image,error)||
       !live::Filesystem::open(live_flash,live_filesystem,error)||
       !live_filesystem.verify(error)) {
        error="not an EZFA3FS image";return false;
    }
    if(!storage_.impl_->openForProgramming(error)) return false;
    constexpr unsigned write_attempts=3;
    bool written=false;
    for(unsigned attempt=1;attempt<=write_attempts;++attempt) {
        progress << "Erasing the complete 32-MiB cartridge...\n";
        if(storage_.impl_->eraseAll(progress,error)&&
           storage_.impl_->programImage(image,progress,error)) {
            written=true;
            break;
        }
        if(attempt==write_attempts)break;
        progress<<"Retrying complete cartridge write (attempt "
                <<(attempt+1)<<'/'<<write_attempts<<"): "<<error<<'\n';
        std::string close_error;
        storage_.impl_->close(close_error,false);
        if(!storage_.impl_->openForProgramming(error,true))break;
    }
    if(!written) {
        std::string ignored;storage_.close(ignored);return false;
    }
    if(!storage_.close(error)) return false;

    if(!options.verify_after_write) {
        progress << "Skipped cartridge read-back verification.\n";
        return true;
    }

    progress << "Reopening cartridge for byte-for-byte verification...\n";
    if(!storage_.open(error)) return false;
    std::vector<std::uint8_t> block(live::NorFlash::block_size);
    for(std::size_t offset=0;offset<image.size();offset+=block.size()) {
        if(!storage_.read(offset,block.data(),block.size(),error)) {
            std::string ignored;storage_.close(ignored);return false;
        }
        const auto mismatch=std::mismatch(block.begin(),block.end(),image.begin()+
            static_cast<std::ptrdiff_t>(offset));
        if(mismatch.first!=block.end()) {
            error="read-back mismatch at cartridge byte "+
                  std::to_string(offset+static_cast<std::size_t>(mismatch.first-block.begin()));
            std::string ignored;storage_.close(ignored);return false;
        }
        progress << "\rVerifying " << offset+block.size() << '/' << image.size() << std::flush;
    }
    progress << '\n';
    return storage_.close(error);
}

bool CartridgeProgrammer::format(
    CartridgeFormatLayout layout,std::ostream& progress,std::string& error)
{
    live::NorFlash image;
    const bool direct_boot=layout==CartridgeFormatLayout::direct_boot;
    if(!(direct_boot?live::Filesystem::formatDirectBootEmpty(image,error):
                     live::Filesystem::format(image,error)))return false;

    live::Filesystem filesystem(image);
    if(!live::Filesystem::open(image,filesystem,error))return false;
    const auto metadata_block=direct_boot?live::NorFlash::block_count-1:
                                          std::size_t{1};
    const auto metadata_offset=metadata_block*live::NorFlash::block_size;
    std::vector<std::uint8_t> metadata(live::NorFlash::block_size);
    if(!image.read(metadata_offset,metadata.data(),metadata.size(),error))return false;
    if(!direct_boot)metadata.resize(metadataTransferSize(metadata));

    if(!storage_.impl_->openForProgramming(error,false,false))return false;
    progress<<"Erasing the complete 32-MiB cartridge...\n";
    if(!storage_.impl_->eraseAll(progress,error)||
       !storage_.impl_->programImageRange(metadata_offset,metadata,
                                          progress,error)||
       !storage_.impl_->clearSaveBanks(error)) {
        std::string ignored;storage_.close(ignored);return false;
    }
    progress<<"Cleared and verified all four cartridge save banks.\n";
    if(!storage_.close(error))return false;

    progress<<"Verifying EZFA3FS metadata...\n";
    if(!storage_.open(error))return false;
    std::vector<std::uint8_t> readback(metadata.size());
    const bool read=storage_.read(metadata_offset,readback.data(),readback.size(),error);
    std::string close_error;const bool closed=storage_.close(close_error);
    if(!read||!closed){if(error.empty())error=close_error;return false;}
    const auto mismatch=std::mismatch(readback.begin(),readback.end(),metadata.begin());
    if(mismatch.first!=readback.end()) {
        error="format metadata read-back mismatch at cartridge byte "+
              std::to_string(metadata_offset+
                  static_cast<std::size_t>(mismatch.first-readback.begin()));
        return false;
    }
    error.clear();return true;
}

bool CartridgeProgrammer::eraseLiveBlock(std::size_t block,std::ostream& progress,std::string& error)
{
    if(block<2||block>=0x200){error="live erase only permits blocks 2 through 511";return false;}
    if(!storage_.impl_->openForProgramming(error))return false;
    if(!storage_.impl_->eraseLiveBlock(block,progress,error)){std::string ignored;storage_.close(ignored);return false;}
    if(!storage_.close(error))return false;
    if(!storage_.open(error))return false;
    std::vector<std::uint8_t> bytes(0x10000);const bool read=storage_.read(block*0x10000,bytes.data(),bytes.size(),error);std::string close_error;const bool closed=storage_.close(close_error);
    if(!read||!closed){if(error.empty())error=close_error;return false;}
    progress<<"Erase readback CRC32: 0x"<<std::hex<<ezfa3fs::Crc32::calculate(bytes.data(),bytes.size())<<std::dec<<"\n";
    const auto first_nonblank=std::find_if(bytes.begin(),bytes.end(),[](std::uint8_t byte){return byte!=0xFF;});
    if(first_nonblank!=bytes.end()){progress<<"First non-FF byte: 0x"<<std::hex<<static_cast<std::size_t>(first_nonblank-bytes.begin())<<" value 0x"<<static_cast<unsigned>(*first_nonblank)<<std::dec<<"\n";error="live block erase readback is not blank";return false;}
    progress<<"Verified erased cartridge block "<<block<<".\n";return true;
}
bool CartridgeProgrammer::programLiveBlock(std::size_t block,const std::vector<std::uint8_t>& bytes,std::ostream& progress,std::string& error)
{
    if(block<2||block>=0x200||bytes.size()!=0x10000){error="live block programming requires a 64-KiB block 2 through 511";return false;}
    if(!storage_.impl_->openForProgramming(error))return false;
    if(!storage_.impl_->programLiveBlock(block,bytes,progress,error)){std::string ignored;storage_.close(ignored);return false;}
    if(!storage_.close(error))return false;if(!storage_.open(error))return false;std::vector<std::uint8_t> readback(bytes.size());
    const bool read=storage_.read(block*0x10000,readback.data(),readback.size(),error);std::string close_error;const bool closed=storage_.close(close_error);if(!read||!closed){if(error.empty())error=close_error;return false;}
    if(readback!=bytes){error="live block program readback mismatch";return false;}progress<<"Verified programmed cartridge block "<<block<<".\n";return true;
}

} // namespace ezfa3fs
