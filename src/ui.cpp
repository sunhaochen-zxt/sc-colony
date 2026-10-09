// 星际争霸：殖民地 (Star Colony) —— C++20 命令行殖民地经营游戏
// Copyright (C) 2026 Star Colony contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published
// by the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// See the LICENSE file at the project root for the full license text.
// SPDX-License-Identifier: AGPL-3.0-or-later

// 星际争霸：殖民地 —— 渲染实现
#include "ui.hpp"

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <sys/ioctl.h>
#include <unistd.h>

namespace sc {

namespace {

bool g_ansi = true;

const char* code(Col c) {
    switch (c) {
    case COL_GREY:    return "90";
    case COL_RED:     return "91";
    case COL_GREEN:   return "92";
    case COL_YELLOW:  return "93";
    case COL_BLUE:    return "94";
    case COL_MAGENTA: return "95";
    case COL_CYAN:    return "96";
    case COL_WHITE:   return "37";
    case COL_BWHITE:  return "97";
    default:          return "0";
    }
}

bool isWide(uint32_t cp) {
    return cp >= 0x1100 &&
           (cp <= 0x115F || cp == 0x2329 || cp == 0x232A ||
            (cp >= 0x2E80 && cp <= 0xA4CF) ||
            (cp >= 0xAC00 && cp <= 0xD7A3) ||
            (cp >= 0xF900 && cp <= 0xFAFF) ||
            (cp >= 0xFE30 && cp <= 0xFE6F) ||
            (cp >= 0xFF00 && cp <= 0xFF60) ||
            (cp >= 0xFFE0 && cp <= 0xFFE6) ||
            (cp >= 0x1F300 && cp <= 0x1FAFF) ||
            (cp >= 0x20000 && cp <= 0x3FFFD));
}

// 去掉 ANSI 转义序列，得到纯显示文本
std::string stripAnsi(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '\033') {
            ++i;
            if (i < s.size() && s[i] == '[') {
                ++i;
                while (i < s.size() && !(s[i] >= '@' && s[i] <= '~')) ++i;
                if (i < s.size()) ++i;
            }
            continue;
        }
        out.push_back(s[i++]);
    }
    return out;
}

std::string sep(const std::string& title, int width = 74) {
    const std::string bar = "─";
    std::string line;
    if (title.empty()) {
        for (int i = 0; i < width; ++i) line += bar;
        return line;
    }
    std::string mid = " " + title + " ";
    int midW = visLen(mid);
    int side = std::max(2, (width - midW) / 2);
    for (int i = 0; i < side; ++i) line += bar;
    line += mid;
    int used = side + midW;
    for (int i = used; i < width; ++i) line += bar;
    return line;
}

std::string fmtTurn(const Game& g) {
    std::ostringstream o;
    o << "  星际争霸：殖民地  STAR COLONY" << "      周期 " << g.turn() << " / " << TUNE.maxTurns;
    if (!g.colonyName().empty()) o << "     [" << g.colonyName() << "]";
    return o.str();
}

std::string fmtRes(const Game& g) {
    const Resources& r = g.res();
    const TurnReport& p = g.report();
    auto sign = [](int v) { return (v >= 0 ? "+" : "") + std::to_string(v); };

    std::string line = "  ";
    line += colorize(COL_GREY, "金属 ") + colorize(COL_WHITE, std::to_string(r.metal)) +
            colorize(COL_GREY, "(" + sign(p.metalNet) + ")");
    line += "   " + colorize(COL_YELLOW, "能源 ") + colorize(COL_WHITE, std::to_string(r.energy)) +
            colorize(COL_GREY, "(" + sign(p.energyNet) + ")");
    line += "   " + colorize(COL_GREEN, "食物 ") + colorize(COL_WHITE, std::to_string(r.food)) +
            colorize(COL_GREY, "(" + sign(p.foodNet) + ")");
    line += "   " + colorize(COL_CYAN, "科研 ") + colorize(COL_WHITE, std::to_string(r.science)) +
            colorize(COL_GREY, "(" + sign(p.scienceNet) + ")");
    return line;
}

std::string fmtPop(const Game& g) {
    const WeatherDef& w = WDEF[static_cast<size_t>(g.weather())];
    std::ostringstream o;
    o << "  " << colorize(g.pop() > g.housing() ? COL_RED : COL_WHITE,
                          "人口 " + std::to_string(g.pop()) + " / " + std::to_string(g.housing()))
      << (g.pop() > g.housing() ? colorize(COL_RED, " 超编!") : std::string())
      << colorize(COL_GREY, "（闲置 " + std::to_string(g.idleWorkers()) + "）")
      << "   " << colorize(COL_WHITE, "士气 " + std::to_string(g.morale()))
      << "   " << colorize(COL_RED, "防御 " + std::to_string(g.defense()))
      << "   " << colorize(COL_MAGENTA, "下一波虫潮 " + std::to_string(g.waveIn()) + " 周期")
      << colorize(COL_GREY, "（预计强度 " + std::to_string(g.waveStrengthEstimate()) + "）")
      << "   " << colorize(static_cast<Col>(w.color), "天气 " + std::string(w.name) + "(" + std::to_string(g.weatherLeft()) + ")");
    return o.str();
}

std::string fmtWeather(const Game& g) {
    const WeatherDef& w = WDEF[static_cast<size_t>(g.weather())];
    char buf[256];
    std::snprintf(buf, sizeof buf, "  产出倍率：金属 x%.2f  能源 x%.2f  食物 x%.2f  科研 x%.2f   %s",
                  w.metal, w.energy, w.food, w.science, w.desc);
    return colorize(COL_GREY, buf);
}

std::string mapCell(const Game& g, int x, int y) {
    const Tile& t = g.tile(x, y);
    if (t.building >= 0) {
        const Building* b = g.building(t.building);
        if (b) {
            const BDef& d = BDEF[static_cast<size_t>(b->type)];
            char glyph = d.glyph;
            if (!b->alive) glyph = '.';
            else if (b->buildLeft > 0) return colorize(COL_GREY, std::string(1, static_cast<char>(std::tolower(glyph))));
            else if (b->damaged > 0) return colorize(COL_RED, "!");
            else if (!b->enabled) return colorize(COL_GREY, "/");
            else return colorize(static_cast<Col>(d.color), std::string(1, glyph));
        }
    }
    switch (t.terrain) {
    case Terrain::Ore:      return colorize(COL_YELLOW, "*");
    case Terrain::Geo:      return colorize(COL_MAGENTA, "~");
    case Terrain::Ice:      return colorize(COL_CYAN, "|");
    case Terrain::Mountain: return colorize(COL_GREY, "^");
    default:                return colorize(COL_GREY, ".");
    }
}

std::string mapLine(const Game& g, int y) {
    std::ostringstream o;
    o << "  ";
    char buf[8];
    std::snprintf(buf, sizeof buf, "%2d ", y);
    o << colorize(COL_GREY, buf);
    for (int x = 0; x < MAP_W; ++x) {
        o << mapCell(g, x, y) << "  ";
    }
    return o.str();
}

std::string buildingLine(const Game& g, const Building& b) {
    const BDef& d = BDEF[static_cast<size_t>(b.type)];
    int need = g.workerNeed(b);
    int got = 0;
    if (b.id < static_cast<int>(g.assigned().size())) got = g.assigned()[static_cast<size_t>(b.id)];

    std::string status = "正常";
    Col statusCol = COL_GREEN;
    if (!b.alive) { status = "已拆除"; statusCol = COL_GREY; }
    else if (b.buildLeft > 0) { status = "在建 剩 " + std::to_string(b.buildLeft) + " 周期"; statusCol = COL_YELLOW; }
    else if (b.damaged > 0) { status = "受损 剩 " + std::to_string(b.damaged) + " 周期"; statusCol = COL_RED; }
    else if (!b.enabled) { status = "已关闭"; statusCol = COL_GREY; }
    else if (need > 0 && got < need) { status = "人手不足"; statusCol = COL_YELLOW; }

    std::ostringstream o;
    char head[32];
    std::snprintf(head, sizeof head, "  #%-3d", b.id);
    o << colorize(COL_GREY, head) << "[" << colorize(static_cast<Col>(d.color), std::string(1, d.glyph)) << "] "
      << colorize(static_cast<Col>(d.color), padRight(d.name, 10));
    char pos[24];
    std::snprintf(pos, sizeof pos, "(%2d,%2d) ", b.x, b.y);
    o << colorize(COL_GREY, pos);
    o << colorize(COL_GREY, "工人 " + std::to_string(got) + "/" + std::to_string(need) + "  ");
    o << colorize(statusCol, padRight(status, 16));
    return o.str();
}

std::string fmtDeficit(const Game& g) {
    std::string warns;
    if (g.report().brownout) warns += colorize(COL_YELLOW, "  ⚠ 能源透支：设施半负荷");
    if (g.report().starving) warns += colorize(COL_RED, "  ⚠ 食物透支：人口下降");
    return warns;
}

} // namespace

TermSize termSize() {
    TermSize t;
    struct winsize ws {};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
        t.cols = ws.ws_col > 0 ? ws.ws_col : 100;
        t.rows = ws.ws_row > 0 ? ws.ws_row : 40;
    }
    return t;
}

void setAnsi(bool on) { g_ansi = on; }
bool ansiEnabled() { return g_ansi; }

void clearScreen() {
    if (!g_ansi) return;
    std::fputs("\033[2J\033[H", stdout);
}

int visLen(const std::string& s) {
    std::string p = stripAnsi(s);
    int w = 0;
    size_t i = 0;
    while (i < p.size()) {
        unsigned char c = static_cast<unsigned char>(p[i]);
        if (c < 0x80) { ++w; ++i; continue; }
        int len = 1;
        uint32_t cp = 0;
        if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1Fu; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0Fu; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07u; }
        else { ++w; ++i; continue; }
        if (i + static_cast<size_t>(len) > p.size()) { ++w; break; }
        for (int k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(p[i + static_cast<size_t>(k)]) & 0x3Fu);
        i += static_cast<size_t>(len);
        w += isWide(cp) ? 2 : 1;
    }
    return w;
}

std::string padRight(const std::string& s, int w) {
    int len = visLen(s);
    if (len >= w) return s;
    return s + std::string(static_cast<size_t>(w - len), ' ');
}

std::string colorize(Col c, const std::string& s) {
    if (!g_ansi || c == COL_DEF) return s;
    return std::string("\033[") + code(c) + "m" + s + "\033[0m";
}

std::string renderFrame(const Game& g, const std::string& prompt, const std::vector<std::string>& panel) {
    TermSize ts = termSize();
    const int budget = std::max(34, ts.rows - 1);
    const std::string bar(74, '=');

    std::vector<std::string> header;
    header.push_back(colorize(COL_BLUE, bar));
    header.push_back(fmtTurn(g));
    header.push_back(colorize(COL_BLUE, bar));
    header.push_back(fmtRes(g));
    header.push_back(fmtPop(g));
    header.push_back(fmtWeather(g));

    std::vector<std::string> mapSec;
    {
        char hdr[256];
        std::string cols = "     ";
        for (int x = 0; x < MAP_W; ++x) {
            std::snprintf(hdr, sizeof hdr, "%2d ", x);
            cols += hdr;
        }
        mapSec.push_back(sep("殖民地地图"));
        mapSec.push_back(colorize(COL_GREY, cols));
        for (int y = 0; y < MAP_H; ++y) mapSec.push_back(mapLine(g, y));
        mapSec.push_back(colorize(COL_GREY, "  图例  . 平原(可建)  * 金属矿脉(钻矿场)  ~ 地热口(地热站)  | 冰层(农场+)  ^ 山脉(不可建)"));
        mapSec.push_back(colorize(COL_GREY, "  建筑  [C]指挥 [S]太阳能 [G]地热 [M]矿场 [F]农场 [H]居住 [L]研究所 [K]医疗 [T]炮塔 [X]星门"));
        mapSec.push_back(colorize(COL_GREY, "  状态  小写=在建   !=受损   /=已关闭"));
    }

    std::vector<std::string> blds;
    for (const Building& b : g.buildings())
        if (b.alive) blds.push_back(buildingLine(g, b));

    std::vector<std::string> msgs;
    for (const std::string& m : g.log()) msgs.push_back("  " + m);

    // 预算分配优先级：信息面板（事件/报告）> 消息 > 建筑
    int fixed = static_cast<int>(header.size()) + static_cast<int>(mapSec.size()) + 3;
    int remaining = std::max(0, budget - fixed);
    if (remaining < 0) remaining = 0;

    int panN = 0;
    if (!panel.empty()) {
        panN = std::min<int>(static_cast<int>(panel.size()), std::min(11, std::max(0, remaining - 1)));
        if (panN > 0) remaining -= (panN + 1);
    }
    int msgN = std::min<int>(static_cast<int>(msgs.size()), std::max(0, std::min(5, remaining - 1)));
    if (msgN > 0) remaining -= (msgN + 1);
    int bldN = std::min<int>(static_cast<int>(blds.size()), std::max(0, std::min(7, remaining - 1)));

    std::ostringstream o;
    for (const std::string& s : header) o << s << "\n";
    for (const std::string& s : mapSec) o << s << "\n";

    if (bldN > 0) {
        o << sep("建筑 (" + std::to_string(blds.size()) + ")", 74) << "\n";
        int shown = bldN;
        if (static_cast<int>(blds.size()) > bldN) shown = std::max(0, bldN - 1);
        for (int i = 0; i < shown; ++i) o << blds[static_cast<size_t>(i)] << "\n";
        if (shown < static_cast<int>(blds.size()))
            o << colorize(COL_GREY, "  … 另有 " + std::to_string(blds.size() - static_cast<size_t>(shown)) +
                                       " 座建筑未显示（detail 查看全部）") << "\n";
    }

    if (msgN > 0) {
        o << sep("最新消息", 74) << "\n";
        int start = static_cast<int>(msgs.size()) - msgN;
        for (int i = start; i < static_cast<int>(msgs.size()); ++i) o << msgs[static_cast<size_t>(i)] << "\n";
    }

    if (!panel.empty()) {
        o << sep("信息", 74) << "\n";
        for (int i = 0; i < panN; ++i) o << "  " << panel[static_cast<size_t>(i)] << "\n";
        if (panN < static_cast<int>(panel.size()))
            o << colorize(COL_GREY, "  … 还有 " + std::to_string(panel.size() - static_cast<size_t>(panN)) +
                                       " 行（缩小字号或加大窗口可看全）") << "\n";
    }

    if (g.over()) {
        o << colorize(g.won() ? COL_GREEN : COL_RED, bar) << "\n";
        o << colorize(g.won() ? COL_GREEN : COL_RED,
                      (g.won() ? "  ★ 撤离成功：" : "  ✖ 殖民失败：") + g.endReason()) << "\n";
    }
    o << fmtDeficit(g) << "\n";
    o << colorize(COL_BLUE, bar) << "\n";
    o << " " << prompt;
    return o.str();
}

} // namespace sc
