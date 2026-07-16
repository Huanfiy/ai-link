---
name: rttenv
description: Documents this workspace's RT-Thread Env (v2) setup on the developer machine and how to invoke menuconfig, pkgs, and sdk. Use when working on superlink firmware with RT-Thread, BSP configuration, Env activation, or package/SDK commands under Linux/zsh.
---

# Superlink 项目：RT-Thread Env 本机记录

## 与项目的关系

`superlink` 计划用 RT-Thread 与 **RT-Thread Env** 管理工程；硬件为 STM32F446RET6 等（详见 `.cursor/design/project-design.md`）。本 skill 只记录**当前这台机器上 Env 的安装与用法**，便于 Agent 与本人对齐上下文。

## 本机当前状态（截至记录时）

| 项 | 值 |
|----|-----|
| Env 根目录 | `~/.env` |
| Env 版本 | `v2.0.2`（见 `~/.env/tools/scripts/env.json`） |
| 激活脚本 | `~/.env/env.sh`（内容：`export PATH=~/.env/tools/scripts:$PATH`） |
| zsh 别名 | `alias get_rtenv='. $HOME/.env/env.sh'`（写在 `~/.zshrc`） |
| Python | `python3`；**kconfiglib** 已安装（env v2 依赖，`pip install kconfiglib`） |

**注意**：Env **v2** 完整支持 **RT-Thread > v5.1.0** 或 master；更老 RT-Thread 需用 env v1.5.x，且与 kconfiglib 冲突（见 `~/.env/tools/scripts/README.md`）。

## 每次开终端如何启用 Env

在新 shell 中执行（或已配置别名则）：

```bash
get_rtenv
# 等价于: . $HOME/.env/env.sh
```

启用后 `PATH` 含 `~/.env/tools/scripts`，可使用下文命令。

## 常用入口（均在 RT-Thread / BSP 工程根目录下使用）

底层均为 `python3 ~/.env/tools/scripts/env.py`；脚本包装：

- **`menuconfig`** → `env.py menuconfig`：图形/终端配置 Kconfig，生成 `.config` / `rtconfig.h` 等。
- **`pkgs`** → `env.py pkg`：软件包索引更新、按 menuconfig 安装/卸载包等。
- **`sdk`** → `env.py sdk`：SDK 相关（无子参数时查看 `--help` 与官方文档）。

子命令帮助示例：

```bash
env.py pkg --help    # 含 --update, --upgrade, --list, --wizard 等
env.py menuconfig --help   # 含 --generate, --silent, -s 等
env.py system --help       # 含 --update
```

官方 Env 使用说明（menuconfig、BSP 配置等）：  
https://github.com/RT-Thread/rt-thread/blob/master/documentation/env/env.md

Linux 安装与 QEMU 快速入门可参考 README 中的 Ubuntu 教程链接。

## 给 Agent 的约定

- 需要 Env 时，假定用户会先 `get_rtenv` 或已 source `env.sh`；长时间命令前可提醒检查。
- 本仓库**尚未**克隆 RT-Thread BSP；具体 BSP 路径、芯片选型与 `scons`/`cmake` 构建方式在工程落地后应**补充进本 skill** 或新建更细的构建 skill。

## 后续可补充（工程就绪后）

- [ ] 实际 `rt-thread` / BSP 根目录路径与目标板名称  
- [ ] 本项目的 `menuconfig` 关键选项与 `pkgs --update` 习惯  
- [ ] 工具链路径（如 `arm-none-eabi-gcc`）是否通过 Env `sdk` 安装及版本  
