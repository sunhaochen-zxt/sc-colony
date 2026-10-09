// 星际争霸：殖民地 (Star Colony) —— C++20 命令行殖民地经营游戏
// Copyright (C) 2026 Star Colony contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published
// by the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// See the LICENSE file at the project root for the full license text.
// SPDX-License-Identifier: AGPL-3.0-or-later

// 星际争霸：殖民地 —— 引擎与界面之间的“协议层”（P1）
//
// 本文件把引擎从“中文日志即协议”中解放出来：
//   * 行动结果用 ActionResult（结构化 ActCode + 参数）表达，界面/工具只需读 .code；
//   * 日志用 LogEntry（结构化 ActCode + 参数）表达；
//   * 同时提供 text()/operator std::string() 兼容层，产出与改造前逐字一致的中文，
//     使既有 edge_tests / ai.hpp / sweep.cpp 里的 find("中文") 与字符串拼接无需改动。
//
// 本文件只依赖标准库，不依赖终端、不依赖 types.hpp，可作为纯数据协议被任意前端复用。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace sc {

// ============================================================
//  行动 / 日志的结构化代码
// ============================================================
// 约定：Ok = 0；其余按“行动结果”与“结构化日志”两大类排列。
// 前端只需 switch(code) 即可，无需再解析任何中文文本。
enum class ActCode : int {
    Ok = 0,

    // ---------------- 行动结果（ActionResult）----------------
    BuildUnknownType,   // 未知建筑类型
    BuildBlocked,       // 无法建造（含具体原因字符串）
    BuildStarted,       // 开始建造
    DemolishInvalid,    // 无效建筑编号
    DemolishHQ,         // 指挥中心不可拆除
    DemolishDone,       // 已拆除并回收金属
    ToggleInvalid,      // 无效建筑编号
    ToggleDone,         // 开/关成功
    FocusInvalid,       // 无效建筑编号
    FocusDone,          // 设为优先成功
    ResearchUnknown,    // 未知科技
    ResearchDup,        // 已经研究过
    ResearchPrereq,     // 前置科技不足
    ResearchNoScience,  // 科研点不足
    ResearchDone,       // 研究完成
    AnswerNone,         // 当前没有待处理事件
    AnswerInvalid,      // 无效选项
    AnswerChoice,       // 已选择某个选项（返回值：“选择：…”）

    // ---------------- 结构化日志（LogEntry）----------------
    Text,               // 兜底：strings[0] 即原始文本（兼容层与读档用）
    TurnHeader,         // ── 周期 N 结算 ──
    PopBorn,            // 新殖民者诞生
    WeatherChange,      // 天气转入
    EventRefugees,      // 事件：难民船请求降落
    EventMarket,        // 事件：黑市商人来访
    EventSignal,        // 事件：神秘信号
    EventLifeSupport,   // 事件：维生系统故障
    EventMeteorHit,     // 陨石击中建筑
    EventMeteorMiss,    // 陨石落在荒地
    EventProspectFound, // 勘探发现新矿脉
    EventProspectNone,  // 勘探一无所获
    EventVentFound,     // 地表裂开露出地热口
    EventVentNone,      // 地热勘探没有结果
    EventFestival,      // 丰收节
    EventCaravan,       // 商队抵达
    LogChoice,          // 玩家选择（日志侧，带 “▶ ” 前缀）
    RefugeesSettled,    // 难民已安置
    OvercrowdWarn,      // 人口超编警告
    RefugeesPartial,    // 食物不足只接收一部分
    RefugeesRefused,    // 拒绝降落
    RefugeesSeized,     // 征用补给
    MarketNoMetal,      // 黑市：金属不足
    MarketTradeMetal,   // 黑市：金属换科研
    MarketNoEnergy,     // 黑市：能源不足
    MarketTradeEnergy,  // 黑市：能源换金属
    MarketLeave,        // 黑市：礼貌送客
    SignalSuccess,      // 信号：调查成功
    SignalSwarm,        // 信号：惊动虫群
    SignalIgnore,       // 信号：忽略
    LifeSupportFixed,   // 维生系统修复完成
    LifeSupportPoor,    // 维生系统临时修补
    LifeSupportFail,    // 维生系统带病运转
    WaveIncoming,       // 虫潮来袭
    WaveRepelled,       // 防线击退虫群
    BuildingDamaged,    // 建筑被虫群破坏
    WaveBreached,       // 防线被突破
    Repaired,           // 建筑修复完毕
    Brownout,           // 能源透支
    Starve,             // 食物短缺饿死
    OreDepleted,        // 矿脉枯竭
    BuildingBuilt,      // 建筑建造完成
    OvercrowdLeft,      // 超编流失人口
    AcidRain,           // 酸雨腐蚀建筑
    GateTheoryUnlocked, // 星门理论解锁
    NewGameText,        // 新局开场白
    GoalText,           // 新局目标
    HintText,           // 新局提示
    GameOver,           // 结局说明（strings[0] 即 endReason_，渲染与改造前逐字一致）
    BlockedByPending,   // 有事件待决：该操作被核心拦截（规则住在 core，前端不再各自实现）
};

// ============================================================
//  ActCode 的稳定名字（协议用）
// ============================================================
// 契约（docs/PROTOCOL.md §3）要求 ActCode 以「枚举名字符串」传输，禁止用整数下标：
// 枚举会随内容扩展而新增，整数下标会漂移，字符串名稳定、可读、便于前端 switch。
// 覆盖全部枚举值；default 返回 "Unknown"（新枚举忘记登记时的兜底，不崩溃）。
inline const char* actCodeName(ActCode c) {
    switch (c) {
    case ActCode::Ok:                return "Ok";
    // 行动结果
    case ActCode::BuildUnknownType:  return "BuildUnknownType";
    case ActCode::BuildBlocked:      return "BuildBlocked";
    case ActCode::BuildStarted:      return "BuildStarted";
    case ActCode::DemolishInvalid:   return "DemolishInvalid";
    case ActCode::DemolishHQ:        return "DemolishHQ";
    case ActCode::DemolishDone:      return "DemolishDone";
    case ActCode::ToggleInvalid:     return "ToggleInvalid";
    case ActCode::ToggleDone:        return "ToggleDone";
    case ActCode::FocusInvalid:      return "FocusInvalid";
    case ActCode::FocusDone:         return "FocusDone";
    case ActCode::ResearchUnknown:   return "ResearchUnknown";
    case ActCode::ResearchDup:       return "ResearchDup";
    case ActCode::ResearchPrereq:    return "ResearchPrereq";
    case ActCode::ResearchNoScience: return "ResearchNoScience";
    case ActCode::ResearchDone:      return "ResearchDone";
    case ActCode::AnswerNone:        return "AnswerNone";
    case ActCode::AnswerInvalid:     return "AnswerInvalid";
    case ActCode::AnswerChoice:      return "AnswerChoice";
    // 结构化日志
    case ActCode::Text:              return "Text";
    case ActCode::TurnHeader:        return "TurnHeader";
    case ActCode::PopBorn:           return "PopBorn";
    case ActCode::WeatherChange:     return "WeatherChange";
    case ActCode::EventRefugees:     return "EventRefugees";
    case ActCode::EventMarket:       return "EventMarket";
    case ActCode::EventSignal:       return "EventSignal";
    case ActCode::EventLifeSupport:  return "EventLifeSupport";
    case ActCode::EventMeteorHit:    return "EventMeteorHit";
    case ActCode::EventMeteorMiss:   return "EventMeteorMiss";
    case ActCode::EventProspectFound:return "EventProspectFound";
    case ActCode::EventProspectNone: return "EventProspectNone";
    case ActCode::EventVentFound:    return "EventVentFound";
    case ActCode::EventVentNone:     return "EventVentNone";
    case ActCode::EventFestival:     return "EventFestival";
    case ActCode::EventCaravan:      return "EventCaravan";
    case ActCode::LogChoice:         return "LogChoice";
    case ActCode::RefugeesSettled:   return "RefugeesSettled";
    case ActCode::OvercrowdWarn:     return "OvercrowdWarn";
    case ActCode::RefugeesPartial:   return "RefugeesPartial";
    case ActCode::RefugeesRefused:   return "RefugeesRefused";
    case ActCode::RefugeesSeized:    return "RefugeesSeized";
    case ActCode::MarketNoMetal:     return "MarketNoMetal";
    case ActCode::MarketTradeMetal:  return "MarketTradeMetal";
    case ActCode::MarketNoEnergy:    return "MarketNoEnergy";
    case ActCode::MarketTradeEnergy: return "MarketTradeEnergy";
    case ActCode::MarketLeave:       return "MarketLeave";
    case ActCode::SignalSuccess:     return "SignalSuccess";
    case ActCode::SignalSwarm:       return "SignalSwarm";
    case ActCode::SignalIgnore:      return "SignalIgnore";
    case ActCode::LifeSupportFixed:  return "LifeSupportFixed";
    case ActCode::LifeSupportPoor:   return "LifeSupportPoor";
    case ActCode::LifeSupportFail:   return "LifeSupportFail";
    case ActCode::WaveIncoming:      return "WaveIncoming";
    case ActCode::WaveRepelled:      return "WaveRepelled";
    case ActCode::BuildingDamaged:   return "BuildingDamaged";
    case ActCode::WaveBreached:      return "WaveBreached";
    case ActCode::Repaired:          return "Repaired";
    case ActCode::Brownout:          return "Brownout";
    case ActCode::Starve:            return "Starve";
    case ActCode::OreDepleted:       return "OreDepleted";
    case ActCode::BuildingBuilt:     return "BuildingBuilt";
    case ActCode::OvercrowdLeft:     return "OvercrowdLeft";
    case ActCode::AcidRain:          return "AcidRain";
    case ActCode::GateTheoryUnlocked:return "GateTheoryUnlocked";
    case ActCode::NewGameText:       return "NewGameText";
    case ActCode::GoalText:          return "GoalText";
    case ActCode::HintText:          return "HintText";
    case ActCode::GameOver:          return "GameOver";
    case ActCode::BlockedByPending:  return "BlockedByPending";
    }
    return "Unknown";
}

// ============================================================
//  内部：中文渲染（兼容层）
// ============================================================
namespace proto_detail {

inline std::string num(long long v) { return std::to_string(v); }

inline long long ival(const std::vector<long long>& v, std::size_t i) {
    return i < v.size() ? v[i] : 0;
}

inline const std::string& sval(const std::vector<std::string>& s, std::size_t i) {
    static const std::string kEmpty;
    return i < s.size() ? s[i] : kEmpty;
}

inline long long toNum(const std::string& s) {
    try {
        return std::stoll(s);
    } catch (...) {
        return 0;
    }
}

// ------- 行动消息（ActionResult 与 LogEntry 共用，保证逐字一致）-------
inline std::string actBuildUnknown(const std::string& key) {
    return "未知建筑类型：" + key + "（输入 list 查看）";
}
inline std::string actBuildBlocked(const std::string& why) { return "无法建造：" + why; }
inline std::string actBuildStarted(const std::string& name, long long id, long long x, long long y,
                                   long long turns, bool hasTurns) {
    std::string m = "开始建造 " + name + " #" + num(id) + " 于 (" + num(x) + "," + num(y) + ")";
    if (hasTurns) m += "，需 " + num(turns) + " 周期";
    return m;
}
inline std::string actDemolishInvalid(long long id) { return "无效的建筑编号 #" + num(id); }
inline std::string actDemolishHQ() { return "指挥中心无法拆除"; }
inline std::string actDemolishDone(const std::string& name, long long id, long long back) {
    return "已拆除 " + name + " #" + num(id) + "，回收金属 " + num(back);
}
inline std::string actToggleDone(const std::string& name, long long id, bool enabled) {
    return name + " #" + num(id) + (enabled ? " 已启用" : " 已关闭（停止耗能与产出）");
}
inline std::string actFocusDone(const std::string& name, long long id) {
    return name + " #" + num(id) + " 已设为工人分配最优先";
}
inline std::string actResearchUnknown(const std::string& key) { return "未知科技：" + key + "（输入 tech 查看）"; }
inline std::string actResearchDup(const std::string& name) { return "已经研究过 " + name; }
inline std::string actResearchPrereq(const std::string& name) { return "前置科技不足，需要先研究：" + name; }
inline std::string actResearchNoScience(const std::string& name, long long cost, long long sci) {
    return "科研点不足：" + name + " 需要 " + num(cost) + "，当前 " + num(sci);
}
inline std::string actResearchDone(const std::string& name, const std::string& desc) {
    return "★ 研究完成：" + name + " —— " + desc;
}

} // namespace proto_detail

// 渲染函数（下文定义）
std::string renderLog(ActCode c, const std::vector<long long>& ints, const std::vector<std::string>& ss);
std::string renderAction(ActCode c, const std::vector<std::string>& args);

// ============================================================
//  日志条目
// ============================================================
struct LogEntry {
    int                    turn = 0;      // 产生该条日志的周期
    ActCode                code = ActCode::Ok;
    std::vector<long long> ints;          // 数值参数
    std::vector<std::string> strings;     // 文本参数

    // 产出与改造前逐字一致的中文（兼容层）
    std::string text() const { return renderLog(code, ints, strings); }
    operator std::string() const { return text(); }

    // 兼容旧代码里直接对日志条目做的字符串查询
    std::size_t find(const std::string& s, std::size_t pos = 0) const { return text().find(s, pos); }
    std::size_t rfind(const std::string& s, std::size_t pos = std::string::npos) const { return text().rfind(s, pos); }
    bool        empty() const { return text().empty(); }
};

// ============================================================
//  行动结果
// ============================================================
struct ActionResult {
    bool                     ok = false;
    ActCode                  code = ActCode::Ok;
    std::vector<std::string> args;   // 文本参数（数值以十进制字符串存放）

    std::string text() const { return renderAction(code, args); }
    operator std::string() const { return text(); }

    std::size_t find(const std::string& s, std::size_t pos = 0) const { return text().find(s, pos); }
    std::size_t rfind(const std::string& s, std::size_t pos = std::string::npos) const { return text().rfind(s, pos); }
    bool        empty() const { return text().empty(); }
};

// ============================================================
//  全值快照（供前端 / RPC 序列化；所有字段均为值类型）
// ============================================================
struct BuildingView {
    int         id = -1;
    int         type = 0;        // BType 的整数值
    std::string typeKey;         // BDef.key（如 "mine"）
    int         x = 0;
    int         y = 0;
    int         buildLeft = 0;   // >0 表示在建
    int         damaged = 0;     // >0 表示受损
    bool        enabled = true;
    bool        alive = true;
    int         assigned = 0;    // 已分配工人数
    int         workerNeed = 0;   // 当前所需工人数（已计入科技修正；0 = 无需工人，前端按“无需工人”处理）
};

struct TileView {
    int terrain = 0;   // Terrain 的字符编码（如 '.' '*' '~' '|' '^'）
    int ore = 0;
    int richness = 0;
    int building = -1; // 占用该地块的建筑 id，-1 为空
};

// 待决事件（P2 §4.4.1）：前端渲染事件弹窗的唯一数据来源
struct PendingView {
    std::string              kind;     // ActCode 枚举名字符串（如 "EventRefugees"）
    std::string              title;    // 事件标题
    std::string              text;     // 事件正文
    std::vector<std::string> options;  // 选项文案，1 基：options[0] 对应 answer option:1
};

struct GameSnapshot {
    int turn = 1;
    // 资源
    int metal = 0, energy = 0, food = 0, science = 0;
    // 人口与士气
    int pop = 0, housing = 0, morale = 0;
    // 环境
    int weather = 0, weatherLeft = 0;
    int waveIn = 0, waveStrengthEstimate = 0;
    int defense = 0;
    // 上一周期报告（值拷贝）
    int metalIn = 0, energyIn = 0, foodIn = 0, scienceIn = 0;
    int energyUp = 0, foodUp = 0;
    int metalNet = 0, energyNet = 0, foodNet = 0, scienceNet = 0;
    bool brownout = false, starving = false;
    // 结局
    bool        over = false, won = false;
    std::string endReason;
    std::string colonyName;
    // 集合
    std::vector<BuildingView> buildings;
    std::vector<TileView>     tiles;     // 按 y*MAP_W+x 展平
    std::vector<LogEntry>     log;
    std::vector<std::string>  techs;     // 已研究科技的 key 列表
    std::vector<int>          assigned;  // 每个建筑分配到的工人数
    // 待决事件（P2 §4.4.1）。hasPending 仅作 C++ 侧内部标志，不单独序列化到 JSON；
    // 序列化层按 hasPending 决定 pending 字段是对象还是 null。
    bool                      hasPending = false;
    PendingView               pending;
    // ---- P2.1 新增（纯追加）----
    int                       idleWorkers = 0;  // 闲置殖民者数（Game::idleWorkers()，前端不得自行按 pop-Σassigned 估算）
    int                       seed = 0;         // 本局实际随机种子；0 = 未知（例如对局由存档载入）
};

// ============================================================
//  渲染实现
// ============================================================

// 日志侧渲染：与改造前 game.cpp 中的中文字符串逐字一致
inline std::string renderLog(ActCode c, const std::vector<long long>& v, const std::vector<std::string>& s) {
    using namespace proto_detail;
    switch (c) {
    case ActCode::Ok:            return "";
    case ActCode::Text:          return sval(s, 0);
    case ActCode::TurnHeader:    return "── 周期 " + num(ival(v, 0)) + " 结算 ──";
    case ActCode::PopBorn:
        return "＋ 一名新殖民者诞生（人口 " + num(ival(v, 0)) + "/" + num(ival(v, 1)) + "）";
    case ActCode::WeatherChange: return "☁ 天气转入 " + sval(s, 0) + "：" + sval(s, 1);

    case ActCode::EventRefugees:    return "◇ 事件：难民船请求降落";
    case ActCode::EventMarket:      return "◇ 事件：黑市商人来访";
    case ActCode::EventSignal:      return "◇ 事件：神秘信号";
    case ActCode::EventLifeSupport: return "◇ 事件：维生系统故障";
    case ActCode::EventMeteorHit:
        return "☄ 陨石击中了 " + sval(s, 0) + " #" + num(ival(v, 0)) + "，停产数周期";
    case ActCode::EventMeteorMiss:  return "☄ 陨石雨落在荒地上，没有损失";
    case ActCode::EventProspectFound:
        return "◈ 勘探队发现新矿脉，位于 (" + num(ival(v, 0)) + "," + num(ival(v, 1)) + ")！";
    case ActCode::EventProspectNone: return "◈ 勘探队一无所获";
    case ActCode::EventVentFound:
        return "◈ 地表裂开，露出新的地热口 (" + num(ival(v, 0)) + "," + num(ival(v, 1)) + ")";
    case ActCode::EventVentNone:    return "◈ 地热勘探没有结果";
    case ActCode::EventFestival:    return "♪ 殖民地举办丰收节：食物 +" + num(ival(v, 0)) + "，士气 +8";
    case ActCode::EventCaravan:     return "⛟ 商队抵达，带来金属 +" + num(ival(v, 0));

    case ActCode::LogChoice:        return "▶ 选择：" + sval(s, 0);
    case ActCode::RefugeesSettled:
        return "难民已安置，人口 +" + num(ival(v, 0)) + "，食物 -" + num(ival(v, 1));
    case ActCode::OvercrowdWarn:
        return "！人口超编 " + num(ival(v, 0)) + " 人（上限 " + num(ival(v, 1)) +
               "）：每周期扣士气并可能流失人口，尽快建造居住舱";
    case ActCode::RefugeesPartial:  return "食物不足，难民无法全部安置，只接收了一部分";
    case ActCode::RefugeesRefused:  return "运输船离开了，殖民地内气氛低落（士气 -5）";
    case ActCode::RefugeesSeized:   return "补给被征用：金属 +60，士气 -10";
    case ActCode::MarketNoMetal:    return "金属不足，商人耸耸肩走了";
    case ActCode::MarketTradeMetal: return "交易完成：金属 -80，科研 +90";
    case ActCode::MarketNoEnergy:   return "能源不足，商人耸耸肩走了";
    case ActCode::MarketTradeEnergy:return "交易完成：能源 -120，金属 +220";
    case ActCode::MarketLeave:      return "商人满意地离开了（士气 +2）";
    case ActCode::SignalSuccess:    return "调查成功！遗迹中的数据库带来科研 +" + num(ival(v, 0));
    case ActCode::SignalSwarm:      return "遗迹里是虫巢！虫群被惊动，提前发动攻击！";
    case ActCode::SignalIgnore:     return "殖民地决定专注于眼前的工作";
    case ActCode::LifeSupportFixed: return "维生系统修复完成（金属 -" + num(ival(v, 0)) + "）";
    case ActCode::LifeSupportPoor:  return "金属不足，只能临时修补";
    case ActCode::LifeSupportFail:  return "维生系统带病运转，3 名殖民者没能挺过来";

    case ActCode::WaveIncoming:
        return "☣ 虫潮来袭！入侵强度 " + num(ival(v, 0)) + "，殖民地防御 " + num(ival(v, 1));
    case ActCode::WaveRepelled:     return "✔ 防线击退了虫群，回收残骸金属 +15（士气 +3）";
    case ActCode::BuildingDamaged:
        return "✖ " + sval(s, 0) + " #" + num(ival(v, 0)) + " 被虫群破坏";
    case ActCode::WaveBreached:
        return "✖ 防线被突破：-" + num(ival(v, 0)) + " 人口，-" + num(ival(v, 1)) + " 金属，士气 -8" +
               (ival(v, 2) ? "" : "（虫群只破坏了空地）");

    case ActCode::Repaired:      return "🔧 " + sval(s, 0) + " #" + num(ival(v, 0)) + " 修复完毕，恢复运转";
    case ActCode::Brownout:      return "⚠ 能源透支：本期有耗能设施只能半负荷运转";
    case ActCode::Starve:        return "⚠ 食物短缺：饿死 " + num(ival(v, 0)) + " 名殖民者，士气 -7";
    case ActCode::OreDepleted:   return "◇ 矿脉枯竭：钻矿场 #" + num(ival(v, 0)) + " 停产，可拆除回收";
    case ActCode::BuildingBuilt: return "✔ " + sval(s, 0) + " #" + num(ival(v, 0)) + " 建造完成";
    case ActCode::OvercrowdLeft:
        return "⚠ 居住空间超编 " + num(ival(v, 0)) + " 人：卫生条件恶化，1 名殖民者离开了殖民地";
    case ActCode::AcidRain:      return "☂ 酸雨腐蚀了 " + sval(s, 0) + " #" + num(ival(v, 0));
    case ActCode::GateTheoryUnlocked:
        return "星门理论已解锁：建造星门（build gate x y）即可撤离！";
    case ActCode::NewGameText:   return "殖民地「" + sval(s, 0) + "」在未知行星着陆，周期 1 开始。";
    case ActCode::GoalText:      return "目标：在 " + num(ival(v, 0)) + " 周期内建成星门，完成撤离。";
    case ActCode::HintText:      return "输入 help 查看命令；直接回车（或 next）推进一个周期。";

    // 行动类代码若出现在日志里（成功时），与返回值逐字一致
    case ActCode::BuildUnknownType: return actBuildUnknown(sval(s, 0));
    case ActCode::BuildBlocked:     return actBuildBlocked(sval(s, 0));
    case ActCode::BuildStarted:
        return actBuildStarted(sval(s, 0), ival(v, 0), ival(v, 1), ival(v, 2), ival(v, 3), ival(v, 4) != 0);
    case ActCode::DemolishInvalid:  return actDemolishInvalid(ival(v, 0));
    case ActCode::DemolishHQ:       return actDemolishHQ();
    case ActCode::DemolishDone:     return actDemolishDone(sval(s, 0), ival(v, 0), ival(v, 1));
    case ActCode::ToggleInvalid:    return actDemolishInvalid(ival(v, 0));
    case ActCode::ToggleDone:       return actToggleDone(sval(s, 0), ival(v, 0), ival(v, 1) != 0);
    case ActCode::FocusInvalid:     return actDemolishInvalid(ival(v, 0));
    case ActCode::FocusDone:        return actFocusDone(sval(s, 0), ival(v, 0));
    case ActCode::ResearchUnknown:  return actResearchUnknown(sval(s, 0));
    case ActCode::ResearchDup:      return actResearchDup(sval(s, 0));
    case ActCode::ResearchPrereq:   return actResearchPrereq(sval(s, 0));
    case ActCode::ResearchNoScience:return actResearchNoScience(sval(s, 0), ival(v, 0), ival(v, 1));
    case ActCode::ResearchDone:     return actResearchDone(sval(s, 0), sval(s, 1));
    // 结局说明：与改造前 log(endReason_) 的兜底文本逐字一致（strings[0] 即原文）
    case ActCode::GameOver:         return sval(s, 0);
    // 待决事件拦截：固定文案（成功文案里不带任何参数）
    case ActCode::BlockedByPending: return "有事件需要先处理（输入选项数字）";
    default:                        return "";
    }
}

// 行动侧渲染：与改造前行动方法返回的中文字符串逐字一致
inline std::string renderAction(ActCode c, const std::vector<std::string>& a) {
    using namespace proto_detail;
    switch (c) {
    case ActCode::BuildUnknownType: return actBuildUnknown(sval(a, 0));
    case ActCode::BuildBlocked:     return actBuildBlocked(sval(a, 0));
    case ActCode::BuildStarted:
        return actBuildStarted(sval(a, 0), toNum(sval(a, 1)), toNum(sval(a, 2)), toNum(sval(a, 3)),
                               toNum(sval(a, 4)), sval(a, 5) == "1");
    case ActCode::DemolishInvalid:  return actDemolishInvalid(toNum(sval(a, 0)));
    case ActCode::DemolishHQ:       return actDemolishHQ();
    case ActCode::DemolishDone:     return actDemolishDone(sval(a, 0), toNum(sval(a, 1)), toNum(sval(a, 2)));
    case ActCode::ToggleInvalid:    return actDemolishInvalid(toNum(sval(a, 0)));
    case ActCode::ToggleDone:       return actToggleDone(sval(a, 0), toNum(sval(a, 1)), sval(a, 2) == "1");
    case ActCode::FocusInvalid:     return actDemolishInvalid(toNum(sval(a, 0)));
    case ActCode::FocusDone:        return actFocusDone(sval(a, 0), toNum(sval(a, 1)));
    case ActCode::ResearchUnknown:  return actResearchUnknown(sval(a, 0));
    case ActCode::ResearchDup:      return actResearchDup(sval(a, 0));
    case ActCode::ResearchPrereq:   return actResearchPrereq(sval(a, 0));
    case ActCode::ResearchNoScience:return actResearchNoScience(sval(a, 0), toNum(sval(a, 1)), toNum(sval(a, 2)));
    case ActCode::ResearchDone:     return actResearchDone(sval(a, 0), sval(a, 1));
    case ActCode::AnswerNone:       return "当前没有待处理事件";
    case ActCode::AnswerInvalid:    return "无效选项，请输入 1-" + sval(a, 0);
    case ActCode::AnswerChoice:     return "选择：" + sval(a, 0);
    case ActCode::BlockedByPending: return "有事件需要先处理（输入选项数字）";
    default:                        return "";
    }
}

} // namespace sc
