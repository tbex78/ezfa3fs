cmake_minimum_required(VERSION 3.20)
project(ezfa3fs VERSION 0.55.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Threads REQUIRED)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(FUSE3 QUIET IMPORTED_TARGET fuse3)
    pkg_check_modules(LIBUSB QUIET IMPORTED_TARGET libusb-1.0)
endif()
set(EZFA3FS_SOURCES
    src/ezfa3fs_crc32.cpp src/ezfa3fs_cached_block_device.cpp
    src/ezfa3fs_finder_metadata_mount_backend.cpp src/ezfa3fs_live_filesystem.cpp
    src/ezfa3fs_live_mount_backend.cpp src/ezfa3fs_mount_session.cpp
    src/ezfa3fs_packed_block.cpp)
set(EZFA3FS_CLI_SOURCES
    ezfa3fs.cpp src/ezfa3fs_fuse_mount.cpp src/ezfa3fs_cartridge_storage.cpp
    src/ezfa3fs_cartridge_live_device.cpp src/ezfa3fs_live_cartridge_session.cpp
    ${EZFA3FS_SOURCES})
function(ezfa3fs_configure target)
    target_include_directories(${target} PRIVATE include)
    target_link_libraries(${target} PRIVATE Threads::Threads)
    if(TARGET PkgConfig::FUSE3)
        target_compile_definitions(${target} PRIVATE EZFA3FS_HAS_FUSE3=1)
        target_link_libraries(${target} PRIVATE PkgConfig::FUSE3)
    endif()
    if(TARGET PkgConfig::LIBUSB)
        target_compile_definitions(${target} PRIVATE EZFA3FS_HAS_LIBUSB=1)
        target_link_libraries(${target} PRIVATE PkgConfig::LIBUSB)
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()
add_executable(ezfa3fs ${EZFA3FS_CLI_SOURCES})
ezfa3fs_configure(ezfa3fs)
include(CTest)
if(BUILD_TESTING)
    function(ezfa3fs_add_test name)
        add_executable(${name} ${ARGN} ${EZFA3FS_SOURCES})
        target_include_directories(${name} PRIVATE include)
        add_test(NAME ${name} COMMAND ${name})
    endfunction()
    ezfa3fs_add_test(ezfa3fs_cached_block_device_test tests/ezfa3fs_cached_block_device_test.cpp)
    ezfa3fs_add_test(ezfa3fs_live_filesystem_test tests/ezfa3fs_live_filesystem_test.cpp)
    ezfa3fs_add_test(ezfa3fs_live_mount_backend_test tests/ezfa3fs_live_mount_backend_test.cpp)
    ezfa3fs_add_test(ezfa3fs_mount_session_test tests/ezfa3fs_mount_session_test.cpp)
    ezfa3fs_add_test(ezfa3fs_save_bank_restore_policy_test tests/ezfa3fs_save_bank_restore_policy_test.cpp)
    add_test(NAME ezfa3fs_cli_version COMMAND ezfa3fs --version)
    set_tests_properties(ezfa3fs_cli_version PROPERTIES PASS_REGULAR_EXPRESSION "ezfa3fs 0.55.0")
    set(EZFA3FS_CLI_INFO_IMAGE
        "${CMAKE_CURRENT_BINARY_DIR}/ezfa3fs_cli_info_test.ezfa3fs")
    add_test(
        NAME ezfa3fs_cli_info_format
        COMMAND ezfa3fs format "${EZFA3FS_CLI_INFO_IMAGE}")
    set_tests_properties(
        ezfa3fs_cli_info_format PROPERTIES
        FIXTURES_SETUP ezfa3fs_cli_info_image)
    add_test(
        NAME ezfa3fs_cli_info
        COMMAND ezfa3fs info "${EZFA3FS_CLI_INFO_IMAGE}")
    set_tests_properties(
        ezfa3fs_cli_info PROPERTIES
        FIXTURES_REQUIRED ezfa3fs_cli_info_image
        PASS_REGULAR_EXPRESSION "Format version:[ ]+0.3.0")
    add_test(
        NAME ezfa3fs_cli_info_cleanup
        COMMAND ${CMAKE_COMMAND} -E rm -f "${EZFA3FS_CLI_INFO_IMAGE}")
    set_tests_properties(
        ezfa3fs_cli_info_cleanup PROPERTIES
        FIXTURES_CLEANUP ezfa3fs_cli_info_image)
endif()
