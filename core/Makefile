# Any_Ps3 core — native build (tests) + static lib
# Portable: same sources compile with g++ (tests), NDK clang (Android .so)
# and llvm-mingw (Windows exe). No cmake dependency on purpose.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter -Iinclude
BUILD    := build

SRCS := $(wildcard src/*.cpp)
OBJS := $(patsubst src/%.cpp,$(BUILD)/%.o,$(SRCS))
TEST_SRCS := $(wildcard tests/test_*.cpp)
TEST_OBJS := $(patsubst tests/%.cpp,$(BUILD)/tests/%.o,$(TEST_SRCS))

.PHONY: all test clean

all: $(BUILD)/libany_ps3_core.a $(BUILD)/any_ps3_tests

$(BUILD):
	mkdir -p $(BUILD)/tests

$(BUILD)/%.o: src/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/tests/%.o: tests/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -Itests -c $< -o $@

$(BUILD)/libany_ps3_core.a: $(OBJS)
	ar rcs $@ $^

$(BUILD)/any_ps3_tests: $(OBJS) $(TEST_OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) $(TEST_OBJS) -o $@

test: $(BUILD)/any_ps3_tests
	./$(BUILD)/any_ps3_tests

clean:
	rm -rf $(BUILD)
