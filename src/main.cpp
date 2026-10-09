// 星际争霸：殖民地 (Star Colony) —— C++20 命令行殖民地经营游戏
// Copyright (C) 2026 Star Colony contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published
// by the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// See the LICENSE file at the project root for the full license text.
// SPDX-License-Identifier: AGPL-3.0-or-later

// 星际争霸：殖民地 —— 命令行入口与指令解析
#include "game.hpp"
#include "content.hpp"
#include "ui.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace sc;

namespace {

std::string num(int v) { return std::to_string(v); }

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream is(s);
    std::string t;
    while (is >> t) out.push_back(t);
    return out;
}

bool parseInt(const std::string& s, int& out) {
    try {
        size_t used = 0;
        int v = std::stoi(s, &used);
        if (used != s.size()) return false;
        out = v;
        return true;
    } catch (...) {
        return false;
    }
}

std::vector<std::string> helpLines() {
    const BDef& gate = BDEF[static_cast<size_t>(BType::Gate)];   // 单一事实来源：星门造价来自 BDEF
    return {
        "命令一览（输入命令后回车；直接回车 = 推进一个周期）",
        "",
        "  next / n / 回车        推进一个周期（结算生产、人口、事件、虫潮）",
        "  status / st            殖民地详细报告（收支、库存、预测）",
        "  detail                 列出全部建筑及其编号",
        "  list                   建筑造价与解锁条件",
        "  tech                   科技树与已研究进度",
        "  build <类型> <x> <y>   在地块 (x,y) 建造，例：build sol 5 4",
        "  demolish <编号>        拆除建筑，回收一半金属",
        "  toggle <编号>          开/关建筑（关闭后不耗能也不产出）",
        "  focus <编号>           提升该建筑的工人分配优先级",
        "  research <科技>        消耗科研点研究科技，例：research fusion",
        "  scan <x> <y>           查看某地块详情（地形、矿量、建筑）",
        "  map                    重新绘制地图（画面每帧自动刷新，此命令仅提示）",
        "  log                    查看最近 30 条消息",
        "  save [文件]            存档（默认 save.txt）",
        "  load [文件]            读档（默认 save.txt）",
        "  restart                以新随机种子重开一局",
        "  help                   显示本帮助",
        "  about                  关于本作与构建说明",
        "  quit / exit            退出游戏",
        "",
        "建筑类型键：hq/指挥中心 sol/太阳能板 geo/地热站 mine/钻矿场 farm/水培农场",
        "            hab/居住舱 lab/研究所 clinic/医疗站 turret/防御炮塔 gate/星门",
        "科技键：hydro autodrill fusion nanomed alloy atmo drone gate",
        "",
        "胜利条件：研究「星门理论」后建成星门（需 " + num(gate.costMetal) + " 金属 / " + num(gate.costEnergy) +
            " 能源 / " + num(gate.workers) + " 工人 / " + num(gate.buildTurns) + " 周期）。",
        "失败条件：人口归零，或撑过 " + num(TUNE.maxTurns) + " 周期仍未撤离。",
    };
}

std::vector<std::string> listLines(const Game& g) {
    std::vector<std::string> v;
    v.push_back("建筑造价（括号内为需求）");
    v.push_back("");
    for (int i = 0; i < BTYPE_COUNT; ++i) {
        const BDef& d = BDEF[static_cast<size_t>(i)];
        std::ostringstream o;
        o << "  " << padRight(d.key, 7) << padRight(d.name, 10) << "  金属 " << d.costMetal
          << "  能源 " << d.costEnergy << "  科研 " << d.costScience
          << "  工期 " << d.buildTurns << "  工人 " << d.workers
          << "  耗能 " << d.upkeep;
        v.push_back(o.str());

        std::string note = std::string("      ") + d.desc;
        if (i == static_cast<int>(BType::HQ)) note += "（已有，不可再建）";
        if (i == static_cast<int>(BType::Gate) && !g.hasTech(Tech::GateTheory)) note += "（需先研究星门理论）";
        v.push_back(colorize(COL_GREY, note));
    }
    v.push_back("");
    v.push_back("提示：矿场必须建在 * 矿脉上，地热站必须建在 ~ 地热口上，其余建筑只能建在 . 平原或 | 冰层上。");
    return v;
}

std::vector<std::string> techLines(const Game& g) {
    std::vector<std::string> v;
    v.push_back("科技树（研究消耗科研点；每项只能研究一次）");
    v.push_back("");
    for (int i = 0; i < TECH_COUNT; ++i) {
        const TechDef& d = TDEF[static_cast<size_t>(i)];
        bool done = g.hasTech(static_cast<Tech>(i));
        bool ready = true;
        for (int k = 0; k < TECH_COUNT; ++k)
            if ((d.req & techBit(static_cast<Tech>(k))) && !g.hasTech(static_cast<Tech>(k))) ready = false;
        std::string mark = done ? "[已研究]" : (ready ? "[可研究]" : "[缺前置]");
        Col c = done ? COL_GREEN : (ready ? COL_BWHITE : COL_GREY);
        std::ostringstream o;
        o << "  " << padRight(d.key, 11) << padRight(d.name, 12) << "  科研 " << d.cost << "  " << mark;
        v.push_back(colorize(c, o.str()));
        std::string req;
        for (int k = 0; k < TECH_COUNT; ++k)
            if (d.req & techBit(static_cast<Tech>(k))) req += std::string(TDEF[static_cast<size_t>(k)].name) + " ";
        v.push_back(colorize(COL_GREY, "      " + std::string(d.desc) + (req.empty() ? "" : "   前置：" + req)));
    }
    v.push_back("");
    v.push_back("当前科研点：" + num(g.res().science) + "    每周期产出：" + num(g.report().scienceNet));
    return v;
}

std::vector<std::string> statusLines(const Game& g) {
    const Resources& r = g.res();
    const TurnReport& p = g.report();
    auto sign = [](int v) { return (v >= 0 ? "+" : "") + num(v); };
    const WeatherDef& w = WDEF[static_cast<size_t>(g.weather())];
    std::vector<std::string> v;

    v.push_back("殖民地「" + g.colonyName() + "」  周期 " + num(g.turn()) + " / " + num(TUNE.maxTurns));
    v.push_back(colorize(COL_GREY, "────────────────────────────────────────────"));
    v.push_back("  金属   " + num(r.metal) + "   (" + sign(p.metalNet) + "/周期)");
    v.push_back("  能源   " + num(r.energy) + "   (" + sign(p.energyNet) + "/周期，设施消耗 " + num(p.energyUp) + ")");
    v.push_back("  食物   " + num(r.food) + "   (" + sign(p.foodNet) + "/周期，人口消耗 " + num(p.foodUp) + ")");
    v.push_back("  科研   " + num(r.science) + "   (" + sign(p.scienceNet) + "/周期)");
    char mbuf[32];
    std::snprintf(mbuf, sizeof mbuf, "%.2f", g.moraleMultiplier());   // 士气系数来自引擎，避免规则重复
    v.push_back("  人口   " + num(g.pop()) + " / " + num(g.housing()) + "（闲置 " + num(g.idleWorkers()) +
                "）   工人按建造顺序优先分配，用 focus <编号> 调整");
    if (g.pop() > g.housing())
        v.push_back(colorize(COL_RED, "  ⚠ 人口超编 " + num(g.pop() - g.housing()) +
                                       " 人：每周期扣士气，并有概率流失人口，请尽快建造居住舱"));
    if (g.idleWorkers() == 0) {
        bool starved = false;
        for (const Building& b : g.buildings())
            if (b.alive && b.buildLeft == 0 && b.enabled && b.damaged == 0 && g.workerNeed(b) > 0 &&
                g.assigned()[static_cast<size_t>(b.id)] == 0) starved = true;
        if (starved) v.push_back(colorize(COL_YELLOW, "  ⚠ 有人手为 0 的建筑在空转：拆掉或用 focus 调整优先级"));
    }
    v.push_back("  士气   " + num(g.morale()) + "   产出系数 x" + std::string(mbuf));
    v.push_back("  防御   " + num(g.defense()) + "   下一波虫潮 " + num(g.waveIn()) + " 周期后，预计强度 " + num(g.waveStrengthEstimate()));

    char buf[256];
    std::snprintf(buf, sizeof buf, "  天气   %s（剩 %d 周期）  金属x%.2f 能源x%.2f 食物x%.2f 科研x%.2f",
                  w.name, g.weatherLeft(), w.metal, w.energy, w.food, w.science);
    v.push_back(std::string(buf));
    if (p.brownout) v.push_back(colorize(COL_YELLOW, "  ⚠ 本周期能源透支，耗能设施将半负荷运转"));
    if (p.starving) v.push_back(colorize(COL_RED, "  ⚠ 本周期食物透支，将有殖民者饿死"));

    v.push_back("");
    v.push_back("建筑构成：");
    for (int i = 0; i < BTYPE_COUNT; ++i) {
        int c = g.countType(static_cast<BType>(i));
        if (c == 0) continue;
        int building = 0, damaged = 0;
        for (const Building& b : g.buildings()) {
            if (!b.alive || b.type != static_cast<BType>(i)) continue;
            if (b.buildLeft > 0) ++building;
            else if (b.damaged > 0) ++damaged;
        }
        std::ostringstream o;
        o << "  " << padRight(BDEF[static_cast<size_t>(i)].name, 10) << " x" << c;
        if (building) o << "（在建 " << building << "）";
        if (damaged) o << colorize(COL_RED, "（受损 " + num(damaged) + "）");
        v.push_back(o.str());
    }
    return v;
}

std::vector<std::string> detailLines(const Game& g) {
    std::vector<std::string> v;
    v.push_back("全部建筑明细（工人 a/b：已分配/需求）");
    v.push_back("");
    for (const Building& b : g.buildings()) {
        if (!b.alive) continue;
        const BDef& d = BDEF[static_cast<size_t>(b.type)];
        int got = 0;
        if (b.id < static_cast<int>(g.assigned().size())) got = g.assigned()[static_cast<size_t>(b.id)];
        int need = g.workerNeed(b);
        std::ostringstream o;
        o << "  #" << b.id << "  " << padRight(d.name, 10) << " (" << b.x << "," << b.y << ")"
          << "  工人 " << got << "/" << need;
        if (b.buildLeft > 0) o << "  在建剩 " << b.buildLeft;
        if (b.damaged > 0) o << "  受损剩 " << b.damaged;
        if (!b.enabled) o << "  已关闭";
        v.push_back(o.str());
    }
    v.push_back("");
    v.push_back("地块索引（可直接用于 build）：例 build mine 5 3");
    return v;
}

std::vector<std::string> logLines(const Game& g) {
    std::vector<std::string> v;
    v.push_back("完整消息记录（" + num(static_cast<int>(g.log().size())) + " 条，最早的在前）");
    v.push_back("");
    // 不截断：保留多少就显示多少（Game 内部日志上限 400 条）
    for (const LogEntry& m : g.log()) v.push_back(m.text());
    return v;
}

std::vector<std::string> scanLines(const Game& g, int x, int y) {
    std::vector<std::string> v;
    if (x < 0 || x >= MAP_W || y < 0 || y >= MAP_H) {
        v.push_back("坐标超出地图范围（x 0-" + num(MAP_W - 1) + "，y 0-" + num(MAP_H - 1) + "）");
        return v;
    }
    const Tile& t = g.tile(x, y);
    const char* tname = "平原";
    switch (t.terrain) {
    case Terrain::Ore: tname = "金属矿脉"; break;
    case Terrain::Geo: tname = "地热口"; break;
    case Terrain::Ice: tname = "冰层"; break;
    case Terrain::Mountain: tname = "山脉"; break;
    default: break;
    }
    v.push_back("地块 (" + num(x) + "," + num(y) + ")");
    v.push_back("  地形：" + std::string(tname));
    if (t.terrain == Terrain::Ore) v.push_back("  矿量：" + num(t.ore) + "  丰度：" + num(t.richness) + "（每周期开采 " + num(g.tileOreYield(x, y)) + "）");
    v.push_back("  相邻冰层：" + num(g.adjIce(x, y)) + "（农场每格 +" + num(g.farmIceBonus()) + " 食物）");
    v.push_back("  相邻山脉：" + num(g.adjMountain(x, y)) + "（矿场每格 +" + num(g.mineMountainBonus()) + " 金属，最多 +" + num(g.mineMountainCap()) + "）");
    if (t.building >= 0) {
        const Building* b = g.building(t.building);
        if (b) v.push_back("  建筑：#" + num(b->id) + " " + BDEF[static_cast<size_t>(b->type)].name);
    } else {
        v.push_back("  建造成本：金属 " + num(g.res().metal) + " / 能源 " + num(g.res().energy));
        std::vector<std::string> ok;
        for (int i = 0; i < BTYPE_COUNT; ++i) {
            const BDef& d = BDEF[static_cast<size_t>(i)];
            if (d.costMetal > g.res().metal || d.costEnergy > g.res().energy) continue;
            std::string why;
            if (g.buildable(static_cast<BType>(i), x, y, &why)) ok.push_back(d.key);
        }
        std::string s;
        for (const std::string& k : ok) s += k + " ";
        v.push_back("  当前可建：" + (s.empty() ? "（资源不足或无可用建筑）" : s));
    }
    return v;
}

} // namespace

int main(int argc, char** argv) {
    if (!ensureContent()) return 1;   // P3a：内容未加载成功则拒绝启动
    uint32_t seed = static_cast<uint32_t>(std::time(nullptr));
    bool ansi = true;
    bool selftestUI = false;
    std::string colony = "新曙光";

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "--seed" || a == "-s") && i + 1 < argc) {
            seed = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (a == "--no-color") {
            ansi = false;
        } else if ((a == "--name" || a == "-n") && i + 1 < argc) {
            colony = argv[++i];
        } else if (a == "--selftest") {
            selftestUI = true;
        } else if (a == "--help" || a == "-h") {
            std::cout << "星际争霸：殖民地 —— 命令行殖民地经营游戏\n"
                      << "用法：starcolony [选项]\n"
                      << "  --seed N        指定随机种子（默认取当前时间，便于复现同一张地图）\n"
                      << "  --name 名字     殖民地名称（默认「新曙光」）\n"
                      << "  --no-color      关闭 ANSI 颜色\n"
                      << "  --selftest      只渲染一帧后退出，用于检查终端显示是否正常\n"
                      << "  --help          显示本帮助\n"
                      << "\n游戏内输入 help 查看全部命令；直接回车推进一个周期。\n";
            return 0;
        }
    }

    if (!isatty(STDOUT_FILENO)) ansi = false;
    setAnsi(ansi);

    Game g;
    g.newGame(seed, colony);

    std::vector<std::string> panel;
    panel.push_back("欢迎来到《星际争霸：殖民地》。输入 help 查看全部命令，直接回车推进一个周期。");
    panel.push_back("行星环境未知，请尽快建造太阳能板与水培农场，否则殖民者会在几周期内饿死。");

    if (selftestUI) {
        // 只打印一帧，便于脚本检查渲染是否正常
        std::cout << renderFrame(g, "命令 > ", panel) << "\n";
        return 0;
    }

    while (true) {
        std::string prompt;
        if (g.hasPending()) {
            const PendingEvent& e = g.pending();
            panel.clear();
            panel.push_back(colorize(COL_YELLOW, "◆ " + e.title));
            panel.push_back(e.text);
            panel.push_back("");
            for (size_t i = 0; i < e.options.size(); ++i)
                panel.push_back("  " + num(static_cast<int>(i) + 1) + ". " + e.options[i]);
            panel.push_back("");
            panel.push_back("输入选项数字后回车。");
            prompt = "选择 1-" + num(static_cast<int>(e.options.size())) + " > ";
        } else if (g.over()) {
            prompt = "游戏已结束：输入 restart 重开、status 查看统计、quit 退出 > ";
        } else {
            prompt = "命令 > ";
        }

        emitFrame(renderFrame(g, prompt, panel));

        std::string line;
        if (!std::getline(std::cin, line)) { std::cout << "\n"; break; }
        line = trim(line);
        if (line.empty()) {
            if (g.hasPending()) {
                g.log("请先输入 1-" + num(static_cast<int>(g.pending().options.size())) + " 处理当前事件。");
                continue;
            }
            if (g.over()) continue;
            g.advanceTurn();
            panel.clear();
            continue;
        }

        std::vector<std::string> tok = split(line);
        std::string cmd = lower(tok[0]);
        g.log("› " + line);
        panel.clear();

        // 待处理事件：数字键即做选择；其他命令（存档/读档/重开/查看）仍然可用，
        // 只有"推进周期"会被拦住，避免玩家在事件未决时无法存档或重开。
        if (g.hasPending()) {
            int choice = 0;
            if (parseInt(tok[0], choice)) {
                std::string r = g.answer(choice);
                if (r.rfind("无效选项", 0) == 0) g.log(r);
                continue;
            }
        }

        if (cmd == "quit" || cmd == "exit") break;
        else if (cmd == "next" || cmd == "n") {
            if (g.hasPending()) {
                g.log("当前有事件待处理，请输入 1-" + num(static_cast<int>(g.pending().options.size())) + " 做出选择。");
                continue;
            }
            if (g.over()) { g.log("游戏已经结束：" + g.endReason()); continue; }
            g.advanceTurn();
        } else if (cmd == "help" || cmd == "?") {
            panel = helpLines();
        } else if (cmd == "status" || cmd == "st") {
            panel = statusLines(g);
        } else if (cmd == "detail") {
            panel = detailLines(g);
        } else if (cmd == "list") {
            panel = listLines(g);
        } else if (cmd == "tech") {
            panel = techLines(g);
        } else if (cmd == "log") {
            panel = logLines(g);
        } else if (cmd == "build" || cmd == "b") {
            if (tok.size() < 4) {
                g.log("用法：build <类型> <x> <y>，例：build mine 5 3（list 查看类型）");
                panel = listLines(g);
            } else {
                int x = 0, y = 0;
                if (!parseInt(tok[2], x) || !parseInt(tok[3], y)) g.log("坐标必须是整数，例：build farm 5 4");
                else g.log(g.doBuild(tok[1], x, y));
            }
        } else if (cmd == "demolish" || cmd == "dem" || cmd == "d") {
            int id = 0;
            if (tok.size() < 2 || !parseInt(tok[1], id)) g.log("用法：demolish <建筑编号>（detail 查看编号）");
            else g.log(g.doDemolish(id));
        } else if (cmd == "toggle" || cmd == "tg") {
            int id = 0;
            if (tok.size() < 2 || !parseInt(tok[1], id)) g.log("用法：toggle <建筑编号>");
            else g.log(g.doToggle(id));
        } else if (cmd == "focus") {
            int id = 0;
            if (tok.size() < 2 || !parseInt(tok[1], id)) g.log("用法：focus <建筑编号>");
            else g.log(g.doFocus(id));
        } else if (cmd == "research" || cmd == "res") {
            if (tok.size() < 2) { g.log("用法：research <科技键>，例：research fusion"); panel = techLines(g); }
            else g.log(g.doResearch(tok[1]));
        } else if (cmd == "scan" || cmd == "look") {
            int x = 0, y = 0;
            if (tok.size() < 3 || !parseInt(tok[1], x) || !parseInt(tok[2], y)) g.log("用法：scan <x> <y>");
            else panel = scanLines(g, x, y);
        } else if (cmd == "map") {
            g.log("地图已在画面上方显示。");
        } else if (cmd == "save") {
            std::string path = tok.size() >= 2 ? tok[1] : "save.txt";
            g.log(g.saveTo(path));
        } else if (cmd == "load") {
            std::string path = tok.size() >= 2 ? tok[1] : "save.txt";
            std::string r = g.loadFrom(path);
            g.log(r);
        } else if (cmd == "restart" || cmd == "new") {
            std::random_device rd;
            uint32_t s = static_cast<uint32_t>(rd());
            g.newGame(s, colony);
            panel = helpLines();
        } else if (cmd == "about") {
            panel = {"《星际争霸：殖民地》 —— 单文件 C++ 命令行殖民地经营游戏",
                     "在地形、天气、虫潮与随机事件的压力下，用 " + num(TUNE.maxTurns) + " 个周期把殖民地送进星门。",
                     "源码：src/ 目录，构建：cmake 或 make。自检：./selftest"};
        } else {
            g.log("未知命令：" + tok[0] + "（输入 help 查看命令）");
        }
    }

    clearScreen();
    std::cout << "感谢游玩《星际争霸：殖民地》。\n";
    return 0;
}
