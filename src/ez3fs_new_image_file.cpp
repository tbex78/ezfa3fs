#include "ez3fs/new_image_file.hpp"

#include <fstream>

namespace ez3fs {

NewImageFile::NewImageFile(std::filesystem::path destination)
    :destination_(std::move(destination)) {}

bool NewImageFile::occupied(const std::filesystem::path& path,
                            std::string& error) const
{
    std::error_code status_error;
    const auto status=std::filesystem::symlink_status(path,status_error);
    if(status_error==std::errc::no_such_file_or_directory){error.clear();return false;}
    if(status_error){error="could not inspect output path: "+status_error.message();return true;}
    return status.type()!=std::filesystem::file_type::not_found;
}

bool NewImageFile::available(std::string& error) const
{
    error.clear();
    if(occupied(destination_,error)) {
        if(error.empty())error="refusing to overwrite destination: "+destination_.string();
        return false;
    }
    auto temporary=destination_;temporary += ".pull.tmp";
    if(occupied(temporary,error)) {
        if(error.empty())error="temporary output already exists: "+temporary.string();
        return false;
    }
    return true;
}

bool NewImageFile::write(const std::vector<std::uint8_t>& image,
                         std::string& error) const
{
    if(!available(error))return false;
    auto temporary=destination_;temporary += ".pull.tmp";
    std::ofstream output(temporary,std::ios::binary|std::ios::trunc);
    if(!output){error="could not create temporary image: "+temporary.string();return false;}
    output.write(reinterpret_cast<const char*>(image.data()),
                 static_cast<std::streamsize>(image.size()));
    output.close();
    if(!output){
        std::error_code ignored;std::filesystem::remove(temporary,ignored);
        error="could not write temporary image: "+temporary.string();return false;
    }
    std::error_code publish_error;
    std::filesystem::create_hard_link(temporary,destination_,publish_error);
    if(publish_error){
        std::error_code ignored;std::filesystem::remove(temporary,ignored);
        error="could not publish new image without overwrite: "+
              publish_error.message();return false;
    }
    std::error_code cleanup_error;std::filesystem::remove(temporary,cleanup_error);
    if(cleanup_error){error="image created, but temporary link cleanup failed: "+
        cleanup_error.message();return false;}
    error.clear();return true;
}

} // namespace ez3fs
