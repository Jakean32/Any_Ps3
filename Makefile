# Any_Ps3 quick dev build (Linux) — no cmake needed.
# The canonical build system is CMake (see CMakeLists.txt); this Makefile exists
# for fast iteration in minimal environments.
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -fno-omit-frame-pointer
BUILD    := build
TESTBIN  := $(BUILD)/anyps3_tests

CORE_SRC := $(wildcard core/src/*.cpp core/src/*/*.cpp core/src/*/*/*.cpp)
TEST_SRC := $(wildcard tests/*.cpp)
HDRS    := $(wildcard core/include/anyps3/*.h core/include/anyps3/*/*.h core/include/anyps3/*/*/*.h)
CORE_OBJ := $(patsubst core/src/%.cpp,$(BUILD)/core/%.o,$(CORE_SRC))
TEST_OBJ := $(patsubst tests/%.cpp,$(BUILD)/tests/%.o,$(TEST_SRC))
INCLUDES := -Icore/include

.PHONY: all test clean

all: $(TESTBIN)

$(TESTBIN): $(CORE_OBJ) $(TEST_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD)/core/%.o: core/src/%.cpp $(HDRS)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/tests/%.o: tests/%.cpp $(wildcard tests/*.h tests/fixtures/*.h) $(HDRS)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

test: $(TESTBIN)
	./$(TESTBIN)

clean:
	rm -rf $(BUILD)
