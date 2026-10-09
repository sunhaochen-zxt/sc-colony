// 星际争霸：殖民地 (Star Colony) —— C++20 命令行殖民地经营游戏
// Copyright (C) 2026 Star Colony contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published
// by the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// See the LICENSE file at the project root for the full license text.
// SPDX-License-Identifier: AGPL-3.0-or-later

// 平衡性批量扫描工具（T3）
// 用内置试玩 AI（tests/ai.hpp）批量跑种子，统计胜率、通关周期分布、失败原因与卡点阶段；
// 并通过运行时 Tuning（src/types.hpp 的 TUNE）做参数敏感性分析，不改动 src/。
//
// 编译：
//   g++ -std=c++20 -O2 -Isrc -Itests tools/sweep.cpp src/game.cpp -o build/sweep
//
// 用法：
//   build/sweep                          # 128 种子基线汇总
//   build/sweep --seeds 256              # 指定种子数（seed 1..N）
//   build/sweep --seed0 1000 --seeds 64  # 指定起点
//   build/sweep --csv                    # 每局一行 CSV（stdout）
//   build/sweep --scan                   # 默认 10 参数 × 3 档敏感性扫描
//   build/sweep --scan farmFood=10,12,14 # 自定义单参数扫描
//   build/sweep --set farmFood=14,maxTurns=80   # 固定覆盖（配合基线用）
//   build/sweep --list-params            # 列出可扫描的参数名
#include "ai.hpp"
#include "game.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

using namespace sc;

namespace {

// =====================================================================
//  单局结果与失败归因
// =====================================================================

struct RunResult {
    uint32_t    seed      = 0;
    bool        won       = false;
    int         turns     = 0;    // 结束时 g.turn()
    int         techs     = 0;    // 已完成科技数 0..8
    bool        gateTech  = false;
    int         gateCount = 0;    // 星门（含在建）
    int         pop       = 0;
    int         metal     = 0;
    int         energy    = 0;
    int         food      = 0;
    int         starveTurns = 0;  // 期间发生食物短缺的周期数
    int         waves       = 0;  // 虫潮来袭次数
    int         breaches    = 0;  // 防线被突破次数
    int         gateTechTurn = -1;
    std::string endReason;
    std::string cause;   // 机器可读失败原因
    std::string stage;   // 失败阶段：early/mid/late（胜利为 "-"）
};

// 取 marker 之后新增的日志（marker 为上一检查点最后一条「── 周期 N 结算 ──」）
std::vector<std::string> entriesAfter(const std::deque<std::string>& lg, const std::string& marker) {
    size_t cut = 0;
    bool   found = false;
    if (!marker.empty()) {
        for (size_t i = lg.size(); i-- > 0;) {
            if (lg[i] == marker) { cut = i + 1; found = true; break; }
        }
    }
    if (!marker.empty() && !found) {           // 日志被截断的兜底：只扫最后 25 条，避免重复计数
        cut = lg.size() > 25 ? lg.size() - 25 : 0;
    }
    std::vector<std::string> out;
    out.reserve(lg.size() - cut);
    for (size_t i = cut; i < lg.size(); ++i) out.push_back(lg[i]);
    return out;
}

std::string newestMarker(const std::deque<std::string>& lg) {
    for (size_t i = lg.size(); i-- > 0;)
        if (lg[i].rfind("── 周期 ", 0) == 0) return lg[i];
    return "";
}

RunResult runOne(uint32_t seed) {
    RunResult R;
    R.seed = seed;

    Game g;
    g.newGame(seed, "SWEEP");

    std::string prevMarker;
    bool hadGate = false;
    bool lastStarve = false, lastBreach = false, lastEventLoss = false;

    for (int guard = 0; guard < TUNE.maxTurns + 8 && !g.over(); ++guard) {
        const int turnNo = g.turn();
        aiTurn(g);
        g.advanceTurn();

        const std::vector<std::string> region = entriesAfter(g.log(), prevMarker);
        prevMarker = newestMarker(g.log());

        bool sb = false, br = false, ev = false;
        for (const std::string& s : region) {
            if (s.find("食物短缺") != std::string::npos) sb = true;
            if (s.find("防线被突破") != std::string::npos) br = true;
            if (s.find("没能挺过来") != std::string::npos) ev = true;
            if (s.find("虫潮来袭") != std::string::npos) ++R.waves;
            if (R.gateTechTurn < 0 && s.find("星门理论已解锁") != std::string::npos) R.gateTechTurn = turnNo;
        }
        if (sb) ++R.starveTurns;
        if (br) ++R.breaches;
        lastStarve = sb;
        lastBreach = br;
        lastEventLoss = ev;
        if (!hadGate && g.hasTech(Tech::GateTheory)) { hadGate = true; if (R.gateTechTurn < 0) R.gateTechTurn = turnNo; }
    }

    R.won = g.won();
    R.turns = g.turn();
    R.techs = 0;
    for (int i = 0; i < TECH_COUNT; ++i)
        if (g.hasTech(static_cast<Tech>(i))) ++R.techs;
    R.gateTech = g.hasTech(Tech::GateTheory);
    R.gateCount = g.countType(BType::Gate);
    R.pop = g.pop();
    R.metal = g.res().metal;
    R.energy = g.res().energy;
    R.food = g.res().food;
    R.endReason = g.endReason();

    const bool timeout = R.endReason.find("补给窗口关闭") != std::string::npos;
    const bool popZero = R.endReason.find("人口归零") != std::string::npos;

    if (R.won) {
        R.cause = "win";
        R.stage = "-";
    } else if (timeout) {
        if (!R.gateTech)                R.cause = "timeout_no_gatetech";
        else if (R.gateCount == 0)      R.cause = "timeout_no_gate";
        else                            R.cause = "timeout_gate_unfinished";
    } else if (popZero) {
        if (lastBreach)                 R.cause = "death_wave";
        else if (lastStarve)            R.cause = "death_starve";
        else if (lastEventLoss)         R.cause = "death_event";
        else if (g.report().starving)   R.cause = "death_starve";
        else                            R.cause = "death_other";
    } else {
        R.cause = "other";
    }

    if (!R.won) {
        if (R.turns <= 20)      R.stage = "early<=20";
        else if (R.turns <= 50) R.stage = "mid21-50";
        else                    R.stage = "late>50";
    }
    return R;
}

// =====================================================================
//  参数注册表（全部落在 TUNE 上，不改 src/）
// =====================================================================

using Setter = std::function<void(double)>;

const std::map<std::string, Setter>& paramRegistry() {
    static const std::map<std::string, Setter> reg = {
        // 全局
        {"maxTurns",          [](double v) { TUNE.maxTurns = static_cast<int>(std::lround(v)); }},
        {"startMetal",        [](double v) { TUNE.startMetal = static_cast<int>(std::lround(v)); }},
        {"startEnergy",       [](double v) { TUNE.startEnergy = static_cast<int>(std::lround(v)); }},
        {"startFood",         [](double v) { TUNE.startFood = static_cast<int>(std::lround(v)); }},
        {"startPop",          [](double v) { TUNE.startPop = static_cast<int>(std::lround(v)); }},
        {"baseHousing",       [](double v) { TUNE.baseHousing = static_cast<int>(std::lround(v)); }},
        {"housingPerHab",     [](double v) { TUNE.housingPerHab = static_cast<int>(std::lround(v)); }},
        {"housingPerHabAtmo", [](double v) { TUNE.housingPerHabAtmo = static_cast<int>(std::lround(v)); }},
        // 产出
        {"solarEnergy",       [](double v) { TUNE.solarEnergy = v; }},
        {"geoEnergy",         [](double v) { TUNE.geoEnergy = v; }},
        {"hqEnergy",          [](double v) { TUNE.hqEnergy = v; }},
        {"hqScience",         [](double v) { TUNE.hqScience = v; }},
        {"labScience",        [](double v) { TUNE.labScience = v; }},
        {"mineMetalPerRich",  [](double v) { TUNE.mineMetalPerRich = v; }},
        {"mineMountainBonus", [](double v) { TUNE.mineMountainBonus = v; }},
        {"mineMountainCap",   [](double v) { TUNE.mineMountainCap = static_cast<int>(std::lround(v)); }},
        {"farmFood",          [](double v) { TUNE.farmFood = v; }},
        {"farmIceBonus",      [](double v) { TUNE.farmIceBonus = v; }},
        {"farmIceCap",        [](double v) { TUNE.farmIceCap = static_cast<int>(std::lround(v)); }},
        {"fusionMult",        [](double v) { TUNE.fusionMult = v; }},
        {"hydroMult",         [](double v) { TUNE.hydroMult = v; }},
        {"drillMult",         [](double v) { TUNE.drillMult = v; }},
        {"foodPerPop",        [](double v) { TUNE.foodPerPop = v; }},
        {"atmoFoodMult",      [](double v) { TUNE.atmoFoodMult = v; }},
        // 士气
        {"moraleBase",        [](double v) { TUNE.moraleBase = v; }},
        {"moraleDivisor",     [](double v) { TUNE.moraleDivisor = v; }},
        {"moraleTarget",      [](double v) { TUNE.moraleTarget = static_cast<int>(std::lround(v)); }},
        {"starveMoraleDrop",  [](double v) { TUNE.starveMoraleDrop = static_cast<int>(std::lround(v)); }},
        {"breachMoraleDrop",  [](double v) { TUNE.breachMoraleDrop = static_cast<int>(std::lround(v)); }},
        // 人口
        {"popGrowthRate",     [](double v) { TUNE.popGrowthRate = v; }},
        {"popGrowthNeed",     [](double v) { TUNE.popGrowthNeed = v; }},
        {"nanoGrowth",        [](double v) { TUNE.nanoGrowth = v; }},
        {"clinicGrowth",      [](double v) { TUNE.clinicGrowth = v; }},
        // 虫潮
        {"waveBase",          [](double v) { TUNE.waveBase = v; }},
        {"wavePerTurn",       [](double v) { TUNE.wavePerTurn = v; }},
        {"waveFirst",         [](double v) { TUNE.waveFirst = static_cast<int>(std::lround(v)); }},
        {"waveInterval",      [](double v) { TUNE.waveInterval = static_cast<int>(std::lround(v)); }},
        {"militiaPerPop",     [](double v) { TUNE.militiaPerPop = v; }},
        {"turretDefense",     [](double v) { TUNE.turretDefense = static_cast<int>(std::lround(v)); }},
        {"turretDefenseAlloy",[](double v) { TUNE.turretDefenseAlloy = static_cast<int>(std::lround(v)); }},
        {"combatLossDiv",     [](double v) { TUNE.combatLossDiv = static_cast<int>(std::lround(v)); }},
        {"combatLossMax",     [](double v) { TUNE.combatLossMax = static_cast<int>(std::lround(v)); }},
        {"clinicMitigationCap",[](double v){ TUNE.clinicMitigationCap = static_cast<int>(std::lround(v)); }},
        {"combatDamageDiv",   [](double v) { TUNE.combatDamageDiv = static_cast<int>(std::lround(v)); }},
        // 事件
        {"eventChance",       [](double v) { TUNE.eventChance = static_cast<int>(std::lround(v)); }},
        // 合成参数
        {"waveScale",         [](double v) { TUNE.waveBase *= v; TUNE.wavePerTurn *= v; }},
        {"turretScale",       [](double v) { TUNE.turretDefense = static_cast<int>(std::lround(TUNE.turretDefense * v));
                                             TUNE.turretDefenseAlloy = static_cast<int>(std::lround(TUNE.turretDefenseAlloy * v)); }},
    };
    return reg;
}

bool setParam(const std::string& key, double v) {
    auto it = paramRegistry().find(key);
    if (it == paramRegistry().end()) return false;
    it->second(v);
    return true;
}

// =====================================================================
//  批量统计
// =====================================================================

struct BatchStats {
    int n = 0, wins = 0;
    std::vector<int> winTurns;
    double techSum = 0.0, techWinSum = 0.0, techLossSum = 0.0;
    int techLossN = 0;
    int starveTurnSum = 0, breachSum = 0, waveSum = 0;
    int gateTechN = 0, gateBuildN = 0;
    std::map<std::string, int> causes;
    std::map<std::string, int> stages;
    std::vector<RunResult> runs;
};

void addTo(BatchStats& S, const RunResult& r) {
    ++S.n;
    S.techSum += r.techs;
    S.starveTurnSum += r.starveTurns;
    S.breachSum += r.breaches;
    S.waveSum += r.waves;
    if (r.gateTech) ++S.gateTechN;
    if (r.gateCount > 0) ++S.gateBuildN;
    if (r.won) {
        ++S.wins;
        S.winTurns.push_back(r.turns);
        S.techWinSum += r.techs;
    } else {
        S.techLossSum += r.techs;
        ++S.techLossN;
        ++S.causes[r.cause];
        ++S.stages[r.stage];
    }
    S.runs.push_back(r);
}

double medianOf(std::vector<int> v) {
    if (v.empty()) return -1.0;
    std::sort(v.begin(), v.end());
    const size_t m = v.size();
    if (m % 2) return v[m / 2];
    return (v[m / 2 - 1] + v[m / 2]) / 2.0;
}

BatchStats runBatch(int nseeds, uint32_t seed0) {
    BatchStats S;
    for (int i = 0; i < nseeds; ++i) addTo(S, runOne(seed0 + static_cast<uint32_t>(i)));
    return S;
}

void printSummary(const BatchStats& S, const std::string& label) {
    std::vector<int> wt = S.winTurns;
    std::sort(wt.begin(), wt.end());
    const double rate = S.n ? 100.0 * S.wins / S.n : 0.0;
    std::printf("== %s ==\n", label.c_str());
    std::printf("种子数 %d  胜利 %d  胜率 %.1f%%\n", S.n, S.wins, rate);
    if (!wt.empty())
        std::printf("通关周期: min %d  中位 %.1f  max %d  (n=%zu)\n", wt.front(), medianOf(wt), wt.back(), wt.size());
    std::printf("平均科技完成数: 全部 %.2f  胜利 %.2f  失败 %.2f\n", S.techSum / S.n,
                S.wins ? S.techWinSum / S.wins : 0.0, S.techLossN ? S.techLossSum / S.techLossN : 0.0);
    std::printf("星门理论完成 %d/%d (%.1f%%)  星门开建 %d/%d (%.1f%%)\n", S.gateTechN, S.n,
                100.0 * S.gateTechN / S.n, S.gateBuildN, S.n, 100.0 * S.gateBuildN / S.n);
    std::printf("平均: 食物短缺周期 %.2f  防线被突破 %.2f  虫潮次数 %.2f\n", 1.0 * S.starveTurnSum / S.n,
                1.0 * S.breachSum / S.n, 1.0 * S.waveSum / S.n);

    std::printf("失败原因分布 (失败 %d 局):\n", S.n - S.wins);
    for (const auto& kv : S.causes) std::printf("  %-26s %4d  %.1f%%\n", kv.first.c_str(), kv.second,
                                                100.0 * kv.second / (S.n - S.wins));
    if (S.wins == S.n) std::printf("  (无失败局)\n");
    std::printf("失败卡点阶段: early(<=20) %d  mid(21-50) %d  late(>50) %d\n",
                S.stages.count("early<=20") ? S.stages.at("early<=20") : 0,
                S.stages.count("mid21-50") ? S.stages.at("mid21-50") : 0,
                S.stages.count("late>50") ? S.stages.at("late>50") : 0);
    std::fflush(stdout);
}

void printCsvHeader() {
    std::printf("seed,won,turns,techs,gate_tech,gate_count,pop,metal,energy,food,starve_turns,breaches,waves,gatetech_turn,cause,stage,end_reason\n");
}

void printCsvRow(const RunResult& r) {
    std::printf("%u,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%s,%s,%s\n", r.seed, r.won ? 1 : 0, r.turns, r.techs,
                r.gateTech ? 1 : 0, r.gateCount, r.pop, r.metal, r.energy, r.food, r.starveTurns, r.breaches,
                r.waves, r.gateTechTurn, r.cause.c_str(), r.stage.c_str(), r.endReason.c_str());
}

int countCause(const BatchStats& S, const char* c) {
    auto it = S.causes.find(c);
    return it == S.causes.end() ? 0 : it->second;
}

int countStage(const BatchStats& S, const char* c) {
    auto it = S.stages.find(c);
    return it == S.stages.end() ? 0 : it->second;
}

// =====================================================================
//  命令行
// =====================================================================

struct ScanSpec { std::string key; std::vector<double> vals; };

void usage() {
    std::printf("用法: sweep [--seeds N] [--seed0 S] [--csv] [--quiet] [--scan [k=v1,v2,v3]]\n"
                "             [--set k=v,k=v] [--list-params]\n");
}

} // namespace

int main(int argc, char** argv) {
    int  nseeds = 128;
    uint32_t seed0 = 1;
    bool csv = false, scan = false, quiet = false;
    std::vector<std::pair<std::string, double>> overrides;
    std::vector<ScanSpec> scanSpecs;
    std::string scanArg;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "缺少 %s 参数\n", what); std::exit(2); }
            return argv[++i];
        };
        if (a == "--seeds") nseeds = std::atoi(next("--seeds").c_str());
        else if (a == "--seed0") seed0 = static_cast<uint32_t>(std::strtoul(next("--seed0").c_str(), nullptr, 10));
        else if (a == "--csv") csv = true;
        else if (a == "--quiet") quiet = true;
        else if (a == "--scan") {
            scan = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') scanArg = argv[++i];
        }
        else if (a == "--set") {
            std::string s = next("--set");
            size_t pos = 0;
            while (pos <= s.size()) {
                size_t comma = s.find(',', pos);
                std::string kv = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                size_t eq = kv.find('=');
                if (eq != std::string::npos)
                    overrides.emplace_back(kv.substr(0, eq), std::atof(kv.substr(eq + 1).c_str()));
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
        }
        else if (a == "--list-params") {
            for (const auto& kv : paramRegistry()) std::printf("%s\n", kv.first.c_str());
            return 0;
        }
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else { std::fprintf(stderr, "未知参数：%s\n", a.c_str()); usage(); return 2; }
    }

    if (nseeds <= 0) { std::fprintf(stderr, "--seeds 必须为正\n"); return 2; }

    // 默认敏感性扫描：>=6 个参数 × 3 档（见 docs/BALANCE.md）
    if (scan) {
        if (!scanArg.empty()) {
            size_t eq = scanArg.find('=');
            ScanSpec sp;
            sp.key = scanArg.substr(0, eq);
            std::string vs = eq == std::string::npos ? "" : scanArg.substr(eq + 1);
            size_t pos = 0;
            while (pos <= vs.size()) {
                size_t comma = vs.find(',', pos);
                std::string one = vs.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                if (!one.empty()) sp.vals.push_back(std::atof(one.c_str()));
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
            scanSpecs.push_back(sp);
        } else {
            scanSpecs = {
                {"waveScale",     {0.85, 1.0, 1.15}},
                {"wavePerTurn",   {0.6, 0.8, 1.0}},
                {"farmFood",      {10, 12, 14}},
                {"labScience",    {4, 6, 8}},
                {"eventChance",   {20, 30, 45}},
                {"maxTurns",      {80, 90, 100}},
                {"startMetal",    {180, 240, 300}},
                {"turretDefense", {12, 16, 20}},
                {"waveFirst",     {6, 9, 12}},
                {"startFood",     {50, 70, 90}},
                {"foodPerPop",    {0.8, 1.0, 1.2}},
            };
        }

        const Tuning baseline = TUNE;
        std::printf("scan,param,value,seeds,wins,winrate,min_turns,median_turns,max_turns,avg_techs,"
                    "gate_tech_pct,"
                    "fail_wave,fail_starve,fail_event,fail_timeout_no_gatetech,fail_timeout_no_gate,"
                    "fail_timeout_gate_unfinished,fail_other,"
                    "stage_early,stage_mid,stage_late\n");
        for (const ScanSpec& sp : scanSpecs) {
            for (double v : sp.vals) {
                TUNE = baseline;
                if (!setParam(sp.key, v)) {
                    std::fprintf(stderr, "未知参数：%s\n", sp.key.c_str());
                    return 2;
                }
                BatchStats S = runBatch(nseeds, seed0);
                std::vector<int> wt = S.winTurns;
                std::sort(wt.begin(), wt.end());
                std::printf("%s,%s,%g,%d,%d,%.1f,%d,%.1f,%d,%.2f,%.1f,"
                            "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
                            sp.key.c_str(), sp.key.c_str(), v, S.n, S.wins, 100.0 * S.wins / S.n,
                            wt.empty() ? -1 : wt.front(), medianOf(wt), wt.empty() ? -1 : wt.back(),
                            S.techSum / S.n, 100.0 * S.gateTechN / S.n,
                            countCause(S, "death_wave"), countCause(S, "death_starve"), countCause(S, "death_event"),
                            countCause(S, "timeout_no_gatetech"), countCause(S, "timeout_no_gate"),
                            countCause(S, "timeout_gate_unfinished"), countCause(S, "other"),
                            countStage(S, "early<=20"), countStage(S, "mid21-50"), countStage(S, "late>50"));
                std::fflush(stdout);
            }
        }
        TUNE = baseline;
        return 0;
    }

    for (const auto& ov : overrides)
        if (!setParam(ov.first, ov.second)) {
            std::fprintf(stderr, "未知参数：%s\n", ov.first.c_str());
            return 2;
        }

    BatchStats S = runBatch(nseeds, seed0);
    if (csv) {
        printCsvHeader();
        for (const RunResult& r : S.runs) printCsvRow(r);
    } else if (!quiet) {
        std::string label = "基线（TUNE 默认值）";
        if (!overrides.empty()) {
            label = "覆盖:";
            for (const auto& ov : overrides) label += " " + ov.first + "=" + std::to_string(ov.second);
        }
        label += "  seeds " + std::to_string(seed0) + ".." + std::to_string(seed0 + nseeds - 1);
        printSummary(S, label);
    }
    return 0;
}
