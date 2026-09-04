#include "ez3fs/new_image_file.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <string>
#include <vector>

namespace {
void requireAt(bool condition,int line) {
    if(!condition){std::cerr<<"requirement failed at line "<<line<<'\n';std::abort();}
}
#define REQUIRE(condition) requireAt((condition),__LINE__)
}

int main()
{
    namespace fs=std::filesystem;
    const auto unique=std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory=fs::temp_directory_path()/
        ("ez3fs-new-image-test-"+std::to_string(unique));
    std::error_code error;REQUIRE(fs::create_directory(directory,error));
    const auto destination=directory/"backup.ez3fs";
    ez3fs::NewImageFile output(destination);std::string message;
    REQUIRE(output.available(message));
    REQUIRE(output.write({1,2,3,4},message));
    REQUIRE(!output.available(message));
    REQUIRE(!output.write({9,9},message));
    std::ifstream input(destination,std::ios::binary);
    const std::vector<std::uint8_t> bytes{
        std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
    REQUIRE(bytes==std::vector<std::uint8_t>({1,2,3,4}));
    REQUIRE(fs::remove(destination,error));REQUIRE(fs::remove(directory,error));
}
