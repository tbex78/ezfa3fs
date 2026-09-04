#include "ez3fs/cartridge_storage.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
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
    ~Impl() { shutdown(); }

    bool open(std::string& error);
    bool close(std::string& error);
    void shutdown() noexcept;
    bool read(std::uint64_t offset, std::uint8_t* destination,
              std::size_t size, std::string& error);
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
    bool initialize(std::string& error);
    bool probePrefix(std::string& error);
    bool readFlashId(std::array<std::uint8_t, 4>& id, std::string& error);
    bool prepareMapping(std::uint64_t end, std::string& error);
    bool mappingBody(std::uint32_t limit, std::string& error);
    bool rawRead(std::uint32_t offset, std::uint8_t* destination,
                 std::size_t size, std::string& error);
#endif
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

bool CartridgeStorage::Impl::initialize(std::string& error)
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
        error = "unsupported cartridge flash identifier"; return false;
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
    if (!initialize(error)) { shutdown(); return false; }
    is_open = true;
    return true;
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
bool CartridgeStorage::isOpen() const noexcept { return impl_->is_open; }
std::array<std::uint8_t,4> CartridgeStorage::flashId() const noexcept { return impl_->flash_id; }
std::uint64_t CartridgeStorage::capacity() const noexcept { return cartridge_capacity; }
bool CartridgeStorage::read(std::uint64_t offset,std::uint8_t* destination,
                            std::size_t size,std::string& error)
{ return impl_->read(offset,destination,size,error); }

} // namespace ez3fs
