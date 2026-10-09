// 星际争霸：殖民地 (Star Colony) —— C++20 命令行殖民地经营游戏
// Copyright (C) 2026 Star Colony contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published
// by the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// See the LICENSE file at the project root for the full license text.
// SPDX-License-Identifier: AGPL-3.0-or-later

// 星际争霸：殖民地 —— 试玩用简易 AI 策略（自检 / 平衡性脚本共用，不参与正式游戏）
#pragma once

#include "game.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace sc {

// ---------------- 选址 ----------------
inline bool bestSpot(const Game& g, BType t, int& bx, int& by) {
    int best = -1 << 30;
    bx = by = -1;
    for (int y = 0; y < MAP_H; ++y)
        for (int x = 0; x < MAP_W; ++x) {
            std::string why;
            if (!g.buildable(t, x, y, &why)) continue;
            int sc;
            if (t == BType::Mine) sc = g.tile(x, y).richness * 10 + g.adjMountain(x, y);
            else if (t == BType::Farm) sc = g.adjIce(x, y) * 10;
            else sc = -(std::abs(x - 8) + std::abs(y - 6));
            if (sc > best) { best = sc; bx = x; by = y; }
        }
    return best > (-1 << 30);
}

inline bool tryBuild(Game& g, BType t) {
    int x = 0, y = 0;
    if (!bestSpot(g, t, x, y)) return false;
    std::string r = g.doBuild(BDEF[static_cast<size_t>(t)].key, x, y);
    return r.find("开始建造") != std::string::npos;
}

// ---------------- 事件选择 ----------------
inline void aiAnswer(Game& g) {
    while (g.hasPending()) {
        const PendingEvent& e = g.pending();
        int choice = 1;
        switch (e.kind) {
        case 1: // 难民船
            if (g.res().food > 110 && g.pop() + e.a + 2 <= g.housing()) choice = 1;
            else if (g.res().metal > 400) choice = 3;
            else choice = 2;
            break;
        case 2: // 黑市
            if (g.res().metal >= 120 && g.report().scienceNet < 12) choice = 1;
            else if (g.res().energy >= 220) choice = 2;
            else choice = 3;
            break;
        case 3: // 神秘信号
            choice = (g.defense() >= g.waveStrengthEstimate() + 15) ? 1 : 2;
            break;
        case 4: // 维生系统
            choice = g.res().metal >= 60 ? 1 : 2;
            break;
        default:
            choice = 1;
            break;
        }
        g.answer(choice);
    }
}

inline int upkeepOf(const Game& g) {
    int u = 0;
    for (const Building& b : g.buildings())
        if (b.alive && b.buildLeft == 0 && b.enabled && b.damaged == 0)
            u += BDEF[static_cast<size_t>(b.type)].upkeep;
    return u;
}

inline int oreTiles(const Game& g) {
    int c = 0;
    for (int y = 0; y < MAP_H; ++y)
        for (int x = 0; x < MAP_W; ++x)
            if (g.tile(x, y).terrain == Terrain::Ore && g.tile(x, y).building < 0) ++c;
    return c;
}

// ---------------- 每周期决策 ----------------
inline const char* const* researchOrder() {
    static const char* kOrder[] = {"hydro", "autodrill", "fusion", "atmo", "alloy", "nanomed", "drone", "gate", nullptr};
    return kOrder;
}

inline void aiTurn(Game& g) {
    aiAnswer(g);

    auto underConstruction = [&] {
        int c = 0;
        for (const Building& b : g.buildings())
            if (b.alive && b.buildLeft > 0) ++c;
        return c;
    };

    const TurnReport& r = g.report();
    const int metal = g.res().metal;
    const int energy = g.res().energy;
    const int farmsWanted = 1 + g.pop() / 9;
    const int minesWanted = std::min(3, oreTiles(g) + g.countType(BType::Mine));
    const bool saving = g.hasTech(Tech::GateTheory) && g.countType(BType::Gate) == 0;

    // 按优先级列出“现在想造的东西”，只执行第一个造得起的
    std::vector<BType> want;
    auto add = [&](BType t, bool cond) { if (cond) want.push_back(t); };

    if (saving) {
        // 星门理论已就绪：停止一切非必要建设，全力攒星门
        add(BType::Farm, r.foodNet < 0 && g.res().food < 30);
        add(BType::Turret, g.waveIn() <= 4 && g.defense() < g.waveStrengthEstimate() + 10);
        add(BType::Gate, metal >= 420 && energy >= 300);
    } else {
        add(BType::Farm, (r.foodNet < 2 || g.res().food < 40) && g.countType(BType::Farm) < farmsWanted);
        add(BType::Hab, g.pop() + 2 > g.housing());
        add(BType::Mine, g.countType(BType::Mine) < 2);
        add(BType::Solar, r.energyNet < 3);
        add(BType::Lab, g.turn() > 6 && g.countType(BType::Lab) < 2);
        add(BType::Turret, g.defense() < g.waveStrengthEstimate() + 14);
        add(BType::Mine, g.countType(BType::Mine) < minesWanted);
        add(BType::Lab, g.countType(BType::Lab) < 3);
        add(BType::Hab, g.pop() + 3 > g.housing());
        add(BType::Geo, g.countType(BType::Geo) < 2 && r.energyNet < 10);
        add(BType::Clinic, g.pop() >= 14 && g.countType(BType::Clinic) < 1);
        add(BType::Clinic, g.pop() >= 22 && g.countType(BType::Clinic) < 2);
        add(BType::Solar, r.energyNet < 8);
    }

    if (underConstruction() < 2) {
        for (BType t : want) {
            // 没有闲置殖民者时不再乱造（居住舱与星门例外：它们本身就是为人手/胜利服务的）
            const bool canStaff = g.idleWorkers() > 0 || t == BType::Hab || t == BType::Gate;
            int x = 0, y = 0;
            if (canStaff && bestSpot(g, t, x, y)) {
                g.doBuild(BDEF[static_cast<size_t>(t)].key, x, y);
                break;
            }
        }
    }

    for (const char* const* key = researchOrder(); *key; ++key)
        if (g.doResearch(*key).find("研究完成") != std::string::npos) break;
}

} // namespace sc
