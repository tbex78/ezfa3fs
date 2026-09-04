#pragma once
#include "ez3fs/archive.hpp"
#include <string>
#include <vector>
namespace ez3fs {
int mountImage(const std::string& image,const std::string& mountpoint,
               bool writable,bool foreground);
int mountArchive(const Archive& archive,const std::string& mountpoint,
                 bool foreground,const std::string& filesystem_name="ez3fs");
int mountLiveContents(const std::vector<InputFile>& contents,
                      const std::string& mountpoint,bool foreground);
int mountLiveCartridge(const std::string& mountpoint,bool foreground);
}
