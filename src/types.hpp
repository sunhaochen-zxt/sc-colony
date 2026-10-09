// 星际争霸：殖民地 (Star Colony) —— C++20 命令行殖民地经营游戏
// Copyright (C) 2026 Star Colony contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published
// by the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// See the LICENSE file at the project root for the full license text.
// SPDX-License-Identifier: AGPL-3.0-or-later

// 星际争霸：殖民地 —— 基础类型与静态数据表
#pragma once

#include <array>
#include <cstdint>

namespace sc {

// ---------- 世界尺寸 ----------
inline constexpr int MAP_W = 18;
inline constexpr int MAP_H = 12;
inline constexpr int MAX_TURNS = 90;

// ---------- 规则常量 ----------
inline constexpr int MINE_DEPLETION_PER_RICH = 4;  // 钻矿场每周期按丰度开采的矿量

// ---------- 终端颜色索引 ----------
enum Col : int {
    COL_DEF = 0,   // 默认
    COL_GREY,      // 暗灰
    COL_RED,
    COL_GREEN,
    COL_YELLOW,
    COL_BLUE,
    COL_MAGENTA,
    COL_CYAN,
    COL_WHITE,
    COL_BWHITE,    // 亮白/粗体
};

// ---------- 地形 ----------
enum class Terrain : char {
    Plain    = '.',  // 平原
    Ore      = '*',  // 金属矿脉
    Geo      = '~',  // 地热口
    Ice      = '|',  // 冰层
    Mountain = '^',  // 山脉
};

struct Tile {
    Terrain terrain = Terrain::Plain;
    int ore      = 0;   // 剩余矿量（仅矿脉地块）
    int richness = 0;   // 矿脉丰度 1..4
    int building = -1;  // 占用该地块的建筑 id，-1 为空
};

// ---------- 建筑 ----------
enum class BType : int {
    HQ = 0,   // 指挥中心
    Solar,    // 太阳能板
    Geo,      // 地热站
    Mine,     // 钻矿场
    Farm,     // 水培农场
    Hab,      // 居住舱
    Lab,      // 研究所
    Clinic,   // 医疗站
    Turret,   // 防御炮塔
    Gate,     // 星门
    COUNT,
};

inline constexpr int BTYPE_COUNT = static_cast<int>(BType::COUNT);

struct BDef {
    const char* key;        // 命令用英文键
    const char* name;       // 中文名
    char        glyph;      // 地图字形
    int         color;      // 显示颜色 (Col)
    int         costMetal;
    int         costEnergy;
    int         costScience;
    int         buildTurns; // 建造所需周期
    int         workers;    // 所需工人
    int         upkeep;     // 每周期能源消耗
    bool        repeatable; // 是否可重复建造
    const char* desc;
};

extern const std::array<BDef, BTYPE_COUNT> BDEF;

// ---------- 科技 ----------
enum class Tech : int {
    Hydro = 0,   // 水培改良
    AutoDrill,   // 自动钻机
    Fusion,      // 聚变核心
    NanoMed,     // 医疗纳米
    MilAlloy,    // 军用合金
    Atmo,        // 大气处理
    DroneNet,    // 无人机网络
    GateTheory,  // 星门理论
    COUNT,
};

inline constexpr int TECH_COUNT = static_cast<int>(Tech::COUNT);
constexpr uint32_t techBit(Tech t) { return 1u << static_cast<int>(t); }

struct TechDef {
    const char* key;
    const char* name;
    int         cost;
    uint32_t    req;   // 前置科技位掩码
    const char* desc;
};

extern const std::array<TechDef, TECH_COUNT> TDEF;

// ---------- 天气 ----------
enum class Weather : int {
    Clear = 0,   // 晴朗
    Sandstorm,   // 沙暴
    ColdSnap,    // 寒潮
    Flare,       // 太阳耀斑
    AcidRain,    // 酸雨
    COUNT,
};

inline constexpr int WEATHER_COUNT = static_cast<int>(Weather::COUNT);

struct WeatherDef {
    const char* name;
    int         color;
    double      metal;    // 产出倍率
    double      energy;
    double      food;
    double      science;
    const char* desc;
};

extern const std::array<WeatherDef, WEATHER_COUNT> WDEF;

// ---------- 平衡性可调参数 ----------
// 默认值就是正式数值；把这组参数抽出来是为了让 tools/sweep.cpp 之类的平衡性工具
// 能在不改源码的情况下扫描参数（直接改 TUNE 的字段即可）。
struct Tuning {
    // 全局
    int    maxTurns           = MAX_TURNS;
    int    startMetal         = 240;
    int    startEnergy        = 90;
    int    startFood          = 70;
    int    startPop           = 6;
    int    baseHousing        = 8;    // 指挥中心提供的人口上限
    int    housingPerHab      = 6;
    int    housingPerHabAtmo  = 8;    // 研究大气处理之后

    // 产出
    double solarEnergy        = 6.0;
    double geoEnergy          = 18.0;
    double hqEnergy           = 4.0;
    double hqScience          = 1.0;
    double labScience         = 6.0;
    double mineMetalPerRich   = 4.0;  // 每点矿脉丰度提供的金属
    double mineMountainBonus  = 2.0;  // 相邻山脉加成
    int    mineMountainCap    = 6;
    double farmFood           = 12.0;
    double farmIceBonus       = 2.0;  // 相邻冰层加成
    int    farmIceCap         = 6;
    double fusionMult         = 1.5;
    double hydroMult          = 1.5;
    double drillMult          = 1.4;
    double foodPerPop         = 1.15;  // BALANCE 方案 C：食物消耗 +15%（512 种子 AI 胜率 65.0% -> 59.6%）
    double atmoFoodMult       = 0.8;

    // 士气
    double moraleBase         = 0.75;
    double moraleDivisor      = 200.0;
    int    moraleTarget       = 70;
    int    starveMoraleDrop   = 7;
    int    breachMoraleDrop   = 8;

    // 人口
    double popGrowthRate      = 1.4;  // 每周期增长进度
    double popGrowthNeed      = 2.0;  // 累积到多少出一名殖民者
    double nanoGrowth         = 0.6;
    double clinicGrowth       = 0.3;
    int    overcrowdRisk      = 15;   // 超编 1 人每周期流失人口的百分比

    // 虫潮
    double waveBase           = 6.0;
    double wavePerTurn        = 0.8;
    int    waveFirst          = 9;    // 第一波在第几周期
    int    waveInterval       = 9;    // 之后每几周期一波
    double militiaPerPop      = 0.8;
    int    turretDefense      = 16;
    int    turretDefenseAlloy = 28;
    int    combatLossDiv      = 14;   // 突破后的伤亡
    int    combatLossMax      = 4;    // 单次虫潮的伤亡上限（ BALANCE §8.3：修复后该上限才真正生效）
    int    clinicMitigationCap= 1;    // 医疗站最多抵消几名伤亡（0=不抵消；原先无上限，等于虫潮永远只掉 1 人）
    int    combatDamageDiv    = 35;   // 突破后受损建筑数

    // 事件
    int    eventChance        = 30;   // 每周期触发随机事件的百分比
};

extern Tuning TUNE;

// ---------- 建筑实例 ----------
struct Building {
    int   id        = -1;
    BType type      = BType::Solar;
    int   x         = 0;
    int   y         = 0;
    int   buildLeft = 0;   // >0 表示在建，倒数到 0 完工
    int   damaged   = 0;   // >0 表示受损停产，自动修复
    bool  enabled   = true;
    bool  alive     = true;
};

} // namespace sc
