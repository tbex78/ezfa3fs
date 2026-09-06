BUILD_DIR ?= build/cmake
all:
	cmake -S . -B $(BUILD_DIR)
	cmake --build $(BUILD_DIR)
test: all
	ctest --test-dir $(BUILD_DIR) --output-on-failure
clean:
	cmake --build $(BUILD_DIR) --target clean
.PHONY: all test clean
