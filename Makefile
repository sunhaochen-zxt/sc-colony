# 星际争霸：殖民地 —— 简易 Makefile（不依赖 cmake）
#
# P3a 起改为「目标文件复用」：每个 .cpp 只编译一次，多个可执行文件共享 .o。
# 这样 src/game.cpp / src/content.cpp 不再被每个 target 各编译一遍，
# 整体构建时间不增反降（content.cpp 含 nlohmann/json，约 6.5s/TU，必须只付一次）。
CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra
CPPFLAGS += -Isrc -Itests -Ithird_party
BUILD    := build
OBJDIR   := $(BUILD)/obj

# 核心库源文件（所有可执行文件共享）
CORE_SRC := src/game.cpp src/content.cpp
CORE_OBJ := $(patsubst %.cpp,$(OBJDIR)/%.o,$(CORE_SRC))

GAME_OBJ := $(OBJDIR)/src/main.o $(OBJDIR)/src/ui.o

HEADERS  := $(wildcard src/*.hpp tests/*.hpp)

.PHONY: all clean test run

all: $(BUILD)/starcolony $(BUILD)/selftest $(BUILD)/trace $(BUILD)/starcolony-rpc

# 通用编译规则：源文件 -> 目标文件（按源路径分层存放于 build/obj/ 下）
$(OBJDIR)/%.o: %.cpp $(HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

$(BUILD)/starcolony: $(GAME_OBJ) $(CORE_OBJ)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD)/starcolony-rpc: $(OBJDIR)/src/rpc_server.o $(CORE_OBJ)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD)/selftest: $(OBJDIR)/tests/selftest.o $(CORE_OBJ)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD)/trace: $(OBJDIR)/tools/trace.o $(CORE_OBJ)
	$(CXX) $(CXXFLAGS) $^ -o $@

# 以下为按需构建的测试/工具（不进 all，但 `make build/qa_edge` 等可直接用）
$(BUILD)/qa_edge: $(OBJDIR)/tests/edge_tests.o $(CORE_OBJ)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD)/protocol_tests: $(OBJDIR)/tests/protocol_tests.o $(CORE_OBJ)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD)/sweep: $(OBJDIR)/tools/sweep.o $(CORE_OBJ)
	$(CXX) $(CXXFLAGS) $^ -o $@

test: $(BUILD)/selftest
	./$(BUILD)/selftest

run: $(BUILD)/starcolony
	./$(BUILD)/starcolony

clean:
	rm -rf $(BUILD)
