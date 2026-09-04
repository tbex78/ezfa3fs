#pragma once
#include "ez3fs/archive.hpp"
#include <string>
namespace ez3fs {
int mountImage(const std::string& image,const std::string& mountpoint,
               bool writable,bool foreground);
int mountArchive(const Archive& archive,const std::string& mountpoint,
                 bool foreground,const std::string& filesystem_name="ez3fs");
}
