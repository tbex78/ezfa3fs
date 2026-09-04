#include "ez3fs/recovery_snapshot.hpp"

#include "ez3fs/new_image_file.hpp"

#include <fstream>
#include <iterator>
#include <vector>

namespace ez3fs {

RecoverySnapshot::RecoverySnapshot(std::filesystem::path staging_path)
    :staging_(std::move(staging_path)) {}

std::filesystem::path RecoverySnapshot::recoveryPath() const
{
    auto path=staging_;path += ".recovery.ez3fs";return path;
}

bool RecoverySnapshot::exists(std::string& error) const
{
    std::error_code status_error;
    const auto status=std::filesystem::symlink_status(recoveryPath(),status_error);
    if(status_error==std::errc::no_such_file_or_directory){error.clear();return false;}
    if(status_error){error="could not inspect recovery snapshot: "+status_error.message();return false;}
    error.clear();return status.type()!=std::filesystem::file_type::not_found;
}

bool RecoverySnapshot::create(const Archive& baseline,std::string& error) const
{
    if(exists(error)){error="recovery snapshot already exists: "+recoveryPath().string();return false;}
    NewImageFile output(recoveryPath());
    return output.write(baseline.image(),error);
}

bool RecoverySnapshot::restore(std::string& error) const
{
    std::ifstream input(recoveryPath(),std::ios::binary);
    if(!input){error="could not open recovery snapshot: "+recoveryPath().string();return false;}
    std::vector<std::uint8_t> bytes(std::istreambuf_iterator<char>(input),{});
    Archive archive;if(!archive.open(bytes,error)||!archive.verify(error))return false;
    auto temporary=staging_;temporary += ".recovery.restore.tmp";
    std::ofstream output(temporary,std::ios::binary|std::ios::trunc);
    if(!output){error="could not create recovery restore temporary file";return false;}
    output.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    output.close();
    if(!output){std::error_code ignored;std::filesystem::remove(temporary,ignored);error="could not write recovery restore temporary file";return false;}
    std::error_code rename_error;std::filesystem::rename(temporary,staging_,rename_error);
    if(rename_error){std::error_code ignored;std::filesystem::remove(temporary,ignored);error="could not restore staging image: "+rename_error.message();return false;}
    error.clear();return true;
}

bool RecoverySnapshot::clear(std::string& error) const
{
    std::error_code remove_error;std::filesystem::remove(recoveryPath(),remove_error);
    if(remove_error){error="could not remove recovery snapshot: "+remove_error.message();return false;}
    error.clear();return true;
}

} // namespace ez3fs
