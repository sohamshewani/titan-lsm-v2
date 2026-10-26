CXX = g++
CXXFLAGS = -std=c++17 -O3 -Wall -Wextra -MMD -MP
LDFLAGS = -pthread

SRC_DIR = src
INC_DIR = include
BUILD_DIR = build
TEST_DIR = tests
BENCH_DIR = benchmarks

SRCS = $(wildcard $(SRC_DIR)/*.cpp)
OBJS = $(patsubst $(SRC_DIR)/%.cpp, $(BUILD_DIR)/%.o, $(SRCS))
DEPS = $(OBJS:.o=.d)

TEST_SRCS = $(wildcard $(TEST_DIR)/*.cpp)
TEST_OBJS = $(patsubst $(TEST_DIR)/%.cpp, $(BUILD_DIR)/tests/%.o, $(TEST_SRCS))
TEST_EXEC = $(BUILD_DIR)/test_titan

BENCH_SRCS = $(wildcard $(BENCH_DIR)/*.cpp)
BENCH_OBJS = $(patsubst $(BENCH_DIR)/%.cpp, $(BUILD_DIR)/benchmarks/%.o, $(BENCH_SRCS))
BENCH_EXEC = $(BUILD_DIR)/bench_titan

TARGET = $(BUILD_DIR)/libtitan.a

.PHONY: all build test clean benchmark directories

all: directories build test

directories:
	@mkdir -p $(BUILD_DIR) $(BUILD_DIR)/tests $(BUILD_DIR)/benchmarks

build: directories $(TARGET)

$(TARGET): $(OBJS)
	ar rcs $@ $^

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -c $< -o $@

$(BUILD_DIR)/tests/%.o: $(TEST_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -c $< -o $@

$(BUILD_DIR)/benchmarks/%.o: $(BENCH_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -c $< -o $@

test: directories $(TEST_EXEC)
	@if [ -f $(TEST_EXEC) ]; then ./$(TEST_EXEC); else echo "No tests found, skipping execution."; fi

$(TEST_EXEC): $(OBJS) $(TEST_OBJS)
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) $^ -o $@ $(LDFLAGS)

benchmark: directories $(BENCH_EXEC)
	@if [ -f $(BENCH_EXEC) ]; then ./$(BENCH_EXEC); else echo "No benchmarks found, skipping execution."; fi

$(BENCH_EXEC): $(OBJS) $(BENCH_OBJS)
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) $^ -o $@ $(LDFLAGS)

clean:
	@rm -rf $(BUILD_DIR)

-include $(DEPS)