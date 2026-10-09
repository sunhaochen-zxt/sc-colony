// 星际争霸：殖民地 (Star Colony) —— C++20 命令行殖民地经营游戏
// Copyright (C) 2026 Star Colony contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published
// by the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// See the LICENSE file at the project root for the full license text.
// SPDX-License-Identifier: AGPL-3.0-or-later

// 星际争霸：殖民地 —— 无头自检：规则不变量 + 存档往返 + AI 试玩平衡性
#include "game.hpp"
#include "ai.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace sc;

namespace {

int g_fail = 0;
int g_checks = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_fail;
        std::printf("  [FAIL] %s\n", what.c_str());
    }
}

// ---------------- 不变量检查 ----------------
void checkInvariants(const Game& g, const std::string& tag) {
    const Resources& r = g.res();
    check(r.metal >= 0 && r.energy >= 0 && r.food >= 0 && r.science >= 0, tag + " 资源非负");
    check(g.pop() >= 0, tag + " 人口非负");
    check(g.morale() >= 0 && g.morale() <= 100, tag + " 士气在 0..100");
    check(g.turn() >= 1, tag + " 周期有效");

    for (const Building& b : g.buildings()) {
        if (!b.alive) continue;
        check(b.x >= 0 && b.x < MAP_W && b.y >= 0 && b.y < MAP_H, tag + " 建筑坐标合法");
        check(g.tile(b.x, b.y).building == b.id, tag + " 地块与建筑互相引用一致 #" + std::to_string(b.id));
        check(b.buildLeft >= 0 && b.damaged >= 0, tag + " 建筑计时非负");
    }
    for (int y = 0; y < MAP_H; ++y)
        for (int x = 0; x < MAP_W; ++x) {
            const Tile& t = g.tile(x, y);
            if (t.building >= 0) {
                const Building* b = g.building(t.building);
                check(b != nullptr, tag + " 地块引用的建筑存在");
                if (b) check(b->x == x && b->y == y, tag + " 地块引用的建筑位置正确");
            }
            if (t.terrain == Terrain::Ore) check(t.ore >= 0, tag + " 矿脉矿量非负");
        }
}

struct AiResult {
    int  turn = 0;
    bool won = false;
    int  pop = 0;
    int  techs = 0;
    int  buildings = 0;
    bool reachedGateTheory = false;
    bool everStable = true;
};

AiResult aiPlay(uint32_t seed, bool verbose) {
    Game g;
    g.newGame(seed, "AI-" + std::to_string(seed));
    AiResult res;

    int guard = 0;
    while (!g.over() && guard++ < MAX_TURNS + 20) {
        aiTurn(g);
        checkInvariants(g, "AI#" + std::to_string(seed) + "T" + std::to_string(g.turn()));
        if (g.res().food == 0 && g.report().starving) res.everStable = false;
        if (g.over()) break;
        g.advanceTurn();
    }

    res.turn = g.turn();
    res.won = g.won();
    res.pop = g.pop();
    res.buildings = static_cast<int>(g.buildings().size());
    int t = 0;
    for (int i = 0; i < TECH_COUNT; ++i)
        if (g.hasTech(static_cast<Tech>(i))) ++t;
    res.techs = t;
    res.reachedGateTheory = g.hasTech(Tech::GateTheory);
    if (verbose) {
        std::printf("  seed %-5u  %s  周期 %2d  人口 %2d  建筑 %2d  科技 %d/8  星门理论:%s\n",
                    seed, res.won ? "★撤离成功" : "✖失败    ", res.turn, res.pop, res.buildings,
                    res.techs, res.reachedGateTheory ? "是" : "否");
    }
    return res;
}

void compareState(const Game& a, const Game& b, const std::string& tag) {
    check(a.turn() == b.turn(), tag + " 周期一致");
    check(a.pop() == b.pop(), tag + " 人口一致");
    check(a.morale() == b.morale(), tag + " 士气一致");
    check(a.res().metal == b.res().metal, tag + " 金属一致");
    check(a.res().energy == b.res().energy, tag + " 能源一致");
    check(a.res().food == b.res().food, tag + " 食物一致");
    check(a.res().science == b.res().science, tag + " 科研一致");
    check(a.techs() == b.techs(), tag + " 科技一致");
    check(a.buildings().size() == b.buildings().size(), tag + " 建筑数一致");
    check(a.weather() == b.weather(), tag + " 天气一致");
    check(a.waveIn() == b.waveIn(), tag + " 虫潮倒计时一致");
    check(a.log().size() == b.log().size(), tag + " 日志条数一致");
}

} // namespace

int main() {
    std::printf("=== 星际争霸：殖民地 自检 ===\n\n");

    // ---------- 1. 地图生成 ----------
    std::printf("[1] 地图生成不变量（20 个随机种子）\n");
    for (uint32_t s = 1; s <= 20; ++s) {
        Game g;
        g.newGame(s * 7919u, "T");
        int ore = 0, geo = 0;
        for (int y = 0; y < MAP_H; ++y)
            for (int x = 0; x < MAP_W; ++x) {
                if (g.tile(x, y).terrain == Terrain::Ore) ++ore;
                if (g.tile(x, y).terrain == Terrain::Geo) ++geo;
            }
        check(ore >= 4, "种子 " + std::to_string(s) + " 至少 4 处矿脉");
        check(geo >= 1, "种子 " + std::to_string(s) + " 至少 1 处地热口");
        check(g.building(0) != nullptr, "种子 " + std::to_string(s) + " 指挥中心存在");
        check(g.pop() == 6 && g.housing() == 8, "种子 " + std::to_string(s) + " 初始人口 6 / 上限 8");
        check(g.res().metal == 240 && g.res().energy == 90 && g.res().food == 70, "种子 " + std::to_string(s) + " 初始资源正确");
        check(g.assigned().size() >= 1 && g.assigned()[0] == 2, "种子 " + std::to_string(s) + " 指挥中心分配到 2 名工人");
        checkInvariants(g, "new#" + std::to_string(s));
    }
    std::printf("    完成\n\n");

    // ---------- 2. 基础规则 ----------
    std::printf("[2] 基础规则\n");
    {
        Game g;
        g.newGame(1234, "T");
        // 非法建造：山脉/已有建筑/资源不足
        std::string why;
        check(!g.buildable(BType::Solar, 8, 6, &why), "不能建在已有建筑上");
        check(g.buildable(BType::Farm, 7, 5, &why), "平原可建农场");
        check(g.doBuild("不存在", 7, 5).find("未知建筑类型") != std::string::npos, "未知建筑类型被拒绝");
        check(g.doBuild("gate", 7, 5).find("星门理论") != std::string::npos, "未研究星门理论时禁止建造星门");
        check(g.doDemolish(0).find("无法拆除") != std::string::npos, "指挥中心不可拆除");

        int before = g.res().metal;
        std::string r = g.doBuild("sol", 7, 5);
        check(r.find("开始建造") != std::string::npos, "合法建造成功");
        check(g.res().metal == before - 40, "建造扣除 40 金属");
        check(g.tile(7, 5).building >= 0, "地块被占用");

        g.advanceTurn();
        g.advanceTurn();
        const Building* b = g.building(1);
        check(b && b->buildLeft == 0, "太阳能板 2 周期后完工");
        check(g.res().metal > 0, "金属未变负");

        // 研究流程
        Game h;
        h.newGame(9, "T");
        check(h.doResearch("gate").find("前置科技不足") != std::string::npos, "星门理论前置校验");
        for (int i = 0; i < 30; ++i) h.advanceTurn();
        check(h.res().science > 0, "只有指挥中心时也会产出少量科研");
    }
    std::printf("    完成\n\n");

    // ---------- 3. 存档往返 ----------
    std::printf("[3] 存档 / 读档一致性\n");
    {
        const char* path = "/tmp/starcolony_selftest.sav";
        Game a;
        a.newGame(20240607u, "存档测试");
        for (int i = 0; i < 12; ++i) {
            aiTurn(a);
            a.advanceTurn();
        }
        std::string sr = a.saveTo(path);
        check(sr.find("已保存") != std::string::npos, "存档返回成功");

        Game b;
        std::string lr = b.loadFrom(path);
        check(lr.find("已从存档") != std::string::npos, "读档返回成功");
        compareState(a, b, "读档后");
        check(a.colonyName() == b.colonyName(), "殖民地名称一致");

        for (int i = 0; i < 10; ++i) {
            aiTurn(a);
            aiTurn(b);
            a.advanceTurn();
            b.advanceTurn();
        }
        compareState(a, b, "读档后继续 10 周期");

        Game c;
        check(c.loadFrom("/tmp/starcolony_missing_file.sav").find("读取失败") != std::string::npos, "不存在的存档被拒绝");
    }
    std::printf("    完成\n\n");

    // ---------- 4. AI 试玩 ----------
    std::printf("[4] AI 试玩（评估平衡性，8 个种子）\n");
    int wins = 0;
    std::vector<AiResult> results;
    for (uint32_t s : {1u, 2u, 3u, 42u, 777u, 12345u, 99991u, 2024u}) {
        AiResult r = aiPlay(s, true);
        results.push_back(r);
        if (r.won) ++wins;
    }
    int gateTheories = 0, early = 0;
    for (const AiResult& r : results) {
        if (r.reachedGateTheory) ++gateTheories;
        if (r.turn < 30) ++early;
    }
    std::printf("\n  胜率 %d/8；摸到星门理论 %d/8；30 周期内结束 %d/8\n", wins, gateTheories, early);
    check(gateTheories >= 1, "AI 至少能在部分局势下摸到星门理论（科技线可达成）");
    check(wins >= 1, "AI 至少能赢下一局（游戏可通关）");
    check(wins < 8, "AI 并非稳赢（仍然存在失败局，保留挑战性）");
    check(wins >= 4, "正常运营的殖民地胜率不低于一半（难度不至于失衡）");
    std::printf("\n");

    std::printf("=== 检查 %d 项，失败 %d 项 ===\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
