// 星际争霸：殖民地 (Star Colony) —— C++20 命令行殖民地经营游戏
// Copyright (C) 2026 Star Colony contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published
// by the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// See the LICENSE file at the project root for the full license text.
// SPDX-License-Identifier: AGPL-3.0-or-later

// 星际争霸：殖民地 —— 核心规则实现
#include "game.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

namespace sc {

Tuning TUNE;

// =====================================================================
//  静态数据表
// =====================================================================

const std::array<BDef, BTYPE_COUNT> BDEF = {{
    // key    name          glyph  color        M    E    S  建 工 耗 可重复  说明
    {"hq",   "指挥中心",   'C', COL_BLUE,      0,   0,   0,  0, 2, 2, false, "殖民地核心：+4 能源 +1 科研，提供 8 人口上限"},
    {"sol",  "太阳能板",   'S', COL_YELLOW,   40,   0,   0,  2, 1, 0, true,  "产出 6 能源/周期，沙暴与寒潮时效率骤降"},
    {"geo",  "地热站",     'G', COL_MAGENTA,  90,  10,   0,  4, 2, 1, true,  "只能建在地热口，产出 18 能源/周期，全天候"},
    {"mine", "钻矿场",     'M', COL_GREY,     60,  10,   0,  3, 3, 3, true,  "只能建在矿脉上，开采金属；矿脉会枯竭"},
    {"farm", "水培农场",   'F', COL_GREEN,    50,  15,   0,  3, 2, 2, true,  "产出 12 食物/周期，相邻冰层每格 +2"},
    {"hab",  "居住舱",     'H', COL_WHITE,    60,  15,   0,  2, 1, 0, true,  "提供 6 人口上限（大气处理科技后 +8）"},
    {"lab",  "研究所",     'L', COL_CYAN,     70,  20,   0,  4, 3, 4, true,  "产出 6 科研/周期，是科技线的核心"},
    {"clinic","医疗站",    'K', COL_GREEN,    90,  25,   0,  3, 2, 3, true,  "加快人口增长，减少虫潮与饥荒的伤亡"},
    {"turret","防御炮塔",  'T', COL_RED,      80,  25,   0,  3, 1, 2, true,  "提供 16 防御力（军用合金后 28）"},
    {"gate", "星门",       'X', COL_BWHITE,  420, 300,   0, 12, 8,12, false, "终极工程：建成即撤离成功（需星门理论）"},
}};

const std::array<TechDef, TECH_COUNT> TDEF = {{
    {"hydro",     "水培改良",    35, 0,                                              "农场食物产出 +50%"},
    {"autodrill", "自动钻机",    50, 0,                                              "矿场金属产出 +40%"},
    {"fusion",    "聚变核心",    70, 0,                                              "太阳能板与地热站产出 +50%"},
    {"nanomed",   "医疗纳米",    80, 0,                                              "人口增长加快，饥荒与伤亡减 1"},
    {"alloy",     "军用合金",    90, 0,                                              "炮塔防御力 +80%"},
    {"atmo",      "大气处理",   110, techBit(Tech::Hydro),                            "食物消耗 -20%，居住舱 +2 人口上限"},
    {"drone",     "无人机网络", 130, techBit(Tech::AutoDrill),                        "所有建筑所需工人 -1（至少 1）"},
    {"gate",      "星门理论",   170, techBit(Tech::Fusion) | techBit(Tech::Atmo),     "解锁星门工程"},
}};

const std::array<WeatherDef, WEATHER_COUNT> WDEF = {{
    {"晴朗",  COL_GREEN,   1.00, 1.00, 1.00, 1.00, "无修正"},
    {"沙暴",  COL_YELLOW,  0.90, 0.50, 0.80, 1.00, "遮天蔽日：能源 -50%"},
    {"寒潮",  COL_CYAN,    1.00, 0.80, 0.70, 0.90, "低温：食物 -30%，能源 -20%"},
    {"耀斑",  COL_MAGENTA, 1.10, 1.30, 0.80, 0.80, "辐射暴涨：能源 +30%，科研 -20%"},
    {"酸雨",  COL_GREEN,   0.90, 1.00, 0.90, 1.00, "腐蚀设备：有小概率损坏建筑"},
}};

namespace {

const BDef& def(BType t) { return BDEF[static_cast<size_t>(t)]; }

int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// 饱和加法：先在 long long 上算，再夹回 int 值域且不低于 0（杜绝有符号溢出 UB，见 BUG B6）
int satAdd(int cur, int delta) {
    const long long v = static_cast<long long>(cur) + static_cast<long long>(delta);
    if (v <= 0) return 0;
    if (v > static_cast<long long>(std::numeric_limits<int>::max())) return std::numeric_limits<int>::max();
    return static_cast<int>(v);
}

// 事件类型
enum EKind {
    EV_REFUGEES = 1,
    EV_MARKET,
    EV_SIGNAL,
    EV_LIFESUPPORT,
    EV_METEOR,
    EV_PROSPECT,
    EV_VENT,
    EV_FESTIVAL,
    EV_CARAVAN,
};

std::string num(int v) { return std::to_string(v); }

// EKind -> 协议层 ActCode 名字（供 snapshot.pending.kind 用；前端据 kind 配色/配图标）。
// 只有 EV_REFUGEES/EV_MARKET/EV_SIGNAL/EV_LIFESUPPORT 会产生待决事件，其余为自动结算，
// 这里仍全量映射以便将来扩展；未知 kind 回落到 "Unknown"。
std::string eventKindName(int kind) {
    switch (kind) {
    case EV_REFUGEES:    return actCodeName(ActCode::EventRefugees);
    case EV_MARKET:      return actCodeName(ActCode::EventMarket);
    case EV_SIGNAL:      return actCodeName(ActCode::EventSignal);
    case EV_LIFESUPPORT: return actCodeName(ActCode::EventLifeSupport);
    case EV_METEOR:      return actCodeName(ActCode::EventMeteorHit);
    case EV_PROSPECT:    return actCodeName(ActCode::EventProspectFound);
    case EV_VENT:        return actCodeName(ActCode::EventVentFound);
    case EV_FESTIVAL:    return actCodeName(ActCode::EventFestival);
    case EV_CARAVAN:     return actCodeName(ActCode::EventCaravan);
    default:             return "Unknown";
    }
}

} // namespace

// =====================================================================
//  基础查询
// =====================================================================

const Building* Game::building(int id) const {
    if (id < 0 || id >= static_cast<int>(blds_.size())) return nullptr;
    const Building& b = blds_[static_cast<size_t>(id)];
    return b.alive ? &b : nullptr;
}

int Game::workerNeed(const Building& b) const {
    int n = def(b.type).workers;
    if (hasTech(Tech::DroneNet) && n > 1) --n;
    return n;
}

int Game::adjIce(int x, int y) const {
    int c = 0;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            if (!dx && !dy) continue;
            int nx = x + dx, ny = y + dy;
            if (nx < 0 || nx >= MAP_W || ny < 0 || ny >= MAP_H) continue;
            if (tile(nx, ny).terrain == Terrain::Ice) ++c;
        }
    return c;
}

int Game::adjMountain(int x, int y) const {
    int c = 0;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            if (!dx && !dy) continue;
            int nx = x + dx, ny = y + dy;
            if (nx < 0 || nx >= MAP_W || ny < 0 || ny >= MAP_H) continue;
            if (tile(nx, ny).terrain == Terrain::Mountain) ++c;
        }
    return c;
}

int Game::countType(BType t) const {
    int c = 0;
    for (const Building& b : blds_)
        if (b.alive && b.type == t) ++c;
    return c;
}

// 只统计已经完工的建筑：在建建筑不应提前提供住房、医疗等效果（BUG B1）
int Game::countReady(BType t) const {
    int c = 0;
    for (const Building& b : blds_)
        if (b.alive && b.type == t && b.buildLeft <= 0) ++c;
    return c;
}

int Game::defense() const {
    int d = 0;
    for (const Building& b : blds_) {
        if (!b.alive || b.type != BType::Turret) continue;
        if (b.buildLeft > 0 || !b.enabled || b.damaged > 0) continue;
        if (assigned_[static_cast<size_t>(b.id)] <= 0) continue;
        d += hasTech(Tech::MilAlloy) ? TUNE.turretDefenseAlloy : TUNE.turretDefense;
    }
    d += static_cast<int>(pop_ * TUNE.militiaPerPop);
    return d;
}

// 去掉 const 并改名：本方法推进随机序列，不应伪装成“查询”（P1 修复 mutable 缺陷）。
// 注意：只做“去 const + 改名”，绝不改变 rng_ 的消耗时机与次数，
// 否则单一随机流的后续结果会整体漂移，违反“行为零变化”红线。
int Game::rollWaveStrength() {
    int s = static_cast<int>(TUNE.waveBase + turn_ * TUNE.wavePerTurn);
    s += static_cast<int>(rng_() % 7) - 3;
    return s < 5 ? 5 : s;
}

int Game::waveStrengthEstimate() const {
    // B8：先夹到 int 可表示范围再转换，否则超大 wavePerTurn 会触发 UB
    const double raw = TUNE.waveBase + static_cast<double>(turn_ + waveIn_) * TUNE.wavePerTurn;
    const double lim = static_cast<double>(std::numeric_limits<int>::max() / 4);
    const double d   = raw > lim ? lim : (raw < -lim ? -lim : raw);
    int s = static_cast<int>(d);
    return s < 5 ? 5 : s;
}

void Game::log(const std::string& s) {
    LogEntry e;
    e.turn = turn_;
    e.code = ActCode::Text;
    e.strings.push_back(s);
    log_.push_back(std::move(e));
    while (log_.size() > 400) log_.pop_front();
}

void Game::log(ActCode c, std::vector<long long> ints, std::vector<std::string> ss) {
    LogEntry e;
    e.turn = turn_;
    e.code = c;
    e.ints = std::move(ints);
    e.strings = std::move(ss);
    log_.push_back(std::move(e));
    while (log_.size() > 400) log_.pop_front();
}

// =====================================================================
//  地图生成
// =====================================================================

void Game::generateMap() {
    for (Tile& t : tiles_) t = Tile{};

    std::uniform_int_distribution<int> dx(0, MAP_W - 1), dy(0, MAP_H - 1);
    auto inHome = [](int x, int y) { return x >= 7 && x <= 9 && y >= 5 && y <= 7; };

    // 山脉
    for (int i = 0; i < 11; ++i) {
        int x = dx(rng_), y = dy(rng_);
        if (inHome(x, y)) continue;
        tiles_[static_cast<size_t>(y * MAP_W + x)].terrain = Terrain::Mountain;
    }

    // 矿脉
    int placed = 0;
    for (int guard = 0; guard < 800 && placed < 6; ++guard) {
        int x = dx(rng_), y = dy(rng_);
        if (inHome(x, y)) continue;
        Tile& t = tiles_[static_cast<size_t>(y * MAP_W + x)];
        if (t.terrain == Terrain::Ore) continue;
        t.terrain = Terrain::Ore;
        t.richness = 2 + static_cast<int>(rng_() % 3); // 2..4
        t.ore = 380 + t.richness * 160;
        ++placed;
    }

    // 地热口
    placed = 0;
    for (int guard = 0; guard < 400 && placed < 2; ++guard) {
        int x = dx(rng_), y = dy(rng_);
        if (inHome(x, y)) continue;
        Tile& t = tiles_[static_cast<size_t>(y * MAP_W + x)];
        if (t.terrain != Terrain::Plain && t.terrain != Terrain::Mountain) continue;
        t.terrain = Terrain::Geo;
        t.ore = 0; t.richness = 0;
        ++placed;
    }

    // 冰层
    placed = 0;
    for (int guard = 0; guard < 400 && placed < 6; ++guard) {
        int x = dx(rng_), y = dy(rng_);
        if (inHome(x, y)) continue;
        Tile& t = tiles_[static_cast<size_t>(y * MAP_W + x)];
        if (t.terrain != Terrain::Plain) continue;
        t.terrain = Terrain::Ice;
        ++placed;
    }

    // 着陆区：3x3 平整空地，指挥中心居中
    for (int y = 5; y <= 7; ++y)
        for (int x = 7; x <= 9; ++x) {
            Tile& t = tiles_[static_cast<size_t>(y * MAP_W + x)];
            t.terrain = Terrain::Plain;
            t.ore = 0; t.richness = 0; t.building = -1;
        }
}

// =====================================================================
//  工人与人口
// =====================================================================

void Game::recomputeWorkers() {
    assigned_.assign(blds_.size(), 0);

    // 人口上限
    int hab = countReady(BType::Hab);          // B1：在建居住舱不提供住房
    const int perHab = hasTech(Tech::Atmo) ? TUNE.housingPerHabAtmo : TUNE.housingPerHab;
    // B7b：脏参数（负的 perHab）不得让上限变负，进而把人口静默钳成负数
    housing_ = std::max(0, TUNE.baseHousing + hab * perHab);
    // 住房是"人口增长上限"而不是硬上限：难民、拆掉居住舱都可能让人口暂时超编，
    // 超编本身保留人口，但每周期扣士气并有概率流失（见 advanceTurn）。

    // 按优先级分配工人
    int free = pop_;
    auto give = [&](int id) {
        const Building* b = building(id);
        if (!b || b->buildLeft > 0 || !b->enabled || b->damaged > 0) return;
        int need = workerNeed(*b);
        int got = std::min(need, free);
        if (got < 0) got = 0;
        assigned_[static_cast<size_t>(id)] = got;
        free -= got;
    };
    for (int id : priority_) give(id);
    // 优先级表里漏掉的建筑（异常保险）
    for (const Building& b : blds_) {
        if (!b.alive) continue;
        if (assigned_[static_cast<size_t>(b.id)] == 0 && free > 0) give(b.id);
    }
}

int Game::idleWorkers() const {
    int used = 0;
    for (int a : assigned_) used += a;
    return std::max(0, pop_ - used);
}

void Game::growPopulation() {
    if (pop_ >= housing_) { popAcc_ = 0; return; }
    double rate = TUNE.popGrowthRate;
    if (hasTech(Tech::NanoMed)) rate += TUNE.nanoGrowth;
    rate += countReady(BType::Clinic) * TUNE.clinicGrowth;   // B1：在建医疗站不加速人口

    popAcc_ += rate;
    const double need = TUNE.popGrowthNeed;
    while (popAcc_ >= need && pop_ < housing_) {
        popAcc_ -= need;
        ++pop_;
        log(ActCode::PopBorn, {pop_, housing_}, {});
    }
    if (pop_ >= housing_) popAcc_ = 0;
}

// =====================================================================
//  产出结算（纯函数）
// =====================================================================

TurnReport Game::evaluate() const {
    const WeatherDef& w = WDEF[static_cast<size_t>(weather_)];
    const double moraleMul = moraleMultiplier();   // 单一事实来源，界面复用同一系数
    const double wm = w.metal * moraleMul;
    const double we = w.energy * moraleMul;
    const double wf = w.food * moraleMul;
    const double ws = w.science * moraleMul;

    const double fusion = hasTech(Tech::Fusion) ? TUNE.fusionMult : 1.0;
    const double hydro = hasTech(Tech::Hydro) ? TUNE.hydroMult : 1.0;
    const double drill = hasTech(Tech::AutoDrill) ? TUNE.drillMult : 1.0;

    auto run = [&](bool brownout) {
        TurnReport r;
        for (const Building& b : blds_) {
            if (!b.alive || b.buildLeft > 0 || !b.enabled || b.damaged > 0) continue;
            const BDef& d = def(b.type);
            int need = workerNeed(b);
            double eff = need > 0 ? std::min(1.0, assigned_[static_cast<size_t>(b.id)] / static_cast<double>(need)) : 1.0;
            if (brownout && d.upkeep > 0) eff *= 0.5;
            if (eff <= 0.0) continue;

            switch (b.type) {
            case BType::Solar:
                r.energyIn += static_cast<int>(std::lround(TUNE.solarEnergy * fusion * we * eff));
                break;
            case BType::Geo:
                r.energyIn += static_cast<int>(std::lround(TUNE.geoEnergy * fusion * we * eff));
                break;
            case BType::HQ:
                r.energyIn += static_cast<int>(std::lround(TUNE.hqEnergy * we * eff));
                r.scienceIn += static_cast<int>(std::lround(TUNE.hqScience * ws * eff));
                break;
            case BType::Mine: {
                const Tile& t = tile(b.x, b.y);
                if (t.terrain != Terrain::Ore || t.ore <= 0) break;
                double amt = t.richness * TUNE.mineMetalPerRich +
                             std::min(static_cast<double>(TUNE.mineMountainCap),
                                      adjMountain(b.x, b.y) * TUNE.mineMountainBonus);
                r.metalIn += static_cast<int>(std::lround(amt * drill * wm * eff));
                break;
            }
            case BType::Farm: {
                double amt = TUNE.farmFood +
                             std::min(static_cast<double>(TUNE.farmIceCap), adjIce(b.x, b.y) * TUNE.farmIceBonus);
                r.foodIn += static_cast<int>(std::lround(amt * hydro * wf * eff));
                break;
            }
            case BType::Lab:
                r.scienceIn += static_cast<int>(std::lround(TUNE.labScience * ws * eff));
                break;
            default:
                break;
            }
        }
        for (const Building& b : blds_) {
            if (!b.alive || b.buildLeft > 0 || !b.enabled || b.damaged > 0) continue;
            r.energyUp += def(b.type).upkeep;
        }
        r.foodUp = static_cast<int>(std::lround(pop_ * TUNE.foodPerPop * (hasTech(Tech::Atmo) ? TUNE.atmoFoodMult : 1.0)));
        return r;
    };

    TurnReport r = run(false);
    const bool brownout = (res_.energy + r.energyIn - r.energyUp) < 0;
    if (brownout) {
        r = run(true);
        r.brownout = true;
    }
    r.metalNet = r.metalIn;
    r.energyNet = r.energyIn - r.energyUp;
    r.foodNet = r.foodIn - r.foodUp;
    r.scienceNet = r.scienceIn;
    return r;
}

// =====================================================================
//  行动
// =====================================================================

// 纯规则层可建性：地形 / 占用 / 前置。**不含**资源是否够付——
// 契约 §4.6.1 要求 preview_build 的 buildable 与 affordable 相互独立，故拆成两层。
bool Game::buildableTerrain(BType t, int x, int y, std::string* why) const {
    auto fail = [&](const char* msg) { if (why) *why = msg; return false; };
    if (x < 0 || x >= MAP_W || y < 0 || y >= MAP_H) return fail("坐标超出地图范围");
    const Tile& tl = tile(x, y);
    if (tl.building >= 0) return fail("该地块已有建筑");
    if (t == BType::HQ) return fail("指挥中心只能有一座");
    if (t == BType::Gate && !hasTech(Tech::GateTheory)) return fail("尚未研究出星门理论");
    if (t == BType::Gate && countType(BType::Gate) > 0) return fail("星门已经在建或已建成");
    if (tl.terrain == Terrain::Mountain) return fail("山脉无法建造");
    if (t == BType::Mine) {
        if (tl.terrain != Terrain::Ore || tl.ore <= 0) return fail("钻矿场必须建在金属矿脉上");
    } else if (t == BType::Geo) {
        if (tl.terrain != Terrain::Geo) return fail("地热站必须建在地热口上");
    } else {
        if (tl.terrain != Terrain::Plain && tl.terrain != Terrain::Ice)
            return fail("该地形无法建造（只有平原/冰层可建）");
    }
    return true;
}

bool Game::buildable(BType t, int x, int y, std::string* why) const {
    if (!buildableTerrain(t, x, y, why)) return false;
    auto fail = [&](const char* msg) { if (why) *why = msg; return false; };
    const BDef& d = def(t);
    if (res_.metal < d.costMetal) return fail("金属不足");
    if (res_.energy < d.costEnergy) return fail("能源不足");
    if (res_.science < d.costScience) return fail("科研点不足");
    return true;
}

ActionResult Game::doBuild(const std::string& key, int x, int y) {
    // 规则上移（P2）：有事件待决时，除 answer 外的一切操作都由核心拦截。
    // 前端不再各自实现该规则（旧 CLI 的四处 if (g.hasPending()) 只是体验优化）。
    if (!pending_.empty()) return {false, ActCode::BlockedByPending, {}};
    int idx = -1;
    for (int i = 0; i < BTYPE_COUNT; ++i)
        if (key == BDEF[static_cast<size_t>(i)].key || key == BDEF[static_cast<size_t>(i)].name) idx = i;
    if (idx < 0) return {false, ActCode::BuildUnknownType, {key}};
    BType t = static_cast<BType>(idx);

    std::string why;
    if (!buildable(t, x, y, &why)) return {false, ActCode::BuildBlocked, {why}};

    const BDef& d = def(t);
    res_.metal -= d.costMetal;
    res_.energy -= d.costEnergy;
    res_.science -= d.costScience;

    Building b;
    b.id = static_cast<int>(blds_.size());
    b.type = t;
    b.x = x;
    b.y = y;
    b.buildLeft = d.buildTurns;
    blds_.push_back(b);
    tiles_[static_cast<size_t>(y * MAP_W + x)].building = b.id;
    if (t == BType::HQ) hqId_ = b.id;
    priority_.push_back(b.id);
    recomputeWorkers();

    const bool hasTurns = d.buildTurns > 0;
    log(ActCode::BuildStarted, {b.id, x, y, d.buildTurns, hasTurns ? 1 : 0}, {d.name});
    return {true, ActCode::BuildStarted,
            {d.name, num(b.id), num(x), num(y), num(d.buildTurns), hasTurns ? "1" : "0"}};
}

ActionResult Game::doDemolish(int id) {
    if (!pending_.empty()) return {false, ActCode::BlockedByPending, {}};
    const Building* bp = building(id);
    if (!bp) return {false, ActCode::DemolishInvalid, {num(id)}};
    if (bp->type == BType::HQ) return {false, ActCode::DemolishHQ, {}};
    BType t = bp->type;
    int x = bp->x, y = bp->y;
    Building& b = blds_[static_cast<size_t>(id)];
    b.alive = false;
    tiles_[static_cast<size_t>(y * MAP_W + x)].building = -1;
    priority_.erase(std::remove(priority_.begin(), priority_.end(), id), priority_.end());

    const BDef& d = def(t);
    int back = d.costMetal / 2;
    res_.metal += back;
    recomputeWorkers();
    log(ActCode::DemolishDone, {id, back}, {d.name});
    return {true, ActCode::DemolishDone, {d.name, num(id), num(back)}};
}

ActionResult Game::doToggle(int id) {
    if (!pending_.empty()) return {false, ActCode::BlockedByPending, {}};
    const Building* bp = building(id);
    if (!bp) return {false, ActCode::ToggleInvalid, {num(id)}};
    Building& b = blds_[static_cast<size_t>(id)];
    b.enabled = !b.enabled;
    recomputeWorkers();
    log(ActCode::ToggleDone, {id, b.enabled ? 1 : 0}, {def(b.type).name});
    return {true, ActCode::ToggleDone, {def(b.type).name, num(id), b.enabled ? "1" : "0"}};
}

ActionResult Game::doFocus(int id) {
    if (!pending_.empty()) return {false, ActCode::BlockedByPending, {}};
    const Building* bp = building(id);
    if (!bp) return {false, ActCode::FocusInvalid, {num(id)}};
    priority_.erase(std::remove(priority_.begin(), priority_.end(), id), priority_.end());
    priority_.insert(priority_.begin(), id);
    recomputeWorkers();
    log(ActCode::FocusDone, {id}, {def(bp->type).name});
    return {true, ActCode::FocusDone, {def(bp->type).name, num(id)}};
}

ActionResult Game::doResearch(const std::string& key) {
    if (!pending_.empty()) return {false, ActCode::BlockedByPending, {}};
    int idx = -1;
    for (int i = 0; i < TECH_COUNT; ++i)
        if (key == TDEF[static_cast<size_t>(i)].key || key == TDEF[static_cast<size_t>(i)].name) idx = i;
    if (idx < 0) return {false, ActCode::ResearchUnknown, {key}};
    Tech t = static_cast<Tech>(idx);
    const TechDef& d = TDEF[static_cast<size_t>(idx)];

    if (hasTech(t)) return {false, ActCode::ResearchDup, {d.name}};
    for (int i = 0; i < TECH_COUNT; ++i) {
        if ((d.req & techBit(static_cast<Tech>(i))) && !hasTech(static_cast<Tech>(i)))
            return {false, ActCode::ResearchPrereq, {TDEF[static_cast<size_t>(i)].name}};
    }
    if (res_.science < d.cost)
        return {false, ActCode::ResearchNoScience, {d.name, num(d.cost), num(res_.science)}};

    res_.science -= d.cost;
    techs_ |= techBit(t);
    recomputeWorkers();
    log(ActCode::ResearchDone, {}, {d.name, d.desc});
    if (t == Tech::GateTheory) log(ActCode::GateTheoryUnlocked);
    return {true, ActCode::ResearchDone, {d.name, d.desc}};
}

// =====================================================================
//  天气
// =====================================================================

void Game::rollWeather() {
    // 恶劣天气权重随周期上升
    int bad = std::min(60, 15 + turn_);
    std::uniform_int_distribution<int> d(1, 100);
    int r = d(rng_);
    Weather w = Weather::Clear;
    if (r <= bad) {
        static const Weather pool[] = {Weather::Sandstorm, Weather::ColdSnap, Weather::Flare, Weather::AcidRain};
        w = pool[rng_() % 4];
    }
    weather_ = w;
    weatherLeft_ = 2 + static_cast<int>(rng_() % 3);
    if (w != Weather::Clear)
        log(ActCode::WeatherChange, {}, {WDEF[static_cast<size_t>(w)].name, WDEF[static_cast<size_t>(w)].desc});
}

// =====================================================================
//  事件
// =====================================================================

void Game::rollEvent() {
    std::uniform_int_distribution<int> d100(1, 100);
    if (d100(rng_) > TUNE.eventChance) return;   // 默认 30% 触发概率

    std::vector<std::pair<int, int>> cand;   // kind, weight
    if (turn_ >= 4) cand.push_back({EV_REFUGEES, 16});
    cand.push_back({EV_MARKET, 14});
    if (turn_ >= 6) cand.push_back({EV_SIGNAL, 12});
    cand.push_back({EV_METEOR, 12});
    if (countReady(BType::Mine) >= 1) cand.push_back({EV_PROSPECT, 10});
    if (countReady(BType::Geo) == 0) cand.push_back({EV_VENT, 6});
    cand.push_back({EV_FESTIVAL, 10});
    if (countReady(BType::Lab) >= 1) cand.push_back({EV_CARAVAN, 12});
    cand.push_back({EV_LIFESUPPORT, 12});
    if (cand.empty()) return;

    int total = 0;
    for (auto& c : cand) total += c.second;
    int pick = static_cast<int>(rng_() % static_cast<unsigned>(total));
    int kind = cand.front().first;
    for (auto& c : cand) {
        if (pick < c.second) { kind = c.first; break; }
        pick -= c.second;
    }

    auto rnd = [&](int lo, int hi) { return lo + static_cast<int>(rng_() % static_cast<unsigned>(hi - lo + 1)); };

    switch (kind) {
    case EV_REFUGEES: {
        int n = rnd(4, 7);
        int cost = n * 8;
        PendingEvent e;
        e.kind = EV_REFUGEES; e.a = n; e.b = cost;
        e.title = "难民船请求降落";
        e.text = "一艘破旧的运输船在轨道上请求降落，船上有 " + num(n) + " 名难民。接收他们会消耗约 " + num(cost) + " 食物。";
        e.options = {"接收难民（+" + num(n) + " 人口，" + num(cost) + " 食物）",
                     "拒绝降落（士气 -5）",
                     "征用他们的补给（+60 金属，士气 -10）"};
        pending_.push_back(e);
        log(ActCode::EventRefugees);
        break;
    }
    case EV_MARKET: {
        PendingEvent e;
        e.kind = EV_MARKET;
        e.title = "黑市商人";
        e.text = "一个自称「自由商人」的家伙愿意和你做点交易。";
        e.options = {"用 80 金属换 90 科研点", "用 120 能源换 220 金属", "礼貌送客（士气 +2）"};
        pending_.push_back(e);
        log(ActCode::EventMarket);
        break;
    }
    case EV_SIGNAL: {
        PendingEvent e;
        e.kind = EV_SIGNAL;
        e.title = "神秘信号";
        e.text = "深空传来一段规律信号，似乎来自行星背面的遗迹。";
        e.options = {"派队调查（50% 获得大量科研，50% 惊动虫群）", "忽略它（士气 +1）"};
        pending_.push_back(e);
        log(ActCode::EventSignal);
        break;
    }
    case EV_LIFESUPPORT: {
        PendingEvent e;
        e.kind = EV_LIFESUPPORT; e.a = 60;
        e.title = "维生系统故障";
        e.text = "居住区的空气循环系统出现故障，修复需要 60 金属。";
        e.options = {"花 60 金属紧急修复", "先凑合着用（-3 人口，士气 -6）"};
        pending_.push_back(e);
        log(ActCode::EventLifeSupport);
        break;
    }
    case EV_METEOR: {
        int n = rnd(1, 2);
        int hits = 0;
        for (int i = 0; i < n; ++i) {
            std::vector<int> cands;
            for (const Building& b : blds_)
                if (b.alive && b.type != BType::HQ) cands.push_back(b.id);
            if (cands.empty()) break;
            int id = cands[rng_() % cands.size()];
            blds_[static_cast<size_t>(id)].damaged = 2 + static_cast<int>(rng_() % 3);
            log(ActCode::EventMeteorHit, {id}, {def(blds_[static_cast<size_t>(id)].type).name});
            ++hits;
        }
        if (!hits) log(ActCode::EventMeteorMiss);
        recomputeWorkers();
        break;
    }
    case EV_PROSPECT: {
        for (int guard = 0; guard < 200; ++guard) {
            int x = static_cast<int>(rng_() % MAP_W), y = static_cast<int>(rng_() % MAP_H);
            Tile& t = tiles_[static_cast<size_t>(y * MAP_W + x)];
            if (t.terrain != Terrain::Plain || t.building >= 0) continue;
            t.terrain = Terrain::Ore;
            t.richness = 3 + static_cast<int>(rng_() % 2);
            t.ore = 380 + t.richness * 160;
            log(ActCode::EventProspectFound, {x, y}, {});
            return;
        }
        log(ActCode::EventProspectNone);
        break;
    }
    case EV_VENT: {
        for (int guard = 0; guard < 200; ++guard) {
            int x = static_cast<int>(rng_() % MAP_W), y = static_cast<int>(rng_() % MAP_H);
            Tile& t = tiles_[static_cast<size_t>(y * MAP_W + x)];
            if (t.terrain != Terrain::Plain || t.building >= 0) continue;
            t.terrain = Terrain::Geo;
            log(ActCode::EventVentFound, {x, y}, {});
            return;
        }
        log(ActCode::EventVentNone);
        break;
    }
    case EV_FESTIVAL: {
        int f = 30 + static_cast<int>(rng_() % 21);
        res_.food += f;
        morale_ = clampInt(morale_ + 8, 0, 100);
        log(ActCode::EventFestival, {f}, {});
        break;
    }
    case EV_CARAVAN: {
        int m = 60 + static_cast<int>(rng_() % 61);
        res_.metal += m;
        log(ActCode::EventCaravan, {m}, {});
        break;
    }
    default: break;
    }
}

ActionResult Game::answer(int option) {
    if (pending_.empty()) return {false, ActCode::AnswerNone, {}};
    PendingEvent e = pending_.front();
    if (option < 1 || option > static_cast<int>(e.options.size()))
        return {false, ActCode::AnswerInvalid, {num(static_cast<int>(e.options.size()))}};
    pending_.pop_front();

    const std::string chosen = e.options[static_cast<size_t>(option - 1)];
    log(ActCode::LogChoice, {}, {chosen});

    switch (e.kind) {
    case EV_REFUGEES:
        if (option == 1) {
            if (res_.food >= e.b) {
                res_.food -= e.b;
                pop_ += e.a;
                recomputeWorkers();
                log(ActCode::RefugeesSettled, {e.a, e.b}, {});
                if (pop_ > housing_)
                    log(ActCode::OvercrowdWarn, {pop_ - housing_, housing_}, {});
            } else {
                log(ActCode::RefugeesPartial);
                res_.food = 0;
                pop_ += std::max(1, e.a / 2);
                morale_ = clampInt(morale_ - 4, 0, 100);
                recomputeWorkers();
            }
        } else if (option == 2) {
            morale_ = clampInt(morale_ - 5, 0, 100);
            log(ActCode::RefugeesRefused);
        } else {
            res_.metal += 60;
            morale_ = clampInt(morale_ - 10, 0, 100);
            log(ActCode::RefugeesSeized);
        }
        break;
    case EV_MARKET:
        if (option == 1) {
            if (res_.metal < 80) { log(ActCode::MarketNoMetal); morale_ = clampInt(morale_ - 2, 0, 100); }
            else { res_.metal -= 80; res_.science += 90; log(ActCode::MarketTradeMetal); }
        } else if (option == 2) {
            if (res_.energy < 120) { log(ActCode::MarketNoEnergy); morale_ = clampInt(morale_ - 2, 0, 100); }
            else { res_.energy -= 120; res_.metal += 220; log(ActCode::MarketTradeEnergy); }
        } else {
            morale_ = clampInt(morale_ + 2, 0, 100);
            log(ActCode::MarketLeave);
        }
        break;
    case EV_SIGNAL:
        if (option == 1) {
            if (rng_() % 2 == 0) {
                int s = 90 + static_cast<int>(rng_() % 61);
                res_.science += s;
                log(ActCode::SignalSuccess, {s}, {});
            } else {
                log(ActCode::SignalSwarm);
                applyCombat(rollWaveStrength() + 12);
            }
        } else {
            morale_ = clampInt(morale_ + 1, 0, 100);
            log(ActCode::SignalIgnore);
        }
        break;
    case EV_LIFESUPPORT:
        if (option == 1) {
            if (res_.metal >= e.a) { res_.metal -= e.a; log(ActCode::LifeSupportFixed, {e.a}, {}); }
            else { log(ActCode::LifeSupportPoor); pop_ = std::max(0, pop_ - 1); morale_ = clampInt(morale_ - 3, 0, 100); recomputeWorkers(); }
        } else {
            pop_ = std::max(0, pop_ - 3);
            morale_ = clampInt(morale_ - 6, 0, 100);
            recomputeWorkers();
            log(ActCode::LifeSupportFail);
        }
        break;
    default:
        break;
    }
    checkEnd();
    return {true, ActCode::AnswerChoice, {chosen}};
}

// =====================================================================
//  虫潮
// =====================================================================

void Game::applyCombat(int strength) {
    int defv = defense();
    log(ActCode::WaveIncoming, {strength, defv}, {});

    if (defv >= strength) {
        morale_ = clampInt(morale_ + 3, 0, 100);
        res_.metal += 15;
        log(ActCode::WaveRepelled);
        return;
    }

    int over = strength - defv;
    // BALANCE §8.2：医疗站减员必须有上限，否则任何强度的虫潮都只掉 1 人
    const int clinics = std::min(countReady(BType::Clinic), std::max(0, TUNE.clinicMitigationCap));
    const int lossDiv = std::max(1, TUNE.combatLossDiv);   // B2：脏参数除零保护
    int loss = 1 + over / lossDiv - clinics;
    if (hasTech(Tech::NanoMed) && loss > 1) --loss;
    loss = clampInt(loss, 1, std::max(1, TUNE.combatLossMax));
    pop_ = std::max(0, pop_ - loss);
    morale_ = clampInt(morale_ - TUNE.breachMoraleDrop, 0, 100);

    int dmgCount = 1 + over / std::max(1, TUNE.combatDamageDiv);
    int hit = 0;
    for (int i = 0; i < dmgCount; ++i) {
        std::vector<int> cands;
        for (const Building& b : blds_)
            if (b.alive && b.type != BType::HQ) cands.push_back(b.id);
        if (cands.empty()) break;
        int id = cands[rng_() % cands.size()];
        blds_[static_cast<size_t>(id)].damaged = 2 + static_cast<int>(rng_() % 3);
        log(ActCode::BuildingDamaged, {id}, {def(blds_[static_cast<size_t>(id)].type).name});
        ++hit;
    }
    int loot = res_.metal / 10;
    res_.metal = std::max(0, res_.metal - loot);
    log(ActCode::WaveBreached, {loss, loot, hit ? 1 : 0}, {});
    recomputeWorkers();
}

// =====================================================================
//  回合推进
// =====================================================================

void Game::advanceTurn() {
    if (over_) return;
    // 规则上移（P2）：有事件待决时不得推进周期——不消耗 rng、不改动任何状态、pending_ 保持不变。
    // answer() 是唯一被允许的操作。
    if (!pending_.empty()) return;

    log(ActCode::TurnHeader, {turn_}, {});

    // 1) 天气
    if (--weatherLeft_ <= 0) rollWeather();

    // 2) 修复受损建筑
    for (Building& b : blds_)
        if (b.alive && b.damaged > 0) {
            if (--b.damaged == 0) log(ActCode::Repaired, {b.id}, {def(b.type).name});
        }

    // 3) 生产与消耗
    report_ = evaluate();
    // B6：全部走 long long 饱和加法，避免 res_.x + net 在 int 上溢出（UB）
    res_.metal   = satAdd(res_.metal,   report_.metalNet);
    res_.energy  = satAdd(res_.energy,  report_.energyNet);
    res_.science = satAdd(res_.science, report_.scienceNet);

    if (report_.brownout) log(ActCode::Brownout);

    // 食物允许为负（那是饥荒判据），所以这里只防溢出、不夹 0
    const long long foodRaw = static_cast<long long>(res_.food) + static_cast<long long>(report_.foodNet);
    const long long iMax    = std::numeric_limits<int>::max();
    const long long iMin    = std::numeric_limits<int>::min();
    int food = static_cast<int>(foodRaw > iMax ? iMax : (foodRaw < iMin ? iMin : foodRaw));
    if (food < 0) {
        report_.starving = true;
        res_.food = 0;
        int loss = 1 + (pop_ >= 15 ? 1 : 0);
        if (hasTech(Tech::NanoMed) && loss > 1) --loss;
        loss -= std::min(countReady(BType::Clinic), std::max(0, TUNE.clinicMitigationCap));   // §8.2
        if (loss < 1) loss = 1;
        pop_ = std::max(0, pop_ - loss);
        morale_ = clampInt(morale_ - TUNE.starveMoraleDrop, 0, 100);
        log(ActCode::Starve, {loss}, {});
    } else {
        res_.food = food;
    }

    // 4) 矿脉开采与枯竭
    for (Building& b : blds_) {
        if (!b.alive || b.type != BType::Mine) continue;
        if (b.buildLeft > 0 || !b.enabled || b.damaged > 0) continue;
        if (assigned_[static_cast<size_t>(b.id)] <= 0) continue;
        Tile& t = tiles_[static_cast<size_t>(b.y * MAP_W + b.x)];
        if (t.terrain != Terrain::Ore || t.ore <= 0) continue;
        t.ore -= t.richness * MINE_DEPLETION_PER_RICH;
        if (t.ore <= 0) {
            t.ore = 0;
            t.richness = 0;
            t.terrain = Terrain::Plain;
            log(ActCode::OreDepleted, {b.id}, {});
        }
    }

    // 5) 在建工程
    for (Building& b : blds_) {
        if (!b.alive || b.buildLeft <= 0) continue;
        if (--b.buildLeft == 0) {
            log(ActCode::BuildingBuilt, {b.id}, {def(b.type).name});
            if (b.type == BType::Gate) {
                won_ = true;
                over_ = true;
                endReason_ = "星门竣工，全体殖民者成功撤离这颗星球。";
            }
        }
    }

    // 6) 人口增长、超编与士气
    growPopulation();
    if (pop_ > housing_) {
        const int over = pop_ - housing_;
        morale_ = clampInt(morale_ - 1 - over / 5, 0, 100);
        const int chance = std::min(90, TUNE.overcrowdRisk * over);
        if (static_cast<int>(rng_() % 100) < chance) {
            --pop_;
            log(ActCode::OvercrowdLeft, {over}, {});
        }
        recomputeWorkers();
    }
    if (morale_ < TUNE.moraleTarget) morale_ = clampInt(morale_ + 1, 0, 100);
    else if (morale_ > TUNE.moraleTarget) morale_ = clampInt(morale_ - 1, 0, 100);
    recomputeWorkers();

    checkEnd();
    // 结局说明结构化为 GameOver（渲染仍是 endReason_ 原文，与改造前逐字一致），
    // 使前端能用 code 识别结局那条日志，而不是退化成 Text 兜底。
    if (over_) { log(ActCode::GameOver, {}, {endReason_}); return; }

    // 7) 随机事件
    rollEvent();

    // 8) 酸雨腐蚀
    if (weather_ == Weather::AcidRain && rng_() % 100 < 25) {
        std::vector<int> cands;
        for (const Building& b : blds_)
            if (b.alive && b.type != BType::HQ && b.damaged == 0 && b.buildLeft == 0) cands.push_back(b.id);
        if (!cands.empty()) {
            int id = cands[rng_() % cands.size()];
            blds_[static_cast<size_t>(id)].damaged = 1 + static_cast<int>(rng_() % 3);
            log(ActCode::AcidRain, {id}, {def(blds_[static_cast<size_t>(id)].type).name});
            recomputeWorkers();
        }
    }

    // 9) 虫潮
    if (waveIn_ > 0) --waveIn_;
    if (waveIn_ == 0) {
        applyCombat(rollWaveStrength());
        waveIn_ = std::max(6, TUNE.waveInterval - turn_ / 40);
    }

    // 10) 周期推进
    ++turn_;
    checkEnd();
}

void Game::checkEnd() {
    if (over_) return;
    if (pop_ <= 0) {
        over_ = true;
        won_ = false;
        endReason_ = "殖民地人口归零，最后一名殖民者在寂静中停止了呼吸。";
    } else if (turn_ > TUNE.maxTurns) {
        over_ = true;
        won_ = false;
        endReason_ = "补给窗口关闭，殖民地与母星彻底失联。";
    }
}

// =====================================================================
//  存档
// =====================================================================

namespace {
std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
} // namespace

std::string Game::saveTo(const std::string& path) const {
    std::ofstream f(path);
    if (!f) return "无法写入文件：" + path;
    f << "STARCOLONY 1\n";
    f << "schema_version 1\n";   // P1 追加段：读档端对未知段静默忽略
    f << "content_version 1\n";  // P1 追加段：内容版本（建筑/科技/事件外置后用）
    f << "name " << name_ << "\n";
    f << "state " << turn_ << ' ' << pop_ << ' ' << morale_ << ' ' << std::setprecision(17) << popAcc_
      << std::setprecision(6) << ' ' << techs_ << ' '
      << static_cast<int>(weather_) << ' ' << weatherLeft_ << ' ' << waveIn_ << ' '
      << over_ << ' ' << won_ << "\n";
    f << "res " << res_.metal << ' ' << res_.energy << ' ' << res_.food << ' ' << res_.science << "\n";
    f << "report " << report_.metalIn << ' ' << report_.energyIn << ' ' << report_.foodIn << ' ' << report_.scienceIn
      << ' ' << report_.energyUp << ' ' << report_.foodUp << ' ' << report_.metalNet << ' ' << report_.energyNet
      << ' ' << report_.foodNet << ' ' << report_.scienceNet << ' ' << report_.brownout << ' ' << report_.starving << "\n";
    {
        std::ostringstream ss;
        ss << rng_;
        f << "rng " << ss.str() << "\n";
    }
    f << "priority " << priority_.size();
    for (int id : priority_) f << ' ' << id;
    f << "\n";
    f << "log " << log_.size() << "\n";
    for (const LogEntry& e : log_) f << e.text() << "\n";
    f << "pending " << pending_.size() << "\n";
    for (const PendingEvent& p : pending_) {
        f << p.kind << ' ' << p.a << ' ' << p.b << ' ' << p.c << ' ' << p.options.size() << "\n";
        f << p.title << "\n" << p.text << "\n";
        for (const std::string& o : p.options) f << o << "\n";
    }
    f << "map\n";
    for (const Tile& t : tiles_)
        f << static_cast<char>(t.terrain) << ' ' << t.ore << ' ' << t.richness << ' ' << t.building << "\n";
    f << "buildings " << blds_.size() << "\n";
    for (const Building& b : blds_)
        f << b.id << ' ' << static_cast<int>(b.type) << ' ' << b.x << ' ' << b.y << ' '
          << b.buildLeft << ' ' << b.damaged << ' ' << b.enabled << ' ' << b.alive << "\n";
    f << "end\n";
    return "已保存到 " + path;
}

bool Game::readFile(const std::string& path, std::string& err) {
    std::ifstream f(path);
    if (!f) { err = "找不到文件 " + path; return false; }

    std::string header;
    int version = 0;
    f >> header >> version;
    if (header != "STARCOLONY") { err = "不是本游戏的存档"; return false; }
    if (version != 1) { err = "存档版本不支持：" + std::to_string(version); return false; }
    bool haveReport = false;
    bool haveState  = false;   // B5：res/state 是必需段，缺失必须判为损坏而不是静默用默认值
    bool haveRes    = false;
    bool bad = false;          // 任一字段解析失败即整体判为损坏，绝不半信半疑地载入
    auto badLine = [&](const char* what) { if (!bad) err = std::string("存档损坏：") + what; bad = true; };

    std::string line;
    std::getline(f, line);   // 吃掉行尾

    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string tag;
        ss >> tag;
        if (tag.empty()) continue;

        if (tag == "name") {
            std::string rest;
            std::getline(ss, rest);
            name_ = trim(rest);
            if (name_.empty()) name_ = "新曙光";
        } else if (tag == "state") {
            int w = 0;
            ss >> turn_ >> pop_ >> morale_ >> popAcc_ >> techs_ >> w >> weatherLeft_ >> waveIn_ >> over_ >> won_;
            if (!ss) badLine("state 字段");
            if (w < 0 || w >= WEATHER_COUNT) { badLine("天气编号越界"); w = 0; }
            // B3a/B3b：值域校验。负人口 / 士气越界会让 evaluate() 与 UI 进入荒谬状态
            if (pop_ < 0)          badLine("人口为负");
            if (morale_ < 0 || morale_ > 100) badLine("士气越界（应为 0..100）");
            if (turn_ < 1)         badLine("周期号非法");
            if (techs_ & ~static_cast<uint32_t>((1u << TECH_COUNT) - 1u)) badLine("存在未知科技位");
            if (popAcc_ < 0)       badLine("人口累积进度为负");
            if (waveIn_ < 0)       badLine("虫潮倒计时为负");
            if (weatherLeft_ < 0)  badLine("天气剩余周期为负");
            weather_ = static_cast<Weather>(w);
            haveState = true;
        } else if (tag == "res") {
            ss >> res_.metal >> res_.energy >> res_.food >> res_.science;
            if (!ss || res_.metal < 0 || res_.energy < 0 || res_.food < 0 || res_.science < 0)
                badLine("资源字段非法");
            haveRes = true;
        } else if (tag == "report") {
            ss >> report_.metalIn >> report_.energyIn >> report_.foodIn >> report_.scienceIn
               >> report_.energyUp >> report_.foodUp >> report_.metalNet >> report_.energyNet
               >> report_.foodNet >> report_.scienceNet >> report_.brownout >> report_.starving;
            if (!ss) badLine("report 字段");   // B5c：解析失败不能算"读到了报告"
            else     haveReport = true;
        } else if (tag == "rng") {
            std::string rest;
            std::getline(ss, rest);
            std::istringstream is(rest);
            is >> rng_;
            if (!is) rng_.seed(20240101u);
        } else if (tag == "priority") {
            size_t n = 0;
            ss >> n;
            if (!ss || n > 100000) badLine("priority 计数非法");
            priority_.clear();
            for (size_t i = 0; i < n && !bad; ++i) {
                int id = -1;
                if (!(ss >> id)) badLine("priority 内容缺失");
                priority_.push_back(id);
            }
        } else if (tag == "log") {
            size_t n = 0;
            ss >> n;
            if (!ss || n > 100000) badLine("log 计数非法");
            log_.clear();
            for (size_t i = 0; i < n && !bad; ++i) {
                std::string m;
                if (!std::getline(f, m)) { badLine("log 内容缺失"); break; }
                LogEntry le;
                le.turn = turn_;
                le.code = ActCode::Text;   // 存档只保存渲染后的文本，读回即视为原始文本条目
                le.strings.push_back(trim(m));
                log_.push_back(std::move(le));
            }
        } else if (tag == "pending") {
            size_t n = 0;
            ss >> n;
            if (!ss || n > 10000) badLine("pending 计数非法");
            pending_.clear();
            for (size_t i = 0; i < n && !bad; ++i) {
                PendingEvent p;
                size_t nopt = 0;
                std::string l;
                if (!std::getline(f, l)) { badLine("pending 内容缺失"); break; }
                std::istringstream hs(l);
                if (!(hs >> p.kind >> p.a >> p.b >> p.c >> nopt)) { badLine("pending 头部非法"); break; }
                if (nopt > 1000) { badLine("pending 选项过多"); break; }
                // B5d：事件字段值域。a/b/c 全都是数量或花费，负值会把人口/资源打穿
                if (p.kind < EV_REFUGEES || p.kind > EV_CARAVAN) { badLine("事件类型非法"); break; }
                if (p.a < 0 || p.b < 0 || p.c < 0) { badLine("事件参数为负"); break; }
                if (p.a > 100000 || p.b > 100000 || p.c > 100000) { badLine("事件参数过大"); break; }
                if (!std::getline(f, p.title) || !std::getline(f, p.text)) { badLine("pending 文本缺失"); break; }
                for (size_t k = 0; k < nopt; ++k) {
                    std::string o;
                    if (!std::getline(f, o)) { badLine("pending 选项缺失"); break; }
                    p.options.push_back(o);
                }
                pending_.push_back(p);
            }
            if (!pending_.empty() && pending_.front().options.empty()) { pending_.clear(); badLine("事件没有选项"); }
        } else if (tag == "map") {
            for (Tile& t : tiles_) {
                if (bad) break;
                std::string l;
                if (!std::getline(f, l)) { badLine("地图数据缺失"); break; }
                std::istringstream ts(l);
                char c = '.';
                if (!(ts >> c >> t.ore >> t.richness >> t.building)) { badLine("地图行非法"); break; }
                switch (c) {
                case '.': case '*': case '~': case '|': case '^': break;
                default: badLine("未知地形字符"); break;
                }
                t.terrain = static_cast<Terrain>(c);
            }
        } else if (tag == "buildings") {
            size_t n = 0;
            ss >> n;
            if (!ss || n > 100000) badLine("buildings 计数非法");
            blds_.clear();
            for (size_t i = 0; i < n && !bad; ++i) {
                std::string l;
                std::getline(f, l);
                std::istringstream bs(l);
                Building b;
                int type = 0;
                if (!(bs >> b.id >> type >> b.x >> b.y >> b.buildLeft >> b.damaged >> b.enabled >> b.alive)) {
                    badLine("建筑行非法");
                    break;
                }
                if (type < 0 || type >= BTYPE_COUNT) { badLine("建筑类型越界"); break; }
                if (b.id != static_cast<int>(i)) { badLine("建筑编号与顺序不一致"); break; }
                if (b.x < 0 || b.x >= MAP_W || b.y < 0 || b.y >= MAP_H) { badLine("建筑坐标越界"); break; }
                if (b.buildLeft < 0 || b.damaged < 0) { badLine("建筑计时为负"); break; }
                b.type = static_cast<BType>(type);
                blds_.push_back(b);
            }
        } else if (tag == "end") {
            break;
        }
    }

    if (bad) return false;
    // B5a/B5b：缺 res 或 state 的存档一律拒绝，不能让资源/回合静默归零
    if (!haveRes)   { err = "存档损坏：缺少 res 段"; return false; }
    if (!haveState) { err = "存档损坏：缺少 state 段"; return false; }

    hqId_ = -1;
    int hqCount = 0;
    for (const Building& b : blds_) {
        if (!b.alive) continue;
        if (b.type == BType::HQ) { hqId_ = b.id; ++hqCount; }
        // 地块与建筑必须互相引用一致，否则世界是坏的
        const Tile& tl = tiles_[static_cast<size_t>(b.y * MAP_W + b.x)];
        if (tl.building != b.id) { err = "存档损坏：建筑与地块引用不一致"; return false; }
    }
    if (blds_.empty() || hqId_ < 0) { err = "存档缺少指挥中心"; return false; }
    if (hqCount > 1) { err = "存档损坏：存在多座指挥中心"; return false; }
    for (int y = 0; y < MAP_H; ++y) {
        for (int x = 0; x < MAP_W; ++x) {
            const Tile& t = tiles_[static_cast<size_t>(y * MAP_W + x)];
            if (t.building < 0) continue;
            if (t.building >= static_cast<int>(blds_.size())) {
                err = "存档损坏：地块引用了不存在的建筑";
                return false;
            }
            // B4：反向一致性。只校验"建筑->地块"会让两块地同时指向同一栋建筑蒙混过关
            const Building& b = blds_[static_cast<size_t>(t.building)];
            if (b.x != x || b.y != y) {
                err = "存档损坏：地块与建筑坐标不一致（同一建筑被多块地引用）";
                return false;
            }
        }
    }
    for (const Tile& t : tiles_) {
        if (t.building >= 0 && t.building >= static_cast<int>(blds_.size())) {
            err = "存档损坏：地块引用了不存在的建筑";
            return false;
        }
        if (t.building >= 0 && !blds_[static_cast<size_t>(t.building)].alive) {
            err = "存档损坏：地块引用了已拆除的建筑";
            return false;
        }
        if (t.ore < 0 || t.richness < 0) { err = "存档损坏：矿量/丰度为负"; return false; }
    }

    recomputeWorkers();
    if (!haveReport) report_ = evaluate();
    return true;
}

std::string Game::loadFrom(const std::string& path) {
    Game tmp;
    tmp.tiles_.fill(Tile{});
    std::string err;
    if (!tmp.readFile(path, err)) return "读取失败：" + err;
    *this = std::move(tmp);
    return "已从存档 " + path + " 恢复";
}

// =====================================================================
//  全值快照（供前端 / RPC；const 且不消耗随机数）
// =====================================================================

GameSnapshot Game::snapshot() const {
    GameSnapshot s;
    s.turn = turn_;
    s.metal = res_.metal;
    s.energy = res_.energy;
    s.food = res_.food;
    s.science = res_.science;
    s.pop = pop_;
    s.housing = housing_;
    s.morale = morale_;
    s.weather = static_cast<int>(weather_);
    s.weatherLeft = weatherLeft_;
    s.waveIn = waveIn_;
    s.waveStrengthEstimate = waveStrengthEstimate();   // 纯公式，不碰 rng_
    s.defense = defense();
    s.metalIn = report_.metalIn;
    s.energyIn = report_.energyIn;
    s.foodIn = report_.foodIn;
    s.scienceIn = report_.scienceIn;
    s.energyUp = report_.energyUp;
    s.foodUp = report_.foodUp;
    s.metalNet = report_.metalNet;
    s.energyNet = report_.energyNet;
    s.foodNet = report_.foodNet;
    s.scienceNet = report_.scienceNet;
    s.brownout = report_.brownout;
    s.starving = report_.starving;
    s.over = over_;
    s.won = won_;
    s.endReason = endReason_;
    s.colonyName = name_;

    s.buildings.reserve(blds_.size());
    for (const Building& b : blds_) {
        BuildingView v;
        v.id = b.id;
        v.type = static_cast<int>(b.type);
        v.typeKey = BDEF[static_cast<size_t>(b.type)].key;
        v.x = b.x;
        v.y = b.y;
        v.buildLeft = b.buildLeft;
        v.damaged = b.damaged;
        v.enabled = b.enabled;
        v.alive = b.alive;
        v.assigned = (b.id >= 0 && b.id < static_cast<int>(assigned_.size()))
                         ? assigned_[static_cast<size_t>(b.id)] : 0;
        v.workerNeed = workerNeed(b);
        s.buildings.push_back(v);
    }

    s.tiles.reserve(static_cast<size_t>(MAP_W) * MAP_H);
    for (const Tile& t : tiles_) {
        TileView v;
        v.terrain = static_cast<int>(t.terrain);
        v.ore = t.ore;
        v.richness = t.richness;
        v.building = t.building;
        s.tiles.push_back(v);
    }

    s.log.assign(log_.begin(), log_.end());

    for (int i = 0; i < TECH_COUNT; ++i)
        if (hasTech(static_cast<Tech>(i))) s.techs.push_back(TDEF[static_cast<size_t>(i)].key);

    s.assigned = assigned_;

    // 待决事件（P2 §4.4.1）：空队列安全，但仍先判 hasPending
    s.hasPending = !pending_.empty();
    if (s.hasPending) {
        const PendingEvent& p = pending_.front();
        s.pending.kind = eventKindName(p.kind);
        s.pending.title = p.title;
        s.pending.text = p.text;
        s.pending.options = p.options;
    }

    // ---- P2.1 新增（纯追加）----
    s.idleWorkers = idleWorkers();          // 纯查询，不耗 rng
    s.seed        = static_cast<int>(seed_);   // 0 = 未知（读档后）
    return s;
}

// =====================================================================
//  新游戏
// =====================================================================

void Game::newGame(uint32_t seed, const std::string& colonyName) {
    name_ = colonyName.empty() ? "新曙光" : colonyName;
    rng_.seed(seed);
    seed_ = seed;   // 记录本局种子（仅运行时，不入存档）

    blds_.clear();
    priority_.clear();
    assigned_.clear();
    log_.clear();
    pending_.clear();
    tiles_.fill(Tile{});

    res_ = Resources{};
    res_.metal = TUNE.startMetal;
    res_.energy = TUNE.startEnergy;
    res_.food = TUNE.startFood;
    res_.science = 0;

    // B7a：脏 TUNE 参数不得把新开局带进非法状态
    pop_     = std::max(0, TUNE.startPop);
    housing_ = std::max(0, TUNE.baseHousing);
    morale_  = clampInt(TUNE.moraleTarget, 0, 100);
    turn_ = 1;
    popAcc_ = 0.0;
    techs_ = 0;
    weather_ = Weather::Clear;
    weatherLeft_ = 3;
    waveIn_ = TUNE.waveFirst;
    hqId_ = -1;
    over_ = false;
    won_ = false;
    endReason_.clear();
    report_ = TurnReport{};

    generateMap();

    Building hq;
    hq.id = 0;
    hq.type = BType::HQ;
    hq.x = 8;
    hq.y = 6;
    blds_.push_back(hq);
    tiles_[static_cast<size_t>(6 * MAP_W + 8)].building = 0;
    hqId_ = 0;
    priority_.push_back(0);

    recomputeWorkers();
    report_ = evaluate();

    log(ActCode::NewGameText, {}, {name_});
    log(ActCode::GoalText, {TUNE.maxTurns}, {});
    log(ActCode::HintText);
}

} // namespace sc
