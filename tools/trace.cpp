// 星际争霸：殖民地 (Star Colony) —— C++20 命令行殖民地经营游戏
// Copyright (C) 2026 Star Colony contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published
// by the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// See the LICENSE file at the project root for the full license text.
// SPDX-License-Identifier: AGPL-3.0-or-later

// 平衡性调试工具：用内置 AI 跑一整局并逐步打印状态
// 用法：trace [seed] [--quiet]
#include "ai.hpp"
#include "game.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace sc;

int main(int argc, char** argv) {
    uint32_t seed = 42;
    bool quiet = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--quiet") quiet = true;
        else seed = static_cast<uint32_t>(std::atoi(argv[i]));
    }

    Game g;
    g.newGame(seed, "T");
    std::printf("=== seed %u ===\n", seed);
    size_t lastLog = 0;

    for (int t = 0; t < MAX_TURNS && !g.over(); ++t) {
        aiTurn(g);
        g.advanceTurn();
        if (!quiet) {
            std::printf("T%-3d M%-5d E%-5d F%-5d S%-4d pop%-3d/%d morale%-3d def%-4d wave%d | 建筑%d\n",
                        g.turn(), g.res().metal, g.res().energy, g.res().food, g.res().science,
                        g.pop(), g.housing(), g.morale(), g.defense(), g.waveIn(),
                        static_cast<int>(g.buildings().size()));
            const std::deque<std::string>& lg = g.log();
            for (size_t i = lastLog; i < lg.size(); ++i) std::printf("        %s\n", lg[i].c_str());
            lastLog = lg.size();
        }
    }
    std::printf("结果: %s  周期 %d  人口 %d/%d  科技 ", g.over() ? (g.won() ? "胜利" : "失败") : "未结束",
                g.turn(), g.pop(), g.housing());
    int techs = 0;
    for (int i = 0; i < TECH_COUNT; ++i) {
        if (g.hasTech(static_cast<Tech>(i))) {
            std::printf("%s ", TDEF[static_cast<size_t>(i)].key);
            ++techs;
        }
    }
    std::printf("(%d/8)  %s\n", techs, g.endReason().c_str());
    return 0;
}
