// 星际争霸：殖民地 (Star Colony) —— P1 协议化重构的独立验收测试
// 作者：QA/Edward（独立验证，不复用工程师的自测断言）
//
// 覆盖：
//   [A] 兼容层真实性：LogEntry / ActionResult 的 text()/operator std::string()/find/rfind/empty
//   [B] ActionResult.code 真的被设置（六个行动方法的成功/失败路径逐一核对）
//   [C] 研究成功路径（用注入科研点的存档驱动）
//   [D] snapshot() 纯查询：不消耗 rng、可在 const 上调用；waveStrengthEstimate() 纯公式
//   [E] 边界：tile() 越界哨兵、pending() 空哨兵、over/pending 下的 snapshot()
//   [F] 存档兼容：新增 schema_version/content_version 段；旧存档可读；未知段忽略；
//       save->load->save 字节一致；原有严格校验未放松
//
// 编译（与 edge_tests 同法，输出到 build/）：
//   g++ -std=c++20 -O2 -Wall -Wextra -Isrc tests/protocol_tests.cpp src/game.cpp -o build/protocol_tests
// sanitizer：
//   g++ -std=c++20 -g -fsanitize=address,undefined -Isrc tests/protocol_tests.cpp src/game.cpp -o build/protocol_tests_asan
// main() 返回非 0 表示发现失败。
#include "game.hpp"
#include "content.hpp"
#include "ai.hpp"

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace sc;

// =====================================================================
//  基础设施
// =====================================================================

static int g_checks = 0;
static int g_fail   = 0;

static void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_fail;
        std::printf("  [FAIL] %s\n", what.c_str());
    }
}

static std::string tmpdir() {
    const char* d = std::getenv("TMPDIR");
    if (d && *d) return d;
    return ".";   // 兜底：当前目录
}

static bool writeAll(const std::string& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary);
    f << s;
    return static_cast<bool>(f);
}
static std::string readAll(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// 在子进程中运行（sanitizer/信号/崩溃由父进程判定），stderr 丢弃
struct ChildResult {
    int  status = 0;
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

// =====================================================================
//  [A] 兼容层真实性
// =====================================================================

static void testCompatibilityLayer() {
    std::printf("[A] 兼容层：LogEntry / ActionResult 的转发与语义\n");

    // --- LogEntry ---
    LogEntry t;
    t.code = ActCode::Text;
    t.strings.push_back("hello world");
    check(t.text() == "hello world", "Text 条目 text() 等于原始文本");
    check(std::string(t) == "hello world", "LogEntry::operator std::string() 与 text() 一致");
    check(t.find("world") == 6, "find 命中返回正确下标");
    check(t.rfind("o") == 7, "rfind 命中返回正确下标");
    check(t.find("zzz") == std::string::npos, "find 未命中返回 npos");
    check(t.find("o", 5) == 7, "find 支持起始位置");
    check(!t.empty(), "非空条目 empty()==false");

    LogEntry ok;
    check(ok.text() == "", "ActCode::Ok 渲染为空串");
    check(ok.empty(), "空条目 empty()==true");

    LogEntry th;
    th.code = ActCode::TurnHeader;
    th.ints.push_back(5);
    check(th.text() == "── 周期 5 结算 ──", "结构化 TurnHeader 渲染正确");
    check(std::string(th) == th.text(), "结构化条目 operator std::string() 与 text() 一致");

    // 缺参不越界：ival/sval 对缺参返回 0/""
    LogEntry partial;
    partial.code = ActCode::EventFestival;   // 需要 ints[0]
    check(!partial.text().empty(), "缺参数的条目仍返回非空文本（不越界）");

    // --- ActionResult ---
    ActionResult a;
    a.ok   = true;
    a.code = ActCode::DemolishDone;
    a.args = {"钻矿场", "3", "30"};
    check(a.text() == "已拆除 钻矿场 #3，回收金属 30", "ActionResult::text() 渲染与改造前逐字一致");
    check(std::string(a) == a.text(), "ActionResult::operator std::string() 与 text() 一致");
    check(a.find("钻矿场") != std::string::npos, "ActionResult::find 转发到 text()");
    check(a.rfind("30") != std::string::npos, "ActionResult::rfind 转发到 text()");
    check(!a.empty(), "非空 ActionResult empty()==false");

    ActionResult an;   // 默认 Ok
    check(an.text() == "", "默认（Ok）ActionResult 渲染为空串");
    check(an.empty(), "默认 ActionResult empty()==true");
}

// =====================================================================
//  [B] ActionResult.code 的真实性
// =====================================================================

static void testActionCodes() {
    std::printf("[B] 六个行动方法的 code 在成功/失败路径上被真实设置\n");

    Game g;
    g.newGame(1, "QA");

    // doBuild：未知 / 被阻止 / 成功
    // 注意：太阳能板的命令键是 "sol"（BDEF[Solar].key），"solar" 不是合法键。
    ActionResult ub = g.doBuild("zzz_nope", 5, 5);
    check(!ub.ok && ub.code == ActCode::BuildUnknownType, "doBuild 未知类型 -> BuildUnknownType");
    ActionResult bb = g.doBuild("sol", 8, 6);   // (8,6) 是指挥中心所在地块，必然被阻止
    check(!bb.ok && bb.code == ActCode::BuildBlocked, "doBuild 非法地块 -> BuildBlocked");

    int bx = -1, by = -1;
    for (int y = 0; y < MAP_H && bx < 0; ++y)
        for (int x = 0; x < MAP_W; ++x) {
            std::string why;
            if (g.buildable(BType::Solar, x, y, &why)) { bx = x; by = y; break; }
        }
    check(bx >= 0, "能找到可建太阳能板的空地");
    ActionResult sb = g.doBuild("sol", bx, by);
    check(sb.ok && sb.code == ActCode::BuildStarted, "doBuild 成功 -> BuildStarted(ok)");

    // doDemolish：无效 / 指挥中心
    check(g.doDemolish(9999).code == ActCode::DemolishInvalid &&
              !g.doDemolish(9999).ok, "doDemolish 无效编号 -> DemolishInvalid");
    check(g.doDemolish(0).code == ActCode::DemolishHQ, "doDemolish 指挥中心 -> DemolishHQ");

    // doToggle：无效 / 成功
    check(g.doToggle(9999).code == ActCode::ToggleInvalid, "doToggle 无效编号 -> ToggleInvalid");
    ActionResult tg = g.doToggle(0);
    check(tg.ok && tg.code == ActCode::ToggleDone, "doToggle 成功 -> ToggleDone(ok)");
    g.doToggle(0);   // 还原

    // doFocus：无效 / 成功
    check(g.doFocus(9999).code == ActCode::FocusInvalid, "doFocus 无效编号 -> FocusInvalid");
    ActionResult fc = g.doFocus(0);
    check(fc.ok && fc.code == ActCode::FocusDone, "doFocus 成功 -> FocusDone(ok)");

    // doResearch：未知 / 前置不足 / 科研不足
    check(g.doResearch("zzz_nope").code == ActCode::ResearchUnknown, "doResearch 未知科技 -> ResearchUnknown");
    check(g.doResearch("atmo").code == ActCode::ResearchPrereq, "doResearch 缺前置 -> ResearchPrereq");
    check(g.doResearch("hydro").code == ActCode::ResearchNoScience, "doResearch 科研不足 -> ResearchNoScience");

    // answer：无待决事件
    ActionResult ans = g.answer(1);
    check(!ans.ok && ans.code == ActCode::AnswerNone, "answer 无事件 -> AnswerNone");

    // 关键：code 不是清一色 Ok —— 统计不同 code 数量
    std::vector<ActCode> seen = {ub.code, bb.code, sb.code, tg.code, fc.code};
    int distinct = 0;
    for (ActCode c : seen) {
        bool dup = false;
        for (int i = 0; i < distinct; ++i)
            if (seen[static_cast<size_t>(i)] == c) dup = true;
        if (!dup) seen[static_cast<size_t>(distinct++)] = c;
    }
    check(distinct >= 4, "不同行动返回不同 code（未退化为单一 Ok）");
}

// =====================================================================
//  [C] 研究成功路径（注入科研点）
// =====================================================================

static void testResearchSuccess() {
    std::printf("[C] doResearch 成功路径 -> ResearchDone(ok)，重复 -> ResearchDup\n");

    const std::string p  = tmpdir() + "/proto_sci.sav";
    const std::string p2 = tmpdir() + "/proto_sci2.sav";
    Game g;
    g.newGame(3, "QA");
    g.saveTo(p);

    // 把 res 行的科研点抬高，其它保持不变
    std::string s = readAll(p);
    std::istringstream in(s);
    std::string line, out;
    bool patched = false;
    while (std::getline(in, line)) {
        if (line.rfind("res ", 0) == 0) { out += "res 500 500 500 1000\n"; patched = true; }
        else out += line + "\n";
    }
    check(patched, "找到并替换 res 行");
    writeAll(p2, out);

    Game h;
    std::string lr = h.loadFrom(p2);
    check(lr.find("已从存档") != std::string::npos, "注入科研点的存档可读入");

    ActionResult r1 = h.doResearch("fusion");   // fusion 无前置
    check(r1.ok && r1.code == ActCode::ResearchDone, "doResearch 成功 -> ResearchDone(ok)");
    check(r1.text().find("研究完成") != std::string::npos, "成功文本含『研究完成』");

    ActionResult r2 = h.doResearch("fusion");
    check(!r2.ok && r2.code == ActCode::ResearchDup, "重复研究 -> ResearchDup(失败)");
}

// =====================================================================
//  [D] snapshot() 纯查询
// =====================================================================

static std::string canon(const Game& g) {
    std::string s;
    s += "turn=" + std::to_string(g.turn()) + "\n";
    s += "m=" + std::to_string(g.res().metal) + " e=" + std::to_string(g.res().energy) +
         " f=" + std::to_string(g.res().food) + " sc=" + std::to_string(g.res().science) + "\n";
    s += "pop=" + std::to_string(g.pop()) + " h=" + std::to_string(g.housing()) +
         " mo=" + std::to_string(g.morale()) + " mm=" + std::to_string(g.moraleMultiplier()) + "\n";
    s += "w=" + std::to_string(static_cast<int>(g.weather())) + " wl=" + std::to_string(g.weatherLeft()) +
         " wi=" + std::to_string(g.waveIn()) + " est=" + std::to_string(g.waveStrengthEstimate()) +
         " def=" + std::to_string(g.defense()) + "\n";
    s += "over=" + std::to_string(g.over()) + " won=" + std::to_string(g.won()) + " r=" + g.endReason() + "\n";
    for (const Building& b : g.buildings())
        s += "B" + std::to_string(b.id) + "," + std::to_string(static_cast<int>(b.type)) + "," +
             std::to_string(b.buildLeft) + "," + std::to_string(b.damaged) + "," +
             std::to_string(b.enabled) + "," + std::to_string(b.alive) + "\n";
    for (int i = 0; i < TECH_COUNT; ++i)
        if (g.hasTech(static_cast<Tech>(i))) s += "T" + std::to_string(i) + "\n";
    for (const LogEntry& e : g.log()) s += "L:" + std::string(e) + "\n";   // 含 rng 结果
    return s;
}

static void testSnapshotPurity() {
    std::printf("[D] snapshot() 是纯查询（不消耗 rng）\n");

    Game A, B;
    A.newGame(2024, "SNAP");
    B.newGame(2024, "SNAP");

    // waveStrengthEstimate 连续调用恒定
    int e1 = A.waveStrengthEstimate();
    bool estStable = true;
    for (int i = 0; i < 200; ++i)
        if (A.waveStrengthEstimate() != e1) estStable = false;
    check(estStable, "waveStrengthEstimate() 连续 200 次调用结果恒定");

    bool constCallOk = false;
    for (int t = 0; t < 85 && !A.over() && !B.over(); ++t) {
        // A 局：每回合调用 snapshot() 一次 + 重复 50 次
        const Game& cA = A;
        GameSnapshot s = cA.snapshot();
        constCallOk = (s.turn == A.turn());
        for (int k = 0; k < 50; ++k) { volatile int z = A.snapshot().turn; (void)z; }

        aiTurn(A);
        A.advanceTurn();
        aiTurn(B);
        B.advanceTurn();
    }
    check(constCallOk, "snapshot() 可在 const Game& 上调用");
    check(canon(A) == canon(B), "调 snapshot 的一局与不调的一局，85 回合后状态逐字一致");

    // snapshot 内容与直接访问器一致
    GameSnapshot s = A.snapshot();
    check(s.turn == A.turn() && s.metal == A.res().metal && s.pop == A.pop() &&
              s.morale == A.morale() && s.defense == A.defense(),
          "snapshot 字段与访问器一致");
    check(s.log.size() == A.log().size(), "snapshot.log 条数与 log() 一致");

    // 推进到终局再比一次
    for (int t = 0; t < 30 && (!A.over() || !B.over()); ++t) {
        if (!A.over()) { A.snapshot(); aiTurn(A); A.advanceTurn(); }
        if (!B.over()) { aiTurn(B); B.advanceTurn(); }
    }
    check(canon(A) == canon(B), "终局后两局状态仍逐字一致");
    check(A.over() && B.over() && A.won() == B.won(), "两局结局与胜负一致");
}

// =====================================================================
//  [E] 边界与健壮性
// =====================================================================

static void testBoundaries() {
    std::printf("[E] 边界：tile() 越界哨兵 / pending() 空哨兵 / over 与 pending 下的 snapshot()\n");

    Game g;
    g.newGame(5, "QA");

    // tile() 越界返回只读哨兵，且不越界写
    const Tile& s = g.tile(-1, 0);
    check(s.terrain == Terrain::Plain && s.ore == 0 && s.richness == 0 && s.building == -1,
          "tile(-1,0) 返回默认哨兵");
    check(g.tile(0, -1).building == -1, "tile(0,-1) 返回哨兵");
    check(g.tile(MAP_W, 0).building == -1, "tile(MAP_W,0) 返回哨兵");
    check(g.tile(0, MAP_H).building == -1, "tile(0,MAP_H) 返回哨兵");
    check(g.tile(INT_MAX, INT_MAX).building == -1, "tile(INT_MAX,INT_MAX) 返回哨兵");
    check(g.tile(INT_MIN, INT_MIN).building == -1, "tile(INT_MIN,INT_MIN) 返回哨兵");
    check(g.tile(INT_MAX, 0).building == -1, "tile(INT_MAX,0) 返回哨兵");
    check(g.tile(0, INT_MIN).building == -1, "tile(0,INT_MIN) 返回哨兵");
    // 哨兵是同一只读对象
    check(&g.tile(-1, 0) == &g.tile(-999, -999), "越界返回同一只读哨兵（无副作用写）");

    // tileOreYield 越界也应走哨兵而非 UB
    check(g.tileOreYield(INT_MIN, INT_MIN) == 0, "tileOreYield 越界返回 0");

    check(!g.hasPending(), "新局无待决事件");
    const PendingEvent& pe = g.pending();   // 不得崩溃
    check(pe.kind == 0 && pe.options.empty(), "pending() 无事件时返回空哨兵");

    // snapshot() 在有待决事件时可用
    bool sawPendingSnapshot = false;
    for (int t = 0; t < MAX_TURNS + 20 && !g.over(); ++t) {
        g.advanceTurn();
        if (g.hasPending()) {
            GameSnapshot sp = g.snapshot();
            sawPendingSnapshot = (sp.turn == g.turn());
            break;
        }
    }
    check(sawPendingSnapshot, "有待决事件时 snapshot() 正常返回");

    // snapshot() 在对局结束时可用
    Game e;
    e.newGame(2, "QA");
    for (int t = 0; t < MAX_TURNS + 40 && !e.over(); ++t) {
        aiTurn(e);
        e.advanceTurn();
    }
    check(e.over(), "对局到达终局");
    GameSnapshot es = e.snapshot();
    check(es.over == e.over() && es.turn == e.turn(), "终局状态 snapshot() 正常返回");

    // fork 隔离：极端坐标 + 极端状态不得触发 sanitizer/信号
    ChildResult cr = runChild([&]() -> int {
        Game h;
        h.newGame(9, "QA");
        volatile int acc = 0;
        for (int k = 0; k < 1000; ++k) {
            acc += h.tile(INT_MIN + (k % 7), INT_MAX - (k % 5)).ore;
            acc += h.tile(-k, k).building;
        }
        for (int k = 0; k < 100; ++k) (void)h.snapshot();
        (void)h.pending();
        (void)acc;
        return 0;
    });
    check(!cr.signaled() && cr.code() == 0, "极端越界访问在子进程中无信号/无崩溃");
}

// =====================================================================
//  [F] 存档兼容性
// =====================================================================

// 一份结构合法的最小存档（HQ #0 位于 (8,6)）；可选带版本段
static std::string baseSave(bool withVersionSegs) {
    std::ostringstream o;
    o << "STARCOLONY 1\n";
    if (withVersionSegs) {
        o << "schema_version 1\n";
        o << "content_version 1\n";
    }
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

// 用给定文本构造一个存档路径并返回读档结果串
static std::string loadText(const std::string& text, const std::string& name) {
    const std::string p = tmpdir() + "/" + name;
    writeAll(p, text);
    Game g;
    return g.loadFrom(p);
}
static bool accepted(const std::string& r) { return r.find("已从存档") != std::string::npos; }

// 精确定位存档 map 段中某个地块 (tx,ty) 的行并改写为 ". 0 0 <ref>"
static void setTileRef(std::string& s, int tx, int ty, int ref) {
    size_t mp = s.find("\nmap\n");
    if (mp == std::string::npos) return;
    size_t cur = mp + 5;                       // 第一行首字符
    const int row = ty * MAP_W + tx;
    for (int i = 0; i < row; ++i) cur = s.find('\n', cur) + 1;
    size_t eol = s.find('\n', cur);
    s.replace(cur, eol - cur, ". 0 0 " + std::to_string(ref));
}

static void testSaveCompat() {
    std::printf("[F] 存档兼容：新增版本段 / 旧存档 / 未知段 / 往返 / 严格校验\n");

    // 新存档确实写了两个新段
    const std::string pNew = tmpdir() + "/proto_new.sav";
    Game g;
    g.newGame(11, "QA");
    g.saveTo(pNew);
    std::string txt = readAll(pNew);
    check(txt.find("\nschema_version 1\n") != std::string::npos, "saveTo 写入 schema_version 段");
    check(txt.find("\ncontent_version 1\n") != std::string::npos, "saveTo 写入 content_version 段");

    // 旧存档（去掉两个版本段）仍可读
    check(accepted(loadText(baseSave(false), "proto_old.sav")), "不带版本段的旧存档可读入");
    // 带版本段的存档可读
    check(accepted(loadText(baseSave(true), "proto_ver.sav")), "带版本段的存档可读入");

    // 未知段被静默忽略
    std::string withUnknown = baseSave(true);
    withUnknown = "STARCOLONY 1\nschema_version 1\ncontent_version 1\nfrobnicate 42\nalien_payload xyz\n" +
                  withUnknown.substr(withUnknown.find("name "));
    check(accepted(loadText(withUnknown, "proto_unknown.sav")), "未知段被静默忽略，仍可读入");

    // save -> load -> save 字节一致（对带结构日志/有人口的真实存档）
    const std::string pa = tmpdir() + "/proto_rt_a.sav";
    const std::string pb = tmpdir() + "/proto_rt_b.sav";
    Game a;
    a.newGame(77, "RTOUND");
    for (int t = 0; t < 25 && !a.over(); ++t) { aiTurn(a); a.advanceTurn(); }
    a.saveTo(pa);
    Game b;
    check(accepted(b.loadFrom(pa)), "往返存档可读入");
    b.saveTo(pb);
    check(readAll(pa) == readAll(pb), "save->load->save 字节一致");

    // 带待决事件的存档：事件需在读档后保留，且往返字节一致
    {
        Game p;
        p.newGame(1, "PEND");
        for (int t = 0; t < MAX_TURNS + 20 && !p.over() && !p.hasPending(); ++t) p.advanceTurn();
        if (p.hasPending()) {
            const std::string pp = tmpdir() + "/proto_pend.sav";
            const std::string pq = tmpdir() + "/proto_pend2.sav";
            p.saveTo(pp);
            Game q;
            check(accepted(q.loadFrom(pp)), "带待决事件的存档可读入");
            check(q.hasPending() && q.pending().kind == p.pending().kind, "待决事件在读档后保留");
            q.saveTo(pq);
            check(readAll(pp) == readAll(pq), "含待决事件的 save->load->save 字节一致");
        } else {
            std::printf("      （窗口内未出现待决事件，跳过 pending 往返）\n");
        }
    }

    // 原有严格校验未放松（[1][2] 组语义，用最小存档对照）
    check(accepted(loadText(baseSave(true), "proto_ok.sav")), "合法最小存档可读入（未误杀）");

    {   // 负人口必须拒绝
        std::string s = baseSave(true);
        size_t pos = s.find("state ");
        size_t eol = s.find('\n', pos);
        // state turn pop morale popAcc techs weather weatherLeft waveIn over won
        s.replace(pos, eol - pos, "state 1 -6 70 0 0 0 3 9 0 0");
        check(!accepted(loadText(s, "proto_negpop.sav")), "负人口存档被拒绝");
    }
    {   // 士气越界必须拒绝
        std::string s = baseSave(true);
        size_t pos = s.find("state ");
        size_t eol = s.find('\n', pos);
        s.replace(pos, eol - pos, "state 1 6 101 0 0 0 3 9 0 0");
        check(!accepted(loadText(s, "proto_morale.sav")), "士气越界存档被拒绝");
    }
    {   // 缺少 res 段必须拒绝
        std::string s = baseSave(true);
        size_t pos = s.find("res ");
        size_t eol = s.find('\n', pos);
        s.erase(pos, eol - pos + 1);
        check(!accepted(loadText(s, "proto_nores.sav")), "缺 res 段被拒绝");
    }
    {   // 缺少 state 段必须拒绝
        std::string s = baseSave(true);
        size_t pos = s.find("state ");
        size_t eol = s.find('\n', pos);
        s.erase(pos, eol - pos + 1);
        check(!accepted(loadText(s, "proto_nostate.sav")), "缺 state 段被拒绝");
    }
    {   // 地块指向不存在的建筑必须拒绝：抹掉 HQ 自身地块 (8,6) 的引用
        std::string s = baseSave(true);
        setTileRef(s, 8, 6, -1);
        check(!accepted(loadText(s, "proto_dangling.sav")), "地块/建筑引用不一致被拒绝");
    }
    {   // 两个地块指向同一建筑（坐标不一致）必须拒绝：(0,0) 也指向 #0
        std::string s = baseSave(true);
        setTileRef(s, 0, 0, 0);
        check(!accepted(loadText(s, "proto_dup.sav")), "同一建筑被多块地引用被拒绝");
    }
}

// =====================================================================
//  [G] 日志结构化统计（兜底码占比）
// =====================================================================

static void testLogCodes() {
    std::printf("[G] 引擎日志结构化程度（兜底 Text 占比）\n");

    Game g;
    g.newGame(42, "STAT");
    for (int t = 0; t < MAX_TURNS + 10 && !g.over(); ++t) {
        aiTurn(g);
        g.advanceTurn();
    }

    int total = 0, textFallback = 0, emptyOk = 0;
    std::vector<std::string> fallbackTexts;
    std::vector<int>         codes;
    int                      distinct = 0;
    for (const LogEntry& e : g.log()) {
        ++total;
        if (e.code == ActCode::Text) { ++textFallback; fallbackTexts.push_back(e.text()); }
        if (e.code == ActCode::Ok) ++emptyOk;
        if (e.code != ActCode::Ok) check(!e.text().empty(), "每条非 Ok 日志都有非空文本");
        const int c = static_cast<int>(e.code);
        bool dup = false;
        for (int i = 0; i < distinct; ++i)
            if (codes[static_cast<size_t>(i)] == c) dup = true;
        if (!dup) { codes.push_back(c); ++distinct; }
    }
    std::printf("      引擎整局日志：%d 条，兜底 Text %d 条（%.1f%%），Ok 空码 %d 条，不同码 %d 种\n",
                total, textFallback, total ? 100.0 * textFallback / total : 0.0, emptyOk, distinct);
    check(total > 0, "整局产生了日志");
    check(emptyOk == 0, "不存在渲染为空的 Ok 日志条目");
    check(distinct >= 20, "日志码种类丰富（结构信息未退化为单一码）");
    // 结局说明已结构化为 ActCode::GameOver
    //（engine: game.cpp 的 log(ActCode::GameOver, {}, {endReason_})，渲染仍是 endReason_ 原文）。
    // 因此引擎对局中不再存在兜底 Text 条目：本断言由「至多 1 条」收紧为「必须 0 条」，测试强度不降反升。
    check(textFallback == 0, "引擎对局无兜底 Text 条目（结局说明已结构化 GameOver）");
    for (const std::string& t : fallbackTexts)
        std::printf("      ※ 意外出现的兜底 Text：%s\n", t.c_str());

    // 兼容层 Text 路径仍可用（UI 层日志）
    Game u;
    u.newGame(1, "UI");
    u.log("› help");
    check(u.log().back().code == ActCode::Text, "log(std::string) 记为 Text 条目");
    check(u.log().back().text() == "› help", "Text 条目渲染回原文");
}

// =====================================================================
//  [H] snapshot.pending（待决事件结构化，P2 §4.4.1）
// =====================================================================

static void testPendingSnapshot() {
    std::printf("[H] snapshot.pending：无事件为 null、有事件与 Game::pending() 一致\n");

    Game g;
    g.newGame(5, "PEND");

    // 无事件：hasPending==false，pending 各字段为空
    GameSnapshot s0 = g.snapshot();
    check(!s0.hasPending, "无事件时 snapshot.hasPending==false");
    check(s0.pending.title.empty() && s0.pending.text.empty() && s0.pending.options.empty(),
          "无事件时 snapshot.pending 各字段为空");

    // 推进到出现待决事件
    bool found = false;
    for (int t = 0; t < MAX_TURNS + 40 && !g.over(); ++t) {
        g.advanceTurn();
        if (g.hasPending()) { found = true; break; }
    }
    check(found, "窗口内出现待决事件");

    if (found) {
        const PendingEvent& pe = g.pending();
        GameSnapshot        s  = g.snapshot();
        check(s.hasPending, "有事件时 snapshot.hasPending==true");
        check(s.pending.title == pe.title, "pending.title 与 Game::pending() 一致");
        check(s.pending.text == pe.text, "pending.text 与 Game::pending() 一致");
        check(s.pending.options == pe.options, "pending.options 与 Game::pending() 一致");
        check(!s.pending.options.empty() && s.pending.options.size() == pe.options.size(),
              "pending.options 条数正确且非空");
        check(!s.pending.kind.empty() && s.pending.kind != "Unknown",
              "pending.kind 是已知 ActCode 名字（非 Unknown）");
        std::printf("      ※ 事件 kind=%s title=%s options=%zu 条\n", s.pending.kind.c_str(),
                    s.pending.title.c_str(), s.pending.options.size());

        // 应答后 pending 应回到无事件（或事件链的下一个），至少 title 应随状态变化
        s = g.snapshot();   // 纯查询不影响
        check(s.hasPending == g.hasPending(), "再次 snapshot 的 hasPending 与引擎一致");
    }
}

// =====================================================================
//  [I] 事件待决期的核心拦截（规则上移 core，P2 §4.4.1 / §4.6）
// =====================================================================

static void testPendingInterception() {
    std::printf("[I] 事件待决期：核心拦截除 answer 外的一切操作\n");

    Game g;
    g.newGame(5, "LOCK");
    bool found = false;
    for (int t = 0; t < MAX_TURNS + 40 && !g.over(); ++t) {
        g.advanceTurn();
        if (g.hasPending()) { found = true; break; }
    }
    check(found, "窗口内出现待决事件");
    if (!found) return;

    // 五个会改变状态的操作都被核心拦截（参数是否合法都应在 pending 检查之后）
    const ActionResult rb = g.doBuild("sol", 0, 0);
    check(!rb.ok && rb.code == ActCode::BlockedByPending, "待决期 doBuild -> BlockedByPending");
    const ActionResult rd = g.doDemolish(0);
    check(!rd.ok && rd.code == ActCode::BlockedByPending, "待决期 doDemolish -> BlockedByPending");
    const ActionResult rt = g.doToggle(0);
    check(!rt.ok && rt.code == ActCode::BlockedByPending, "待决期 doToggle -> BlockedByPending");
    const ActionResult rf = g.doFocus(0);
    check(!rf.ok && rf.code == ActCode::BlockedByPending, "待决期 doFocus -> BlockedByPending");
    const ActionResult rr = g.doResearch("fusion");
    check(!rr.ok && rr.code == ActCode::BlockedByPending, "待决期 doResearch -> BlockedByPending");
    check(rb.text() == "有事件需要先处理（输入选项数字）", "BlockedByPending 渲染固定文案");

    // advanceTurn 在待决期必须完全冻结状态（不推进、不消耗 rng、不改 pending）
    const GameSnapshot before = g.snapshot();
    g.advanceTurn();
    const GameSnapshot after = g.snapshot();
    check(after.turn == before.turn, "待决期 advanceTurn 不推进周期");
    check(after.metal == before.metal && after.energy == before.energy && after.food == before.food &&
              after.science == before.science && after.pop == before.pop && after.morale == before.morale,
          "待决期 advanceTurn 不改动资源与人口");
    check(after.hasPending && after.pending.title == before.pending.title &&
              after.pending.text == before.pending.text && after.pending.options == before.pending.options,
          "待决期 advanceTurn 不改动 pending");
    check(after.log.size() == before.log.size(), "待决期 advanceTurn 不产生任何日志");

    // answer 仍然可用（唯一被允许的操作）
    const ActionResult   ans = g.answer(1);
    check(ans.ok && ans.code == ActCode::AnswerChoice, "待决期 answer 仍然可用");
    int guard = 0;
    while (g.hasPending() && guard++ < 16) g.answer(1);   // 清空事件链
    const GameSnapshot cleared = g.snapshot();
    if (!cleared.over) {
        g.advanceTurn();
        check(g.turn() != cleared.turn, "应答后 advanceTurn 恢复正常推进");
    }
}

// =====================================================================
//  [J] P2.1：snapshot 新字段（idleWorkers / seed / workerNeed）
//       与 preview_build 的底层查询（纯查询、buildable/affordable 独立）
// =====================================================================

static void testNewSnapshotFields() {
    std::printf("[J] snapshot 新字段：idleWorkers / seed / workerNeed\n");

    Game g;
    g.newGame(12345, "FIELDS");
    GameSnapshot s = g.snapshot();
    check(s.idleWorkers == g.idleWorkers(), "snapshot.idleWorkers 与 Game::idleWorkers() 一致");
    check(static_cast<uint32_t>(s.seed) == 12345u && g.seed() == 12345u,
          "snapshot.seed 等于本局实际种子 12345");
    check(s.seed > 0, "新局 seed > 0（前端据此显示）");

    // workerNeed 逐个建筑与引擎一致
    auto needMatches = [&](const GameSnapshot& snap) {
        for (const BuildingView& bv : snap.buildings) {
            const Building* b = g.building(bv.id);
            if (!b || bv.workerNeed != g.workerNeed(*b)) return false;
        }
        return true;
    };
    check(needMatches(s), "buildings[].workerNeed 与 Game::workerNeed() 一致（初始只有 HQ）");

    // 多建几栋后再核对一次（确认不是只对初始建筑成立）
    for (int y = 0; y < MAP_H; ++y)
        for (int x = 0; x < MAP_W; ++x) g.doBuild("sol", x, y);
    for (int y = 0; y < MAP_H; ++y)
        for (int x = 0; x < MAP_W; ++x) g.doBuild("hab", x, y);
    const GameSnapshot s2 = g.snapshot();
    check(needMatches(s2), "建造多栋后 buildings[].workerNeed 仍与引擎一致");
    std::printf("      ※ 本局 workerNeed: ");
    for (const BuildingView& bv : s2.buildings) std::printf("#%d=%d ", bv.id, bv.workerNeed);
    std::printf("\n");

    // 读档后 seed 必须为 0（未知）——存档格式刻意不含种子
    const std::string p = tmpdir() + "/p21_seed.sav";
    g.saveTo(p);
    Game h;
    check(accepted(h.loadFrom(p)), "P2.1 存档可读入");
    check(h.snapshot().seed == 0 && h.seed() == 0u, "读档后 snapshot.seed == 0（未知）");
    check(readAll(p).find("\nseed ") == std::string::npos,
          "存档文件不含 seed 段（存档格式未变，守住基线红线）");
}

// preview_build 的规则层语义 + buildable/affordable 相互独立
static void testPreviewBuildRules() {
    std::printf("[J2] preview_build 规则层：buildable 与 affordable 相互独立、纯查询\n");

    Game g;
    g.newGame(7, "INDEP");

    // 坐标越界
    {
        std::string why;
        check(!g.buildableTerrain(BType::Solar, -1, 0, &why) && why == "坐标超出地图范围",
              "坐标越界 -> buildableTerrain=false 且 reason=坐标超出地图范围");
        std::string why2;
        check(!g.buildableTerrain(BType::Solar, 0, MAP_H, &why2) && why2 == "坐标超出地图范围",
              "y 越界同样报坐标超出地图范围");
    }

    // 地形不符：钻矿场建在平原上
    int plainX = -1, plainY = -1;
    for (int y = 0; y < MAP_H && plainX < 0; ++y)
        for (int x = 0; x < MAP_W; ++x) {
            std::string why;
            if (g.buildableTerrain(BType::Mine, x, y, &why)) continue;   // 矿脉位跳过
            if (g.tile(x, y).terrain == Terrain::Plain) { plainX = x; plainY = y; break; }
        }
    if (plainX >= 0) {
        std::string why;
        check(!g.buildableTerrain(BType::Mine, plainX, plainY, &why) &&
                  why == "钻矿场必须建在金属矿脉上",
              "地形不符 -> buildableTerrain=false 且 reason=钻矿场必须建在金属矿脉上");
    } else {
        check(false, "找到平原用于地形不符验证");
    }

    // 独立性：把金属花到不足以支付 lab（lab 需 70 金属），规则层仍应可建
    for (int y = 0; y < MAP_H && g.res().metal >= 70; ++y)
        for (int x = 0; x < MAP_W && g.res().metal >= 70; ++x) g.doBuild("sol", x, y);
    int lx = -1, ly = -1;
    for (int y = 0; y < MAP_H && lx < 0; ++y)
        for (int x = 0; x < MAP_W; ++x) {
            std::string why;
            if (g.buildableTerrain(BType::Lab, x, y, &why)) { lx = x; ly = y; break; }
        }
    check(lx >= 0, "找到规则层可建研究所的格子");
    if (lx >= 0) {
        std::string whyRules, whyFull;
        const bool rules = g.buildableTerrain(BType::Lab, lx, ly, &whyRules);
        const bool full  = g.buildable(BType::Lab, lx, ly, &whyFull);
        check(rules && !full,
              "资源不足时：buildableTerrain=true 而 buildable=false（两者相互独立，未被合并）");
        check(g.res().metal < BDEF[static_cast<size_t>(BType::Lab)].costMetal, "当前金属确实不够付 lab");
        check(whyFull == "金属不足", "资源不足的原因文案为『金属不足』");
    }
}

// 纯查询：连续调用可行性查询不消耗 rng、不改状态（照 P1 [D] 验证 snapshot 纯度的写法）
static void testPreviewBuildPurity() {
    std::printf("[J3] preview_build 纯查询：连续调用不耗 rng、不改状态\n");

    Game A, B;
    A.newGame(2024, "PREVIEW");
    B.newGame(2024, "PREVIEW");

    // A 每回合做 50 次可行性查询（等价于连续调 50 次 preview_build 的查询部分），B 不做
    for (int t = 0; t < 60 && !A.over() && !B.over(); ++t) {
        for (int k = 0; k < 50; ++k) {
            std::string why;
            A.buildableTerrain(BType::Mine, k % MAP_W, (k / MAP_W) % MAP_H, &why);
            A.buildableTerrain(BType::Solar, (k * 3) % MAP_W, (k * 5) % MAP_H, &why);
        }
        aiTurn(A);
        A.advanceTurn();
        aiTurn(B);
        B.advanceTurn();
    }
    check(canon(A) == canon(B),
          "每回合查询 50 次的一局与不查询的一局，60 回合后状态逐字一致（未消耗 rng、未改状态）");
}

// =====================================================================
//  [K] P3a：内容外置 —— 加载 / 校验 / 落地
// =====================================================================

static bool strContains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}
static std::string joinErrs(const std::vector<std::string>& e) {
    std::string r;
    for (const std::string& x : e) { r += x; r += "\n"; }
    return r;
}
// 复制真实内容包到独立目录（便于注入坏内容而不污染仓库）
static std::string copyBaseTo(const std::string& name) {
    const std::string dir = tmpdir() + "/" + name;
    ::mkdir(dir.c_str(), 0777);
    const char* files[] = {"manifest.json", "buildings.json", "techs.json",
                           "weathers.json", "tuning.json"};
    for (const char* fn : files) {
        writeAll(dir + "/" + fn, readAll(std::string("content/base/") + fn));
    }
    return dir;
}
// 对目录内某文件做一次文本替换（替换首处）
static void patchFile(const std::string& dir, const std::string& fn,
                      const std::string& from, const std::string& to) {
    const std::string p = dir + "/" + fn;
    std::string s = readAll(p);
    const size_t pos = s.find(from);
    if (pos != std::string::npos) s.replace(pos, from.size(), to);
    writeAll(p, s);
}

static void testContentValidation() {
    std::printf("[K] 内容外置：加载 / 校验 / 落地（P3a）\n");

    // --- 真实内容包：加载 + 校验通过，值转写正确 ---
    {
        ContentPack pack;
        std::vector<std::string> errs;
        check(loadContentPack("content/base", pack, errs), "真实内容包加载 + 校验通过");
        check(pack.schema == 1 && pack.packId == "base", "manifest.schema/pack_id 正确");
        check(std::string(pack.buildings[0].key) == "hq" &&
                  std::string(pack.buildings[0].name) == "指挥中心",
              "buildings[0] = hq 指挥中心");
        check(pack.buildings[9].costMetal == 420 && pack.buildings[9].costEnergy == 300 &&
                  std::string(pack.buildings[9].key) == "gate",
              "buildings[9] = 星门 造价 420/300");
        check(pack.buildings[1].costMetal == 40 && pack.buildings[1].workers == 1,
              "buildings[1] = 太阳能板 造价 40、1 工人");
        check(std::string(pack.techs[5].key) == "atmo" && pack.techs[5].cost == 110 &&
                  pack.techs[5].req == (1u << 0),
              "techs[5] = atmo 造价 110，前置位掩码 = hydro(位0)");
        check(pack.techs[7].req == ((1u << 2) | (1u << 5)),
              "techs[7] = 星门理论，前置 = fusion(位2) | atmo(位5)");
        check(std::string(pack.weathers[0].name) == "晴朗" && pack.weathers[0].energy == 1.0,
              "weathers[0] = 晴朗 倍率 1.0");
        check(std::string(pack.weathers[1].name) == "沙暴" && pack.weathers[1].energy == 0.5,
              "weathers[1] = 沙暴 能源 0.5");
        check(pack.tuning.startMetal == 240 && pack.tuning.maxTurns == 90 &&
                  pack.tuning.foodPerPop == 1.15 && pack.tuning.clinicMitigationCap == 1,
              "tuning 关键字段（含后加入的 foodPerPop/clinicMitigationCap）正确");
    }

    // --- 全局表已由内容填充，且与 pack 一致 ---
    {
        check(std::string(BDEF[BType::HQ < BType::COUNT ? static_cast<size_t>(BType::HQ) : 0].key) == "hq",
              "全局 BDEF 已被内容填充（BDEF[0].key == hq）");
        check(std::string(TDEF[5].key) == "atmo" && TDEF[5].req == (1u << 0),
              "全局 TDEF 已被内容填充（atmo 前置 = hydro）");
        check(std::string(WDEF[1].name) == "沙暴" && WDEF[1].energy == 0.5,
              "全局 WDEF 已被内容填充");
        check(TUNE.startMetal == 240 && TUNE.maxTurns == 90 && TUNE.wavePerTurn == 0.8,
              "全局 TUNE 已被内容填充");
    }

    // --- 六类错误：各自给出可定位信息 ---
    std::vector<std::string> errs;

    // (1) 缺字段
    {
        const std::string dir = copyBaseTo("cv_missing");
        patchFile(dir, "tuning.json", "\"startMetal\": 240,", "");
        errs.clear();
        ContentPack p;
        check(!loadContentPack(dir, p, errs), "缺字段：拒绝加载");
        check(strContains(joinErrs(errs), "缺少必填字段") && strContains(joinErrs(errs), "startMetal"),
              "缺字段：报 \"startMetal\" 缺少必填字段");
    }
    // (2) 类型不符
    {
        const std::string dir = copyBaseTo("cv_type");
        patchFile(dir, "tuning.json", "\"maxTurns\": 90,", "\"maxTurns\": 90.0,");
        errs.clear();
        ContentPack p;
        check(!loadContentPack(dir, p, errs), "类型不符：拒绝加载");
        check(strContains(joinErrs(errs), "maxTurns") && strContains(joinErrs(errs), "期望整数"),
              "类型不符：整数字段给浮点被拒（maxTurns 期望整数）");
    }
    // (3) 未知 key（科技 requires）
    {
        const std::string dir = copyBaseTo("cv_unknownkey");
        patchFile(dir, "techs.json", "\"requires\": [\"hydro\"]", "\"requires\": [\"atmoo\"]");
        errs.clear();
        ContentPack p;
        check(!loadContentPack(dir, p, errs), "未知科技 key：拒绝加载");
        const std::string all = joinErrs(errs);
        check(strContains(all, "未知科技 key \"atmoo\"") && strContains(all, "是否想写 \"atmo\""),
              "未知科技 key：给出 \"是否想写 atmo？\" 提示");
    }
    // (4) 数组长度不符
    {
        const std::string dir = copyBaseTo("cv_length");
        writeAll(dir + "/weathers.json", "[]");
        errs.clear();
        ContentPack p;
        check(!loadContentPack(dir, p, errs), "数组长度不符：拒绝加载");
        check(strContains(joinErrs(errs), "数组长度") && strContains(joinErrs(errs), "期望 5"),
              "数组长度不符：报实际长度 != 期望 5");
    }
    // (5) 科技成环
    {
        const std::string dir = copyBaseTo("cv_cycle");
        patchFile(dir, "techs.json", "\"requires\": [],", "\"requires\": [\"gate\"],");
        errs.clear();
        ContentPack p;
        check(!loadContentPack(dir, p, errs), "科技成环：拒绝加载");
        check(strContains(joinErrs(errs), "成环"), "科技成环：报 \"前置科技成环\"");
    }
    // (6) 拼错字段名
    {
        const std::string dir = copyBaseTo("cv_typo");
        patchFile(dir, "tuning.json", "\"foodPerPop\":", "\"foodPerpop\":");
        errs.clear();
        ContentPack p;
        check(!loadContentPack(dir, p, errs), "拼错字段名：拒绝加载");
        const std::string all = joinErrs(errs);
        check(strContains(all, "未知字段") && strContains(all, "是否想写 \"foodPerPop\""),
              "拼错字段名：给出 \"是否想写 foodPerPop？\" 提示");
    }

    // --- 一次报告全部错误（不是逐个失败） ---
    {
        const std::string dir = copyBaseTo("cv_multi");
        patchFile(dir, "tuning.json", "\"startMetal\": 240,", "");
        patchFile(dir, "tuning.json", "\"startEnergy\": 90,", "");
        errs.clear();
        ContentPack p;
        check(!loadContentPack(dir, p, errs), "多个错误：拒绝加载");
        check(errs.size() >= 2, "一次报告全部错误（>=2 条）");
    }

    // --- 半初始化保护：initContent 失败不得改动全局表 ---
    {
        const std::string dir = copyBaseTo("cv_halfinit");
        patchFile(dir, "buildings.json", "\"cost\": { \"metal\": 40,", "\"cost\": { \"metal\": \"x\",");
        const int beforeMetal = TUNE.startMetal;
        const std::string beforeHq = std::string(BDEF[0].key);
        errs.clear();
        check(!initContent(dir, errs), "坏内容：initContent 返回 false");
        check(TUNE.startMetal == beforeMetal, "半初始化保护：TUNE 未被改动");
        check(std::string(BDEF[0].key) == beforeHq, "半初始化保护：BDEF 未被改动");
    }

    // --- 验收 #1：改一处 JSON 数值，不重编译即生效，可还原 ---
    {
        const std::string dir = copyBaseTo("cv_live");
        patchFile(dir, "tuning.json", "\"startMetal\": 240,", "\"startMetal\": 999,");
        errs.clear();
        check(initContent(dir, errs) && TUNE.startMetal == 999,
              "改 tuning.json 的 startMetal=999 立即生效（未重编译）");
        errs.clear();
        check(initContent("content/base", errs) && TUNE.startMetal == 240,
              "还原 content/base 后 startMetal 回到 240");
    }
}

// =====================================================================

int main() {
    if (!ensureContent()) return 1;   // P3a：内容未加载成功则拒绝启动
    std::printf("==== P1 协议化重构 · 独立验收测试 (tests/protocol_tests.cpp) ====\n");

    testCompatibilityLayer();
    testActionCodes();
    testResearchSuccess();
    testSnapshotPurity();
    testBoundaries();
    testSaveCompat();
    testLogCodes();
    testPendingSnapshot();
    testPendingInterception();
    testNewSnapshotFields();
    testPreviewBuildRules();
    testPreviewBuildPurity();
    testContentValidation();

    std::printf("==== 共 %d 项检查，失败 %d 项 ====\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
