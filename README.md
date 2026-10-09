# 星际争霸：殖民地 (Star Colony)

> **许可**：GNU Affero 通用公共许可证 v3.0（AGPL-3.0），详见 [LICENSE](LICENSE)。
> **仓库**：<https://github.com/sunhaochen-zxt/sc-colony>

一款 C++20 编写的**命令行殖民地经营游戏**：在 18×12 的行星地图上，用 90 个周期管理金属 / 能源 / 食物 / 科研，应对天气、随机事件与周期性虫潮，最终研究出「星门理论」并建成星门，完成撤离。

```
  ══════════════════════════════════════════════════════════════════════
    星际争霸：殖民地  STAR COLONY      周期 1 / 90     [新曙光]
  ══════════════════════════════════════════════════════════════════════
    金属 240(+0)   能源 90(+2)   食物 70(-7)   科研 0(+1)
    人口 6 / 8（闲置 4）   士气 70   防御 4   下一波虫潮 9 周期（预计强度 14）
    产出倍率：金属 x1.00  能源 x1.00  食物 x1.00  科研 x1.00   无修正
```

## 玩法核心循环

**看地图 → 建造 → 推进周期结算 → 研究科技 → 扛住虫潮 → 造星门撤离。**

每按一次回车（或输入 `next`）推进一个周期，游戏依次结算：天气 → 建筑修复 → 生产与消耗 →
矿脉开采与枯竭 → 在建工程 → 人口增长与士气 → 随机事件 → 酸雨腐蚀 → 虫潮 → 周期 +1。

- **资源**：金属（建造 / 回收）、能源（维持设施与星门）、食物（养人）、科研（解锁科技）。
- **地形**：平原 `.`、金属矿脉 `*`、地热口 `~`、冰层 `|`、山脉 `^`。矿场只能建在矿脉上，地热站只能建在地热口上，其余建筑只能建在平原或冰层上。
- **天气**：晴朗 / 沙暴 / 寒潮 / 耀斑 / 酸雨，对四种产出施加不同倍率，还会损坏建筑。
- **虫潮**：约每 9 个周期一波，强度随周期上升；防御不足会死人、毁建筑、抢金属。
- **胜利**：研究「星门理论」后建成星门（**420 金属 / 300 能源 / 8 工人 / 12 周期**）。
- **失败**：人口归零，或到达第 90 周期仍未撤离。

完整的命令表、建筑表、科技树、天气修正、事件分支与开局思路见 **[docs/MANUAL.md](docs/MANUAL.md)**。

## 编译与运行

项目只依赖 C++20 编译器（GCC / Clang 均可）和标准库，没有第三方依赖。两种构建方式任选其一。

### 方式一：Make（最快）

```bash
cd /home/shc/starcolony
make -j4            # 生成 build/starcolony、build/starcolony-rpc、build/selftest、build/trace
./build/starcolony  # 开始游戏（传统 CLI）
```

### 方式二：CMake

```bash
cd /home/shc/starcolony
cmake -S . -B build-cmake
cmake --build build-cmake -j4
./build-cmake/starcolony
ctest --test-dir build-cmake --output-on-failure   # 运行自检（即 selftest）
```

> 建议 CMake 使用 `build-cmake/`，因为 Makefile 会用到 `build/`；两套构建产物分开可避免互相覆盖。

### 常用启动参数

| 参数 | 说明 |
| --- | --- |
| `--seed N` / `-s N` | 指定随机种子，便于复现同一张地图与事件序列 |
| `--name 名字` / `-n 名字` | 指定殖民地名称（默认「新曙光」） |
| `--no-color` | 关闭 ANSI 颜色；输出非终端时也会自动关闭 |
| `--selftest` | 渲染一帧后立即退出，便于脚本检查界面 |
| `--help` / `-h` | 打印用法并退出 |

### 三分钟上手

```bash
make -j4
./build/starcolony --seed 42
```

进入游戏后：

1. 输入 `help` 查看全部命令，输入 `list` 查看建筑造价；
2. 输入 `build farm 7 5`（在平原上建水培农场），再输入 `build sol 9 5`；
3. 直接按回车推进周期，重复建造 / 研究即可。

## 测试

```bash
make test                                   # 等价于 ./build/selftest
# 或
ctest --test-dir build-cmake --output-on-failure
```

`selftest` 是无头自检，覆盖三类内容：

1. **规则不变量**：20 个随机种子的地图生成、初始资源与人口、资源非负、地块与建筑互相引用一致；
2. **存档往返**：存盘 / 读盘后状态逐字段一致，读档后继续推进 10 周期仍保持一致；
3. **AI 试玩**：内置 AI 在 8 个固定种子上完整跑局，校验科技线可达成、胜率不低于一半且并非稳赢。

全部通过时输出 `=== 检查 N 项，失败 0 项 ===` 并以退出码 0 结束。

## 目录结构

```
starcolony/
├── README.md              # 本文件：项目门面与快速上手
├── docs/
│   ├── MANUAL.md          # 玩家手册：命令、数值表、机制与开局
│   ├── PROTOCOL.md        # 引擎/前端接口契约（冻结版）
│   ├── QA_REPORT.md       # 对抗性边界测试报告
│   └── BALANCE.md         # 平衡性扫描与调参建议
├── Makefile               # 简易构建：all / test / run / clean
├── CMakeLists.txt         # CMake 构建
├── src/                   # 引擎 core（C++20，无终端依赖）
│   ├── types.hpp          # 地图尺寸、枚举、地形、静态数据表声明
│   ├── game.hpp           # Game 状态与规则接口（与界面解耦，可无头测试）
│   ├── game.cpp           # BDEF / TDEF / WDEF 三张数值表 + 全部核心规则
│   ├── protocol.hpp       # 契约类型：ActCode / LogEntry / ActionResult / GameSnapshot
│   ├── rpc_json.hpp       # 极简 JSON 读写（自研，保持零依赖）
│   ├── rpc_server.cpp     # starcolony-rpc：JSON-RPC over stdio 服务端
│   ├── ui.hpp / ui.cpp    # 传统 CLI 的终端渲染
│   └── main.cpp           # 传统 CLI 入口、命令解析与帮助文本
├── frontend/              # 图形化前端（Python + Textual）
│   ├── starcolony_tui/    # rpc / app / screens / widgets / i18n
│   └── tests/             # 契约测试与 P1 基线门禁（pytest）
├── tests/                 # C++ 测试
│   ├── selftest.cpp       # 无头自检：不变量 + 存档往返 + AI 试玩
│   ├── edge_tests.cpp     # 对抗性边界测试（存档、命令边界、长局）
│   ├── protocol_tests.cpp # 结构化协议、快照纯度、待决事件语义
│   └── ai.hpp             # 简易试玩 AI（selftest / trace / sweep 共用）
└── tools/
    ├── trace.cpp          # 平衡性调试：用 AI 跑一整局并逐周期打印
    └── sweep.cpp          # 批量扫描：512 种子胜率与参数敏感性
```

## 两种前端

引擎（`src/game.cpp`）不依赖任何界面代码，两个前端都只是它的客户端：

| 前端 | 入口 | 说明 |
| --- | --- | --- |
| **图形化 TUI（推荐）** | `PYTHONPATH=frontend .venv/bin/python -m starcolony_tui` | Python + [Textual](https://textual.textualize.io/)，方向键操作地图、建造菜单、事件弹窗、面板切换 |
| 传统 CLI | `./build/starcolony` | 纯 C++ 打字命令界面，零依赖，保留用于调试与脚本化 |

### 图形化前端

需要 Python 3.11+ 与 `textual`（仓库内已备 `.venv/`）：

```bash
python3 -m venv .venv && .venv/bin/python -m pip install textual   # 首次
PYTHONPATH=frontend .venv/bin/python -m starcolony_tui --seed 42
```

它会自动拉起 `build/starcolony-rpc` 作为子进程（可用 `--rpc` 或环境变量 `STARCOLONY_RPC` 指定路径）。
操作方式：`↑↓←→` 移动地图光标 · `Enter` 打开建造菜单 · 建筑字形键（`C S G M F H L K T X`）直接在光标处建造 ·
`Space`/`n` 推进周期 · `F2`~`F5` 切换建筑/科技/日志/帮助面板 · `e` 打开待决事件 · `?` 查看全部快捷键。

> 前端**不做任何规则判定**：能不能建、要花多少、人手够不够，一律问引擎。界面只是呈现层。

## 架构（v2 微内核）

```
content/            内容数据（后续阶段外置，改数值不重编译）
src/                引擎 core（C++20，无终端依赖）
  game.cpp/hpp        规则与状态
  protocol.hpp        前后端契约的 C++ 侧类型（ActCode / LogEntry / GameSnapshot）
  rpc_server.cpp      独立进程 starcolony-rpc：JSON-RPC over stdio
  main.cpp + ui.cpp   传统 CLI（另一个前端）
frontend/           Textual 前端（Python，独立进程）
docs/PROTOCOL.md    引擎/前端接口契约（冻结版）
```

设计要点：

- **规则住在 core**。例如"待决事件期间不允许其它操作"由引擎强制返回 `BlockedByPending`，前端只做体验层的模态拦截，正确性不依赖前端自觉。
- **前端不做规则运算**。`preview_build` 由引擎回答"能不能建/要多少钱/够不够"，`affordable` 也由服务端算。
- **可复现**。随机数用单一 `std::mt19937` 并全文入档，`--seed` 可复现同一局；同 seed 同操作的结果完全一致。
- **行为契约可验证**。`sweep` 用 512 个种子扫描胜率，`trace` 对固定种子逐周期打印，任何改动都要与基线逐字节比对。

## 设计要点

- **规则与界面解耦**：所有游戏逻辑都在 `sc::Game`（`game.hpp` / `game.cpp`）中，不依赖终端；`ui.cpp` 只负责把状态渲染成文本，因此自检与 AI 试玩可以完全无头运行。
- **数值集中在一处**：建筑 `BDEF`、科技 `TDEF`、天气 `WDEF` 三张表定义在 `src/game.cpp` 顶部，改数值不用碰逻辑；枚举与结构体在 `src/types.hpp`。
- **可复现**：随机数使用 `std::mt19937` 并支持 `--seed`；附带的 `trace` 工具与 `selftest` 都用固定种子，方便对比平衡性改动。
- **存档为纯文本**：`save [文件]` 写出带版本号的 `STARCOLONY 1` 文本存档，`load [文件]` 恢复，便于调试与手工检查。
- **无外部依赖**：仅标准库 + POSIX 终端尺寸查询（`ioctl`），`make` 或 `cmake` 直接可编。

## 文档索引

| 文档 | 内容 |
| --- | --- |
| [docs/MANUAL.md](docs/MANUAL.md) | 玩家手册：完整命令表、建筑表（造价 / 工期 / 工人 / 耗能 / 地形）、科技树、天气修正表、随机事件、虫潮机制、胜负条件、前 10 周期推荐开局 |
| [docs/PROTOCOL.md](docs/PROTOCOL.md) | 引擎/前端接口契约：JSON-RPC 方法清单、快照字段、事件码语义、两种失败的区分 |
| [docs/QA_REPORT.md](docs/QA_REPORT.md) | 对抗性边界测试报告：13 个已修缺陷的现象 / 根因 / 修法，ASan+UBSan 结论 |
| [docs/BALANCE.md](docs/BALANCE.md) | 平衡性扫描报告：512 种子胜率、24 个参数的敏感性分析、调参建议与修订记录 |

## 许可

本项目采用 **GNU Affero General Public License v3.0**。要点：

- 你可以自由使用、修改、分发本软件，包括商用；
- 但你**必须**以同等许可（AGPL-3.0）开源你的修改版源码；
- 若你把它改造成通过网络提供服务（例如挂在服务器上供人游玩），AGPL 要求你同样向用户提供修改后的完整源码。

完整条款见 [LICENSE](LICENSE)。版权声明与许可头建议在每个源文件顶部保留。
