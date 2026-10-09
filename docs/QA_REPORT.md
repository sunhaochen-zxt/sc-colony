# 星际争霸：殖民地 —— 对抗性边界测试报告（T2）

测试文件：`tests/edge_tests.cpp`（799 行，33959 项断言）
被测对象：`src/game.cpp` + `src/types.hpp`（`Game` 类，无头、可脱离界面运行）

## 0. 结论速览

| 阶段 | 检查项 | 失败项 | 标记缺陷 | ASan/UBSan |
|---|---:|---:|---:|---|
| 修复前 | 33959 | 20 | **13** | fork 隔离用例崩溃（SIGFPE / UB） |
| 修复后 | 32331 | **0** | **0** | **0 条报告，退出码 0** |

13 个缺陷已全部修复并回归验证。修复集中在 `src/game.cpp`（9 处）与 `src/types.hpp`（1 处新增参数）。

复现命令：

```bash
# 常规构建
g++ -std=c++20 -O2 -Isrc -Itests tests/edge_tests.cpp src/game.cpp -o build/qa_edge && ./build/qa_edge

# 内存 / 未定义行为检查
g++ -std=c++20 -g -fsanitize=address,undefined -Isrc -Itests tests/edge_tests.cpp src/game.cpp -o build/qa_asan
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 ./build/qa_asan
```

---

## 1. 缺陷清单与修法

### B1　在建建筑提前提供效果（真实玩法缺陷，影响最大）
- **现象**：`doBuild("hab")` 后 `buildLeft=2`（尚未完工），住房上限立刻从 8 跳到 14。
- **根因**：`recomputeWorkers()` 用 `countType(BType::Hab)` 计算住房，而 `countType` 只看 `alive`，不看 `buildLeft`。
- **连带**：同一根因让在建医疗站提前加速人口增长（`growPopulation`）、提前减免虫潮与饥荒伤亡（`applyCombat` / `advanceTurn`）。
- **修法**：新增 `Game::countReady(BType)`（只数 `buildLeft <= 0`），住房、人口增长、虫潮减伤、饥荒减伤四处改用它。星门的"禁止重复建造"检查保留 `countType`（在建也必须拦住），事件触发条件也改用 `countReady`（语义是"已拥有该设施"）。
- **副作用**：该修复让 AI 胜率从 72.9% 降到 65.0%（512 种子），属预期内的难度上升。

### B2　脏参数导致整数除零（SIGFPE）
- **现象**：`TUNE.combatLossDiv = 0` 时 `applyCombat` 中 `over / TUNE.combatLossDiv` 触发信号 8，进程直接终结。
- **修法**：`const int lossDiv = std::max(1, TUNE.combatLossDiv);`（同文件的 `combatDamageDiv` 早已有此保护，`combatLossDiv` 是漏网的）。

### B3a / B3b　`state` 段不做值域校验
- **现象**：`pop=-5`、`morale=500` 的存档都能读档成功。负人口直接破坏工人分配；`morale=500` 会把产出士气倍率放大到 3.25。
- **修法**：`readFile` 的 `state` 分支增加值域校验——`pop_ >= 0`、`morale ∈ [0,100]`、`turn_ >= 1`、`popAcc_ >= 0`、`waveIn_ >= 0`、`weatherLeft_ >= 0`、未知科技位，任一越界即整体判为存档损坏。

### B4　地块与建筑的引用只校验了单向
- **现象**：两个地块同时标注 `building=0` 也能读档成功，世界里出现"一栋建筑占两格"。
- **根因**：原校验只做了"建筑 → 地块"方向，缺"地块 → 建筑"方向。
- **修法**：新增遍历全部地块，要求 `blds_[t.building].x/y` 与该地块坐标一致，否则判损坏。

### B5a / B5b　必需段缺失被静默接受
- **现象**：删掉 `res` 段后资源全部变 0，删掉 `state` 段后回合/人口/科技全部用默认值——读档"成功"，但世界是错的。
- **修法**：用 `haveRes` / `haveState` 标志跟踪，末尾缺失即拒绝载入。

### B5c　`report` 段解析失败仍算成功
- **现象**：`report` 行字段不全时 `haveReport` 依然置 true，于是跳过 `evaluate()` 重算，界面拿到一份全 0 的收支预测。
- **修法**：补 `if (!ss) badLine("report 字段"); else haveReport = true;`。

### B5d　待决事件字段未校验值域
- **现象**：存档里把难民事件的 `a` 改成 `-50`，读档后选择"接收"会令 `pop()` 变成 -44。
- **修法**：`pending` 分支校验 `kind ∈ [EV_REFUGEES, EV_CARAVAN]` 且 `a/b/c ∈ [0, 100000]`。

### B6　超大资源导致有符号整数溢出（UB）
- **现象**：存档写入 `res.science = INT_MAX`，推进一个周期后 `res_.science += scienceNet` 溢出变负。
- **根因**：`metal` / `energy` 都有 `std::max(0, ·)` 兜底（但同样先溢出），`science` 连兜底都没有。
- **修法**：新增 `satAdd(int cur, int delta)`，在 `long long` 上计算后夹回 `[0, INT_MAX]`；食物走另一条路径（允许为负以判定饥荒），只夹 `int` 值域不夹 0。金属/能源/科研三处全部改用 `satAdd`。

### B7a / B7b　脏 `Tuning` 参数把新开局带进非法状态
- **现象**：`startPop` 为负时开局人口为负；`housingPerHab` 为负时住房上限变负，进而把人口静默钳成负数，无日志、无结束判定。
- **修法**：`newGame()` 对 `startPop` / `baseHousing` 取 `max(0, ·)`、`moraleTarget` 夹到 `[0,100]`；`recomputeWorkers()` 中 `housing_` 取 `max(0, ·)`。

### B8　`wavePerTurn` 极大导致 double→int 未定义转换
- **现象**：`waveStrengthEstimate()` 直接 `static_cast<int>` 一个远超 int 范围的 double，UBSan 报 `float-cast-overflow`，结果是 `INT_MIN` 被钳成 5——比 `waveBase` 还小，虫潮强度反而归零。
- **修法**：先把 double 夹到 `±(INT_MAX/4)` 再转换。

---

## 2. 覆盖的路径（未发现问题的部分）

以下路径全部通过，无缺陷标记：

- **读档失败必须保留原状态**：`loadFrom` 用临时对象解析，失败时原局不受影响（严格校验路径验证通过）。
- **命令边界**：`doBuild` 在 `(0,0)`、`(17,11)`、越界坐标、非整数参数、类型名大小写、中文名；`doDemolish` / `doToggle` / `doFocus` 传 `-1`、`9999`、已拆除 id；`doResearch` 重复研究、前置缺失、科研不足；重复建星门；在矿脉 / 地热 / 山脉 / 冰层上建房的合法性。
- **数值不变量**：资源非负、人口非负、士气 ∈ [0,100]、`idleWorkers ∈ [0,pop]`、工人分配之和 ≤ 人口、矿脉开采不会让矿石变负、建筑与地块双向一致。
- **长局与极端**：连续 500 次 `advanceTurn`（终局后停止结算）、资源设为极大后推进、0 工人、全民饥饿、连续虫潮、重新开局后状态完全重置。

---

## 3. 平衡性联动发现

修复 B1 后重跑 512 种子扫描，基线胜率由 **72.9% → 65.0%**（在建建筑不再提前生效，难度上升符合预期）。
因此 `docs/BALANCE.md` 里基于旧基线标定的方案需要重新测量，详见该文件的「修订记录」章节。
