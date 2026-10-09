// 星际争霸：殖民地 (Star Colony) —— C++20 命令行殖民地经营游戏
// Copyright (C) 2026 Star Colony contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published
// by the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// See the LICENSE file at the project root for the full license text.
// SPDX-License-Identifier: AGPL-3.0-or-later

// 星际争霸：殖民地 —— JSON-RPC 服务端（P2：微内核/前后端分离）
//
// 严格实现 docs/PROTOCOL.md（v1，已冻结）：
//   * 分帧：newline-delimited，一行一个 JSON 对象；请求走 stdin，响应走 stdout；
//     stderr 只放调试信息；服务端「绝不主动推送」，每条响应与请求同序配对。
//   * 两种失败严格区分：协议/传输错误 -> JSON-RPC 标准 error 对象；
//     游戏内业务失败 -> result.ok=false + result.result.code（这是正常响应）。
//   * code 用 ActCode 的枚举名字符串（sc::actCodeName），禁止整数下标。
//   * shutdown 响应后 exit(0)。
//
// 编译：
//   g++ -std=c++20 -O2 -Wall -Wextra -Isrc src/rpc_server.cpp src/game.cpp -o build/starcolony-rpc
#include "game.hpp"
#include "rpc_json.hpp"
#include "types.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <iostream>
#include <string>
#include <vector>

namespace sc {
namespace {

using json::Json;

// ---------- JSON-RPC 2.0 标准错误码（契约 §2） ----------
constexpr long long kParseError     = -32700;
constexpr long long kInvalidRequest = -32600;
constexpr long long kMethodNotFound = -32601;
constexpr long long kInvalidParams  = -32602;

// ============================================================
//  响应构造
// ============================================================
Json makeResult(const Json& id, Json result) {
    Json resp = Json::object();
    resp.set("jsonrpc", Json("2.0"));
    resp.set("id", id);
    resp.set("result", std::move(result));
    return resp;
}

Json makeError(const Json& id, long long code, const std::string& message) {
    Json resp = Json::object();
    resp.set("jsonrpc", Json("2.0"));
    resp.set("id", id);
    Json err = Json::object();
    err.set("code", Json(code));
    err.set("message", Json(message));
    resp.set("error", std::move(err));
    return resp;
}

// ============================================================
//  参数提取（不存在返回 nullptr；类型错误交给调用方判定）
// ============================================================
const Json* member(const Json* obj, const char* key) {
    return (obj && obj->isObject()) ? obj->find(key) : nullptr;
}

bool reqStr(const Json* params, const char* key, std::string& out, std::string& err) {
    const Json* v = member(params, key);
    if (!v) { err = std::string("缺少参数 ") + key; return false; }
    if (!v->isString()) { err = std::string("参数 ") + key + " 必须是字符串"; return false; }
    out = v->asString();
    return true;
}

bool reqInt(const Json* params, const char* key, long long& out, std::string& err) {
    const Json* v = member(params, key);
    if (!v) { err = std::string("缺少参数 ") + key; return false; }
    if (!v->isNumber()) { err = std::string("参数 ") + key + " 必须是整数"; return false; }
    out = v->asInt();
    return true;
}

// ============================================================
//  领域对象 -> JSON
// ============================================================
Json buildingsInfoJson() {
    Json arr = Json::array();
    for (int i = 0; i < BTYPE_COUNT; ++i) {
        const BDef& d = BDEF[static_cast<size_t>(i)];
        Json o = Json::object();
        o.set("key", Json(d.key));
        o.set("name", Json(d.name));
        o.set("glyph", Json(std::string(1, d.glyph)));
        o.set("color", Json(static_cast<long long>(d.color)));
        o.set("costMetal", Json(static_cast<long long>(d.costMetal)));
        o.set("costEnergy", Json(static_cast<long long>(d.costEnergy)));
        o.set("costScience", Json(static_cast<long long>(d.costScience)));
        o.set("buildTurns", Json(static_cast<long long>(d.buildTurns)));
        o.set("workers", Json(static_cast<long long>(d.workers)));
        o.set("upkeep", Json(static_cast<long long>(d.upkeep)));
        o.set("repeatable", Json(d.repeatable));
        o.set("desc", Json(d.desc));
        arr.push(std::move(o));
    }
    return arr;
}

Json techsInfoJson() {
    Json arr = Json::array();
    for (int i = 0; i < TECH_COUNT; ++i) {
        const TechDef& d = TDEF[static_cast<size_t>(i)];
        Json o = Json::object();
        o.set("key", Json(d.key));
        o.set("name", Json(d.name));
        o.set("cost", Json(static_cast<long long>(d.cost)));
        Json req = Json::array();
        for (int k = 0; k < TECH_COUNT; ++k) {
            if (d.req & techBit(static_cast<Tech>(k)))
                req.push(Json(TDEF[static_cast<size_t>(k)].key));
        }
        o.set("req", std::move(req));
        o.set("desc", Json(d.desc));
        arr.push(std::move(o));
    }
    return arr;
}

Json weathersInfoJson() {
    Json arr = Json::array();
    for (int i = 0; i < WEATHER_COUNT; ++i) {
        const WeatherDef& d = WDEF[static_cast<size_t>(i)];
        Json o = Json::object();
        o.set("name", Json(d.name));
        o.set("color", Json(static_cast<long long>(d.color)));
        o.set("metal", Json(d.metal));
        o.set("energy", Json(d.energy));
        o.set("food", Json(d.food));
        o.set("science", Json(d.science));
        o.set("desc", Json(d.desc));
        arr.push(std::move(o));
    }
    return arr;
}

Json contentInfoResult() {
    Json r = Json::object();
    r.set("ok", Json(true));
    r.set("mapW", Json(MAP_W));
    r.set("mapH", Json(MAP_H));
    r.set("maxTurns", Json(TUNE.maxTurns));
    r.set("buildings", buildingsInfoJson());
    r.set("techs", techsInfoJson());
    r.set("weathers", weathersInfoJson());
    return r;
}

Json logEntryJson(const LogEntry& e) {
    Json o = Json::object();
    o.set("turn", Json(static_cast<long long>(e.turn)));
    o.set("code", Json(std::string(actCodeName(e.code))));
    Json ints = Json::array();
    for (long long v : e.ints) ints.push(Json(v));
    o.set("ints", std::move(ints));
    Json strs = Json::array();
    for (const std::string& s : e.strings) strs.push(Json(s));
    o.set("strings", std::move(strs));
    o.set("text", Json(e.text()));
    return o;
}

Json actionResultJson(const ActionResult& a) {
    Json o = Json::object();
    o.set("code", Json(std::string(actCodeName(a.code))));
    o.set("text", Json(a.text()));
    Json args = Json::array();
    for (const std::string& s : a.args) args.push(Json(s));
    o.set("args", std::move(args));
    return o;
}

Json snapshotJson(const GameSnapshot& s) {
    Json o = Json::object();
    o.set("turn", Json(static_cast<long long>(s.turn)));
    o.set("metal", Json(static_cast<long long>(s.metal)));
    o.set("energy", Json(static_cast<long long>(s.energy)));
    o.set("food", Json(static_cast<long long>(s.food)));
    o.set("science", Json(static_cast<long long>(s.science)));
    o.set("pop", Json(static_cast<long long>(s.pop)));
    o.set("housing", Json(static_cast<long long>(s.housing)));
    o.set("morale", Json(static_cast<long long>(s.morale)));
    o.set("weather", Json(static_cast<long long>(s.weather)));
    o.set("weatherLeft", Json(static_cast<long long>(s.weatherLeft)));
    o.set("waveIn", Json(static_cast<long long>(s.waveIn)));
    o.set("waveStrengthEstimate", Json(static_cast<long long>(s.waveStrengthEstimate)));
    o.set("defense", Json(static_cast<long long>(s.defense)));
    o.set("metalIn", Json(static_cast<long long>(s.metalIn)));
    o.set("energyIn", Json(static_cast<long long>(s.energyIn)));
    o.set("foodIn", Json(static_cast<long long>(s.foodIn)));
    o.set("scienceIn", Json(static_cast<long long>(s.scienceIn)));
    o.set("energyUp", Json(static_cast<long long>(s.energyUp)));
    o.set("foodUp", Json(static_cast<long long>(s.foodUp)));
    o.set("metalNet", Json(static_cast<long long>(s.metalNet)));
    o.set("energyNet", Json(static_cast<long long>(s.energyNet)));
    o.set("foodNet", Json(static_cast<long long>(s.foodNet)));
    o.set("scienceNet", Json(static_cast<long long>(s.scienceNet)));
    o.set("brownout", Json(s.brownout));
    o.set("starving", Json(s.starving));
    o.set("over", Json(s.over));
    o.set("won", Json(s.won));
    o.set("endReason", Json(s.endReason));
    o.set("colonyName", Json(s.colonyName));

    Json blds = Json::array();
    for (const BuildingView& b : s.buildings) {
        Json bv = Json::object();
        bv.set("id", Json(static_cast<long long>(b.id)));
        bv.set("type", Json(static_cast<long long>(b.type)));
        bv.set("typeKey", Json(b.typeKey));
        bv.set("x", Json(static_cast<long long>(b.x)));
        bv.set("y", Json(static_cast<long long>(b.y)));
        bv.set("buildLeft", Json(static_cast<long long>(b.buildLeft)));
        bv.set("damaged", Json(static_cast<long long>(b.damaged)));
        bv.set("enabled", Json(b.enabled));
        bv.set("alive", Json(b.alive));
        bv.set("assigned", Json(static_cast<long long>(b.assigned)));
        bv.set("workerNeed", Json(static_cast<long long>(b.workerNeed)));
        blds.push(std::move(bv));
    }
    o.set("buildings", std::move(blds));

    Json tiles = Json::array();
    for (const TileView& t : s.tiles) {
        Json tv = Json::object();
        tv.set("terrain", Json(static_cast<long long>(t.terrain)));
        tv.set("ore", Json(static_cast<long long>(t.ore)));
        tv.set("richness", Json(static_cast<long long>(t.richness)));
        tv.set("building", Json(static_cast<long long>(t.building)));
        tiles.push(std::move(tv));
    }
    o.set("tiles", std::move(tiles));

    Json log = Json::array();
    for (const LogEntry& e : s.log) log.push(logEntryJson(e));
    o.set("log", std::move(log));

    Json techs = Json::array();
    for (const std::string& t : s.techs) techs.push(Json(t));
    o.set("techs", std::move(techs));

    Json assigned = Json::array();
    for (int a : s.assigned) assigned.push(Json(static_cast<long long>(a)));
    o.set("assigned", std::move(assigned));

    // ---- P2.1 新增（纯追加）----
    o.set("idleWorkers", Json(static_cast<long long>(s.idleWorkers)));
    o.set("seed", Json(static_cast<long long>(s.seed)));

    // 待决事件（契约 §4.4.1）：无事件时输出 null，有事件时输出对象。
    // 注意：hasPending 是 C++ 侧内部标志，**不**单独序列化为字段。
    if (s.hasPending) {
        Json p = Json::object();
        p.set("kind", Json(s.pending.kind));
        p.set("title", Json(s.pending.title));
        p.set("text", Json(s.pending.text));
        Json opts = Json::array();
        for (const std::string& opt : s.pending.options) opts.push(Json(opt));
        p.set("options", std::move(opts));
        o.set("pending", std::move(p));
    } else {
        o.set("pending", Json());   // null
    }
    return o;
}

// ============================================================
//  command 的「新增日志」增量
// ============================================================
// 契约 §4.6：command 返回的 log 是「本次命令新产生的日志」（失败示例为 []）。
// 引擎日志是上限 400 的 deque，中后期长期处于满仓状态，此时「新增条数」无法由前后 size 直接得出，
// 因此用「旧日志尾部 = 新日志头部」这一性质，从尾到头找最大的、仍保留的旧条目数 m，
// 则新增条目 = 新日志的 [m, end)。
bool logEq(const LogEntry& a, const LogEntry& b) {
    return a.turn == b.turn && a.code == b.code && a.ints == b.ints && a.strings == b.strings;
}

Json logDeltaJson(const std::vector<LogEntry>& before, const std::deque<LogEntry>& after) {
    const std::vector<LogEntry> aft(after.begin(), after.end());
    const std::size_t n = aft.size();
    const std::size_t b = before.size();

    std::size_t kept = 0;   // 新日志开头仍保留的旧条目数
    for (std::size_t cand = std::min(n, b) + 1; cand-- > 0;) {
        bool match = true;
        for (std::size_t i = 0; i < cand; ++i) {
            if (!logEq(aft[i], before[b - cand + i])) { match = false; break; }
        }
        if (match) { kept = cand; break; }
    }

    Json arr = Json::array();
    for (std::size_t i = kept; i < n; ++i) arr.push(logEntryJson(aft[i]));
    return arr;
}

// ============================================================
//  服务端
// ============================================================
class Server {
public:
    Server(uint32_t seed, std::string name) : seed_(seed), name_(std::move(name)) {
        game_.newGame(seed_, name_);
    }

    // 处理一行请求，返回一行响应文本；shouldExit 置位表示 shutdown。
    std::string processLine(const std::string& line, bool& shouldExit) {
        Json req;
        std::string perr;
        if (!json::parse(line, req, perr))
            return makeError(Json(), kParseError, "解析错误：" + perr).dump();

        if (!req.isObject())
            return makeError(Json(), kInvalidRequest, "非法请求：顶层必须是 JSON 对象").dump();

        const Json* idp = req.find("id");
        // JSON-RPC 2.0：id 只允许 String / Number / Null。若为对象/数组/布尔等非法形态，
        // 返回 -32600 且响应 id 置 null（不原样回填非法值）。
        Json id;   // 默认 null（缺省或显式 null 都回填 null）
        if (idp && !idp->isNull()) {
            if (idp->isNumber() || idp->isString()) {
                id = *idp;
            } else {
                return makeError(Json(), kInvalidRequest, "非法请求：id 必须是整数或字符串").dump();
            }
        }

        const Json* methp = req.find("method");
        if (!methp || !methp->isString())
            return makeError(id, kInvalidRequest, "非法请求：缺少 method").dump();

        const std::string method = methp->asString();
        const Json* params = req.find("params");

        if (method == "ping")          return handlePing(id).dump();
        if (method == "new_game")      return handleNewGame(id, params).dump();
        if (method == "content_info")  return handleContentInfo(id).dump();
        if (method == "snapshot")      return handleSnapshot(id).dump();
        if (method == "preview_build") return handlePreviewBuild(id, params).dump();
        if (method == "command")       return handleCommand(id, params).dump();
        if (method == "save")          return handleSave(id, params).dump();
        if (method == "load")          return handleLoad(id, params).dump();
        if (method == "validate")      return handleValidate(id).dump();
        if (method == "shutdown") {
            shouldExit = true;
            Json r = Json::object();
            r.set("ok", Json(true));
            return makeResult(id, std::move(r)).dump();
        }
        return makeError(id, kMethodNotFound, "未知方法：" + method).dump();
    }

private:
    Game        game_;
    uint32_t    seed_;
    std::string name_;

    Json handlePing(const Json& id) {
        Json r = Json::object();
        r.set("ok", Json(true));
        r.set("pong", Json(true));
        return makeResult(id, std::move(r));
    }

    Json handleNewGame(const Json& id, const Json* params) {
        uint32_t    seed = static_cast<uint32_t>(std::time(nullptr));
        std::string name = "新曙光";
        if (const Json* sv = member(params, "seed")) {
            if (!sv->isNumber()) return makeError(id, kInvalidParams, "参数 seed 必须是整数");
            seed = static_cast<uint32_t>(sv->asInt());
        }
        if (const Json* nv = member(params, "name")) {
            if (!nv->isString()) return makeError(id, kInvalidParams, "参数 name 必须是字符串");
            name = nv->asString();
        }
        game_.newGame(seed, name);
        seed_ = seed;
        name_ = name;
        Json r = Json::object();
        r.set("ok", Json(true));
        r.set("snapshot", snapshotJson(game_.snapshot()));
        return makeResult(id, std::move(r));
    }

    Json handleContentInfo(const Json& id) { return makeResult(id, contentInfoResult()); }

    Json handleSnapshot(const Json& id) {
        Json r = Json::object();
        r.set("ok", Json(true));
        r.set("snapshot", snapshotJson(game_.snapshot()));
        return makeResult(id, std::move(r));
    }

    // preview_build 响应体（契约 §4.6.1）。
    // buildable = 规则层（地形/占用/前置）；affordable = 资源是否够付。二者相互独立，
    // 前端应分别标注「地形不符」与「资源不足」，不得把后者塞进前者。
    Json previewPayload(bool ok, bool buildable, const std::string& reason, const BDef* d) {
        Json r = Json::object();
        r.set("ok", Json(ok));
        r.set("buildable", Json(buildable));
        r.set("reason", Json(reason));
        Json cost = Json::object();
        Json aff  = Json::object();
        if (d) {
            cost.set("metal", Json(static_cast<long long>(d->costMetal)));
            cost.set("energy", Json(static_cast<long long>(d->costEnergy)));
            cost.set("science", Json(static_cast<long long>(d->costScience)));
            const Resources& res = game_.res();          // 只读，不改状态
            aff.set("metal", Json(res.metal >= d->costMetal));
            aff.set("energy", Json(res.energy >= d->costEnergy));
            aff.set("science", Json(res.science >= d->costScience));
        } else {   // 未知 key：无从谈造价
            cost.set("metal", Json(0));
            cost.set("energy", Json(0));
            cost.set("science", Json(0));
            aff.set("metal", Json(true));
            aff.set("energy", Json(true));
            aff.set("science", Json(true));
        }
        r.set("cost", std::move(cost));
        r.set("affordable", std::move(aff));
        return r;
    }

    // 失败时额外给出顶层 code 与 result（与 command{build} 同形，前端可统一处理）
    Json previewFailure(const ActionResult& ar, const std::string& reason, const BDef* d) {
        Json r = previewPayload(false, false, reason, d);
        r.set("code", Json(std::string(actCodeName(ar.code))));
        r.set("result", actionResultJson(ar));
        return r;
    }

    // preview_build：建造可行性的**只读**查询——不消耗 rng、不改动任何状态。
    Json handlePreviewBuild(const Json& id, const Json* params) {
        if (!params || !params->isObject())
            return makeError(id, kInvalidParams, "preview_build 需要 params 对象");
        std::string key;
        long long   x = 0, y = 0;
        std::string err;
        if (!reqStr(params, "key", key, err)) return makeError(id, kInvalidParams, err);
        if (!reqInt(params, "x", x, err))     return makeError(id, kInvalidParams, err);
        if (!reqInt(params, "y", y, err))     return makeError(id, kInvalidParams, err);
        const int xi = static_cast<int>(x);
        const int yi = static_cast<int>(y);

        // 待决事件期间同样受核心规则约束（与 command 各 action 一致）
        if (game_.hasPending()) {
            const ActionResult ar{false, ActCode::BlockedByPending, {}};
            return makeResult(id, previewFailure(ar, ar.text(), nullptr));
        }

        // key 匹配规则与 doBuild 一致（接受 key 或中文名）
        int idx = -1;
        for (int i = 0; i < BTYPE_COUNT; ++i)
            if (key == BDEF[static_cast<size_t>(i)].key ||
                key == BDEF[static_cast<size_t>(i)].name) idx = i;
        if (idx < 0) {
            const ActionResult ar{false, ActCode::BuildUnknownType, {key}};
            return makeResult(id, previewFailure(ar, ar.text(), nullptr));
        }

        const BDef& d = BDEF[static_cast<size_t>(idx)];

        // 坐标越界：属于参数非法（ok:false），区别于「地形不符」（ok:true + buildable:false）
        if (xi < 0 || xi >= MAP_W || yi < 0 || yi >= MAP_H) {
            std::string why;
            game_.buildableTerrain(static_cast<BType>(idx), xi, yi, &why);   // 纯查询，取「坐标超出地图范围」
            const ActionResult ar{false, ActCode::BuildBlocked, {why}};
            return makeResult(id, previewFailure(ar, why, &d));
        }

        std::string why;
        const bool canBuild = game_.buildableTerrain(static_cast<BType>(idx), xi, yi, &why);
        return makeResult(id, previewPayload(true, canBuild, why, &d));
    }

    Json handleCommand(const Json& id, const Json* params) {
        if (!params || !params->isObject())
            return makeError(id, kInvalidParams, "command 需要 params 对象");
        const Json* act = params->find("action");
        if (!act || !act->isString())
            return makeError(id, kInvalidParams, "缺少参数 action");

        const std::string action = act->asString();
        const std::vector<LogEntry> before(game_.log().begin(), game_.log().end());

        ActionResult ar;
        std::string  err;
        if (action == "build") {
            std::string key;
            long long   x = 0, y = 0;
            if (!reqStr(params, "key", key, err)) return makeError(id, kInvalidParams, err);
            if (!reqInt(params, "x", x, err))     return makeError(id, kInvalidParams, err);
            if (!reqInt(params, "y", y, err))     return makeError(id, kInvalidParams, err);
            ar = game_.doBuild(key, static_cast<int>(x), static_cast<int>(y));
        } else if (action == "demolish") {
            long long v = 0;
            if (!reqInt(params, "id", v, err)) return makeError(id, kInvalidParams, err);
            ar = game_.doDemolish(static_cast<int>(v));
        } else if (action == "toggle") {
            long long v = 0;
            if (!reqInt(params, "id", v, err)) return makeError(id, kInvalidParams, err);
            ar = game_.doToggle(static_cast<int>(v));
        } else if (action == "focus") {
            long long v = 0;
            if (!reqInt(params, "id", v, err)) return makeError(id, kInvalidParams, err);
            ar = game_.doFocus(static_cast<int>(v));
        } else if (action == "research") {
            std::string key;
            if (!reqStr(params, "key", key, err)) return makeError(id, kInvalidParams, err);
            ar = game_.doResearch(key);
        } else if (action == "answer") {
            long long v = 0;
            if (!reqInt(params, "option", v, err)) return makeError(id, kInvalidParams, err);
            ar = game_.answer(static_cast<int>(v));
        } else if (action == "advance") {
            // advanceTurn() 返回 void：用「周期是否前进」读出核心的决定，而不是在前端重复实现
            // 「待决时禁止推进」这条规则（规则住在 core）。待决被核心挡住时给出 BlockedByPending。
            const int turnBefore = game_.turn();
            game_.advanceTurn();
            if (game_.turn() != turnBefore) {
                ar.ok   = true;
                ar.code = ActCode::Ok;
            } else if (game_.hasPending()) {
                ar.ok   = false;
                ar.code = ActCode::BlockedByPending;
            } else {
                ar.ok   = true;   // 已终局等：advanceTurn 无副作用，保持改造前的 ok 语义
                ar.code = ActCode::Ok;
            }
        } else {
            return makeError(id, kInvalidParams, "未知 action：" + action);
        }

        Json r = Json::object();
        r.set("ok", Json(ar.ok));
        r.set("result", actionResultJson(ar));
        r.set("log", logDeltaJson(before, game_.log()));
        r.set("snapshot", snapshotJson(game_.snapshot()));
        return makeResult(id, std::move(r));
    }

    // 统一的存档/读档响应体（含 result / log / snapshot）
    //
    // 契约 §4.7 中 save 与 load 的示例 log 均为 []：
    // 「log」语义是「本次操作新产生的日志」，而存/读档不产生日志条目；
    // 读档后的完整日志由 snapshot.log 承载，前端据此重绘。因此这里恒定返回空数组。
    Json fileOpResponse(bool ok, const std::string& code, const std::string& text) {
        Json r = Json::object();
        r.set("ok", Json(ok));
        Json res = Json::object();
        res.set("code", Json(code));
        res.set("text", Json(text));
        res.set("args", Json::array());
        r.set("result", std::move(res));
        r.set("log", Json::array());
        r.set("snapshot", snapshotJson(game_.snapshot()));
        return r;
    }

    Json handleSave(const Json& id, const Json* params) {
        std::string path = "save.txt";
        if (const Json* pv = member(params, "path")) {
            if (!pv->isString()) return makeError(id, kInvalidParams, "参数 path 必须是字符串");
            path = pv->asString();
        }
        const std::string msg = game_.saveTo(path);
        const bool ok = msg.rfind("无法写入文件", 0) != 0;   // 引擎失败时返回「无法写入文件：…」
        return makeResult(id, fileOpResponse(ok, ok ? "Ok" : "LoadFailed", ok ? "" : msg));
    }

    Json handleLoad(const Json& id, const Json* params) {
        std::string path = "save.txt";
        if (const Json* pv = member(params, "path")) {
            if (!pv->isString()) return makeError(id, kInvalidParams, "参数 path 必须是字符串");
            path = pv->asString();
        }
        const std::string msg = game_.loadFrom(path);
        const bool ok = msg.rfind("读取失败", 0) != 0;       // 引擎失败时返回「读取失败：…」
        static const std::string kPrefix = "读取失败：";
        const std::string reason = (msg.rfind(kPrefix, 0) == 0) ? msg.substr(kPrefix.size()) : msg;
        return makeResult(id, fileOpResponse(ok, ok ? "Ok" : "LoadFailed", ok ? "" : reason));
    }

    // P6 预留：现在返回 ok:true 与空 errors
    Json handleValidate(const Json& id) {
        Json r = Json::object();
        r.set("ok", Json(true));
        r.set("errors", Json::array());
        return makeResult(id, std::move(r));
    }
};

std::string stripTrailingNewline(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

bool isBlank(const std::string& s) {
    for (char c : s)
        if (!std::isspace(static_cast<unsigned char>(c))) return false;
    return true;
}

} // namespace
} // namespace sc

int main(int argc, char** argv) {
    using namespace sc;

    uint32_t    seed = static_cast<uint32_t>(std::time(nullptr));
    std::string name = "新曙光";

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if ((a == "--seed" || a == "-s") && i + 1 < argc) {
            seed = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if ((a == "--name" || a == "-n") && i + 1 < argc) {
            name = argv[++i];
        } else if (a == "--help" || a == "-h") {
            std::cout << "starcolony-rpc —— 星际争霸：殖民地 RPC 服务端\n"
                         "用法：starcolony-rpc [选项]\n"
                         "  --seed N     初始随机种子（默认取当前时间）\n"
                         "  --name 名字  初始殖民地名称（默认「新曙光」）\n"
                         "  --help       显示本帮助\n"
                         "协议：一行一个 JSON-RPC 2.0 请求（stdin），一行一个响应（stdout）。\n";
            return 0;
        } else {
            std::cerr << "未知参数：" << a << "\n";
            return 2;
        }
    }

    Server server(seed, name);

    std::ios::sync_with_stdio(false);
    std::string line;
    while (std::getline(std::cin, line)) {
        line = stripTrailingNewline(std::move(line));
        if (isBlank(line)) continue;   // 空行不是请求，静默忽略，避免应答错位
        bool shouldExit = false;
        const std::string resp = server.processLine(line, shouldExit);
        std::cout << resp << '\n';
        std::cout.flush();
        if (shouldExit) return 0;
    }
    return 0;
}
