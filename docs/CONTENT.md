# 内容数据契约（Content Schema v1）

> 本文件是 **P3 阶段「内容外置」的唯一契约**。引擎从 `content/` 目录读取定义，
> 不再把建筑 / 科技 / 天气 / 平衡参数编译进二进制。
>
> 状态：**P3a 冻结** · 依赖决策见 §7

---

## 1. 目标与阶段划分

用户目标：**能自己设定科技树与建筑**，为后续的地图/内容编辑器打基础。

| 阶段 | 内容 | 能否改数值而不重编译 | 存档格式 |
|---|---|---|---|
| **P3a**（本契约） | 外置**数值**：建筑 / 科技 / 天气 / 平衡参数 | ✅ 能 | **不变**（内部仍是枚举） |
| P3b | 类型字符串化：`BType`/`Tech`/`Weather` 枚举退场，`techs_` 位掩码改有序集合 | ✅ 能 | **会变**（改存 key） |
| P3c | 内容包覆盖（mod）、`validate` 子命令 | ✅ 能 | 不变 |

**P3a 的验收红线仍是"行为零变化"**：同一份内容下 `selftest` / `qa_edge` / `trace ×8` / `sweep 512` /
`存档 ×5` 全部与 P1 基线（`22af9bb`）逐字节一致。这条红线在 P3a 依然成立，因为内部表示没动。
**P3b 起该红线到期**（用户已确认无存量存档，格式可变）。

---

## 2. 文件布局

```
content/base/
  manifest.json     包元信息与 schema 版本
  buildings.json    建筑表（对应 BDEF，10 条）
  techs.json        科技表（对应 TDEF，8 条）
  weathers.json     天气表（对应 WDEF，5 条）
  tuning.json       平衡参数（对应 `Tuning` 的 **46** 个字段）
```

启动顺序：读 `manifest.json` → 按其 `files` 字段加载其余文件 → 校验 → 构建运行时表。
**任何一个文件缺失或校验失败都必须让引擎启动失败并给出可定位的错误**（文件名 + JSON 路径），
不得静默使用默认值 —— 静默回退会让玩家在错误的数值下游戏且毫无察觉。

---

## 3. `manifest.json`

```json
{
  "schema": 1,
  "pack_id": "base",
  "name": "基础内容包",
  "files": {
    "buildings": "buildings.json",
    "techs": "techs.json",
    "weathers": "weathers.json",
    "tuning": "tuning.json"
  }
}
```
- `schema`（int，必填）：内容格式版本。引擎声明 `kMinSchema`/`kMaxSchema`，超出范围**拒绝启动**并提示。
- `pack_id`（string，必填）：内容包标识，后续写进存档（P3b）用于校验内容与存档是否匹配。
- `files`（object，必填）：四个键都必须存在。

---

## 4. `buildings.json`

顶层是**数组**，元素对应 `BDef`（`src/types.hpp`）。顺序即运行时索引顺序。

```json
[
  {
    "key": "hq",
    "name": "指挥中心",
    "glyph": "C",
    "color": "blue",
    "cost": { "metal": 0, "energy": 0, "science": 0 },
    "build_turns": 0,
    "workers": 2,
    "upkeep": 2,
    "repeatable": false,
    "desc": "殖民地核心：+4 能源 +1 科研，提供 8 人口上限"
  }
]
```

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `key` | string | ✅ | 英文命令键（`build <key> x y`）。**全局唯一**，只允许 `[a-z0-9_]+` |
| `name` | string | ✅ | 中文显示名 |
| `glyph` | string | ✅ | 单字符，地图字形。前端据此生成建造快捷键，**必须唯一** |
| `color` | string | ✅ | 颜色**名**而非整数：`default`/`grey`/`red`/`green`/`yellow`/`blue`/`magenta`/`cyan`/`white`/`bright_white`（对应 `Col` 枚举顺序）。用名字而不是数字，避免枚举增删导致颜色漂移 |
| `cost.metal` `.energy` `.science` | int ≥ 0 | ✅ | 造价 |
| `build_turns` | int ≥ 0 | ✅ | 建造所需周期（0 = 立即完工） |
| `workers` | int ≥ 0 | ✅ | 所需工人数；`0` 表示无需工人 |
| `upkeep` | int ≥ 0 | ✅ | 每周期能源消耗 |
| `repeatable` | bool | ✅ | 是否可重复建造 |
| `desc` | string | ✅ | 说明文案（玩家手册与界面共用） |

**约束**：数组长度必须等于 `BTYPE_COUNT`（P3a 阶段内部仍是编译期枚举，数量必须吻合）；
`key` 与 `glyph` 各自唯一；`hq` 必须存在且为 `repeatable: false`。

---

## 5. `techs.json`

```json
[
  { "key": "hydro", "name": "水培改良", "cost": 35, "requires": [], "desc": "农场食物产出 +50%" },
  { "key": "gate",  "name": "星门理论", "cost": 170, "requires": ["fusion", "atmo"], "desc": "解锁星门工程" }
]
```

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `key` | string | ✅ | 科技键（`research <key>`）。唯一 |
| `name` | string | ✅ | 中文名 |
| `cost` | int ≥ 0 | ✅ | 研究所需科研点 |
| `requires` | array\<string\> | ✅ | **前置科技的 key 数组**（不允许空科技；无前置写 `[]`）。P3a 内部会转成原有的位掩码，因此**必须能对应到合法 key** |
| `desc` | string | ✅ | 效果说明 |

**约束**：数组长度必须等于 `TECH_COUNT`；`requires` 里每个 key 都必须存在；不允许自引用；
科技树必须无环（P3a 逐条校验，发现环直接拒绝启动 —— 位掩码语义下成环会导致永远研究不出来）。

> **顺序很重要**：P3a 阶段科技索引同时决定 `techs_` 位掩码的位号，**改变顺序会改变存档语义**。
> 若与 P1 基线比对时发现差异，先检查顺序是否被无意改动。P3b 起顺序不再有语义。

---

## 6. `weathers.json` 与 `tuning.json`

### 6.1 `weathers.json`
```json
[
  { "key": "clear", "name": "晴朗", "color": "green",
    "mult": { "metal": 1.0, "energy": 1.0, "food": 1.0, "science": 1.0 }, "desc": "无修正" }
]
```
`key` 是**新增字段**（原 `WeatherDef` 没有 key，靠枚举下标识别）——P3a 就加上，
这样 P3b 做类型字符串化时不必再改一次数据文件。数组长度必须等于 `WEATHER_COUNT`，
第一个必须是 `clear`（晴朗），且倍率字段名固定为 `metal`/`energy`/`food`/`science`。

### 6.2 `tuning.json`
**扁平对象**，字段名与 `src/types.hpp` 的 `Tuning` 结构**逐字对应**（**46** 个字段）：
```json
{
  "maxTurns": 90, "startMetal": 240, "startEnergy": 90, "startFood": 70, "startPop": 6,
  "foodPerPop": 1.15, "wavePerTurn": 0.8, "turretDefense": 16
}
```
- **缺少任何字段都算校验失败**（不设默认值）—— 否则新增字段时容易漏配而无人发现。
- 类型必须严格匹配：整数字段给浮点（如 `"maxTurns": 90.0`）算失败；浮点字段给整数可以。
- 所有字段名都必须在 `Tuning` 里存在，**多余字段是校验失败**（拼错字段名必须报错而不是被忽略）。
- 取值范围：沿用原代码里的隐含约束（如概率类字段 ≥ 0、`maxTurns ≥ 1`）。P3a 只做基本范围检查，
  不做平衡性判断。

---

## 7. 依赖与构建决策（效率优先）

用户明确指示：**不以"零构建期依赖"为目标，怎么开发效率高怎么来**。据此：

- **JSON 库**：vendored **nlohmann/json 3.11.3** 单头文件，落在 `third_party/nlohmann/json.hpp`。
  理由：无需包管理器、克隆即可编译；`dump()` / 带行列号的解析错误对**手工编辑内容文件**的场景
  非常重要（玩家写错 JSON 时要能一眼看出错在哪）。相比自研解析器，这是省时间的选择。
- **编译成本隔离**：该头文件**只允许被 `src/content.cpp` 一个翻译单元包含**。
  `game.cpp` / `rpc_server.cpp` 等一律只 include `src/content.hpp`（纯 struct，不含 JSON 依赖）。
  实测该头文件会让单个 TU 的编译时间增加约 6.5 秒 —— 必须只付一次。
- **Makefile 目标文件化**：当前 `game.cpp` 被 4 个 target 各编译一遍（约 4×3.8s）。
  P3a 顺带改成目标文件复用的形式（`build/obj/%.o` + 链接），整体构建时间应**不增反降**。
  CMake 侧把 `content.cpp` 并入已有的 `sc_core` 静态库即可。
- **`make` 与 `cmake` 两条路径都必须可用且一致**，不得出现只有一条能编的情况。

---

## 8. 校验与错误报告（P3a 必须实现）

启动时 `validate()` 逐项检查，任一失败即**拒绝启动**，错误信息必须包含：

```
content/base/buildings.json: [3].cost.energy: 缺少必填字段
content/base/techs.json: [6].requires[0]: 未知科技 key "atmoo"（是否想写 "atmo"？）
content/base/tuning.json: unknown field "foodPerpop"（是否想写 "foodPerPop"？）
content/base/weathers.json: 数组长度 4 != 期望 5
```

要求：
1. **定位到具体文件 + JSON 路径 + 字段名**，不要只说"内容格式错误"
2. 拼写相近的 key 给出"是否想写 X？"的提示（编辑体验的关键）
3. 一次性报告**全部**错误，不要逐个失败（玩家不该改一个跑一次）
4. 校验失败时**不得留下半初始化的引擎状态**（沿用既有"读档失败保留原状态"的思路）

P3c 会把 `validate` 暴露成 `starcolony-rpc validate` 子命令，P3a 先做成可被调用的函数并**加测试**。

---

## 9. 给 P3a 的验收清单

| # | 验收项 | 判定 |
|---|---|---|
| 1 | 改一处 JSON 数值（如 `tuning.json` 的 `startMetal`），**不重编译**即生效 | 开一局看初始金属变化 |
| 2 | 同内容下 `selftest` / `qa_edge` / `trace ×8` / `sweep 512` / **`存档 ×5`** 与 `22af9bb` 基线逐字节一致 | 红线 |
| 3 | 值转写正确：JSON 里的 10 建筑 / 8 科技 / 5 天气 / **46** 参数与 P3a 之前的硬编码值**逐字段相等** | 由 QA 独立逐字段比对，非抽样 |
| 4 | 缺字段 / 类型错 / 未知 key / 长度不符 / 科技成环 / 拼错字段名 —— 六类错误都能给出可定位信息且拒绝启动 | 构造坏内容实测 |
| 5 | `make` 与 `cmake` 均可构建，`-Wall -Wextra` 零警告，且总构建时间**不劣于**改造前 | 计时对比 |
| 6 | `nlohmann/json.hpp` 只被 `content.cpp` 包含 | `grep` 验证 |
