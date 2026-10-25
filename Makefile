# Any_Ps3 quick dev build (Linux) — no cmake needed.
# The canonical build system is CMake (see CMakeLists.txt); this Makefile exists
# for fast iteration in minimal environments.
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -fno-omit-frame-pointer
BUILD    := build
TESTBIN  := $(BUILD)/anyps3_tests

# Version from the single VERSION file (same source of truth as CMake/Gradle).
ANYPS3_VERSION := $(shell sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$$//' VERSION 2>/dev/null)
ifeq ($(strip $(ANYPS3_VERSION)),)
  ANYPS3_VERSION := 0.0.0-dev
endif
VERSION_FLAGS := -DANYPS3_VERSION='"$(ANYPS3_VERSION)"'

CORE_SRC := $(wildcard core/src/*.cpp core/src/*/*.cpp core/src/*/*/*.cpp)
TEST_SRC := $(wildcard tests/*.cpp)
HDRS    := $(wildcard core/include/anyps3/*.h core/include/anyps3/*/*.h core/include/anyps3/*/*/*.h)
CORE_OBJ := $(patsubst core/src/%.cpp,$(BUILD)/core/%.o,$(CORE_SRC))
TEST_OBJ := $(patsubst tests/%.cpp,$(BUILD)/tests/%.o,$(TEST_SRC))
INCLUDES := -Icore/include

.PHONY: all test clean

all: $(TESTBIN)

$(TESTBIN): $(CORE_OBJ) $(TEST_OBJ)
	$(CXX) $(CXXFLAGS) $(VERSION_FLAGS) -o $@ $^

$(BUILD)/core/%.o: core/src/%.cpp $(HDRS)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(VERSION_FLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/tests/%.o: tests/%.cpp $(wildcard tests/*.h tests/fixtures/*.h) $(HDRS)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(VERSION_FLAGS) $(INCLUDES) -c $< -o $@

test: $(TESTBIN)
	@echo "Any_Ps3 $(ANYPS3_VERSION) — core tests"
	./$(TESTBIN)

clean:
	rm -rf $(BUILD)
