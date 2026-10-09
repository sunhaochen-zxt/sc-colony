// 星际争霸：殖民地 (Star Colony) —— C++20 命令行殖民地经营游戏
// Copyright (C) 2026 Star Colony contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published
// by the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// See the LICENSE file at the project root for the full license text.
// SPDX-License-Identifier: AGPL-3.0-or-later

// 星际争霸：殖民地 —— 全屏彩色终端界面
#pragma once

#include "game.hpp"

#include <string>
#include <vector>

namespace sc {

struct TermSize {
    int cols = 100;
    int rows = 40;
};

TermSize    termSize();
void        setAnsi(bool on);
bool        ansiEnabled();
void        clearScreen();
int         visLen(const std::string& s);          // 显示宽度（中文按 2 列）
std::string padRight(const std::string& s, int w); // 按显示宽度右侧补空格
std::string colorize(Col c, const std::string& s);

// 生成整帧画面，末尾是提示符（不带换行），等待玩家输入
std::string renderFrame(const Game& g, const std::string& prompt, const std::vector<std::string>& panel);

// 输出整帧：内容一屏放得下就直接打印；放不下则分屏输出（回车翻页），
// 保证任何一行都不会被丢弃（非交互环境直接全量输出，不做分页）
void        emitFrame(const std::string& frame);

} // namespace sc
