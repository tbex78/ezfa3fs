#include "ez3fs/cartridge_storage.hpp"
#include "ez3fs/cartridge_programmer.hpp"
#include "ez3fs/live_filesystem.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

#if defined(EZ3FS_HAS_LIBUSB)
#  if __has_include(<libusb-1.0/libusb.h>)
#    include <libusb-1.0/libusb.h>
#  else
#    include <libusb.h>
#  endif
#endif

namespace ez3fs {

class CartridgeStorage::Impl final {
public:
    friend class CartridgeProgrammer;
    ~Impl() { shutdown(); }

    bool open(std::string& error);
    bool openForProgramming(std::string& error);
    bool close(std::string& error);
    void shutdown() noexcept;
    bool read(std::uint64_t offset, std::uint8_t* destination,
              std::size_t size, std::string& error);
    bool eraseLiveBlock(std::size_t block,std::ostream& progress,std::string& error,bool allow_metadata=false);
    bool programLiveBlock(std::size_t block,const std::vector<std::uint8_t>& bytes,
                          std::ostream& progress,std::string& error,bool allow_metadata=false);
    bool is_open = false;
    std::array<std::uint8_t, 4> flash_id{};

private:
#if defined(EZ3FS_HAS_LIBUSB)
    static constexpr unsigned timeout_ms = 15000;
    libusb_context* context = nullptr;
    libusb_device_handle* handle = nullptr;
    bool claimed = false;
    std::uint32_t mapped_limit = 0x00800000u;

    bool out(const std::vector<std::uint8_t>& bytes, std::string& error);
    bool in(std::vector<std::uint8_t>& bytes, std::size_t size,
            std::string& error);
    bool commandEcho(const std::vector<std::uint8_t>& command,
                     const std::vector<std::uint8_t>& data,
                     std::string& error);
    bool tx92(std::uint8_t first, std::uint8_t second, std::string& error,
              std::uint32_t word_address = 0);
    bool waitReady(unsigned attempts, std::string& error);
    bool initialize(std::string& error, bool allow_erased);
    bool probePrefix(std::string& error);
    bool readFlashId(std::array<std::uint8_t, 4>& id, std::string& error);
    bool prepareMapping(std::uint64_t end, std::string& error);
    bool mappingBody(std::uint32_t limit, std::string& error);
    bool rawRead(std::uint32_t offset, std::uint8_t* destination,
                 std::size_t size, std::string& error);
    bool tx92One(std::uint8_t selector, std::uint8_t value,
                 std::string& error);
    bool selectWriteWindow(unsigned window, std::string& error);
    bool finishWriteOperation(std::string& error);
#endif
    bool eraseAll(std::ostream& progress, std::string& error);
    bool programImage(const std::vector<std::uint8_t>& image,
                      std::ostream& progress, std::string& error);
};

#if defined(EZ3FS_HAS_LIBUSB)
namespace {
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

bool hasEz3fsMagic(const std::uint8_t* bytes) noexcept
{
    static constexpr std::array<std::uint8_t,8> current =
        {{'E','Z','3','F','S','\r','\n',0x1A}};
    static constexpr std::array<std::uint8_t,8> legacy =
        {{'E','Z','F','S','\r','\n',0x1A,'\n'}};
    return std::equal(current.begin(),current.end(),bytes) ||
           std::equal(legacy.begin(),legacy.end(),bytes);
}

bool hasEz3fsLiveMagic(const std::uint8_t* bytes) noexcept
{
    static constexpr std::array<std::uint8_t,8> magic =
        {{'E','Z','3','L','I','V','E',0}};
    return std::equal(magic.begin(),magic.end(),bytes);
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
    int transferred = 0;
    const int result = libusb_bulk_transfer(handle, 0x02,
        const_cast<unsigned char*>(bytes.data()), static_cast<int>(bytes.size()),
        &transferred, timeout_ms);
    if (result != 0) { error = usbError("USB OUT failed", result); return false; }
    if (transferred != static_cast<int>(bytes.size())) {
        error = "USB OUT returned a short transfer"; return false;
    }
    return true;
}

bool CartridgeStorage::Impl::in(std::vector<std::uint8_t>& bytes,
                                std::size_t size, std::string& error)
{
    bytes.assign(size, 0);
    int transferred = 0;
    const int result = libusb_bulk_transfer(handle, 0x81, bytes.data(),
        static_cast<int>(size), &transferred, timeout_ms);
    if (result != 0) { error = usbError("USB IN failed", result); return false; }
    if (transferred != static_cast<int>(size)) {
        error = "USB IN returned a short transfer"; return false;
    }
    return true;
}

bool CartridgeStorage::Impl::commandEcho(
    const std::vector<std::uint8_t>& command,
    const std::vector<std::uint8_t>& data, std::string& error)
{
    if (!out(command, error)) return false;
    std::this_thread::sleep_for(std::chrono::microseconds(750));
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

bool CartridgeStorage::Impl::probePrefix(std::string& error)
{
    if (!tx92(0x55,0xAA,error) || !tx92(0,0,error) ||
        !tx92(0,0,error) || !tx92(0,0,error)) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(125));
    return tx92(0xAA,0x55,error) && tx92(0,0,error) &&
           tx92(0,0,error) && tx92(0,0,error);
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

bool CartridgeStorage::Impl::initialize(std::string& error, bool allow_erased)
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
    if (!probePrefix(error) || !tx92(0xAA,0,error,0x555) ||
        !tx92(0x55,0,error,0x2AA) || !tx92(0x90,0,error,0x555) ||
        !readFlashId(ignored,error) || !tx92(0x90,0,error) ||
        !tx92(0xF0,0,error) || !probePrefix(error) ||
        !tx92(0x90,0,error) || !readFlashId(flash_id,error) ||
        !tx92(0xFF,0xFF,error)) return false;

    const std::array<std::uint8_t,4> b8{{0x1C,0,0xB8,0}};
    const std::array<std::uint8_t,4> b9{{0x1C,0,0xB9,0}};
    if (flash_id != b8 && flash_id != b9) {
        // Some genuine EZ3 units do not return either captured ID reliably.
        // The historical reader therefore used content as a read-only fallback.
        // Accept only an EZ3FS signature at offset zero; arbitrary cartridges
        // still cannot enter the EZ3-specific mapping path.
        std::array<std::uint8_t,8> header{};
        if (!rawRead(0,header.data(),header.size(),error)) return false;
        const bool erased = std::all_of(header.begin(),header.end(),
            [](std::uint8_t byte){return byte==0xFF;});
        std::array<std::uint8_t,8> live_header{};
        if (!hasEz3fsLiveMagic(header.data()) && !rawRead(0x10000u,live_header.data(),live_header.size(),error)) return false;
        if (!hasEz3fsMagic(header.data()) && !hasEz3fsLiveMagic(header.data()) &&
            !hasEz3fsLiveMagic(live_header.data()) && !(allow_erased && erased)) {
            error = "unsupported cartridge flash identifier: " +
                    formatFlashId(flash_id) +
                    "; no EZ3FS image found at offset 0";
            return false;
        }
    }

    const std::vector<std::uint8_t> c95 =
        {0x5A,0xA5,0x95,0,0x80,0,0,0,0,0,0,0,0};
    if (!out(c95,error) || !in(response,c95.size(),error) || response != c95) {
        if (error.empty()) error = "cartridge read-prime echo mismatch";
        return false;
    }
    std::array<std::uint8_t,0xAC> prime{};
    return rawRead(0, prime.data(), prime.size(), error);
}

bool CartridgeStorage::Impl::mappingBody(std::uint32_t limit,
                                         std::string& error)
{
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
    return tx92(0xAA,0x55,error) && tx92(0,0,error) &&
           tx92(0,0,error) && tx92(0,0,error) &&
           tx92One(0,0xAA,error) && tx92One(0,0x55,error) &&
           tx92One(1,0x06,error);
}

bool CartridgeStorage::Impl::finishWriteOperation(std::string& error)
{
    const bool finished=tx92(0xFF,0xFF,error) && tx92One(1,0x04,error) &&
                        tx92One(0,0,error) && tx92One(0,0,error);
    // A write-window selection invalidates the cached read mapping. Force the
    // next read to restore a known mapping before issuing a 0x91 command.
    if(finished)mapped_limit=0;
    return finished;
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
        std::vector<std::uint8_t> command =
            {0x5A,0xA5,0x92,0,0,0,0,0,0,0,0,0,0x41};
        putLe32(command,4,local/2);putLe32(command,8,static_cast<std::uint32_t>(size));
        if (!out(command,error)) return false;
        std::this_thread::sleep_for(std::chrono::microseconds(750));
        std::vector<std::uint8_t> data(image.begin()+static_cast<std::ptrdiff_t>(offset),
                                       image.begin()+static_cast<std::ptrdiff_t>(offset+size));
        if (!out(data,error)) return false;
        std::vector<std::uint8_t> response;
        if (!in(response,command.size(),error)) return false;
        command[12]=0;
        if (response!=command) { error="cartridge program completion mismatch";return false; }
        progress << "\rProgramming " << offset+size << '/' << image.size() << std::flush;
    }
    progress << '\n';
    return finishWriteOperation(error);
}
bool CartridgeStorage::Impl::eraseLiveBlock(std::size_t block,std::ostream& progress,std::string& error,bool allow_metadata)
{
    if((!allow_metadata&&block<2)||block>=0x200){error="live erase block is outside the permitted range";return false;}
    const unsigned window=static_cast<unsigned>(block/128);const auto local=static_cast<std::uint32_t>((block%128)*0x8000u);
    if(!selectWriteWindow(window,error))return false;
    std::vector<std::uint32_t> addresses;
    if(block==0)for(std::uint32_t address=0;address<0x8000;address+=0x1000)addresses.push_back(address);else addresses.push_back(local);
    for(const auto address:addresses){
        std::vector<std::uint8_t> command={0x5A,0xA5,0x96,0,static_cast<std::uint8_t>(address),static_cast<std::uint8_t>(address>>8),static_cast<std::uint8_t>(address>>16),static_cast<std::uint8_t>(address>>24),0,0,0,0,0};
        std::vector<std::uint8_t> response;if(!out(command,error)||!in(response,command.size(),error))return false;
        if(response.size()!=command.size()||!std::equal(command.begin(),command.begin()+12,response.begin())||response[12]!=0){error="cartridge live block erase failed";return false;}
        progress<<"Erase response status: 0x"<<std::hex<<static_cast<unsigned>(response[12])<<std::dec<<"\n";
    }
    if(!finishWriteOperation(error))return false;progress<<"Erased cartridge block "<<block<<".\n";return true;
}
bool CartridgeStorage::Impl::programLiveBlock(std::size_t block,const std::vector<std::uint8_t>& bytes,std::ostream& progress,std::string& error,bool allow_metadata)
{
    if((!allow_metadata&&block<2)||block>=0x200||bytes.size()!=0x10000){error="live block programming is outside the permitted range";return false;}
    const unsigned window=static_cast<unsigned>(block/128);const auto local=static_cast<std::uint32_t>((block%128)*0x8000u);
    if(!selectWriteWindow(window,error))return false;
    std::vector<std::uint8_t> command={0x5A,0xA5,0x92,0,0,0,0,0,0,0,0,0,0x41};putLe32(command,4,local);putLe32(command,8,static_cast<std::uint32_t>(bytes.size()));
    if(!out(command,error))return false;std::this_thread::sleep_for(std::chrono::microseconds(750));if(!out(bytes,error))return false;std::vector<std::uint8_t> response;
    if(!in(response,command.size(),error))return false;command[12]=0;if(response!=command){error="cartridge live block program completion mismatch";return false;}
    if(!finishWriteOperation(error))return false;progress<<"Programmed cartridge block "<<block<<".\n";return true;
}
#else
bool CartridgeStorage::Impl::eraseAll(std::ostream&,std::string& error)
{
    error="EZ3FS was built without libusb support";return false;
}

bool CartridgeStorage::Impl::programImage(
    const std::vector<std::uint8_t>&,std::ostream&,std::string& error)
{
    error="EZ3FS was built without libusb support";return false;
}
bool CartridgeStorage::Impl::eraseLiveBlock(std::size_t,std::ostream&,std::string& error,bool)
{ error="EZ3FS was built without libusb support";return false; }
bool CartridgeStorage::Impl::programLiveBlock(std::size_t,const std::vector<std::uint8_t>&,std::ostream&,std::string& error,bool)
{ error="EZ3FS was built without libusb support";return false; }
#endif

bool CartridgeStorage::Impl::open(std::string& error)
{
#if !defined(EZ3FS_HAS_LIBUSB)
    error = "EZ3FS was built without libusb support";
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

bool CartridgeStorage::Impl::openForProgramming(std::string& error)
{
#if !defined(EZ3FS_HAS_LIBUSB)
    error="EZ3FS was built without libusb support";return false;
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
    if(!initialize(error,true)){shutdown();return false;}
    is_open=true;return true;
#endif
}

bool CartridgeStorage::Impl::close(std::string& error)
{
#if defined(EZ3FS_HAS_LIBUSB)
    bool finished = true;
    if (is_open) {
        for (unsigned i=0; i<3 && finished; ++i) finished = waitReady(1,error);
        if (finished) std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    shutdown();
    return finished;
#else
    error.clear(); return true;
#endif
}

void CartridgeStorage::Impl::shutdown() noexcept
{
#if defined(EZ3FS_HAS_LIBUSB)
    is_open = false;
    if (handle && claimed) libusb_release_interface(handle,0);
    claimed = false;
    if (handle) libusb_close(handle);
    handle = nullptr;
    if (context) libusb_exit(context);
    context = nullptr;
    mapped_limit = 0x00800000u;
#else
    is_open = false;
#endif
}

bool CartridgeStorage::Impl::read(std::uint64_t offset,
                                  std::uint8_t* destination,
                                  std::size_t size, std::string& error)
{
#if !defined(EZ3FS_HAS_LIBUSB)
    (void)offset; (void)destination; (void)size;
    error = "EZ3FS was built without libusb support"; return false;
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
bool CartridgeStorage::openForLiveWrite(std::string& error) { return impl_->openForProgramming(error); }
bool CartridgeStorage::eraseLiveBlock(std::size_t block,std::string& error) { return impl_->eraseLiveBlock(block,std::cerr,error); }
bool CartridgeStorage::programLiveBlock(std::size_t block,const std::vector<std::uint8_t>& bytes,std::string& error) { return impl_->programLiveBlock(block,bytes,std::cerr,error); }
bool CartridgeStorage::eraseLiveFilesystemBlock(std::size_t block,std::string& error) { return impl_->eraseLiveBlock(block,std::cerr,error,true); }
bool CartridgeStorage::programLiveFilesystemBlock(std::size_t block,const std::vector<std::uint8_t>& bytes,std::string& error) { return impl_->programLiveBlock(block,bytes,std::cerr,error,true); }
bool CartridgeStorage::isOpen() const noexcept { return impl_->is_open; }
std::array<std::uint8_t,4> CartridgeStorage::flashId() const noexcept { return impl_->flash_id; }
std::uint64_t CartridgeStorage::capacity() const noexcept { return cartridge_capacity; }
bool CartridgeStorage::read(std::uint64_t offset,std::uint8_t* destination,
                            std::size_t size,std::string& error)
{ return impl_->read(offset,destination,size,error); }

bool CartridgeProgrammer::programAndVerify(
    const std::vector<std::uint8_t>& image,std::ostream& progress,
    std::string& error)
{
    Archive validated;
    if(!validated.open(image,error) || !validated.verify(error)) {
        live::NorFlash live_flash; live::Filesystem live_filesystem(live_flash); std::string live_error;
        if(!live_flash.load(image,live_error) || !live::Filesystem::open(live_flash,live_filesystem,live_error) || !live_filesystem.verify(live_error)) {
            error="not an EZ3FS or EZ3FS-LIVE image"; return false;
        }
    }
    if(!storage_.impl_->openForProgramming(error)) return false;
    progress << "Erasing the complete 32-MiB cartridge...\n";
    if(!storage_.impl_->eraseAll(progress,error) ||
       !storage_.impl_->programImage(image,progress,error)) {
        std::string ignored;storage_.close(ignored);return false;
    }
    if(!storage_.close(error)) return false;

    progress << "Reopening cartridge for byte-for-byte verification...\n";
    if(!storage_.open(error)) return false;
    std::vector<std::uint8_t> block(ImageBuilder::program_block_size);
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

bool CartridgeProgrammer::eraseLiveBlock(std::size_t block,std::ostream& progress,std::string& error)
{
    if(block<2||block>=0x200){error="live erase only permits blocks 2 through 511";return false;}
    if(!storage_.impl_->openForProgramming(error))return false;
    if(!storage_.impl_->eraseLiveBlock(block,progress,error)){std::string ignored;storage_.close(ignored);return false;}
    if(!storage_.close(error))return false;
    if(!storage_.open(error))return false;
    std::vector<std::uint8_t> bytes(0x10000);const bool read=storage_.read(block*0x10000,bytes.data(),bytes.size(),error);std::string close_error;const bool closed=storage_.close(close_error);
    if(!read||!closed){if(error.empty())error=close_error;return false;}
    progress<<"Erase readback CRC32: 0x"<<std::hex<<ez3fs::Crc32::calculate(bytes.data(),bytes.size())<<std::dec<<"\n";
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

} // namespace ez3fs
