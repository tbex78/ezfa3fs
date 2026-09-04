CXX = c++
CXXFLAGS = -std=c++17 -O2
WARNFLAGS = -Wall -Wextra -Wpedantic
CPPFLAGS = -Iinclude
LDFLAGS =
LDLIBS =
PROGRAM = ez3fs
SOURCES = ez3fs.cpp src/ez3fs_archive.cpp src/ez3fs_archive_comparison.cpp src/ez3fs_archive_loader.cpp src/ez3fs_new_image_file.cpp src/ez3fs_virtual_filesystem.cpp src/ez3fs_fuse_mount.cpp src/ez3fs_cartridge_storage.cpp
FUSE3_CFLAGS = `if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists fuse3 >/dev/null 2>&1; then pkg-config --cflags fuse3; fi`
FUSE3_LIBS = `if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists fuse3 >/dev/null 2>&1; then pkg-config --libs fuse3; fi`
FUSE3_DEFINE = `if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists fuse3 >/dev/null 2>&1; then printf '%s\n' '-DEZ3FS_HAS_FUSE3=1'; fi`
LIBUSB_CFLAGS = `if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libusb-1.0 >/dev/null 2>&1; then pkg-config --cflags libusb-1.0; fi`
LIBUSB_LIBS = `if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libusb-1.0 >/dev/null 2>&1; then pkg-config --libs libusb-1.0; fi`
LIBUSB_DEFINE = `if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libusb-1.0 >/dev/null 2>&1; then printf '%s\n' '-DEZ3FS_HAS_LIBUSB=1'; fi`

all: $(PROGRAM)
$(PROGRAM): $(SOURCES) include/ez3fs/archive.hpp include/ez3fs/archive_comparison.hpp include/ez3fs/virtual_filesystem.hpp include/ez3fs/fuse_mount.hpp include/ez3fs/cartridge_storage.hpp include/ez3fs/cartridge_programmer.hpp include/ez3fs/timestamp.hpp include/ez3fs/version.hpp
	$(CXX) $(CPPFLAGS) $(FUSE3_CFLAGS) $(FUSE3_DEFINE) $(LIBUSB_CFLAGS) $(LIBUSB_DEFINE) $(CXXFLAGS) $(WARNFLAGS) $(SOURCES) $(LDFLAGS) $(FUSE3_LIBS) $(LIBUSB_LIBS) $(LDLIBS) -o $@

test:
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(WARNFLAGS) tests/ez3fs_archive_test.cpp src/ez3fs_archive.cpp src/ez3fs_archive_loader.cpp src/ez3fs_virtual_filesystem.cpp -o build/ez3fs_archive_test
	./build/ez3fs_archive_test
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(WARNFLAGS) tests/ez3fs_virtual_filesystem_test.cpp src/ez3fs_archive.cpp src/ez3fs_archive_loader.cpp src/ez3fs_virtual_filesystem.cpp -o build/ez3fs_virtual_filesystem_test
	./build/ez3fs_virtual_filesystem_test
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(WARNFLAGS) tests/ez3fs_new_image_file_test.cpp src/ez3fs_new_image_file.cpp -o build/ez3fs_new_image_file_test
	./build/ez3fs_new_image_file_test
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(WARNFLAGS) tests/ez3fs_archive_comparison_test.cpp src/ez3fs_archive.cpp src/ez3fs_archive_comparison.cpp -o build/ez3fs_archive_comparison_test
	./build/ez3fs_archive_comparison_test

clean:
	rm -f $(PROGRAM) build/ez3fs_archive_test build/ez3fs_virtual_filesystem_test build/ez3fs_new_image_file_test build/ez3fs_archive_comparison_test
.PHONY: all test clean
