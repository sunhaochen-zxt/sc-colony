# 星际争霸：殖民地 —— 引擎/前端接口协议（P2 冻结版）

> 本文件是 **core（C++）与前端（Python + Textual）之间唯一的契约**。
> 两侧可据此并行开发；任何一方需要改动契约，必须先改本文件并同步另一方。
>
> 状态：**已冻结（v1）** · 对应提交 `22af9bb` 之后的 P2 阶段

---

## 1. 传输层

| 项 | 约定 |
|---|---|
| 载体 | 独立进程 `starcolony-rpc`，前端把它作为**子进程**启动 |
| 协议 | JSON-RPC 2.0 子集 |
| 分帧 | **一行一个 JSON 对象**（newline-delimited），UTF-8，行内不得有裸换行（由 JSON 转义保证） |
| 方向 | 请求走子进程 stdin，响应走子进程 stdout |
| stderr | 服务端日志/调试信息，前端**忽略**（但可转发给用户便于排错） |
| 主动消息 | **服务端绝不主动推送**。所有输出都是对某条请求的响应，前端无需处理乱序消息 |

服务端与客户端各写一行、各读一行，严格请求-响应配对。响应必须与请求同序（P2 不用 id 匹配，但 `id` 字段仍按 JSON-RPC 规范回填）。

### 1.1 空行与 `id` 校验（补充规定）

- **空行策略**：**空行不是请求，服务端静默忽略、不产生任何响应**。理由是避免应答错位——若为空行回一条错误，客户端与请求的配对关系会被打乱。**客户端因此不得发送空行，也不得等待空行的响应**。实现见 `src/rpc_server.cpp` 的 `isBlank(line)` 判断。
- **`id` 类型校验**：`id` 只允许 **string / number / null**（JSON-RPC 2.0 §4）。出现对象、数组等其它类型时，服务端必须返回 `-32600` 非法请求，且响应里的 `id` 置为 `null`（因为原 id 不可回填）。
- 畸形输入（非法 JSON、缺 `method`、`method` 非字符串、未知 method）一律返回对应协议错误，**进程不得崩溃或退出**；`-32700` / `-32600` / `-32601` / `-32602` 的分配见第 2 节。

---

## 2. 错误处理：两种失败必须区分

| 类型 | 表达 | 何时用 |
|---|---|---|
| **协议/传输错误** | JSON-RPC 标准 `error` 对象 | 请求不是合法 JSON、缺 `method`、未知 method、参数类型错误 |
| **游戏内失败** | `result.ok = false` + `result.result.code` | 未知建筑、坐标越界、科研点不足等 —— **这是正常业务结果，不是协议错误** |

```json
// 协议错误（非法请求）
{"jsonrpc":"2.0","id":7,"error":{"code":-32601,"message":"未知方法: foo"}}

// 游戏内失败（业务结果）
{"jsonrpc":"2.0","id":8,"result":{"ok":false,"result":{"code":"BuildBlocked","text":"无法建造：该地块已被占用","args":["该地块已被占用"]}}}
```

`code=-32700` 解析错误 / `-32600` 非法请求 / `-32601` 未知方法 / `-32602` 参数错误。

---

## 3. `code` 的序列化：用**枚举名字符串**，不用整数

`ActCode` 以**字符串**传输（如 `"WaveBreached"`、`"BuildStarted"`），**禁止**用整数下标。

理由：`src/protocol.hpp` 的 `ActCode` 会随内容扩展而新增枚举值，整数下标会因插入位置变化而漂移，字符串名稳定且可读、便于前端 `switch`。完整清单见 `src/protocol.hpp:33-105`。

---

## 4. 方法清单

### 4.1 `ping`
```json
→ {"jsonrpc":"2.0","id":1,"method":"ping"}
← {"jsonrpc":"2.0","id":1,"result":{"ok":true,"pong":true}}
```
用于前端确认子进程已就绪。

---

### 4.2 `new_game`
```json
→ {"jsonrpc":"2.0","id":2,"method":"new_game","params":{"seed":12345,"name":"新曙光"}}
← {"jsonrpc":"2.0","id":2,"result":{"ok":true,"snapshot":{...}}}
```
| 参数 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `seed` | int | 否 | 省略则由服务端取当前时间（不可复现，仅用于真实游玩） |
| `name` | string | 否 | 省略为「新曙光」 |

**返回里直接带完整 snapshot**，前端一次往返即可开画。

---

### 4.3 `content_info`
```json
→ {"jsonrpc":"2.0","id":3,"method":"content_info"}
← {"jsonrpc":"2.0","id":3,"result":{"ok":true,"mapW":18,"mapH":12,"maxTurns":90,
     "buildings":[...],"techs":[...],"weathers":[...]}}
```
前端启动时拉**一次**并缓存。字段（全部来自引擎的 `BDEF`/`TDEF`/`WDEF`，前端不得硬编码）：

```json
"buildings":[{"key":"mine","name":"钻矿场","glyph":"M","color":1,
              "costMetal":60,"costEnergy":10,"costScience":0,
              "buildTurns":3,"workers":3,"upkeep":3,"repeatable":true,"desc":"..."}],
"techs":    [{"key":"hydro","name":"水培改良","cost":35,"req":[],"desc":"..."}],
"weathers": [{"name":"晴朗","color":3,"metal":1.0,"energy":1.0,"food":1.0,"science":1.0,"desc":"无修正"}]
```
`color` 沿用引擎的 `Col` 整数枚举（`src/types.hpp:25-36`，0=默认 1=灰 2=红 3=绿 4=黄 5=蓝 6=品红 7=青 8=白 9=亮白）。前端把它映射成终端颜色，**不要**自己编一套。

`mapW`/`mapH` 必须从这里取 —— 前端**禁止**硬编码 18×12（后续星际层会改尺寸）。

---

### 4.4 `snapshot`
```json
→ {"jsonrpc":"2.0","id":4,"method":"snapshot"}
← {"jsonrpc":"2.0","id":4,"result":{"ok":true,"snapshot":{...}}}
```
**纯查询**：`snapshot` 是 `const` 且**不消耗随机数**（已由 `tests/protocol_tests.cpp` 的 [D] 组断言）。前端可以任意频率调用。

`snapshot` 对象字段（与 `GameSnapshot`，`src/protocol.hpp:227-252` 一一对应）：

| 字段 | 类型 | 说明 |
|---|---|---|
| `turn` | int | 当前周期 |
| `metal` `energy` `food` `science` | int | 资源 |
| `pop` `housing` `morale` | int | 人口 / 住房上限 / 士气 |
| `weather` | int | 天气下标（配合 `content_info.weathers` 取值） |
| `weatherLeft` | int | 当前天气剩余周期 |
| `waveIn` | int | 距下一波虫潮的周期数 |
| `waveStrengthEstimate` | int | 虫潮强度预估（纯公式，不含随机波动） |
| `defense` | int | 当前防御力 |
| `metalIn` `energyIn` `foodIn` `scienceIn` | int | 上周期产出 |
| `energyUp` `foodUp` | int | 上周期消耗 |
| `metalNet` `energyNet` `foodNet` `scienceNet` | int | 上周期净收支 |
| `brownout` `starving` | bool | 上周期是否能源透支 / 食物短缺 |
| `over` `won` | bool | 是否结束 / 是否胜利 |
| `endReason` | string | 结局说明 |
| `colonyName` | string | 殖民地名 |
| `buildings` | array | 见下 |
| `tiles` | array | 按 `y*mapW+x` 展平，长度 = `mapW*mapH` |
| `log` | array | 结构化日志（见 4.5） |
| `techs` | array\<string\> | 已研究科技的 **key** 列表 |
| `assigned` | array\<int\> | 每个建筑分到的工人数 |
| `idleWorkers` | int | 闲置殖民者数（引擎 `idleWorkers()`；**前端不得自行按 `pop-Σassigned` 估算**） |
| `seed` | int | 本局**实际**使用的随机种子；**`0` 表示未知**（例如对局由存档载入——存档格式在 P2 阶段不含种子，P3 重构存档时再持久化）。前端仅在 `seed > 0` 时显示 |
| `pending` | object \| null | 当前待决事件；无事件时为 `null`（见 4.4.1） |

#### 4.4.1 `pending` —— 待决事件

**这是前端渲染事件弹窗的唯一数据来源**（`log` 里只有「事件触发」的提示，不含选项文案与编号，无法据此应答）。

```json
"pending": {
  "kind": "EventRefugees",
  "title": "难民船请求降落",
  "text": "一艘破旧的运输船在轨道上请求降落，船上有 5 名难民。接收他们会消耗约 40 食物。",
  "options": ["接收难民（+5 人口，40 食物）", "拒绝降落（士气 -5）", "征用他们的补给（+60 金属，士气 -10）"]
}
```
| 字段 | 说明 |
|---|---|
| `kind` | 事件类型，`ActCode` 枚举名字符串，取值是 `EventRefugees` / `EventMarket` / `EventSignal` / `EventLifeSupport` / `EventMeteorHit` / `EventProspectFound` / `EventVentFound` / `EventFestival` / `EventCaravan` 之一（前端可据此配色与配图标） |
| `title` | 事件标题 |
| `text` | 事件正文 |
| `options` | 选项文案数组，**下标 1 基**：`options[0]` 对应 `command{action:"answer", option:1}` |

前端处理规则：
- `pending !== null` 时**应当**拦截常规操作并弹出选择界面
- 选项通过 `command{action:"answer","option":N}` 提交，`N` 从 1 开始
- 应答后 `command` 返回的新 `snapshot.pending` 若仍非 null，说明还有后续事件（事件链），前端继续弹

**引擎侧的强制规则（core 负责，前端不得依赖自己的拦截）**：

`pending !== null` 期间，除 `answer` 之外的**所有** action（`build` / `demolish` / `toggle` / `focus` / `research` / `advance`）都必须被**引擎拒绝**，返回：

```json
"result": {"ok": false, "code": "BlockedByPending", "text": "有事件需要先处理（输入选项数字）", "args": []}
```

为什么必须由 core 强制而不是前端自觉：

1. **规则必须住在 core**。这是微内核的基本约定 —— 前端只做呈现，不做规则判定。此前这条规则只存在于旧 CLI（`main.cpp:325/348/365/376`），引擎完全不知情，导致 CLI 拦、RPC 不拦，两个前端行为分叉。
2. **允许期间操作会破坏事件语义**：事件选项的代价（如「接收难民需 40 食物」）在触发时就已确定，若允许中途 `advance` 或 `build`，资源会在玩家做选择前发生变化，可能出现「选项说得出、实际做不起」的矛盾状态。

前端仍应自己做拦截以给出更好的交互（例如直接把弹窗设为模态），但**不得把正确性建立在拦截之上** —— 引擎一定会兜住。

**对旧 CLI 的已知影响（已接受，非缺陷）**：

旧 CLI 只在 `next` / 空行 / 数字选择四处做了待决守卫（`src/main.cpp:325/348/365/376`），对 `build` / `demolish` / `toggle` / `focus` / `research` **是放行的**——那属于旧实现的规则缺失。规则上移 core 之后，这五个命令在待决期间由「成功」变为「被拒」，提示「有事件需要先处理（输入选项数字）」。

因此 **「CLI 输出与 `22af9bb` 基线逐字节一致」这条回归红线，只在会话过程中不出现待决事件时成立**；含待决事件的会话存在**预期内**的输出差异。这条差异是本次修正的目的，不是回归。

```json
"buildings":[{"id":0,"type":0,"typeKey":"hq","x":8,"y":6,
              "buildLeft":0,"damaged":0,"enabled":true,"alive":true,
              "assigned":2,"workerNeed":2}]
"tiles":    [{"terrain":46,"ore":0,"richness":0,"building":-1}]
```
`tiles[].terrain` 是地形字符的**字符编码**（`.`=46 `*`=42 `~`=126 `|`=124 `^`=94），前端据此渲染，并**从 `content_info.buildings[].glyph` 取建筑字形**，不要硬编码图例。

`buildings[].workerNeed` 是该建筑**当前所需工人数**（引擎 `workerNeed()`，已计入科技等修正）。前端用 `assigned / workerNeed` 显示人手是否充足，**不得自行推断需求数**；`workerNeed == 0` 时按"无需工人"处理。人手不足（`assigned < workerNeed`）应给出可见告警——这是玩家最容易忽视的产出损失来源。

**`log` 全量返回**（引擎内部上限 400 条）。前端若需要窗口化显示，自行截取；协议层不做截断。

---

### 4.5 日志条目 `LogEntry`
```json
{"turn":12,"code":"WaveBreached","ints":[3,120,1],"strings":[],"text":"✖ 防线被突破：-3 人口，-120 金属，士气 -8"}
```
| 字段 | 说明 |
|---|---|
| `turn` | 产生该条日志的周期 |
| `code` | `ActCode` 枚举名字符串 |
| `ints` | 数值参数（按 `code` 的定义位置取用） |
| `strings` | 文本参数 |
| `text` | **服务端渲染好的中文**（由 `LogEntry::text()` 产出，与改造前 CLI 逐字一致） |

**P2 的显示策略**：前端**优先用 `code` 做布局与配色决策**（例如虫潮红色、建造完成绿色），**文本先用 `text` 字段**保证与 CLI 输出一致；等到 P3 内容外置时，再把文案迁到 `content/i18n/zh-CN.json` 由前端自行查表。这样 P2 不必先搬一份文案表，也不会出现前后端文案不一致。

---

### 4.6 `command` —— 唯一的变更入口

```json
→ {"jsonrpc":"2.0","id":5,"method":"command",
   "params":{"action":"build","key":"mine","x":5,"y":3}}
← {"jsonrpc":"2.0","id":5,"result":{
     "ok":true,
     "result":{"code":"BuildStarted","text":"开始建造 钻矿场 #3 于 (5,3)，需 3 周期","args":["钻矿场","3","5","3","3","1"]},
     "log":[{...}], 
     "snapshot":{...}}}
```

**`command` 的返回一定同时包含 `log` 与 `snapshot`** —— 一个回合一次往返，前端不需要额外的 `snapshot` 调用。

`action` 清单（**冻结**）：

| action | 参数 | 对应引擎方法 |
|---|---|---|
| `build` | `key`(string), `x`(int), `y`(int) | `doBuild` |
| `demolish` | `id`(int) | `doDemolish` |
| `toggle` | `id`(int) | `doToggle` |
| `focus` | `id`(int) | `doFocus` |
| `research` | `key`(string) | `doResearch` |
| `answer` | `option`(int, 从 1 开始) | `answer` |
| `advance` | 无 | `advanceTurn`（推进一个周期） |

`advance` 走 `command` 而不是独立方法，是为了让"一次操作 = 一次往返 + 一个 snapshot"这条规则没有例外。

**前提约束**：当 `snapshot.pending !== null` 时，上表中除 `answer` 外的所有 action 都会被引擎拒绝并返回 `code = "BlockedByPending"`（详见 §4.4.1）。这是 core 强制的规则，不是前端的自觉。

失败示例（业务失败，HTTP/协议层仍成功）：
```json
← {"jsonrpc":"2.0","id":6,"result":{"ok":false,
     "result":{"code":"ResearchNoScience","text":"科研点不足：星门理论 需要 170，当前 40","args":["星门理论","170","40"]},
     "log":[],"snapshot":{...}}}
```
前端**必须**用 `result.code` 判定成败，**禁止**去解析 `text`。

---

### 4.6.1 `preview_build` —— 建造可行性的只读查询

**纯查询，不改任何状态**（建造菜单靠它把非法选项直接灰掉，而不是"先试一下看报什么错"）。

```json
→ {"jsonrpc":"2.0","id":11,"method":"preview_build","params":{"key":"mine","x":5,"y":3}}
← {"jsonrpc":"2.0","id":11,"result":{
     "ok":true,
     "buildable":true,
     "reason":"",
     "cost":{"metal":60,"energy":10,"science":0},
     "affordable":{"metal":true,"energy":true,"science":true}}}
```

| 字段 | 说明 |
|---|---|
| `buildable` | **纯规则层面**是否可建：地形 / 地块占用 / 前置科技 / 唯一性（对应引擎 `Game::buildableTerrain()`）。**不含资源检查** |
| `reason` | 不可建时的原因文案（与 `command{build}` 失败时 `text` 中的原因同源，前端可直接显示） |
| `cost` | 该建筑的造价（来自 `BDEF`） |
| `affordable` | 当前资源是否够付 `cost` —— **由服务端计算**，前端不得重复这条规则 |

> **实现提示**：引擎原有的 `Game::buildable()` 是「规则 + 资源」的合并判断，会把"金属不足"报成不可建。
> 因此 P2.1 起将其拆为 `buildableTerrain()`（纯规则）与 `buildable()`（= 地形规则 + 原资源检查）。
> 拆分是纯内部重构，`doBuild` 与既有调用方**行为与文案逐字不变**。`preview_build` 只使用前者。

**成功响应（严格只含这 5 个键）**
```json
{"ok":true,"buildable":true,"reason":"","cost":{"metal":60,"energy":10,"science":0},
 "affordable":{"metal":true,"energy":true,"science":true}}
```

**失败响应（P2.1 修订：顶层与镜像并存，有意为之）**
```json
{"ok":false,
 "code":"BuildUnknownType",
 "text":"未知建筑类型：zzz_nope（输入 list 查看）",
 "args":["zzz_nope"],
 "buildable":false,
 "reason":"未知建筑类型：zzz_nope（输入 list 查看）",
 "cost":{"metal":0,"energy":0,"science":0},
 "affordable":{"metal":true,"energy":true,"science":true},
 "result":{"code":"BuildUnknownType","text":"未知建筑类型：zzz_nope（输入 list 查看）","args":["zzz_nope"]}}
```
- **顶层 `code` / `text` / `args` 是规范字段**，前端优先读这三个。
- `result:{code,text,args}` 是**镜像**，形状与 `command{build}` 的失败返回完全一致，这样前端可以用同一套解析代码处理 `preview_build` 与 `command` 两类失败，不必写分支。
- 两处同时存在是**有意设计**，不是冗余；契约测试应断言顶层 `code`。

**约定**
- `buildable` 与 `affordable` **相互独立**：资源不足时 `buildable` 仍为 `true`、`reason` 为空，只有 `affordable` 对应项为 false。这是最容易实现错的一点，契约测试必须有专项用例（并用 mock 注入 `preview_merge_affordable` 缺陷验证该用例真的有效）。
- 未知 `key` → `code:"BuildUnknownType"`；坐标越界 → `code:"BuildBlocked"` 且 `reason` 说明越界
- `pending !== null` 期间同样受 §4.4.1 约束 → `code:"BlockedByPending"`
- **纯查询**：不消耗随机数、不改任何状态。前端在移动光标时会高频调用，实现若带副作用会直接破坏可复现性。

### 4.7 `save` / `load`
```json
→ {"jsonrpc":"2.0","id":7,"method":"save","params":{"path":"/tmp/a.sav"}}
← {"jsonrpc":"2.0","id":7,"result":{"ok":true,"result":{"code":"Ok","text":""},"log":[],"snapshot":{...}}}

→ {"jsonrpc":"2.0","id":8,"method":"load","params":{"path":"/tmp/a.sav"}}
← {"jsonrpc":"2.0","id":8,"result":{"ok":true,"result":{...},"log":[],"snapshot":{...}}}
```
`path` 缺省时由服务端决定（沿用引擎默认存档名）。读档失败返回 `ok:false` 且 `code` 为 `LoadFailed`，`text` 给出原因；**失败时原对局状态不得被破坏**（引擎已保证，测试已覆盖）。

---

### 4.8 `validate`（P6 预留，现在可先返回 `ok:true` 与空 errors）
```json
→ {"jsonrpc":"2.0","id":9,"method":"validate","params":{"path":"..."}}
← {"jsonrpc":"2.0","id":9,"result":{"ok":true,"errors":[]}}
```

### 4.9 `shutdown`（测试用）
```json
→ {"jsonrpc":"2.0","id":10,"method":"shutdown"}
← {"jsonrpc":"2.0","id":10,"result":{"ok":true}}
```
响应之后服务端退出（`exit 0`），前端应回收子进程。

---

## 5. `ActCode` 名字符串 ↔ 语义对照（前端配色用）

以下是前端最需要区分的一批代码（完整清单以 `src/protocol.hpp` 为准）：

| code | 语义 | 建议配色 |
|---|---|---|
| `WaveIncoming` `WaveBreached` `Starve` `OvercrowdLeft` | 危险 | 红 |
| `WaveRepelled` `BuildingBuilt` `ResearchDone` `GateTheoryUnlocked` | 正面 | 绿 |
| `Brownout` `OvercrowdWarn` `AcidRain` `BuildingDamaged` | 警告 | 黄 |
| `MarketTradeMetal` `MarketTradeEnergy` `SignalSuccess` `RefugeesSettled` | 交易/收益 | 青 |
| `TurnHeader` `WeatherChange` | 结构分隔 | 灰 / 蓝 |
| `Text` | 兜底（`strings[0]` 即原文） | 默认 |

---

## 6. 前端文件布局（与 core 侧文件互不重叠）

```
frontend/
  starcolony_tui/
    __init__.py
    __main__.py        # python -m starcolony_tui 入口
    rpc.py             # 子进程管理 + 行协议编解码 + 超时/退出处理
    app.py             # Textual App 主类
    screens/           # surface.py（地表）/ help.py / event.py（待决事件）
    widgets/           # 可复用组件（地图、资源栏、建筑列表）
    i18n.py            # P2 仅做 code→配色/图标映射；P3 接文案表
  tests/
    contract_test.py   # 契约测试（QA 维护）
```

core 侧（互不重叠）：
```
src/rpc_server.cpp     # 服务端主程序
src/rpc_json.hpp       # 极简 JSON 读写（vendored 单头或自研）
```

---

## 7. 启动与联调

```bash
# 直接手测服务端（一行请求一行响应）
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"ping"}' \
  '{"jsonrpc":"2.0","id":2,"method":"new_game","params":{"seed":42}}' \
  '{"jsonrpc":"2.0","id":3,"method":"shutdown"}' | ./build/starcolony-rpc

# 前端
.venv/bin/python -m starcolony_tui
```

---

## 8. 版本与兼容

- 本契约当前为 **v1**。`schema_version` 随存档一起写入（`schema_version 1`）。
- 新增 `action`、新增 `ActCode`、给 `snapshot` 追加字段 → **向后兼容**，前端应忽略不认识的字段与代码。
- 删除/改名任何已有字段、改变 `command` 的 action 名 → **破坏性**，必须升 v2 并同步两侧。
