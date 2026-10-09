// 星际争霸：殖民地 (Star Colony) —— C++20 命令行殖民地经营游戏
// Copyright (C) 2026 Star Colony contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published
// by the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// See the LICENSE file at the project root for the full license text.
// SPDX-License-Identifier: AGPL-3.0-or-later

// 星际争霸：殖民地 —— 内容包加载 / 校验 / 运行时表构建（对外接口）
//
// 本头文件**不含**任何 JSON 依赖：nlohmann/json 只允许被 src/content.cpp 一个
// 翻译单元包含（见 docs/CONTENT.md §7）。其余 TU 只 include 本文件（纯 struct）。
#pragma once

#include "types.hpp"

#include <array>
#include <deque>
#include <string>
#include <vector>

namespace sc {

// 引擎声明支持的内容格式版本范围（manifest.schema）
inline constexpr int kMinSchema = 1;
inline constexpr int kMaxSchema = 1;

// 默认内容目录（相对进程工作目录；可用环境变量 SC_CONTENT_DIR 覆盖）
inline constexpr const char* kDefaultContentDir = "content/base";

// 一个内容包的完整数据。
// 字符串后备存储 strPool 用 std::deque：push_back 不会使已有元素失效，
// 因此 BDef / TechDef / WeatherDef 里的 const char* 指向的元素终生稳定。
// 正因如此 ContentPack 不可拷贝 / 移动（否则指针会指向已析构的缓冲）。
struct ContentPack {
    std::deque<std::string>               strPool;
    std::array<BDef, BTYPE_COUNT>         buildings{};
    std::array<TechDef, TECH_COUNT>       techs{};
    std::array<WeatherDef, WEATHER_COUNT> weathers{};
    Tuning                                tuning{};
    int                                   schema = 0;
    std::string                           packId;

    ContentPack() = default;
    ContentPack(const ContentPack&) = delete;
    ContentPack& operator=(const ContentPack&) = delete;
    ContentPack(ContentPack&&) = delete;
    ContentPack& operator=(ContentPack&&) = delete;

    // 把一条字符串放进稳定存储，返回其长期有效的 C 字符串指针。
    const char* keep(const std::string& s) {
        strPool.push_back(s);
        return strPool.back().c_str();
    }
};

// 解析并校验内容目录下的内容包。**无副作用**（不触碰任何全局表）。
// 成功：填充 out，返回 true。
// 失败：返回 false，并把**全部**可定位错误写入 errs
//       （形如 "content/base/buildings.json: [3].cost.energy: 缺少必填字段"）。
bool loadContentPack(const std::string& dir, ContentPack& out, std::vector<std::string>& errs);

// 加载 + 校验 + 落地到全局运行时表（BDEF / TDEF / WDEF / TUNE）。
// 失败时不改动任何全局表（半初始化保护）；errs 语义同 loadContentPack。
bool initContent(const std::string& dir, std::vector<std::string>& errs);

// 运行时便捷入口：加载 contentDir()（或环境变量 SC_CONTENT_DIR）。
// 若默认目录不存在，会依次尝试可执行文件同级的 content/ 候选（便于从任意
// 工作目录启动）。失败时把全部错误写到 stderr。**幂等**：仅首次真正加载。
bool ensureContent();

// 内容目录（默认 content/base；setContentDir 或环境变量可覆盖）
void setContentDir(const std::string& dir);
const std::string& contentDir();

// 已加载内容包的元信息（ensureContent / initContent 成功后有效；否则为 0 / 空）
int contentSchema();
const std::string& contentPackId();

} // namespace sc
