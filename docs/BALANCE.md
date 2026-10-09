# 星际争霸：殖民地 —— 平衡性扫描报告（T3）

- 工具：`tools/sweep.cpp`（内置试玩 AI `tests/ai.hpp` × 运行时数值结构 `sc::TUNE`）
- 数据：基线 256/512 种子；敏感性 24 个参数 × 3 档（各 256 种子）；推荐方案 512 种子复核
- 结论一句话：**默认数值的胜率（512 种子 72.9%）本身已在目标区间内，真正的问题是"赢得太满、输得太单一"——139 个失败局 100% 都卡在第 91 周期超时，485/512 局点满 8 项科技。** 推荐用 3~4 个中等强度旋钮（`farmFood` / `foodPerPop` / `militiaPerPop` / `wavePerTurn`）把 AI 胜率压到 56%~61%，失败类型从 1 类扩到 4 类。
- 最敏感参数：`maxTurns`、`popGrowthRate`、`labScience`、`baseHousing`、`startMetal`（单档可翻转 45~57 个百分点）；最钝参数：`combatLossMax`（完全无效）、`eventChance`、`startFood`、`geoEnergy`（±1.6pp 以内）。

---

## 1. 复现命令

```bash
cd /home/shc/starcolony
g++ -std=c++20 -O2 -Isrc -Itests tools/sweep.cpp src/game.cpp -o build/sweep

# 基线（默认 TUNE）汇总 / 每局 CSV
./build/sweep --seeds 256
./build/sweep --seeds 512 --csv

# 敏感性扫描（24 参数 × 3 档）
./build/sweep --scan --seeds 256
./build/sweep --scan farmFood=10,12,14 --seeds 256

# 组合方案复核
./build/sweep --seeds 512 --set "farmFood=10.5,foodPerPop=1.15,militiaPerPop=0.7"

# 参数名清单
./build/sweep --list-params
```

`--scan` 输出为 CSV（`param,value,seeds,wins,winrate,min_turns,median_turns,max_turns,avg_techs,gate_tech_pct,fail_*,stage_*`），便于二次分析。
性能：128 种子 ≈ 0.08s，256 种子 ≈ 0.15s，远低于 30s 验收线。

---

## 2. 基线（TUNE 默认值）

### 2.1 256 种子原始输出

```
$ ./build/sweep --seeds 256
== 基线（TUNE 默认值）  seeds 1..256 ==
种子数 256  胜利 187  胜率 73.0%
通关周期: min 63  中位 81.0  max 90  (n=187)
平均科技完成数: 全部 7.92  胜利 8.00  失败 7.71
星门理论完成 241/256 (94.1%)  星门开建 234/256 (91.4%)
平均: 食物短缺周期 0.00  防线被突破 9.62  虫潮次数 18.90
失败原因分布 (失败 69 局):
  timeout_gate_unfinished      47  68.1%
  timeout_no_gate               7  10.1%
  timeout_no_gatetech          15  21.7%
失败卡点阶段: early(<=20) 0  mid(21-50) 0  late(>50) 69
```

### 2.2 512 种子（统计更稳，本文结论以 512 为准）

```
$ ./build/sweep --seeds 512
种子数 512  胜利 373  胜率 72.9%
通关周期: min 62  中位 82.0  max 90  (n=373)
平均科技完成数: 全部 7.91  胜利 8.00  失败 7.67
星门理论完成 485/512 (94.7%)  星门开建 471/512 (92.0%)
平均: 食物短缺周期 0.03  防线被突破 9.61  虫潮次数 18.97
失败原因分布 (失败 139 局):
  timeout_gate_unfinished      98  70.5%
  timeout_no_gatetech          27  19.4%
  timeout_no_gate              14  10.1%
失败卡点阶段: early(<=20) 0  mid(21-50) 0  late(>50) 139
```

通关周期分位（512 胜利局）：p10=73、p25=76、中位=82、p75=86、p90=88、max=90。
**139 个失败局的结束周期全部是 91**（= `maxTurns+1`），即没有一局"死"在中途。
科技完成数分布：8 项 485 局、7 项 19 局、6 项 4 局、5/4/3/1 项各 1 局、0 项 0 局。

### 2.3 与自检工具的交叉验证

`tests/selftest.cpp` 使用种子 `{1,2,3,42,777,12345,99991,2024}`，报告 7/8 胜。用本工具逐种子复核：

```
$ for s in 1 2 3 42 777 12345 99991 2024; do ./build/sweep --seeds 1 --seed0 $s --csv | tail -1; done
seed=1      won=1 turns=81
seed=2      won=0 turns=91  （timeout_gate_unfinished）
seed=3      won=1 turns=72
seed=42     won=1 turns=77
seed=777    won=1 turns=74
seed=12345  won=1 turns=87
seed=99991  won=1 turns=75
seed=2024   won=1 turns=63
→ 7/8，与 selftest 一致
```

---

## 3. 失败归因现状：为什么"不够多样化"

| 观察 | 数字 | 含义 |
|---|---|---|
| 失败局全部为超时 | 139/139 | 没有饿死、没有防线崩、没有早期死亡 |
| 失败回合 | 全部 = 91 | 是"到点判负"，不是被打死 |
| 满科技率 | 485/512 = 94.7% | 科技线毫无压力 |
| 平均防线被突破 | 9.61 次/局 | 虫潮每局打穿约 10 次，但只是掉人口/金属，**打不死** |
| 平均食物短缺周期 | 0.03 周期/局 | 食物系统几乎不构成威胁 |

根因（读源码 `src/game.cpp` 后确认）：

1. **人口即战力**：`defense() = 炮塔×16 + pop×0.8`，中后期 pop 常在 40~80，仅民兵就有 32~64 防御，虫潮强度 `6 + turn×0.8` 在 90 周期约 78，很难被持续击穿致死。
2. **突破损失有硬上限且被医疗抵消**：`loss = clamp(1 + over/14 - 医疗站数, 1, combatLossMax)`，医疗站直接把损失压到 1，而 `combatLossMax` 默认 4 根本碰不到上限（见 §4 数据：4/8/12 三档输出完全相同）。
3. **人口增长快**：1.4/2.0 = 每周期 +0.7 人，突破损失很快补回。
4. 因此所有压力最终只体现为**经济节奏慢一点 → 攒不齐星门 420 金属 / 300 能源 → 超时**。

---

## 4. 参数敏感性（各 256 种子，默认 = 中间档）

按最大档位差 `Δpp` 排序：

| 参数 | 低档 | 中档(默认) | 高档 | Δpp | 备注 |
|---|---|---:|---:|---:|---:|---|
| `maxTurns` | 80 → 33.2% | **90 → 73.0%** | 100 → 90.6% | **57.4** | 硬上限，是"悬崖"不是难度旋钮 |
| `popGrowthRate` | 1.0 → 37.1% | **1.4 → 73.0%** | 1.8 → 87.9% | **50.8** | 极敏感，牵动人口/战力/工人 |
| `labScience` | 4 → 37.1% | **6 → 73.0%** | 8 → 85.5% | **48.4** | 科技速度是第一经济杠杆 |
| `baseHousing` | 6 → 41.4% | **8 → 73.0%** | 10 → 87.5% | **46.1** | 初始人口上限 |
| `startMetal` | 180 → 42.2% | **240 → 73.0%** | 300 → 87.5% | **45.3** | 开局节奏 |
| `popGrowthNeed` | 2.5 → 50.8% | **2.0 → 73.0%** | 1.5 → 90.2% | **39.4** | 反向：需求越低越易 |
| `solarEnergy` | 4 → 49.2% | **6 → 73.0%** | 8 → 82.8% | **33.6** | 早期电力 |
| `housingPerHab` | 4 → 51.6% | **6 → 73.0%** | 8 → 81.2% | **29.6** | |
| `waveInterval` | 6 → 60.9% | **9 → 73.0%** | 12 → 79.3% | 18.4 | 反向：间隔越短越难 |
| `wavePerTurn` | 0.6 → 82.0% | **0.8 → 73.0%** | 1.0 → 64.1% | 17.9 | 主要虫潮旋钮 |
| `militiaPerPop` | 0.2 → 57.0% | **0.8 → 73.0%** | 0.8（同默认） | 16.0 | 0.2/0.5/0.8 → 57.0/67.2/73.0 |
| `combatLossDiv` | 8 → 62.5% | **14 → 73.0%** | 20 → 76.6% | 14.1 | 分母越小越痛 |
| `foodPerPop` | 0.8 → 77.7% | **1.0 → 73.0%** | 1.2 → 64.1% | 13.6 | 主要食物旋钮 |
| `startEnergy` | 60 → 71.9% | **90 → 73.0%** | 150 → 84.4% | 12.5 | |
| `waveScale`* | 0.85 → 80.5% | **1.0 → 73.0%** | 1.15 → 68.4% | 12.1 | 合成参数，同时乘 waveBase+wavePerTurn |
| `farmFood` | 10 → 65.2% | **12 → 73.0%** | 14 → 75.0% | 9.8 | |
| `waveFirst` | 6 → 74.6% | **9 → 73.0%** | 12 → 80.9% | 6.3 | 非单调：首波越早越弱 → 反而更容易 |
| `waveBase` | 5 → 77.0% | **6 → 73.0%** | 8 → 73.0% | 4.0 | |
| `turretDefense` | 12 → 72.3% | **16 → 73.0%** | 20 → 75.0% | 2.7 | 钝 |
| `combatDamageDiv` | 20 → 75.0% | **35 → 73.0%** | 50 → 73.0% | 2.0 | 钝 |
| `eventChance` | 20 → 74.6% | **30 → 73.0%** | 45 → 74.2% | 1.6 | 钝（但影响"事件体验"丰富度，与胜负无关） |
| `geoEnergy` | 12 → 71.5% | **18 → 73.0%** | 24 → 73.0% | 1.5 | 钝（地热不是瓶颈） |
| `startFood` | 50 → 71.9% | **70 → 73.0%** | 90 → 73.0% | 1.1 | 钝 |
| `combatLossMax` | 4 → 73.0% | **4 → 73.0%** | 12 → 73.0% | **0.0** | **完全无效：4/8/12 三档 16 项指标逐字节相同** |

\* `waveScale` 为 `sweep.cpp` 内的合成参数，不在 `TUNE` 中。

<details>
<summary>原始 CSV 片段（256 种子，节选）</summary>

```csv
scan,value,seeds,wins,winrate,min_turns,median_turns,max_turns,avg_techs,gate_tech_pct,fail_wave,fail_starve,fail_event,fail_timeout_no_gatetech,fail_timeout_no_gate,fail_timeout_gate_unfinished,fail_other,stage_early,stage_mid,stage_late
labScience,4,256,95,37.1,64,82.0,90,7.70,77.7,0,0,0,57,13,91,0,0,0,161
labScience,6,256,187,73.0,63,81.0,90,7.92,94.1,0,0,0,15,7,47,0,0,0,69
labScience,8,256,219,85.5,64,77.0,90,7.95,96.1,0,0,0,10,9,18,0,0,0,37
foodPerPop,1.2,256,164,64.1,63,82.0,90,7.76,90.2,2,1,0,22,6,61,0,0,0,92
maxTurns,80,256,85,33.2,63,75.0,80,7.68,80.5,0,0,0,50,10,111,0,0,0,171
startMetal,180,256,108,42.2,67,86.0,90,7.61,82.4,2,0,2,41,7,96,0,0,0,148
```
</details>

**解读：**
- `maxTurns` / `popGrowthRate` / `labScience` / `baseHousing` / `startMetal` 一动就翻天（40~50pp），适合做"整体难度档位"，不适合做微调。
- 真正适合"精细调难度"的是中等档：`wavePerTurn`、`waveInterval`、`militiaPerPop`、`foodPerPop`、`farmFood`、`combatLossDiv`（每档 1~2pp，方向单调）。
- `eventChance` 对胜率几乎无影响（74.6/73.0/74.2），它只负责"体验多样性"，调它不解决平衡问题。

---

## 5. 失败类型多样性的专项实验

目标：在 55%~80% 胜率下同时出现"饿死 / 防线崩 / 超时"。结论：**饿死可以做到，防线崩几乎不可控。**

### 5.1 造出多种失败原因（256 种子）

```
$ ./build/sweep --seeds 128 --set "farmFood=9,foodPerPop=1.25,startFood=50,popGrowthRate=1.5"
胜率 45.3%
  death_event                   1  1.4%
  death_starve                 12 17.1%
  death_wave                    8 11.4%
  （其余为 timeout 各档）
```

但有死亡的档位全部跌破 55% 胜率。逐档收窄后：

| 方案 | 胜率 | death_starve | death_wave | timeout 分布 |
|---|---:|---:|---:|---|
| `farmFood=10.5, foodPerPop=1.20, militiaPerPop=0.6` | 49.2% | 3 | 3 | 33 / 4 / 22 |
| `farmFood=11, foodPerPop=1.15, militiaPerPop=0.6` | 55.5% | 0 | 0 | 34 / 3 / 20 |
| `farmFood=10.5, foodPerPop=1.15, militiaPerPop=0.7` | **61.3%** | 6/512 | 0~1 | 111 / 19 / 61 |
| `farmFood=10.5, foodPerPop=1.20, militiaPerPop=0.65, wavePerTurn=0.85` | **56.2%** | 10/512 | 0 | 119 / 16 / 74 |

### 5.2 为什么"防线崩"很难做出来

强行加大虫潮致死能力时，胜率是断崖式下跌，而不是平滑过渡：

```
$ ./build/sweep --seeds 256 --set "waveInterval=5,combatLossDiv=5,militiaPerPop=0.2,popGrowthRate=1.0"
胜率 0.4%   death_wave 97 (38.0%)   timeout_no_gatetech 142 (55.7%)

$ ./build/sweep --seeds 256 --set "waveInterval=6,combatLossDiv=6,militiaPerPop=0.2,popGrowthRate=1.2"
胜率 1.2%   death_wave 3 (1.2%)    timeout_no_gatetech 221 (87.4%)
```

原因见 §3：`combatLossMax` 无效、医疗站抵消损失、人口增长快。**要真正做出"中期防线崩"的失败类型，需要改代码而不是改 TUNE：**
- 让 `combatLossMax` 真正生效（当前被 `1 + over/combatLossDiv - clinics` 的小值绕过）；
- 给医疗站的减员效果设上限（如 `min(clinics,1)`），否则任何强度的虫潮都只掉 1 人；
- 可选：把虫潮强度与人寿/建筑损毁做成连锁（当前只掉人口+金属+随机损坏）。

在只动 TUNE 的约束下，**可交付的多样性是：饿死（1%~5%）+ 超时三细分（理论未完成 / 星门未开建 / 星门在建未完工）**，共 4 类。

---

## 6. 推荐调参方案（512 种子复核）

### 方案 A：标准档（推荐，胜率 61.3%，失败 4 类）

改动 3 个字段（当前默认 → 建议值）：

| 参数 | 当前 | 建议 | 理由 |
|---|---:|---:|---|
| `farmFood` | 12.0 | **10.5** | 农场单产 -12.5%，制造真实食物压力；10→65.2%、12→73.0%（256） |
| `foodPerPop` | 1.0 | **1.15** | 食物消耗 +15%，让"人口红利"变成双刃剑；1.2→64.1%（256） |
| `militiaPerPop` | 0.8 | **0.7** | 民兵防御 -12.5%，削弱"人口即无敌"的护城河；0.5→67.2%（256） |

```
$ ./build/sweep --seeds 512 --set "farmFood=10.5,foodPerPop=1.15,militiaPerPop=0.7"
种子数 512  胜利 314  胜率 61.3%
通关周期: min 63  中位 82.0  max 90
平均科技完成数: 全部 7.67  胜利 8.00  失败 7.15
星门理论完成 444/512 (86.7%)  星门开建 425/512 (83.0%)
平均: 食物短缺周期 1.07  防线被突破 11.02  虫潮次数 19.17
失败原因分布 (失败 198 局):
  death_starve                  6  3.0%
  timeout_gate_unfinished     111  56.1%
  timeout_no_gate              19   9.6%
  timeout_no_gatetech          61  30.8%
失败卡点阶段: early(<=20) 0  mid(21-50) 2  late(>50) 196
```

对比基线：胜率 72.9% → **61.3%**；失败类型 1 家族 → **4 类**；星门理论完成率 94.7% → 86.7%（不再人人满科技）；出现 2 局中期(21-50)失败。

### 方案 B：高挑战档（胜率 56.2%，失败 4 类）

| 参数 | 当前 | 建议 |
|---|---:|---:|
| `farmFood` | 12.0 | **10.5** |
| `foodPerPop` | 1.0 | **1.20** |
| `militiaPerPop` | 0.8 | **0.65** |
| `wavePerTurn` | 0.8 | **0.85** |

```
$ ./build/sweep --seeds 512 --set "farmFood=10.5,foodPerPop=1.20,militiaPerPop=0.65,wavePerTurn=0.85"
种子数 512  胜利 288  胜率 56.2%
通关周期: min 63  中位 82.0  max 90
平均科技完成数: 全部 7.53  胜利 8.00  失败 6.92
星门理论完成 423/512 (82.6%)  星门开建 407/512 (79.5%)
失败原因分布 (失败 224 局):
  death_starve                 10  4.5%
  timeout_gate_unfinished     119  53.1%
  timeout_no_gate              16  7.1%
  timeout_no_gatetech          74  33.0%
失败卡点阶段: early(<=20) 0  mid(21-50) 3  late(>50) 221
```

### 方案 C：只改一个参数（最保守）

`foodPerPop: 1.0 → 1.15` → 512 种子胜率 **66.8%**，失败仍以超时为主但三档分布更均匀（unfinished 68.8% / no_gatetech 23.5% / no_gate 5.9%）。

---

## 7. 逐参数调参清单（可直接照改 `src/types.hpp` 的 `Tuning` 默认值）

| # | 参数 | 当前 | 建议 | 预计胜率变化 | 理由 |
|---:|---|---:|---:|---|---|
| 1 | `foodPerPop` | 1.0 | **1.15**（标准）/ **1.20**（高难） | 73.0% → 64.1%（@1.2，256 种子） | 主食物旋钮，方向单调、每档约 5~7pp，是唯一能在目标区间内制造饿死失败的旋钮 |
| 2 | `farmFood` | 12.0 | **10.5** | 10→65.2% / 14→75.0% | 与 `foodPerPop` 对冲使用：单产降但冰层加成仍在，逼迫多建农场、挤占地块与工人 |
| 3 | `militiaPerPop` | 0.8 | **0.7**（标准）/ **0.65**（高难） | 0.5→67.2%、0.2→57.0% | 削弱"人口=防御"，让炮塔真正有存在价值；0.7 约 -2~3pp |
| 4 | `wavePerTurn` | 0.8 | **0.85**（高难，标准档不动） | 1.0→64.1%、0.6→82.0% | 每档约 9pp，是虫潮主旋钮；只建议 +0.05 微调 |
| 5 | `waveInterval` | 9 | 保持 9（如需更硬用 **8**） | 6→60.9%、12→79.3% | 每档约 9pp，与 `wavePerTurn` 二选一，避免叠加过猛 |
| 6 | `labScience` | 6.0 | **保持 6**（想让玩家更轻松可升到 7） | 4→37.1%、8→85.5% | 最敏感经济参数之一，**降到 4 直接劝退（37%）**，不要用于微调 |
| 7 | `popGrowthRate` | 1.4 | **保持 1.4** | 1.0→37.1%、1.8→87.9% | 牵动人口/工人/战力三重杠杆，一动就是 50pp，禁止用于微调 |
| 8 | `maxTurns` | 90 | **保持 90** | 80→33.2%、100→90.6% | 是"悬崖"不是难度：+10 周期 = +17pp，且失败回合全部集中在上限 |
| 9 | `startMetal` | 240 | 保持 240 | 180→42.2%、300→87.5% | 同上，属整体难度档位 |
| 10 | `eventChance` | 30 | 保持 30（体验向可到 40） | 20→74.6%、45→74.2% | 对胜负几乎无影响（±1.6pp），只决定剧情密度 |
| 11 | `turretDefense` | 16 | 保持 16 | 12→72.3%、20→75.0% | 钝（±2.7pp），因为防御主要来自人口民兵 |
| 12 | `startFood` | 70 | 保持 70 | 50→71.9%、90→73.0% | 钝（±1.1pp），AI 前 3 周期就补农场 |
| 13 | `geoEnergy` | 18 | 保持 18 | 12→71.5%、24→73.0% | 钝，地热不是瓶颈 |
| 14 | `combatLossMax` | 4 | **当前无效，建议要么修公式要么删** | 4/8/12 三档输出逐项相同（0.0pp） | `applyCombat` 实际损失 = `1 + over/14 - 医疗站数`，几乎永远 ≤ 4，永远碰不到该上限 |

---

## 8. 建议补充进 `Tuning` 的字段（当前无法扫，但是真正的杠杆）

1. **科技成本 / 星门造价**：`TDEF` 各项成本（35/50/70/80/90/110/130/170）与 `BDEF` 星门造价（420 金属 / 300 能源 / 12 周期）都不在 `TUNE` 里。证据：`labScience` 是全场最敏感参数之一（4→37.1%、8→85.5%），而它只是科技速度的代理。建议加 `techCostScale`（乘 `TDEF[].cost`）与 `gateMetal` / `gateEnergy` / `gateBuildTurns`，这样"后期攒不出星门"这一类失败才能被直接、精细地调节。
2. **医疗站减员上限**：`applyCombat` / 饥荒结算里的 `- countType(Clinic)` 是硬编码。建议改成 `- min(countType(Clinic), clinicMitigationCap)`（默认 1），否则虫潮永远打不死人。
3. **`combatLossMax` 生效化**：把损失公式改为 `loss = max(computed, ...)` 之外的另一种封顶逻辑，或在 `over` 很大时让上限真正起作用。

---

## 9. 给 Lead 的最终建议（一句话版）

- **首选**：采纳方案 A（`farmFood=10.5`、`foodPerPop=1.15`、`militiaPerPop=0.7`）→ 512 种子胜率 61.3%，落在 55%~80% 中段，失败 4 类，中期失败开始出现。
- **若嫌简单**：切换到方案 B（再 `foodPerPop=1.20`、`wavePerTurn=0.85`）→ 56.2%。
- **不要用** `labScience` / `popGrowthRate` / `maxTurns` / `startMetal` / `baseHousing` 做微调（一动 45~57pp）。
- **不要指望** `eventChance` / `turretDefense` / `startFood` / `geoEnergy` 改变难度（±2.7pp 以内）。
- 若坚持要"中期防线崩"的失败类型，必须在 `src/game.cpp` 里改医疗站减员与 `combatLossMax` 的实际生效方式（§5.2、§8）。

---

### 附录：完整敏感性扫描命令

```bash
for k in "wavePerTurn=0.6,0.8,1.0" "waveBase=5,6,8" "waveInterval=6,9,12" \
         "militiaPerPop=0.2,0.5,0.8" "combatLossDiv=8,14,20" "combatLossMax=4,8,12" \
         "combatDamageDiv=20,35,50" "farmFood=10,12,14" "foodPerPop=0.8,1.0,1.2" \
         "labScience=4,6,8" "startMetal=180,240,300" "startFood=50,70,90" \
         "maxTurns=80,90,100" "popGrowthRate=1.0,1.4,1.8" "eventChance=20,30,45" \
         "turretDefense=12,16,20" "waveFirst=6,9,12" "waveScale=0.85,1.0,1.15" \
         "startEnergy=60,90,150" "baseHousing=6,8,10" "housingPerHab=4,6,8" \
         "solarEnergy=4,6,8" "geoEnergy=12,18,24" "popGrowthNeed=1.5,2.0,2.5"; do
  ./build/sweep --scan $k --seeds 256
done
```

---

# 修订记录（T2 修复后重新标定）

> 本章为 **`BUG B1` 修复之后**的复测结果。B1（在建建筑提前提供住房 / 医疗效果）修复后，
> 同一套 AI 的基线胜率从 **72.9% 降到 65.0%**，因此上文所有基于旧基线标定的方案都必须重测。

## R1. 新基线与各方案复测（512 种子，修复后代码）

| 方案 | 参数改动 | 胜率 | 星门理论完成率 | 失败类型数 |
|---|---|---:|---:|---:|
| 基线（不动） | — | **65.0%** | 93.2% | 3（全为超时） |
| **C 保守** | `foodPerPop` 1.0 → **1.15** | **59.6%** | 89.1% | **5**（首次出现饿死 / 虫潮致死） |
| D | `foodPerPop` 1.15 + `militiaPerPop` 0.75 | 57.6% | 87.1% | 5 |
| A 标准 | `farmFood` 10.5 + `foodPerPop` 1.15 + `militiaPerPop` 0.7 | 52.1% | 81.6% | 5 |
| B 高难 | A + `foodPerPop` 1.20 + `wavePerTurn` 0.85 | 45.1% | 75.6% | 5 |

**已采用：方案 C**（`src/types.hpp` 的 `Tuning::foodPerPop = 1.15`）。
理由：单参数即可落入 55%~80% 目标区间中段，且首次让"饿死"与"虫潮致死"成为真实失败类型。

## R2. §8 两个引擎级问题的复测结论

### R2.1 医疗站减员上限（§8.2）—— 已修，但**不是**虫潮无力的根因

新增 `Tuning::clinicMitigationCap = 1`，`applyCombat` 与饥荒结算改为
`- min(已完工医疗站数, clinicMitigationCap)`。

实测 256 种子，`clinicMitigationCap` 取 0 / 1 / 3 / 99 四档，**胜率全部为 59.4%**，完全不变。
原因不在参数，而在公式：`loss = 1 + over / combatLossDiv - clinics`，其中 `over / 14` 通常等于 0
（单次突破幅度普遍小于 14），于是 `loss` 被 `clampInt(..., 1, ...)` 直接压回下限 1。
**堆多少医疗站都是 1 人伤亡，减不减上限都还是 1 人。**

### R2.2 伤亡上限 `combatLossMax`（§8.3）—— 修复后仍非瓶颈

| `combatLossMax` | 1 | 2 | 4 | 8 |
|---|---:|---:|---:|---:|
| 胜率（256 种子） | 59.8% | 59.4% | 59.4% | 59.4% |

修复前该参数 4/8/12 三档输出完全相同（0.0pp）；修复后 cap=1 与 cap=8 相差 0.4pp，**已脱离"完全无效"，但仍是弱旋钮**。

### R2.3 真正的伤亡旋钮是 `combatLossDiv`

| `combatLossDiv` | 6 | 8 | 10 | 14（默认） | 20 |
|---|---:|---:|---:|---:|---:|
| 胜率 | 40.2% | 50.0% | 57.4% | 59.4% | 58.6% |
| 终局平均人口 | 58.6 | 63.7 | 66.4 | 68.9 | — |
| 平均突破次数 | 13.1 | 11.8 | 11.3 | 11.0 | — |
| 虫潮致死局数 / 256 | 2 | 2 | 1 | 1 | — |

即便把 `combatLossDiv` 压到 6（胜率暴跌 19pp），虫潮致死仍只有 **2/256**。
**结论：虫潮在本作里是"骚扰"而非"威胁"——它压制人口与产能，但打不死殖民地。**
要造出"中期防线崩"这一失败类型，靠调参做不到（与 §5.2 的判断一致），需要结构性改动：
让虫潮的破坏作用于**产能**（例如受损建筑连锁停产、矿脉被污染），而不是只扣人口与金属。

## R3. 给后续调参的约束（基于新代码）

- 想微调难度：`foodPerPop` 每 0.05 约 3~5pp，是唯一方向单调且温和的旋钮。
- 想让虫潮有牙齿：改 `combatLossDiv`（每档 8~19pp，很猛），别动 `combatLossMax`。
- 不要碰：`labScience`、`popGrowthRate`、`maxTurns`、`startMetal`（一动 45~57pp）。
- 没用：`eventChance`、`turretDefense`、`startFood`、`geoEnergy`（±2.7pp 以内）。
