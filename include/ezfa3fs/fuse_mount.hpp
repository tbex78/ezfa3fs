#pragma once
#include "ezfa3fs/mount_backend.hpp"
#include <memory>
#include <string>
namespace ezfa3fs {
int mountLiveCartridge(const std::string& mountpoint,bool foreground,
                       bool verify_referenced_data = false);
int mountBackend(std::unique_ptr<MountBackend> backend,
                 const std::string& mountpoint,bool foreground,
                 const std::string& filesystem_name);
}
