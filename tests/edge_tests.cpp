// 星际争霸：殖民地 (Star Colony) —— C++20 命令行殖民地经营游戏
// Copyright (C) 2026 Star Colony contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published
// by the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// See the LICENSE file at the project root for the full license text.
// SPDX-License-Identifier: AGPL-3.0-or-later

// 星际争霸：殖民地 —— 对抗性边界测试（QA / task-2）
// 目标：用恶意输入与极端参数找出真实 bug，而不是复述 tests/selftest.cpp。
//
// 编译（不要跑 make，避免与 Lead 的构建竞争）：
//   g++ -std=c++20 -O2 -Isrc tests/edge_tests.cpp src/game.cpp -o /tmp/edge_tests
//   g++ -std=c++20 -g -fsanitize=address,undefined -Isrc tests/edge_tests.cpp src/game.cpp -o /tmp/edge_asan
//
// 所有会触发 ASan/UBSan/信号的用例都在 fork 出的子进程里执行，并把子进程 stderr
// 重定向到 /dev/null，因此主进程自身不会产生 sanitizer 报告；子进程是否崩溃/逻辑失败
// 由父进程通过 waitpid 状态判定。main() 返回非 0 表示发现失败/缺陷。
#include "game.hpp"
#include "ai.hpp"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace sc;

// =====================================================================
//  基础设施
// =====================================================================

static int g_checks = 0;
static int g_fail   = 0;
static int g_bugs   = 0;

static void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_fail;
        std::printf("  [FAIL] %s\n", what.c_str());
    }
}

static void bug(const std::string& id, const std::string& title, const std::string& detail) {
    ++g_bugs;
    ++g_fail;
    std::printf("  [BUG %s] %s\n        %s\n", id.c_str(), title.c_str(), detail.c_str());
}

// ---------------- 文件 ----------------
static std::string readAll(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
static void writeAll(const std::string& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary);
    f << s;
}

// ---------------- fork 隔离的用例 ----------------
struct ChildResult {
    int status = 0;
    bool signaled() const { return WIFSIGNALED(status); }
    int  signal() const { return WTERMSIG(status); }
    int  code() const { return WIFEXITED(status) ? WEXITSTATUS(status) : -1; }
};

static ChildResult runChild(const std::function<int()>& fn) {
    std::fflush(nullptr);
    pid_t p = fork();
    if (p == 0) {
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, 2); if (dn != 2) close(dn); }
        int r = 0;
        try { r = fn(); } catch (...) { r = 99; }
        _exit(r);
    }
    int st = 0;
    waitpid(p, &st, 0);
    ChildResult cr;
    cr.status = st;
    return cr;
}

// ---------------- 状态快照 ----------------
struct Snap {
    int turn = 0, pop = 0, morale = 0, housing = 0;
    int metal = 0, energy = 0, food = 0, science = 0;
    uint32_t techs = 0;
    size_t nb = 0, nlog = 0;
    bool over = false, won = false;
    std::string name;
};

static Snap snap(const Game& g) {
    Snap s;
    s.turn = g.turn(); s.pop = g.pop(); s.morale = g.morale(); s.housing = g.housing();
    s.metal = g.res().metal; s.energy = g.res().energy; s.food = g.res().food; s.science = g.res().science;
    s.techs = g.techs(); s.nb = g.buildings().size(); s.nlog = g.log().size();
    s.over = g.over(); s.won = g.won(); s.name = g.colonyName();
    return s;
}
static bool sameSnap(const Snap& a, const Snap& b) {
    return a.turn == b.turn && a.pop == b.pop && a.morale == b.morale && a.housing == b.housing &&
           a.metal == b.metal && a.energy == b.energy && a.food == b.food && a.science == b.science &&
           a.techs == b.techs && a.nb == b.nb && a.nlog == b.nlog && a.over == b.over && a.won == b.won &&
           a.name == b.name;
}

// ---------------- 结构不变量 ----------------
static void checkInvariants(const Game& g, const std::string& tag) {
    const Resources& r = g.res();
    check(r.metal >= 0 && r.energy >= 0 && r.food >= 0 && r.science >= 0, tag + " 资源非负");
    check(g.pop() >= 0, tag + " 人口非负");
    check(g.morale() >= 0 && g.morale() <= 100, tag + " 士气 0..100");
    check(g.turn() >= 1, tag + " 周期 >=1");
    check(g.idleWorkers() >= 0 && g.idleWorkers() <= g.pop(), tag + " 闲置工人 0..pop");
    check(g.assigned().size() == g.buildings().size(), tag + " 工人分配表长度与建筑数一致");

    int used = 0;
    for (int a : g.assigned()) { check(a >= 0, tag + " 分配数非负"); used += a; }
    check(used <= g.pop(), tag + " 已分配工人之和 <= 人口");

    for (const Building& b : g.buildings()) {
        if (!b.alive) continue;
        check(b.x >= 0 && b.x < MAP_W && b.y >= 0 && b.y < MAP_H, tag + " 建筑坐标合法");
        check(b.buildLeft >= 0 && b.damaged >= 0, tag + " 建筑计时非负");
        if (b.x >= 0 && b.x < MAP_W && b.y >= 0 && b.y < MAP_H)
            check(g.tile(b.x, b.y).building == b.id, tag + " 地块->建筑 引用一致 #" + std::to_string(b.id));
        if (b.id < static_cast<int>(g.assigned().size()))
            check(g.assigned()[static_cast<size_t>(b.id)] <= g.workerNeed(b) || !b.enabled,
                  tag + " 分配不超过需求 #" + std::to_string(b.id));
    }
    for (int y = 0; y < MAP_H; ++y)
        for (int x = 0; x < MAP_W; ++x) {
            const Tile& t = g.tile(x, y);
            if (t.building >= 0) {
                const Building* b = g.building(t.building);
                check(b != nullptr, tag + " 地块引用的建筑存在");
                if (b) check(b->x == x && b->y == y, tag + " 地块引用的建筑坐标正确");
            }
            if (t.terrain == Terrain::Ore) check(t.ore >= 0 && t.richness >= 0, tag + " 矿脉数值非负");
        }
}

// =====================================================================
//  存档文本构造与篡改
// =====================================================================

static const char* kSave = "/tmp/qa_edge.sav";

static std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> v;
    std::string cur;
    for (char c : s) {
        if (c == '\n') { v.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    if (!cur.empty()) v.push_back(cur);
    return v;
}
static std::string joinLines(const std::vector<std::string>& v) {
    std::string s;
    for (const std::string& l : v) { s += l; s += '\n'; }
    return s;
}

// 一份最小的、结构完全合法的存档：HQ #0 位于 (8,6)
static std::string baseSave() {
    std::ostringstream o;
    o << "STARCOLONY 1\n";
    o << "name QA\n";
    o << "state 1 6 70 0 0 0 3 9 0 0\n";
    o << "res 240 90 70 0\n";
    o << "report 4 0 1 0 2 6 0 -2 -5 1 0 0\n";
    o << "priority 1 0\n";
    o << "log 1\nhello\n";
    o << "pending 0\n";
    o << "map\n";
    for (int y = 0; y < MAP_H; ++y)
        for (int x = 0; x < MAP_W; ++x)
            o << ". 0 0 " << ((x == 8 && y == 6) ? 0 : -1) << "\n";
    o << "buildings 1\n";
    o << "0 0 8 6 0 0 1 1\n";
    o << "end\n";
    return o.str();
}

static std::string replaceLine(const std::string& s, const std::string& prefix, const std::string& nl) {
    std::vector<std::string> v = splitLines(s);
    for (std::string& l : v)
        if (l.rfind(prefix, 0) == 0) { l = nl; break; }
    return joinLines(v);
}
static std::string dropLine(const std::string& s, const std::string& prefix) {
    std::vector<std::string> v = splitLines(s), out;
    for (const std::string& l : v)
        if (l.rfind(prefix, 0) == 0) continue;
        else out.push_back(l);
    return joinLines(out);
}
static std::string setTileRef(const std::string& s, int x, int y, int bid) {
    std::vector<std::string> v = splitLines(s);
    int mapIdx = -1;
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i] == "map") { mapIdx = static_cast<int>(i); break; }
    if (mapIdx < 0) return s;
    int idx = mapIdx + 1 + y * MAP_W + x;
    if (idx >= 0 && idx < static_cast<int>(v.size()))
        v[static_cast<size_t>(idx)] = ". 0 0 " + std::to_string(bid);
    return joinLines(v);
}
static std::string setBuildings(const std::string& s, const std::vector<std::string>& rows) {
    std::vector<std::string> v = splitLines(s), out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i].rfind("buildings ", 0) == 0) {
            size_t old = 0;
            std::istringstream(v[i].substr(10)) >> old;
            out.push_back("buildings " + std::to_string(rows.size()));
            for (const std::string& r : rows) out.push_back(r);
            i += old;  // 跳过旧数据行
        } else {
            out.push_back(v[i]);
        }
    }
    return joinLines(out);
}

// =====================================================================
//  [1] 读档失败必须保留原状态 + 现有严格校验
// =====================================================================
static void testLoadRejectPreserves() {
    std::printf("[1] 读档失败必须保留原状态（严格校验路径）\n");
    Game g;
    g.newGame(20240607u, "保留状态");
    for (int i = 0; i < 6; ++i) g.advanceTurn();
    const Snap before = snap(g);
    const std::string base = baseSave();

    auto expectReject = [&](const std::string& content, const std::string& tag) {
        writeAll(kSave, content);
        Snap b = snap(g);
        std::string r = g.loadFrom(kSave);
        bool rejected = r.find("读取失败") != std::string::npos;
        check(rejected, tag + " 应被拒绝（实际返回：" + r + "）");
        check(sameSnap(before, snap(g)), tag + " 失败后原状态必须逐字段不变");
        if (!sameSnap(b, snap(g))) bug("B0", tag + " 破坏了原状态", "loadFrom 失败后 *this 被修改");
    };

    expectReject("", "空文件");
    expectReject("not-a-save\n", "魔数错误");
    expectReject("STARCOLONY 1\n", "只有文件头（截断）");
    expectReject("STARCOLONY 2\nstate 1 6 70 0 0 0 3 9 0 0\n", "版本号不支持");
    expectReject(dropLine(base, "buildings "), "缺少 buildings 段");
    expectReject(replaceLine(base, "0 0 8 6 0 0 1 1", "0 99 8 6 0 0 1 1"), "建筑类型越界");
    expectReject(replaceLine(base, "0 0 8 6 0 0 1 1", "0 0 99 6 0 0 1 1"), "建筑坐标越界");
    expectReject(replaceLine(base, "0 0 8 6 0 0 1 1", "1 0 8 6 0 0 1 1"), "建筑 id 与顺序不一致");
    expectReject(setTileRef(base, 8, 6, -1), "建筑未被地块引用");
    expectReject(replaceLine(base, "res ", "res -1 90 70 0"), "负数资源");
    expectReject(replaceLine(base, "state ", "state 1 6 70 0 0 99 3 9 0 0"), "天气编号越界");
    expectReject(replaceLine(base, "buildings 1", "buildings 2"), "buildings 计数与数据不一致");
    // 未知地形字符：改一条地图行
    {
        std::vector<std::string> v = splitLines(base);
        int mapIdx = -1;
        for (size_t i = 0; i < v.size(); ++i) if (v[i] == "map") { mapIdx = static_cast<int>(i); break; }
        v[static_cast<size_t>(mapIdx + 1 + 5 * MAP_W + 5)] = "Z 0 0 -1";
        expectReject(joinLines(v), "未知地形字符");
    }
    // 两座指挥中心（地块引用都正确）
    {
        std::string dup = setTileRef(base, 7, 5, 1);
        dup = setBuildings(dup, {"0 0 8 6 0 0 1 1", "1 0 7 5 0 0 1 1"});
        expectReject(dup, "存在两座指挥中心");
    }
    // 正常存档仍能读入（防止校验把合法存档误杀）
    writeAll(kSave, base);
    Game h;
    std::string r = h.loadFrom(kSave);
    check(r.find("已从存档") != std::string::npos, "合法最小存档可读入（实际：" + r + "）");
    if (r.find("已从存档") != std::string::npos) checkInvariants(h, "minimal");
    std::printf("    完成\n\n");
}

// =====================================================================
//  [2] 静默接受的坏存档（校验缺口）—— 这里的目标是找出应拒未拒
// =====================================================================
static void testLoadValidationGaps() {
    std::printf("[2] 坏存档是否被静默接受（校验缺口）\n");
    const std::string base = baseSave();

    // a) state 字段值域未校验：负人口
    {
        writeAll(kSave, replaceLine(base, "state ", "state 1 -5 70 0 0 0 3 9 0 0"));
        Game h;
        std::string r = h.loadFrom(kSave);
        bool accepted = r.find("已从存档") != std::string::npos;
        check(!accepted, "负人口存档应被拒绝（实际：" + r + "）");
        if (accepted) bug("B3a", "state 未校验值域：负人口被接受",
                          "state 行 pop=-5 仍读档成功，pop()=" + std::to_string(h.pop()) +
                          "；根因 src/game.cpp readFile 的 state 分支只做 !ss 检查，无值域检查");
    }
    // b) state 字段值域未校验：士气 > 100
    {
        writeAll(kSave, replaceLine(base, "state ", "state 1 6 500 0 0 0 3 9 0 0"));
        Game h;
        std::string r = h.loadFrom(kSave);
        bool accepted = r.find("已从存档") != std::string::npos;
        check(!accepted, "士气超界存档应被拒绝（实际：" + r + "）");
        if (accepted) bug("B3b", "state 未校验值域：士气 500 被接受",
                          "morale=500 读档成功后 morale()=" + std::to_string(h.morale()) +
                          "，evaluate() 会把 moraleMul 放大到 3.25，破坏平衡与不变量");
    }
    // c) 地图地块重复引用同一建筑（位置不一致）
    {
        writeAll(kSave, setTileRef(base, 2, 2, 0));
        Game h;
        std::string r = h.loadFrom(kSave);
        bool accepted = r.find("已从存档") != std::string::npos;
        if (accepted) {
            bool bad = !(h.tile(2, 2).building == 0 && h.building(0)->x == 8 && h.building(0)->y == 6);
            check(!bad, "地块重复引用同一建筑应被拒绝");
            bug("B4", "地图地块->建筑引用只校验范围，未校验位置唯一性",
                "格 (2,2) 与 (8,6) 同时 building=0 仍读档成功；"
                "readFile 末尾只检查 t.building < size 且 alive，未验证 blds_[id].x/y 与 (x,y) 一致");
        } else {
            check(true, "地块重复引用同一建筑被拒绝");
        }
    }
    // d) 缺失 res 段 -> 静默变成全 0
    {
        writeAll(kSave, dropLine(base, "res "));
        Game h;
        std::string r = h.loadFrom(kSave);
        bool accepted = r.find("已从存档") != std::string::npos;
        check(!accepted, "缺少 res 段应被拒绝（实际：" + r + "）");
        if (accepted)
            bug("B5a", "缺失 res 段被静默接受，资源直接变成 0",
                "金属=" + std::to_string(h.res().metal) + " 能源=" + std::to_string(h.res().energy) +
                " 食物=" + std::to_string(h.res().food) +
                "；readFile 不要求必需 tag，缺失即沿用临时对象默认值");
    }
    // e) 缺失 state 段 -> 静默沿用默认（turn=1,pop=6,...）
    {
        writeAll(kSave, dropLine(base, "state "));
        Game h;
        std::string r = h.loadFrom(kSave);
        bool accepted = r.find("已从存档") != std::string::npos;
        check(!accepted, "缺少 state 段应被拒绝（实际：" + r + "）");
        if (accepted)
            bug("B5b", "缺失 state 段被静默接受，回合/人口/科技全用默认值",
                "turn=" + std::to_string(h.turn()) + " pop=" + std::to_string(h.pop()) +
                " techs=" + std::to_string(h.techs()));
    }
    // f) report 行语法坏掉却仍标记 haveReport
    {
        writeAll(kSave, replaceLine(base, "report ", "report xxx yyy"));
        Game h;
        std::string r = h.loadFrom(kSave);
        bool accepted = r.find("已从存档") != std::string::npos;
        check(!accepted, "非法 report 行应被拒绝（实际：" + r + "）");
        if (accepted)
            bug("B5c", "report 段解析失败仍置 haveReport=true",
                "报告被解析成全 0 并跳过 evaluate() 重算；readFile 的 report 分支缺少 !ss 检查");
    }
    // g) pending 事件字段不校验：kind=1 且 a=-50 的难民事件
    {
        std::string s = dropLine(base, "pending ");
        s = replaceLine(s, "log ", "log 0");
        // 在 log 段之后插入 pending 段
        {
            std::vector<std::string> v = splitLines(s), out;
            for (const std::string& l : v) {
                out.push_back(l);
                if (l.rfind("log ", 0) == 0) {
                    out.push_back("pending 1");
                    out.push_back("1 -50 0 0 3");
                    out.push_back("难民船");
                    out.push_back("文本");
                    out.push_back("接收");
                    out.push_back("拒绝");
                    out.push_back("征用");
                }
            }
            s = joinLines(out);
        }
        writeAll(kSave, s);
        Game h;
        std::string r = h.loadFrom(kSave);
        if (r.find("已从存档") != std::string::npos && h.hasPending()) {
            h.answer(1);
            check(h.pop() >= 0, "恶意难民事件不应把人口变成负数");
            if (h.pop() < 0)
                bug("B5d", "pending 事件字段未校验：a=-50 可把人口变负",
                    "读档后 answer(1) 使 pop()=" + std::to_string(h.pop()) +
                    "；readFile 的 pending 分支只检查计数/文本行数，不检查 kind 与 a/b 值域");
        } else {
            check(true, "恶意 pending 事件被拒绝或未生效");
        }
    }
    std::printf("    完成\n\n");
}

// =====================================================================
//  [3] fork 隔离的崩溃 / UB / 脏 TUNE 用例
// =====================================================================
static void reportChild(const std::string& tag, const ChildResult& cr, bool expectCrash,
                        const std::string& bugId, const std::string& bugTitle, const std::string& detail) {
    if (cr.signaled()) {
        if (!expectCrash)
            bug(bugId, bugTitle, detail + "；子进程被信号 " + std::to_string(cr.signal()) + " 终结");
        else
            bug(bugId, bugTitle, detail + "；子进程被信号 " + std::to_string(cr.signal()) + " 终结");
        return;
    }
    if (cr.code() != 0) {
        bug(bugId, bugTitle, detail + "；子进程返回 " + std::to_string(cr.code()));
        return;
    }
    check(true, tag + " 通过（无崩溃、无不变量破坏）");
}

static void testForkedRobustness() {
    std::printf("[3] fork 隔离：崩溃 / UB / 脏 TUNE 参数\n");
    const std::string base = baseSave();

    // B6: 超大资源 -> advanceTurn 有符号溢出
    {
        writeAll(kSave, replaceLine(base, "res ", "res 0 0 0 2147483647"));
        ChildResult cr = runChild([] {
            Game g;
            if (g.loadFrom(kSave).find("已从存档") == std::string::npos) return 1;
            for (int i = 0; i < 3; ++i) g.advanceTurn();
            const Resources& r = g.res();
            return (r.metal >= 0 && r.energy >= 0 && r.food >= 0 && r.science >= 0) ? 0 : 1;
        });
        reportChild("超大 science 推进", cr, false, "B6",
                    "超大资源使 advanceTurn 有符号整数溢出（UB）",
                    "存档 res.science=INT_MAX，推进一周期后 res_.science += scienceNet 溢出变负；"
                    "根因 src/game.cpp:772（metal/energy 有 max(0,·) 兜底，science 没有）");
    }

    // B2: combatLossDiv = 0 -> 整数除零
    {
        ChildResult cr = runChild([] {
            TUNE.combatLossDiv = 0;   // 子进程 COW，不影响父进程
            TUNE.waveFirst = 1;
            TUNE.wavePerTurn = 0.0;
            Game g;
            g.newGame(1, "qa");
            g.advanceTurn();          // waveIn 1->0 触发 applyCombat
            return 0;
        });
        reportChild("TUNE.combatLossDiv=0", cr, false, "B2",
                    "脏 TUNE 参数导致整数除零（SIGFPE）",
                    "combatLossDiv=0 时 applyCombat 的 over / TUNE.combatLossDiv 除零；"
                    "同类 combatDamageDiv 已有 std::max(1,·) 保护，combatLossDiv 漏了（src/game.cpp:725）");
    }

    // B7a: startPop 负数
    {
        ChildResult cr = runChild([] {
            TUNE.startPop = -5;
            Game g;
            g.newGame(1, "qa");
            return g.pop() >= 0 ? 0 : 1;
        });
        reportChild("TUNE.startPop=-5", cr, false, "B7a",
                    "脏 TUNE 参数：startPop 为负时新开局人口为负",
                    "newGame 直接采用 TUNE.startPop，无钳制；recomputeWorkers 也不会把负 pop 拉回 0"
                    "（housing_ < pop_ 不成立），游戏在人口为负的状态下开始");
    }

    // B7b: housingPerHab 负数 -> 建居住舱后人口被静默清成负数
    {
        ChildResult cr = runChild([] {
            TUNE.housingPerHab = -100;
            Game g;
            g.newGame(1, "qa");
            g.doBuild("hab", 7, 5);
            return (g.pop() >= 0 && g.housing() >= 0) ? 0 : 1;
        });
        reportChild("TUNE.housingPerHab=-100", cr, false, "B7b",
                    "脏 TUNE 参数：housingPerHab 为负时人口/上限变负且无提示",
                    "recomputeWorkers 里 housing_ 可为负，随后把 pop_ 静默钳到负数"
                    "（src/game.cpp:220-221），无日志、无 checkEnd");
    }

    // B8: wavePerTurn 极大 -> double 转 int 的 UB / 估计值错误
    {
        ChildResult cr = runChild([] {
            TUNE.wavePerTurn = 1e300;
            Game g;
            g.newGame(1, "qa");
            double expectedMin = TUNE.waveBase;   // 估计值至少应 >= waveBase
            return g.waveStrengthEstimate() >= expectedMin ? 0 : 1;
        });
        reportChild("TUNE.wavePerTurn=1e300", cr, false, "B8",
                    "脏 TUNE 参数：wavePerTurn 极大导致 double->int 未定义转换",
                    "waveStrengthEstimate 对超出 int 范围的 double 做 static_cast<int>（UB，UBSan 报 "
                    "float-cast-overflow），结果是 INT_MIN 被钳成 5，比 waveBase 还小");
    }

    // 以下脏参数用例预期“应当健壮”：不崩溃、资源不为负、能正常结束
    {
        ChildResult cr = runChild([] {
            TUNE.maxTurns = 0;
            Game g;
            g.newGame(2, "qa");
            g.advanceTurn();
            return g.over() ? 0 : 1;
        });
        reportChild("TUNE.maxTurns=0", cr, false, "B7c", "maxTurns=0 未立即结束", "应在一个周期内进入结束态");
    }
    {
        ChildResult cr = runChild([] {
            TUNE.maxTurns = -10;
            Game g;
            g.newGame(2, "qa");
            g.advanceTurn();
            return g.over() ? 0 : 1;
        });
        reportChild("TUNE.maxTurns=-10", cr, false, "B7d", "maxTurns 为负未立即结束", "应在一个周期内进入结束态");
    }
    {
        ChildResult cr = runChild([] {
            TUNE.farmFood = -1000.0;
            Game g;
            g.newGame(3, "qa");
            g.doBuild("farm", 7, 5);
            for (int i = 0; i < 30; ++i) {
                g.advanceTurn();
                const Resources& r = g.res();
                if (r.metal < 0 || r.energy < 0 || r.food < 0 || r.science < 0) return 1;
                if (g.pop() < 0) return 1;
            }
            return 0;
        });
        reportChild("TUNE.farmFood=-1000", cr, false, "B7e", "负 farmFood 破坏不变量",
                    "负产出应被钳制，资源与人口不得为负");
    }
    {
        ChildResult cr = runChild([] {
            TUNE.eventChance = 0;
            Game g;
            g.newGame(4, "qa");
            for (int i = 0; i < 20; ++i) g.advanceTurn();
            return g.hasPending() ? 1 : 0;
        });
        reportChild("TUNE.eventChance=0", cr, false, "B7f", "eventChance=0 仍触发事件", "不应有待处理事件");
    }
    std::printf("    完成\n\n");
}

// =====================================================================
//  [4] 命令边界
// =====================================================================
static void testCommandBoundaries() {
    std::printf("[4] 命令边界\n");
    Game g;
    g.newGame(123456u, "边界");
    const Snap before = snap(g);

    auto unchanged = [&](const std::string& tag) {
        check(sameSnap(before, snap(g)), tag + " 失败的指令不得改变状态");
    };

    // 未知 / 大小写 / 中文名
    check(g.doBuild("nope", 7, 5).find("未知建筑类型") != std::string::npos, "未知建筑键被拒绝");
    unchanged("未知建筑键");
    check(g.doBuild("SOL", 7, 5).find("未知建筑类型") != std::string::npos, "大写键被拒绝（键区分大小写）");
    unchanged("大写键");
    {
        std::string r = g.doBuild("太阳能板", 0, 0);
        check(r.find("无法建造") != std::string::npos || r.find("开始建造") != std::string::npos,
              "中文建筑名可解析（实际：" + r + "）");
    }
    // 中文名用例会改变状态，重置基线
    Game g2;
    g2.newGame(123456u, "边界2");

    // 坐标边界（只验证不崩溃 + 越界被拒）
    for (int x = -1; x <= MAP_W; ++x) {
        std::string r = g2.doBuild("sol", x, -1);
        check(!r.empty(), "越界坐标返回日志（x=" + std::to_string(x) + "）");
    }
    check(g2.doBuild("sol", -1, 0).find("坐标超出地图范围") != std::string::npos, "x=-1 越界被拒");
    check(g2.doBuild("sol", MAP_W, 0).find("坐标超出地图范围") != std::string::npos, "x=MAP_W 越界被拒");
    check(g2.doBuild("sol", 0, -1).find("坐标超出地图范围") != std::string::npos, "y=-1 越界被拒");
    check(g2.doBuild("sol", 0, MAP_H).find("坐标超出地图范围") != std::string::npos, "y=MAP_H 越界被拒");
    check(g2.doBuild("sol", INT_MAX, INT_MAX).find("坐标超出地图范围") != std::string::npos, "INT_MAX 越界被拒");
    check(g2.doBuild("sol", INT_MIN, INT_MIN).find("坐标超出地图范围") != std::string::npos, "INT_MIN 越界被拒");
    check(g2.doBuild("hq", 7, 5).find("无法建造") != std::string::npos, "HQ 不可再建");
    check(g2.doBuild("mine", 7, 5).find("矿脉") != std::string::npos, "Mine 必须建在矿脉上");
    check(g2.doBuild("geo", 7, 5).find("地热") != std::string::npos, "Geo 必须建在地热口上");

    // 山脉地形
    bool sawMountain = false;
    for (int y = 0; y < MAP_H && !sawMountain; ++y)
        for (int x = 0; x < MAP_W && !sawMountain; ++x)
            if (g2.tile(x, y).terrain == Terrain::Mountain) {
                sawMountain = true;
                check(g2.buildable(BType::Solar, x, y, nullptr) == false, "山脉不可建造");
                check(g2.doBuild("sol", x, y).find("无法建造") != std::string::npos, "山脉建造被拒");
            }
    check(sawMountain, "地图应含山脉（测试前提）");
    // 矿石地块：非 Mine 不可建，Mine 可建
    bool sawOre = false;
    for (int y = 0; y < MAP_H && !sawOre; ++y)
        for (int x = 0; x < MAP_W && !sawOre; ++x)
            if (g2.tile(x, y).terrain == Terrain::Ore && g2.tile(x, y).building < 0) {
                sawOre = true;
                check(g2.doBuild("sol", x, y).find("无法建造") != std::string::npos, "矿脉上不可建太阳能板");
                check(g2.doBuild("mine", x, y).find("开始建造") != std::string::npos, "矿脉上可建钻矿场");
            }
    check(sawOre, "地图应含矿脉（测试前提）");
    // 冰层：允许建造（设计：只有平原/冰层可建）
    bool sawIce = false;
    for (int y = 0; y < MAP_H && !sawIce; ++y)
        for (int x = 0; x < MAP_W && !sawIce; ++x)
            if (g2.tile(x, y).terrain == Terrain::Ice && g2.tile(x, y).building < 0) {
                sawIce = true;
                std::string r = g2.doBuild("sol", x, y);
                check(r.find("开始建造") != std::string::npos, "冰层可建（实际：" + r + "）");
            }
    // (0,0) 与 (17,11) 是合法坐标：结果取决于地形，但绝不能报“坐标超出地图范围”
    check(g2.doBuild("sol", 0, 0).find("坐标超出地图范围") == std::string::npos, "(0,0) 是合法坐标");
    check(g2.doBuild("sol", MAP_W - 1, MAP_H - 1).find("坐标超出地图范围") == std::string::npos, "(17,11) 是合法坐标");

    // 无效建筑编号
    Game g3;
    g3.newGame(7u, "编号");
    check(g3.doDemolish(-1).find("无效") != std::string::npos, "doDemolish(-1) 被拒");
    check(g3.doDemolish(9999).find("无效") != std::string::npos, "doDemolish(9999) 被拒");
    check(g3.doDemolish(INT_MIN).find("无效") != std::string::npos, "doDemolish(INT_MIN) 被拒");
    check(g3.doDemolish(0).find("无法拆除") != std::string::npos, "HQ 不可拆除");
    check(g3.doToggle(-1).find("无效") != std::string::npos, "doToggle(-1) 被拒");
    check(g3.doToggle(9999).find("无效") != std::string::npos, "doToggle(9999) 被拒");
    check(g3.doFocus(-1).find("无效") != std::string::npos, "doFocus(-1) 被拒");
    check(g3.doFocus(9999).find("无效") != std::string::npos, "doFocus(9999) 被拒");
    // 已拆除的 id
    {
        Game d;
        d.newGame(11u, "拆");
        check(d.doBuild("sol", 7, 5).find("开始建造") != std::string::npos, "拆除前置：建造成功");
        check(d.doDemolish(1).find("已拆除") != std::string::npos, "拆除成功");
        check(d.doDemolish(1).find("无效") != std::string::npos, "已拆除建筑再拆被拒");
        check(d.doToggle(1).find("无效") != std::string::npos, "已拆除建筑开关被拒");
        check(d.doFocus(1).find("无效") != std::string::npos, "已拆除建筑聚焦被拒");
    }

    // 研究
    Game r;
    r.newGame(13u, "研究");
    check(r.doResearch("nope").find("未知科技") != std::string::npos, "未知科技被拒");
    check(r.doResearch("HYDRO").find("未知科技") != std::string::npos, "大写科技键被拒");
    check(r.doResearch("hydro").find("科研点不足") != std::string::npos, "科研不足被拒");
    const Snap rs = snap(r);
    check(r.doResearch("atmo").find("前置科技不足") != std::string::npos, "前置缺失被拒");
    check(sameSnap(rs, snap(r)), "前置不足不得消耗资源");
    check(r.doResearch("水培改良").find("科研点不足") != std::string::npos, "中文科技名可解析");

    // 重复研究 / 重复星门：用带科技与资源的存档驱动
    {
        std::string s = replaceLine(baseSave(), "res ", "res 2000 2000 500 500");
        s = replaceLine(s, "state ", "state 1 6 70 0 128 0 3 9 0 0");  // techs = 1<<7 = 星门理论
        writeAll(kSave, s);
        Game h;
        std::string lr = h.loadFrom(kSave);
        check(lr.find("已从存档") != std::string::npos, "带科技存档可读入");
        check(h.doResearch("gate").find("已经研究过") != std::string::npos, "重复研究被拒");
        check(h.doBuild("gate", 7, 5).find("开始建造") != std::string::npos, "星门理论后首个星门可建");
        check(h.doBuild("gate", 2, 2).find("无法建造") != std::string::npos, "第二个星门被拒");
        check(h.countType(BType::Gate) == 1, "星门数量恒为 1");
    }
    std::printf("    完成\n\n");
}

// =====================================================================
//  [5] 未完工建筑是否被计入“已建成”效果
// =====================================================================
static void testUnderConstructionEffects() {
    std::printf("[5] 在建建筑不应提供效果\n");
    Game g;
    g.newGame(2024u, "在建");
    const int baseHousing = g.housing();
    check(baseHousing == TUNE.baseHousing, "初始人口上限 = baseHousing");

    std::string r = g.doBuild("hab", 7, 5);
    check(r.find("开始建造") != std::string::npos, "居住舱开始建造");
    const Building* hab = g.building(1);
    check(hab && hab->type == BType::Hab && hab->buildLeft > 0, "居住舱处于在建状态");

    int expect = TUNE.baseHousing;  // 未完工 -> 不提供上限
    check(g.housing() == expect,
          "在建居住舱不应提供人口上限：期望 " + std::to_string(expect) + "，实际 " + std::to_string(g.housing()));
    if (g.housing() != expect)
        bug("B1", "在建的居住舱立即计入 housing（countType 忽略 buildLeft）",
            "doBuild(\"hab\") 后 buildLeft=2（尚未完工），housing 却立刻从 " + std::to_string(TUNE.baseHousing) +
            " 变成 " + std::to_string(g.housing()) +
            "；根因 src/game.cpp:219 用 countType(BType::Hab)，而 countType 不检查 buildLeft。"
            "同一根因还让在建的医疗站提前加速人口/减少伤亡（game.cpp:252/724/782）");

    // 完工后应生效
    for (int i = 0; i < 3; ++i) g.advanceTurn();
    check(g.housing() == TUNE.baseHousing + TUNE.housingPerHab, "完工后居住舱提供上限");
    checkInvariants(g, "afterHab");
    std::printf("    完成\n\n");
}

// =====================================================================
//  [6] 长局、结束冻结、重开重置、存档往返
// =====================================================================
static void testLongRunAndRoundTrip() {
    std::printf("[6] 长局 / 重开 / 存档往返\n");
    // 长局：最多 500 次推进，必须结束且结束状态冻结
    {
        Game g;
        g.newGame(7u, "长局");
        int steps = 0;
        for (; steps < 500 && !g.over(); ++steps) {
            // P2：规则上移 core —— 待决事件期间 advanceTurn 会冻结，必须先应答才能继续推进。
            // （旧规则「待决也照推」已废弃；这里保持长局推进不崩溃/不变量成立的原有意图。）
            while (g.hasPending()) g.answer(1);
            g.advanceTurn();
            checkInvariants(g, "long@" + std::to_string(g.turn()));
        }
        check(g.over(), "500 次推进内必定结束");
        const Snap s = snap(g);
        for (int i = 0; i < 5; ++i) g.advanceTurn();
        check(sameSnap(s, snap(g)), "结束后 advanceTurn 必须完全冻结状态");
        check(g.turn() <= TUNE.maxTurns + 1, "结束后回合数不超过 maxTurns+1");
    }
    // 连续 AI 试玩不变量
    for (uint32_t seed : {1u, 42u, 99991u}) {
        Game g;
        g.newGame(seed, "AI");
        int guard = 0;
        while (!g.over() && guard++ < TUNE.maxTurns + 10) {
            aiTurn(g);
            checkInvariants(g, "ai@" + std::to_string(seed) + "T" + std::to_string(g.turn()));
            if (g.over()) break;
            g.advanceTurn();
        }
    }
    // 重开必须完全重置
    {
        Game g;
        g.newGame(5u, "重开");
        for (int i = 0; i < 40; ++i) { aiTurn(g); g.advanceTurn(); }
        g.newGame(5u, "重开");
        Game fresh;
        fresh.newGame(5u, "重开");
        check(sameSnap(snap(g), snap(fresh)), "newGame 后状态与全新对象一致");
        check(g.log().size() == fresh.log().size(), "newGame 后日志重置");
        check(g.buildings().size() == fresh.buildings().size(), "newGame 后建筑重置");
        check(!g.over() && !g.won(), "newGame 清除结束标记");
    }
    // 存档往返：save -> load -> save 必须字节一致，并继续同步演化
    {
        Game a;
        a.newGame(20250101u, "往返测试");
        for (int i = 0; i < 25; ++i) { aiTurn(a); a.advanceTurn(); }
        a.saveTo("/tmp/qa_rt_a.sav");
        Game b;
        std::string lr = b.loadFrom("/tmp/qa_rt_a.sav");
        check(lr.find("已从存档") != std::string::npos, "往返存档可读入");
        b.saveTo("/tmp/qa_rt_b.sav");
        const std::string fa = readAll("/tmp/qa_rt_a.sav"), fb = readAll("/tmp/qa_rt_b.sav");
        check(fa == fb, "save->load->save 字节一致");
        if (fa != fb)
            bug("B9", "存档往返不精确（save->load->save 字节不一致）",
                "A/B 长度 " + std::to_string(fa.size()) + "/" + std::to_string(fb.size()));
        check(sameSnap(snap(a), snap(b)), "读档后状态字段一致");
        check(a.colonyName() == b.colonyName(), "读档后名称一致");
        for (int i = 0; i < 12; ++i) { aiTurn(a); aiTurn(b); a.advanceTurn(); b.advanceTurn(); }
        check(sameSnap(snap(a), snap(b)), "读档后继续 12 周期仍与原件一致");
    }
    std::printf("    完成\n\n");
}

// =====================================================================
//  [7] 待决事件：规则上移 core（docs/PROTOCOL.md §4.4.1）
//      规则住在 core，前端不得依赖自己的拦截 —— 本组独立于 protocol_tests [I] 再验一遍。
// =====================================================================
static void testPendingBlocksCore() {
    std::printf("[7] 待决事件：六操作拦截 / advance 冻结 / answer 恢复（规则上移 core）\n");

    Game g;
    g.newGame(5u, "待决");
    int guard = 0;
    while (!g.hasPending() && !g.over() && guard++ < TUNE.maxTurns + 10) g.advanceTurn();
    check(g.hasPending(), "若干周期内出现待决事件");
    if (!g.hasPending()) { std::printf("    跳过（未造出事件）\n\n"); return; }

    // advanceTurn 必须完全冻结：turn / 所有快照字段 / pending 均不变（不消耗 rng、无副作用）
    const Snap before = snap(g);
    const int  turn0  = g.turn();
    g.advanceTurn();
    check(g.turn() == turn0, "待决期间 advanceTurn 不推进周期");
    check(sameSnap(before, snap(g)), "待决期间 advanceTurn 不改变任何快照字段");
    check(g.hasPending(), "待决期间 advanceTurn 不消耗/改变事件");

    // 五个返回 ActionResult 的行动必须被拦且 code=BlockedByPending，且无副作用
    auto blocked = [&](const ActionResult& r, const char* what) {
        check(!r.ok, std::string("待决期间 ") + what + " 被拒绝");
        check(r.code == ActCode::BlockedByPending,
              std::string("待决期间 ") + what + " code=BlockedByPending");
    };
    blocked(g.doBuild("sol", 0, 0), "build");
    blocked(g.doDemolish(0), "demolish");
    blocked(g.doToggle(0), "toggle");
    blocked(g.doFocus(0), "focus");
    blocked(g.doResearch("hydro"), "research");
    // 第六个操作 advanceTurn 返回 void，用「周期是否前进」读出核心的决定
    g.advanceTurn();
    check(g.turn() == turn0, "待决期间 advance（第六个操作）亦被拦（turn 不变）");
    check(sameSnap(before, snap(g)), "被拦的六个操作均无副作用");
    check(g.hasPending(), "被拦操作不消耗待决事件");

    // answer 不受影响；应答后 advance 恢复推进
    ActionResult ar = g.answer(1);
    check(ar.ok && ar.code == ActCode::AnswerChoice, "待决期间 answer(1) 成功");
    while (g.hasPending()) g.answer(1);   // 应对可能的事件链
    const int t1 = g.turn();
    g.advanceTurn();
    check(g.turn() == t1 + 1, "应答后 advanceTurn 恢复推进");

    std::printf("    完成\n\n");
}

// =====================================================================
int main() {
    std::printf("=== 星际争霸：殖民地 对抗性边界测试 (task-2) ===\n");
    std::printf("构建时间: %s %s（被测源码修订哈希见 docs/QA_REPORT.md）\n\n", __DATE__, __TIME__);

    testLoadRejectPreserves();
    testLoadValidationGaps();
    testForkedRobustness();
    testCommandBoundaries();
    testUnderConstructionEffects();
    testLongRunAndRoundTrip();
    testPendingBlocksCore();

    std::printf("=== 检查 %d 项，失败 %d 项，其中标记缺陷 %d 个 ===\n", g_checks, g_fail, g_bugs);
    std::printf("复现：g++ -std=c++20 -O2 -Isrc tests/edge_tests.cpp src/game.cpp -o /tmp/edge_tests && /tmp/edge_tests\n");
    std::printf("ASan: g++ -std=c++20 -g -fsanitize=address,undefined -Isrc tests/edge_tests.cpp src/game.cpp -o /tmp/edge_asan && /tmp/edge_asan\n");
    return g_fail == 0 ? 0 : 1;
}
