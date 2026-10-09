// QA 探针（P3a）：把 src/content.cpp 的校验入口暴露成命令行，供 Python 对抗测试驱动。
// 不测任何 UI，只测「校验器该不该拒、错误信息是否可定位、是否一次报全部、失败是否半初始化」。
//
// 用法：
//   qa_content_probe load  <dir>          调 loadContentPack：打印全部错误，成功 exit 0，失败 exit 1
//   qa_content_probe init  <dir>          调 initContent（会落地全局表）：语义同上
//   qa_content_probe globals <dir>        先 initContent 再用全局表 dump（验内容是否真的生效）
//   qa_content_probe halfinit <good> <bad> 先 init(good) 成功，再 init(bad) 失败，打印全局表是否被改坏
#include "content.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace sc;

static std::string bdef0Desc() { return std::string(BDEF[0].desc); }
static std::string tdef0Key() { return std::string(TDEF[0].key); }
static int wdefCount() { return WEATHER_COUNT; }

static void dumpGlobals(const char* tag) {
    std::printf("%s BTYPE_COUNT %d\n", tag, BTYPE_COUNT);
    std::printf("%s BDEF0_DESC %s\n", tag, bdef0Desc().c_str());
    std::printf("%s TDEF0_KEY %s\n", tag, tdef0Key().c_str());
    std::printf("%s WDEF_COUNT %d\n", tag, wdefCount());
    std::printf("%s TUNE_startMetal %d\n", tag, TUNE.startMetal);
    std::printf("%s TUNE_foodPerPop %.10f\n", tag, TUNE.foodPerPop);
    std::printf("%s SCHEMA %d\n", tag, contentSchema());
}

static int runLoad(const std::string& dir) {
    std::vector<std::string> errs;
    ContentPack pack;
    const bool ok = loadContentPack(dir, pack, errs);
    std::printf("RESULT %s\n", ok ? "OK" : "FAIL");
    std::printf("ERROR_COUNT %zu\n", errs.size());
    for (const std::string& e : errs) std::printf("ERR %s\n", e.c_str());
    return ok ? 0 : 1;
}

static int runInit(const std::string& dir) {
    std::vector<std::string> errs;
    const bool ok = initContent(dir, errs);
    std::printf("RESULT %s\n", ok ? "OK" : "FAIL");
    std::printf("ERROR_COUNT %zu\n", errs.size());
    for (const std::string& e : errs) std::printf("ERR %s\n", e.c_str());
    return ok ? 0 : 1;
}

static int runGlobals(const std::string& dir) {
    std::vector<std::string> errs;
    const bool ok = initContent(dir, errs);
    std::printf("RESULT %s\n", ok ? "OK" : "FAIL");
    std::printf("ERROR_COUNT %zu\n", errs.size());
    for (const std::string& e : errs) std::printf("ERR %s\n", e.c_str());
    if (ok) dumpGlobals("G");
    return ok ? 0 : 1;
}

static int runHalfInit(const std::string& good, const std::string& bad) {
    std::vector<std::string> e1;
    if (!initContent(good, e1)) {
        std::printf("PRELOAD FAIL\n");
        for (const std::string& e : e1) std::printf("ERR %s\n", e.c_str());
        return 2;
    }
    // 记录成功加载后的全局表
    const std::string d = bdef0Desc();
    const std::string tk = tdef0Key();
    const int wc = wdefCount();
    const int sm = TUNE.startMetal;

    std::vector<std::string> e2;
    const bool ok = initContent(bad, e2);
    std::printf("SECOND_RESULT %s\n", ok ? "OK" : "FAIL");
    std::printf("SECOND_ERROR_COUNT %zu\n", e2.size());
    for (const std::string& e : e2) std::printf("ERR %s\n", e.c_str());

    const bool unchanged = (bdef0Desc() == d) && (tdef0Key() == tk) &&
                           (wdefCount() == wc) && (TUNE.startMetal == sm);
    std::printf("GLOBALS_UNCHANGED %s\n", unchanged ? "YES" : "NO");
    if (!unchanged) {
        std::printf("OLD_DESC %s\n", d.c_str());
        std::printf("NEW_DESC %s\n", bdef0Desc().c_str());
    }
    return ok ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "用法: %s load|init|globals <dir> | halfinit <good> <bad>\n", argv[0]);
        return 2;
    }
    const std::string mode = argv[1];
    if (mode == "load")    return runLoad(argv[2]);
    if (mode == "init")    return runInit(argv[2]);
    if (mode == "globals") return runGlobals(argv[2]);
    if (mode == "halfinit") {
        if (argc < 4) { std::fprintf(stderr, "halfinit 需要 <good> <bad>\n"); return 2; }
        return runHalfInit(argv[2], argv[3]);
    }
    std::fprintf(stderr, "未知 mode: %s\n", mode.c_str());
    return 2;
}
