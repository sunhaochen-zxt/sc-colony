// 星际争霸：殖民地 (Star Colony) —— C++20 命令行殖民地经营游戏
// Copyright (C) 2026 Star Colony contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published
// by the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// See the LICENSE file at the project root for the full license text.
// SPDX-License-Identifier: AGPL-3.0-or-later

// 星际争霸：殖民地 —— 内容包加载 / 校验 / 运行时表构建
//
// ⚠ nlohmann/json 只在本翻译单元包含（见 docs/CONTENT.md §7）：
//   该头文件会让单个 TU 的编译时间增加约 6.5 秒，因此只允许付一次。
//   其余 TU 一律只 include "content.hpp"（纯 struct，不含 JSON 依赖）。
#include "content.hpp"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <sstream>
#include <type_traits>
#include <unordered_map>
#include <unistd.h>

namespace sc {

// =====================================================================
//  平衡参数字段清单（唯一事实来源）
//
//  用 X 宏展开一次，即可同时得到：字段名、是否整数、赋值分发。
//  这样新增 Tuning 字段时只需改这一处，不会出现"漏配而行为悄悄变化"。
//  注意：字段必须与 src/types.hpp 的 Tuning 结构逐字对应。
// =====================================================================
#define SC_TUNING_FIELDS(X)                                                     \
    X(maxTurns, int) X(startMetal, int) X(startEnergy, int) X(startFood, int)   \
    X(startPop, int) X(baseHousing, int) X(housingPerHab, int)                  \
    X(housingPerHabAtmo, int)                                                   \
    X(solarEnergy, double) X(geoEnergy, double) X(hqEnergy, double)             \
    X(hqScience, double) X(labScience, double) X(mineMetalPerRich, double)      \
    X(mineMountainBonus, double) X(mineMountainCap, int) X(farmFood, double)    \
    X(farmIceBonus, double) X(farmIceCap, int) X(fusionMult, double)            \
    X(hydroMult, double) X(drillMult, double) X(foodPerPop, double)             \
    X(atmoFoodMult, double)                                                     \
    X(moraleBase, double) X(moraleDivisor, double) X(moraleTarget, int)         \
    X(starveMoraleDrop, int) X(breachMoraleDrop, int)                           \
    X(popGrowthRate, double) X(popGrowthNeed, double) X(nanoGrowth, double)     \
    X(clinicGrowth, double) X(overcrowdRisk, int)                              \
    X(waveBase, double) X(wavePerTurn, double) X(waveFirst, int)                \
    X(waveInterval, int) X(militiaPerPop, double) X(turretDefense, int)         \
    X(turretDefenseAlloy, int) X(combatLossDiv, int) X(combatLossMax, int)      \
    X(clinicMitigationCap, int) X(combatDamageDiv, int)                         \
    X(eventChance, int)

namespace {

using nlohmann::json;

// ---------- 颜色名 -> Col 枚举（契约 §4：用具名颜色避免枚举增删导致漂移） ----------
struct ColorEntry { const char* name; int col; };
const ColorEntry kColors[] = {
    {"default", COL_DEF},     {"grey", COL_GREY},       {"red", COL_RED},
    {"green", COL_GREEN},     {"yellow", COL_YELLOW},   {"blue", COL_BLUE},
    {"magenta", COL_MAGENTA}, {"cyan", COL_CYAN},       {"white", COL_WHITE},
    {"bright_white", COL_BWHITE},
};

bool colorFromName(const std::string& n, int& out) {
    for (const ColorEntry& c : kColors) {
        if (n == c.name) { out = c.col; return true; }
    }
    return false;
}

// ---------- Tuning 字段元信息（由 X 宏生成） ----------
struct TuneFieldInfo { const char* name; bool isInt; };
const TuneFieldInfo kTuneFields[] = {
#define X(n, tp) {#n, std::is_same_v<tp, int>},
    SC_TUNING_FIELDS(X)
#undef X
};

// 把已通过类型校验的 JSON 值写入对应字段（整数走 long long 精确取值）。
void tuneAssign(const std::string& name, const json& v, Tuning& t) {
#define X(n, tp)                                                                    \
    if (name == #n) {                                                               \
        if constexpr (std::is_same_v<tp, int>) t.n = static_cast<int>(v.get<long long>()); \
        else                                   t.n = v.get<double>();               \
        return;                                                                     \
    }
    SC_TUNING_FIELDS(X)
#undef X
}

// ---------- 错误报告 ----------
void pushErr(std::vector<std::string>& errs, const std::string& file,
             const std::string& path, const std::string& msg) {
    if (path.empty()) errs.push_back(file + ": " + msg);
    else              errs.push_back(file + ": " + path + ": " + msg);
}

// 大小写不敏感的编辑距离（用于"是否想写 X？"提示）
int levCI(const std::string& a, const std::string& b) {
    const size_t n = a.size(), m = b.size();
    std::vector<int> prev(m + 1), cur(m + 1);
    for (size_t j = 0; j <= m; ++j) prev[j] = static_cast<int>(j);
    for (size_t i = 1; i <= n; ++i) {
        cur[0] = static_cast<int>(i);
        for (size_t j = 1; j <= m; ++j) {
            const char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i - 1])));
            const char cb = static_cast<char>(std::tolower(static_cast<unsigned char>(b[j - 1])));
            const int cost = (ca == cb) ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        std::swap(prev, cur);
    }
    return prev[m];
}

// 在候选集中挑一个"最像"的拼写（距离 ≥ 3 视为不相似，返回空串）
std::string suggest(const std::string& word, const std::vector<std::string>& cands) {
    std::string best;
    int bestD = 3;
    for (const std::string& c : cands) {
        const int d = levCI(word, c);
        if (d < bestD) { bestD = d; best = c; }
    }
    return best;
}

// ---------- 文件与 JSON ----------
bool readFileText(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool parseJson(const std::string& text, json& j, const std::string& file,
               std::vector<std::string>& errs) {
    try {
        j = json::parse(text);
        return true;
    } catch (const json::parse_error& e) {
        const size_t off = (e.byte > 0) ? static_cast<size_t>(e.byte - 1) : 0;
        size_t line = 1, col = 1;
        for (size_t i = 0; i < off && i < text.size(); ++i) {
            if (text[i] == '\n') { ++line; col = 1; }
            else                 ++col;
        }
        errs.push_back(file + ": 第 " + std::to_string(line) + " 行第 " +
                       std::to_string(col) + " 列: JSON 解析错误：" + e.what());
        return false;
    }
}

// ---------- 字段访问与校验原语 ----------
const json* getField(const json& o, const char* k) {
    if (!o.is_object()) return nullptr;
    auto it = o.find(k);
    return it == o.end() ? nullptr : &(*it);
}

std::string joinPath(const std::string& base, const char* k) {
    return base.empty() ? std::string(k) : base + "." + k;
}

bool reqStr(const json& o, const char* k, const std::string& base,
            const std::string& file, std::string& out, std::vector<std::string>& errs) {
    const json* v = getField(o, k);
    const std::string p = joinPath(base, k);
    if (!v) { pushErr(errs, file, p, "缺少必填字段"); return false; }
    if (!v->is_string()) { pushErr(errs, file, p, "类型不符（期望字符串）"); return false; }
    out = v->get<std::string>();
    return true;
}

bool reqInt(const json& o, const char* k, const std::string& base,
            const std::string& file, int& out, std::vector<std::string>& errs) {
    const json* v = getField(o, k);
    const std::string p = joinPath(base, k);
    if (!v) { pushErr(errs, file, p, "缺少必填字段"); return false; }
    if (!v->is_number_integer()) { pushErr(errs, file, p, "类型不符（期望整数）"); return false; }
    const long long x = v->get<long long>();
    if (x < 0) { pushErr(errs, file, p, "取值范围：不得为负"); return false; }
    out = static_cast<int>(x);
    return true;
}

bool reqNum(const json& o, const char* k, const std::string& base,
            const std::string& file, double& out, std::vector<std::string>& errs) {
    const json* v = getField(o, k);
    const std::string p = joinPath(base, k);
    if (!v) { pushErr(errs, file, p, "缺少必填字段"); return false; }
    if (!v->is_number()) { pushErr(errs, file, p, "类型不符（期望数值）"); return false; }
    const double x = v->get<double>();
    if (x < 0) { pushErr(errs, file, p, "取值范围：不得为负"); return false; }
    out = x;
    return true;
}

bool reqBool(const json& o, const char* k, const std::string& base,
             const std::string& file, bool& out, std::vector<std::string>& errs) {
    const json* v = getField(o, k);
    const std::string p = joinPath(base, k);
    if (!v) { pushErr(errs, file, p, "缺少必填字段"); return false; }
    if (!v->is_boolean()) { pushErr(errs, file, p, "类型不符（期望布尔）"); return false; }
    out = v->get<bool>();
    return true;
}

// 未知字段：报错并给出拼写建议（契约 §8）
void checkUnknownKeys(const json& o, const std::vector<std::string>& known,
                      const std::string& base, const std::string& file,
                      std::vector<std::string>& errs) {
    if (!o.is_object()) return;
    for (auto it = o.begin(); it != o.end(); ++it) {
        const std::string k = it.key();
        bool ok = false;
        for (const std::string& kk : known) { if (kk == k) { ok = true; break; } }
        if (ok) continue;
        const std::string s = suggest(k, known);
        std::string msg = "未知字段 \"" + k + "\"";
        if (!s.empty()) msg += "（是否想写 \"" + s + "\"？）";
        pushErr(errs, file, base, msg);
    }
}

// UTF-8 码点计数（用于"glyph 必须是单字符"）
size_t utf8Count(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) ++n;   // 非续接字节
    }
    return n;
}

// ---------- 各文件校验 ----------
struct ManifestInfo {
    int         schema = 0;
    std::string packId;
    std::string buildingsFile, techsFile, weathersFile, tuningFile;
};

void validateManifest(const json& m, const std::string& file, ManifestInfo& out,
                      std::vector<std::string>& errs) {
    if (!m.is_object()) { pushErr(errs, file, "", "顶层必须是对象"); return; }
    checkUnknownKeys(m, {"schema", "pack_id", "name", "files"}, "", file, errs);

    const json* sch = getField(m, "schema");
    if (!sch) {
        pushErr(errs, file, "schema", "缺少必填字段");
    } else if (!sch->is_number_integer()) {
        pushErr(errs, file, "schema", "类型不符（期望整数）");
    } else {
        out.schema = sch->get<int>();
        if (out.schema < kMinSchema || out.schema > kMaxSchema) {
            pushErr(errs, file, "schema",
                    "不支持的版本 " + std::to_string(out.schema) + "（引擎支持 " +
                    std::to_string(kMinSchema) + ".." + std::to_string(kMaxSchema) + "）");
        }
    }

    reqStr(m, "pack_id", "", file, out.packId, errs);

    const json* f = getField(m, "files");
    if (!f) {
        pushErr(errs, file, "files", "缺少必填字段");
    } else if (!f->is_object()) {
        pushErr(errs, file, "files", "类型不符（期望对象）");
    } else {
        checkUnknownKeys(*f, {"buildings", "techs", "weathers", "tuning"}, "files", file, errs);
        reqStr(*f, "buildings", "files", file, out.buildingsFile, errs);
        reqStr(*f, "techs",     "files", file, out.techsFile, errs);
        reqStr(*f, "weathers",  "files", file, out.weathersFile, errs);
        reqStr(*f, "tuning",    "files", file, out.tuningFile, errs);
    }
}

void validateBuildings(const json& arr, const std::string& file, ContentPack& out,
                       std::vector<std::string>& errs) {
    const std::vector<std::string> known = {
        "key", "name", "glyph", "color", "cost",
        "build_turns", "workers", "upkeep", "repeatable", "desc"};
    const std::vector<std::string> costKeys = {"metal", "energy", "science"};

    if (!arr.is_array()) { pushErr(errs, file, "", "顶层必须是数组"); return; }
    if (static_cast<int>(arr.size()) != BTYPE_COUNT) {
        pushErr(errs, file, "", "数组长度 " + std::to_string(arr.size()) +
                                    " != 期望 " + std::to_string(BTYPE_COUNT));
    }

    std::vector<std::string> keys, glyphs;
    bool hqFound = false, hqRepeatable = false;

    for (size_t i = 0; i < arr.size(); ++i) {
        const json& e = arr[i];
        const std::string base = "[" + std::to_string(i) + "]";
        if (!e.is_object()) { pushErr(errs, file, base, "元素必须是对象"); continue; }
        checkUnknownKeys(e, known, base, file, errs);

        std::string key, name, glyph, colorName, desc;
        reqStr(e, "key",   base, file, key, errs);
        reqStr(e, "name",  base, file, name, errs);
        reqStr(e, "glyph", base, file, glyph, errs);
        reqStr(e, "color", base, file, colorName, errs);
        reqStr(e, "desc",  base, file, desc, errs);

        int cm = 0, ce = 0, cs = 0;
        const json* c = getField(e, "cost");
        if (!c) {
            pushErr(errs, file, base + ".cost", "缺少必填字段");
        } else if (!c->is_object()) {
            pushErr(errs, file, base + ".cost", "类型不符（期望对象）");
        } else {
            checkUnknownKeys(*c, costKeys, base + ".cost", file, errs);
            reqInt(*c, "metal",   base + ".cost", file, cm, errs);
            reqInt(*c, "energy",  base + ".cost", file, ce, errs);
            reqInt(*c, "science", base + ".cost", file, cs, errs);
        }

        int buildTurns = 0, workers = 0, upkeep = 0;
        reqInt(e, "build_turns", base, file, buildTurns, errs);
        reqInt(e, "workers",     base, file, workers, errs);
        reqInt(e, "upkeep",      base, file, upkeep, errs);

        bool repeatable = false;
        reqBool(e, "repeatable", base, file, repeatable, errs);

        // key：非空、字符集、唯一
        if (!key.empty()) {
            bool valid = true;
            for (char ch : key) {
                if (!(std::islower(static_cast<unsigned char>(ch)) ||
                      std::isdigit(static_cast<unsigned char>(ch)) || ch == '_')) {
                    valid = false;
                }
            }
            if (!valid) pushErr(errs, file, base + ".key", "非法 key（只允许 [a-z0-9_]+）");
            for (const std::string& k : keys) {
                if (k == key) pushErr(errs, file, base + ".key", "重复的 key \"" + key + "\"");
            }
            keys.push_back(key);
        }

        // glyph：单字符、唯一
        if (!glyph.empty()) {
            if (utf8Count(glyph) != 1) pushErr(errs, file, base + ".glyph", "必须是单字符");
            for (const std::string& g : glyphs) {
                if (g == glyph) pushErr(errs, file, base + ".glyph", "重复的 glyph \"" + glyph + "\"");
            }
            glyphs.push_back(glyph);
        }

        // color：具名
        int col = COL_DEF;
        if (!colorName.empty() && !colorFromName(colorName, col)) {
            pushErr(errs, file, base + ".color", "未知颜色 \"" + colorName +
                       "\"（可用：default/grey/red/green/yellow/blue/magenta/cyan/white/bright_white）");
        }

        // 落地（只在索引合法时；失败时整包不会 apply）
        if (i < static_cast<size_t>(BTYPE_COUNT)) {
            BDef& d = out.buildings[i];
            d.key         = out.keep(key);
            d.name        = out.keep(name);
            d.glyph       = glyph.empty() ? '?' : glyph[0];
            d.color       = col;
            d.costMetal   = cm;
            d.costEnergy  = ce;
            d.costScience = cs;
            d.buildTurns  = buildTurns;
            d.workers     = workers;
            d.upkeep      = upkeep;
            d.repeatable  = repeatable;
            d.desc        = out.keep(desc);
        }
        if (key == "hq") { hqFound = true; hqRepeatable = repeatable; }
    }

    if (!hqFound) {
        pushErr(errs, file, "", "缺少必需的建筑 key \"hq\"");
    } else if (hqRepeatable) {
        pushErr(errs, file, "", "建筑 \"hq\" 必须为 repeatable: false");
    }
}

void validateTechs(const json& arr, const std::string& file, ContentPack& out,
                   std::vector<std::string>& errs) {
    const std::vector<std::string> known = {"key", "name", "cost", "requires", "desc"};

    if (!arr.is_array()) { pushErr(errs, file, "", "顶层必须是数组"); return; }
    if (static_cast<int>(arr.size()) != TECH_COUNT) {
        pushErr(errs, file, "", "数组长度 " + std::to_string(arr.size()) +
                                    " != 期望 " + std::to_string(TECH_COUNT));
    }

    std::vector<std::string>              keys;
    std::vector<std::vector<std::string>> allReqs(arr.size());

    for (size_t i = 0; i < arr.size(); ++i) {
        const json& e = arr[i];
        const std::string base = "[" + std::to_string(i) + "]";
        if (!e.is_object()) { pushErr(errs, file, base, "元素必须是对象"); continue; }
        checkUnknownKeys(e, known, base, file, errs);

        std::string key, name, desc;
        reqStr(e, "key",  base, file, key, errs);
        reqStr(e, "name", base, file, name, errs);
        reqStr(e, "desc", base, file, desc, errs);

        int cost = 0;
        reqInt(e, "cost", base, file, cost, errs);

        const json* rq = getField(e, "requires");
        if (!rq) {
            pushErr(errs, file, base + ".requires", "缺少必填字段");
        } else if (!rq->is_array()) {
            pushErr(errs, file, base + ".requires", "类型不符（期望数组）");
        } else {
            for (size_t j = 0; j < rq->size(); ++j) {
                const json& rv = (*rq)[j];
                const std::string rp = base + ".requires[" + std::to_string(j) + "]";
                if (!rv.is_string()) { pushErr(errs, file, rp, "类型不符（期望字符串）"); continue; }
                allReqs[i].push_back(rv.get<std::string>());
            }
        }

        for (const std::string& k : keys) {
            if (k == key) pushErr(errs, file, base + ".key", "重复的 key \"" + key + "\"");
        }
        keys.push_back(key);

        if (i < static_cast<size_t>(TECH_COUNT)) {
            TechDef& d = out.techs[i];
            d.key  = out.keep(key);
            d.name = out.keep(name);
            d.cost = cost;
            d.req  = 0;
            d.desc = out.keep(desc);
        }
    }

    // 解析 requires -> 位掩码（P3a 内部仍是位掩码，索引 = 数组位置 = 枚举值）
    std::unordered_map<std::string, int> indexOf;
    for (size_t i = 0; i < keys.size(); ++i) {
        if (!indexOf.count(keys[i])) indexOf[keys[i]] = static_cast<int>(i);
    }

    std::vector<std::vector<int>> adj(allReqs.size());
    for (size_t i = 0; i < allReqs.size(); ++i) {
        uint32_t mask = 0;
        for (size_t j = 0; j < allReqs[i].size(); ++j) {
            const std::string& rk = allReqs[i][j];
            auto it = indexOf.find(rk);
            const std::string rp = "[" + std::to_string(i) + "].requires[" + std::to_string(j) + "]";
            if (it == indexOf.end()) {
                const std::string s = suggest(rk, keys);
                std::string msg = "未知科技 key \"" + rk + "\"";
                if (!s.empty()) msg += "（是否想写 \"" + s + "\"？）";
                pushErr(errs, file, rp, msg);
                continue;
            }
            if (it->second == static_cast<int>(i)) {
                pushErr(errs, file, rp, "不允许自引用");
                continue;
            }
            mask |= (1u << it->second);
            adj[i].push_back(it->second);
        }
        if (i < static_cast<size_t>(TECH_COUNT)) out.techs[i].req = mask;
    }

    // 科技树无环检测（DFS 三色）
    std::vector<int> color(allReqs.size(), 0);   // 0 未访问 / 1 在栈 / 2 完成
    std::vector<int> stack;
    bool reported = false;
    std::function<void(int)> dfs = [&](int u) {
        if (reported) return;
        color[u] = 1;
        stack.push_back(u);
        for (int v : adj[u]) {
            if (reported) return;
            if (color[v] == 1) {
                std::string chain;
                for (size_t k = 0; k < stack.size(); ++k) {
                    if (stack[k] == v || !chain.empty()) {
                        chain += (chain.empty() ? "" : " -> ");
                        chain += "\"" + (stack[k] < static_cast<int>(keys.size())
                                             ? keys[stack[k]] : std::string("?")) + "\"";
                    }
                }
                chain += " -> \"" + (v < static_cast<int>(keys.size()) ? keys[v] : std::string("?")) + "\"";
                pushErr(errs, file, "[" + std::to_string(u) + "]", "前置科技成环（" + chain + "）");
                reported = true;
                return;
            }
            if (color[v] == 0) dfs(v);
        }
        color[u] = 2;
        stack.pop_back();
    };
    for (size_t i = 0; i < allReqs.size() && !reported; ++i) {
        if (color[i] == 0) dfs(static_cast<int>(i));
    }
}

void validateWeathers(const json& arr, const std::string& file, ContentPack& out,
                      std::vector<std::string>& errs) {
    const std::vector<std::string> known = {"key", "name", "color", "mult", "desc"};
    const std::vector<std::string> multKeys = {"metal", "energy", "food", "science"};

    if (!arr.is_array()) { pushErr(errs, file, "", "顶层必须是数组"); return; }
    if (static_cast<int>(arr.size()) != WEATHER_COUNT) {
        pushErr(errs, file, "", "数组长度 " + std::to_string(arr.size()) +
                                    " != 期望 " + std::to_string(WEATHER_COUNT));
    }

    std::string firstKey;
    for (size_t i = 0; i < arr.size(); ++i) {
        const json& e = arr[i];
        const std::string base = "[" + std::to_string(i) + "]";
        if (!e.is_object()) { pushErr(errs, file, base, "元素必须是对象"); continue; }
        checkUnknownKeys(e, known, base, file, errs);

        std::string key, name, colorName, desc;
        reqStr(e, "key",   base, file, key, errs);
        reqStr(e, "name",  base, file, name, errs);
        reqStr(e, "color", base, file, colorName, errs);
        reqStr(e, "desc",  base, file, desc, errs);
        if (i == 0) firstKey = key;

        int col = COL_DEF;
        if (!colorName.empty() && !colorFromName(colorName, col)) {
            pushErr(errs, file, base + ".color", "未知颜色 \"" + colorName + "\"");
        }

        double mv = 1.0, ev = 1.0, fv = 1.0, sv = 1.0;
        const json* mt = getField(e, "mult");
        if (!mt) {
            pushErr(errs, file, base + ".mult", "缺少必填字段");
        } else if (!mt->is_object()) {
            pushErr(errs, file, base + ".mult", "类型不符（期望对象）");
        } else {
            checkUnknownKeys(*mt, multKeys, base + ".mult", file, errs);
            reqNum(*mt, "metal",   base + ".mult", file, mv, errs);
            reqNum(*mt, "energy",  base + ".mult", file, ev, errs);
            reqNum(*mt, "food",    base + ".mult", file, fv, errs);
            reqNum(*mt, "science", base + ".mult", file, sv, errs);
        }

        if (i < static_cast<size_t>(WEATHER_COUNT)) {
            WeatherDef& d = out.weathers[i];
            d.name    = out.keep(name);
            d.color   = col;
            d.metal   = mv;
            d.energy  = ev;
            d.food    = fv;
            d.science = sv;
            d.desc    = out.keep(desc);
        }
    }

    if (!firstKey.empty() && firstKey != "clear") {
        pushErr(errs, file, "[0].key", "第一个天气必须是 \"clear\"（晴朗）");
    }
}

void validateTuning(const json& o, const std::string& file, ContentPack& out,
                    std::vector<std::string>& errs) {
    if (!o.is_object()) { pushErr(errs, file, "", "顶层必须是对象"); return; }

    std::vector<std::string> names;
    names.reserve(sizeof(kTuneFields) / sizeof(kTuneFields[0]));
    for (const TuneFieldInfo& f : kTuneFields) names.emplace_back(f.name);

    // 多余 / 拼错字段
    for (auto it = o.begin(); it != o.end(); ++it) {
        const std::string k = it.key();
        bool known = false;
        for (const std::string& n : names) { if (n == k) { known = true; break; } }
        if (known) continue;
        const std::string s = suggest(k, names);
        std::string msg = "未知字段 \"" + k + "\"";
        if (!s.empty()) msg += "（是否想写 \"" + s + "\"？）";
        pushErr(errs, file, "", msg);
    }

    // 缺失 / 类型 / 范围
    for (const TuneFieldInfo& f : kTuneFields) {
        const json* v = getField(o, f.name);
        if (!v) {
            pushErr(errs, file, "", std::string("缺少必填字段 \"") + f.name + "\"");
            continue;
        }
        if (f.isInt) {
            if (!v->is_number_integer()) {
                pushErr(errs, file, "", std::string("字段 \"") + f.name + "\" 类型不符（期望整数）");
                continue;
            }
            const long long x = v->get<long long>();
            if (x < 0) {
                pushErr(errs, file, "", std::string("字段 \"") + f.name + "\" 取值范围：不得为负");
                continue;
            }
            if (std::string(f.name) == "maxTurns" && x < 1) {
                pushErr(errs, file, "", "字段 \"maxTurns\" 取值范围：至少为 1");
                continue;
            }
        } else {
            if (!v->is_number()) {
                pushErr(errs, file, "", std::string("字段 \"") + f.name + "\" 类型不符（期望数值）");
                continue;
            }
            if (v->get<double>() < 0) {
                pushErr(errs, file, "", std::string("字段 \"") + f.name + "\" 取值范围：不得为负");
                continue;
            }
        }
        tuneAssign(f.name, *v, out.tuning);
    }
}

// ---------- 全局运行时表的稳定字符串存储 ----------
// 函数内 static：首次使用时构造、程序生命周期内有效；deque 的引用/指针稳定。
const char* poolKeep(const char* s) {
    static std::deque<std::string> pool;
    pool.push_back(s ? std::string(s) : std::string());
    return pool.back().c_str();
}

} // namespace

// =====================================================================
//  对外接口
// =====================================================================

// 元信息 / 目录访问器（定义见本文件下半部分）
int& schemaRef();
std::string& packIdRef();
std::string& dirRef();
bool& dirExplicitRef();

bool loadContentPack(const std::string& dir, ContentPack& out, std::vector<std::string>& errs) {
    errs.clear();

    const std::string manifestPath = dir + "/manifest.json";
    std::string text;
    if (!readFileText(manifestPath, text)) {
        errs.push_back(manifestPath + ": 文件不存在或无法读取");
        return false;
    }
    json m;
    if (!parseJson(text, m, manifestPath, errs)) return false;

    ManifestInfo info;
    validateManifest(m, manifestPath, info, errs);
    if (!errs.empty()) return false;   // manifest 不可用，无法定位其余文件

    out.schema = info.schema;
    out.packId = info.packId;

    auto loadOne = [&](const std::string& filePath,
                       void (*validate)(const json&, const std::string&, ContentPack&,
                                        std::vector<std::string>&)) {
        std::string t;
        if (!readFileText(filePath, t)) {
            errs.push_back(filePath + ": 文件不存在或无法读取");
            return;
        }
        json j;
        if (parseJson(t, j, filePath, errs)) validate(j, filePath, out, errs);
    };

    loadOne(dir + "/" + info.buildingsFile, &validateBuildings);
    loadOne(dir + "/" + info.techsFile,     &validateTechs);
    loadOne(dir + "/" + info.weathersFile,  &validateWeathers);
    loadOne(dir + "/" + info.tuningFile,    &validateTuning);

    return errs.empty();
}

// 把已校验的包落地到全局运行时表（本 TU 内部使用，失败路径不会调用到）。
void applyContent(const ContentPack& pack) {
    for (int i = 0; i < BTYPE_COUNT; ++i) {
        BDEF[i] = pack.buildings[i];
        BDEF[i].key  = poolKeep(pack.buildings[i].key);
        BDEF[i].name = poolKeep(pack.buildings[i].name);
        BDEF[i].desc = poolKeep(pack.buildings[i].desc);
    }
    for (int i = 0; i < TECH_COUNT; ++i) {
        TDEF[i] = pack.techs[i];
        TDEF[i].key  = poolKeep(pack.techs[i].key);
        TDEF[i].name = poolKeep(pack.techs[i].name);
        TDEF[i].desc = poolKeep(pack.techs[i].desc);
    }
    for (int i = 0; i < WEATHER_COUNT; ++i) {
        WDEF[i] = pack.weathers[i];
        WDEF[i].name = poolKeep(pack.weathers[i].name);
        WDEF[i].desc = poolKeep(pack.weathers[i].desc);
    }
    TUNE = pack.tuning;
}

bool initContent(const std::string& dir, std::vector<std::string>& errs) {
    ContentPack pack;
    if (!loadContentPack(dir, pack, errs)) return false;   // 失败：全局表原封不动
    applyContent(pack);
    schemaRef() = pack.schema;
    packIdRef()  = pack.packId;
    return true;
}

// ---- 元信息与目录（函数内 static，避免静态初始化顺序问题） ----
int& schemaRef() { static int v = 0; return v; }
std::string& packIdRef() { static std::string v; return v; }
// 是否被显式指定过内容目录（环境变量或 setContentDir）——显式指定时禁止回退，
// 否则会把「故意构造的坏内容」误判为可用（掩盖错误）。
bool& dirExplicitRef() { static bool v = false; return v; }
int contentSchema() { return schemaRef(); }
const std::string& contentPackId() { return packIdRef(); }

std::string& dirRef() {
    static std::string d = [] {
        const char* e = std::getenv("SC_CONTENT_DIR");
        if (e && *e) { dirExplicitRef() = true; return std::string(e); }
        return std::string(kDefaultContentDir);
    }();
    return d;
}
void setContentDir(const std::string& dir) { dirRef() = dir; dirExplicitRef() = true; }
const std::string& contentDir() { return dirRef(); }

namespace {

// 可执行文件所在目录（读 /proc/self/exe；失败返回空）
std::string exeDir() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return "";
    buf[n] = '\0';
    std::string p(buf);
    const size_t slash = p.find_last_of('/');
    return (slash == std::string::npos) ? std::string(".") : p.substr(0, slash);
}

// 内容目录候选：默认目录优先，其次可执行文件同级的 content/（build/ 或 build/..）
std::vector<std::string> contentCandidates() {
    std::vector<std::string> cands;
    cands.push_back(contentDir());
    const std::string ed = exeDir();
    if (!ed.empty()) {
        cands.push_back(ed + "/content/base");
        cands.push_back(ed + "/../content/base");
        cands.push_back(ed + "/../../content/base");
    }
    return cands;
}

} // namespace

bool ensureContent() {
    static const bool ok = [] {
        std::vector<std::string> firstErrs;
        const std::string primary = contentDir();
        if (initContent(primary, firstErrs)) return true;

        // 仅当内容目录是「默认值」（未被环境变量 / setContentDir 显式指定）时，
        // 才尝试可执行文件同级的候选目录 —— 便于从任意工作目录启动，且不会
        // 把故意构造的坏内容目录回退掉（那会掩盖校验错误）。
        if (!dirExplicitRef()) {
            for (const std::string& cand : contentCandidates()) {
                if (cand == primary) continue;
                std::vector<std::string> e2;
                if (initContent(cand, e2)) { setContentDir(cand); return true; }
            }
        }

        std::fprintf(stderr, "内容加载失败：%s\n", primary.c_str());
        for (const std::string& e : firstErrs) std::fprintf(stderr, "  %s\n", e.c_str());
        return false;
    }();
    return ok;
}

} // namespace sc
