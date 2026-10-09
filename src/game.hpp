// 星际争霸：殖民地 (Star Colony) —— C++20 命令行殖民地经营游戏
// Copyright (C) 2026 Star Colony contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published
// by the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// See the LICENSE file at the project root for the full license text.
// SPDX-License-Identifier: AGPL-3.0-or-later

// 星际争霸：殖民地 —— 游戏状态与规则（与界面完全解耦，可无头测试）
#pragma once

#include "protocol.hpp"
#include "types.hpp"

#include <deque>
#include <random>
#include <string>
#include <vector>

namespace sc {

struct Resources {
    int metal = 0;
    int energy = 0;
    int food = 0;
    int science = 0;
};

// 需要玩家做决定的事件
struct PendingEvent {
    int                      kind = 0;
    std::string              title;
    std::string              text;
    std::vector<std::string> options;
    int                      a = 0, b = 0, c = 0;
};

// 单周期产出报告（既用于结算，也用于界面预测）
struct TurnReport {
    int metalIn = 0, energyIn = 0, foodIn = 0, scienceIn = 0;
    int energyUp = 0, foodUp = 0;
    int metalNet = 0, energyNet = 0, foodNet = 0, scienceNet = 0;
    bool brownout = false;   // 能源透支，设施半负荷
    bool starving = false;   // 食物透支，饿死殖民者
};

class Game {
public:
    void newGame(uint32_t seed, const std::string& colonyName);

    // ---------------- 查询 ----------------
    const std::string& colonyName() const { return name_; }
    int                turn() const { return turn_; }
    // 越界访问返回只读哨兵地块，而不是 UB（P1 边界加固）
    const Tile&        tile(int x, int y) const {
        static const Tile sentinel{};
        if (x < 0 || x >= MAP_W || y < 0 || y >= MAP_H) return sentinel;
        return tiles_[static_cast<size_t>(y * MAP_W + x)];
    }
    const std::vector<Building>& buildings() const { return blds_; }
    const Building*    building(int id) const;
    const Resources&   res() const { return res_; }
    int                pop() const { return pop_; }
    int                housing() const { return housing_; }
    int                morale() const { return morale_; }
    // 士气产出系数：引擎内唯一来源，界面直接复用（避免规则重复）
    double             moraleMultiplier() const { return TUNE.moraleBase + morale_ / TUNE.moraleDivisor; }
    uint32_t           techs() const { return techs_; }
    bool               hasTech(Tech t) const { return (techs_ & techBit(t)) != 0; }
    Weather            weather() const { return weather_; }
    int                weatherLeft() const { return weatherLeft_; }
    int                waveIn() const { return waveIn_; }
    int                waveStrengthEstimate() const;
    int                defense() const;
    const TurnReport&  report() const { return report_; }
    bool               over() const { return over_; }
    bool               won() const { return won_; }
    const std::string& endReason() const { return endReason_; }
    const std::deque<LogEntry>& log() const { return log_; }
    const std::vector<int>&        assigned() const { return assigned_; }
    // 本局实际随机种子；0 = 未知（例如对局由存档载入）。仅运行时成员，不写入存档。
    uint32_t                      seed() const { return seed_; }
    int                idleWorkers() const;   // 已完工建筑用不完的闲置殖民者

    TurnReport         evaluate() const;   // 纯预测，不改变状态
    int                workerNeed(const Building& b) const;
    int                adjIce(int x, int y) const;
    int                adjMountain(int x, int y) const;
    int                countType(BType t) const;    // 含在建（用于"是否已拥有/禁止重复建造"）
    int                countReady(BType t) const;   // 只数已完工（用于"效果是否生效"）
    bool               buildable(BType t, int x, int y, std::string* why) const;
    // 纯规则层可建性（地形/占用/前置），**不含**资源是否够付。
    // 供 preview_build 把「地形不符」与「资源不足」分开回报（契约 §4.6.1 要求两者相互独立）。
    bool               buildableTerrain(BType t, int x, int y, std::string* why) const;

    // ---------------- 界面复用的规则查询（单一事实来源） ----------------
    int                tileOreYield(int x, int y) const { return tile(x, y).richness * MINE_DEPLETION_PER_RICH; }
    int                farmIceBonus() const { return static_cast<int>(TUNE.farmIceBonus); }
    int                mineMountainBonus() const { return static_cast<int>(TUNE.mineMountainBonus); }
    int                mineMountainCap() const { return TUNE.mineMountainCap; }

    // ---------------- 行动（返回结构化结果） ----------------
    ActionResult doBuild(const std::string& key, int x, int y);
    ActionResult doDemolish(int id);
    ActionResult doToggle(int id);
    ActionResult doFocus(int id);
    ActionResult doResearch(const std::string& key);
    void         advanceTurn();

    // ---------------- 事件 ----------------
    bool                  hasPending() const { return !pending_.empty(); }
    // 无待处理事件时返回只读空哨兵，而不是对空 deque 取 front()（P1 边界加固）
    const PendingEvent&   pending() const {
        static const PendingEvent emptyEvent{};
        return pending_.empty() ? emptyEvent : pending_.front();
    }
    ActionResult          answer(int option);

    // ---------------- 快照（全值，const 且不消耗随机数） ----------------
    GameSnapshot snapshot() const;

    // ---------------- 存档 ----------------
    std::string saveTo(const std::string& path) const;
    std::string loadFrom(const std::string& path);

    // ---------------- 日志 ----------------
    // 兼容层：任意中文文本直接记一条 Text 条目
    void log(const std::string& s);
    // 结构化：记一条带代码与参数的条目
    void log(ActCode c, std::vector<long long> ints = {}, std::vector<std::string> ss = {});

private:
    void generateMap();
    void rollWeather();
    void rollEvent();
    void recomputeWorkers();
    void growPopulation();
    void applyCombat(int strength);
    void checkEnd();
    int  rollWaveStrength();
    bool readFile(const std::string& path, std::string& err);

    std::string                   name_ = "新曙光";
    std::array<Tile, MAP_W * MAP_H> tiles_{};
    std::vector<Building>         blds_;
    std::vector<int>              priority_;  // 工人分配优先级（建筑 id）
    std::vector<int>              assigned_;  // 每个建筑实际分配到的工人数
    Resources                     res_;
    int                           pop_ = 6;
    int                           housing_ = 6;
    int                           morale_ = 70;
    int                           turn_ = 1;
    double                        popAcc_ = 0.0;
    uint32_t                      techs_ = 0;
    Weather                       weather_ = Weather::Clear;
    int                           weatherLeft_ = 3;
    int                           waveIn_ = 8;
    int                           hqId_ = -1;
    std::mt19937                  rng_{1};   // 单一随机流：消耗时机决定后续所有随机结果
    // 本局种子（仅运行时）。刻意**不**写入存档：存档格式保持与基线逐字节一致，
    // 0 表示未知（读档后即为 0）。P3 重构存档时再持久化。
    uint32_t                      seed_ = 0;
    std::deque<LogEntry>          log_;
    std::deque<PendingEvent>      pending_;
    TurnReport                    report_;
    bool                          over_ = false;
    bool                          won_ = false;
    std::string                   endReason_;
};

} // namespace sc
