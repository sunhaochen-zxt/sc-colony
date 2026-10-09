# 星际争霸：殖民地 —— 简易 Makefile（不依赖 cmake）
CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra
CPPFLAGS += -Isrc
BUILD    := build

CORE_SRC := src/game.cpp
GAME_SRC := src/main.cpp src/ui.cpp

.PHONY: all clean test run

all: $(BUILD)/starcolony $(BUILD)/selftest $(BUILD)/trace

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/starcolony: $(GAME_SRC) $(CORE_SRC) src/game.hpp src/types.hpp src/ui.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(GAME_SRC) $(CORE_SRC) -o $@

$(BUILD)/selftest: tests/selftest.cpp tests/ai.hpp $(CORE_SRC) src/game.hpp src/types.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) tests/selftest.cpp $(CORE_SRC) -o $@

$(BUILD)/trace: tools/trace.cpp tests/ai.hpp $(CORE_SRC) src/game.hpp src/types.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -Itests tools/trace.cpp $(CORE_SRC) -o $@

test: $(BUILD)/selftest
	./$(BUILD)/selftest

run: $(BUILD)/starcolony
	./$(BUILD)/starcolony

clean:
	rm -rf $(BUILD)
