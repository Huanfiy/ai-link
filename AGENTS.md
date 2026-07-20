# AGENTS.md

本文件为 AI 助手（Claude Code / Codex / Cursor）提供本仓库的开发协作指南。

## 项目概览

ailink 是基于 RT-Thread 的 STM32F446 固件工程。

| 项 | 内容 |
| --- | --- |
| MCU | STM32F446RET6 核心板（LQFP64，Cortex-M4F，硬件 FPU，HSE 8MHz + LSE 32.768kHz）；Kconfig 选择 `SOC_STM32F446RETX` |
| RTOS | RT-Thread v5.2.0（源码在仓库外 `$RTT_ROOT`，默认 `~/SDK/rt-thread`） |
| 控制台 | UART1（RT-Thread 设备名 `uart1`） |
| 构建系统 | SCons + `run.sh` 封装 |
| 开发环境 | Linux |

## 环境与依赖

### 必需工具

| 工具 | 用途 | 说明 |
| --- | --- | --- |
| ARM GNU Toolchain 13.3.rel1 | 交叉编译 | `arm-none-eabi-*`，路径由 `RTT_EXEC_PATH` 指定 |
| Python 3 + python3-venv | 构建系统 | scons / kconfiglib 版本锁定于 `requirements.txt`，`run.sh` 首次构建时自动装入 `.venv/`（不入库） |
| bear | 生成 `compile_commands.json` | `run.sh` 自动调用 |
| OpenOCD | 烧录与调试 | `run.sh flash` 与 VSCode 调试均依赖 |
| RT-Thread Env | 包管理 | `pkgs --update` 拉取 `packages/` 下的软件包 |

### 常用环境变量

- `RTT_ROOT`：RT-Thread 源码根目录（默认 `~/SDK/rt-thread`）
- `RTT_EXEC_PATH`：工具链 `bin` 路径（默认 `~/toolchain/arm-eabi-toolchain/bin`）
- `AILINK_PROJECT_NAME`：覆盖产物名（默认 `ailink`）
- `OPENOCD_INTERFACE` / `OPENOCD_TARGET`：OpenOCD 配置（默认 `stlink` / `stm32f4x`，可切 `jlink`、`cmsis-dap`）
- `BUILD_MODE`：`Debug` / `Release`（由 `run.sh` 注入）

## 构建与烧录

```bash
./run.sh build [debug|release]          # 构建固件
./run.sh rebuild [debug|release]        # 清理后完整构建
./run.sh rebuild-flash [debug|release]  # 清理、构建并烧录
./run.sh clean                          # 清理 build/ 与 .sconsign.dblite
./run.sh flash                          # OpenOCD 烧录到 0x08000000
./run.sh help                           # 帮助；--verbose 输出详细日志
```

- 默认 Release（`-Os -DNDEBUG`），Debug 为 `-O0 -g`；优化选项以 `rtconfig.py` 为准。
- 产物：`build/ailink.elf` / `.bin` / `.map`；链接后由 `tools/build/report_firmware_info.py` 输出内存占用报告。
- 烧录默认走外接 ST-Link（核心板 SWD 排针，无板载探针）；`compile_commands.json` 由 bear 生成后自动移入 `.vscode/`。
- 配置变更用 `scons --menuconfig`（更新 `.config` 与 `rtconfig.h`，两者入库，勿手改 `rtconfig.h`）；新增软件包后执行 `pkgs --update`。

## 目录结构

```text
applications/   应用层：业务逻辑（主要开发区）
board/          板级层：board.c、CubeMX 配置、链接脚本、board/Kconfig
libraries/      HAL_Drivers：RT-Thread 设备框架对接 STM32 HAL 的适配驱动（drv_*）
packages/       CMSIS-Core 与 STM32F4 CMSIS/HAL 驱动包（pkgs 拉取，不入库，只读）
docs/           项目文档（design / notes / refs）
tools/          构建辅助脚本（tools/build/）
.agents/        入库共享的 AI 协作配置（skills/rttenv）
run.sh          构建与烧录主入口
```

## 调试

- VSCode 配置在 `.vscode/`（不入库，本地维护；参考副本见 `docs/refs/cfg/.vscode/`）：
  - `debug-load`：OpenOCD + ST-Link 烧录并调试，停在 `main`
  - `debug-attach`：attach 到运行中的目标，不烧录
  - tasks：`build-debug`（默认构建任务）、`clean`、`flash`
- C/C++ 语义引擎为 clangd，配置在根目录 `.clangd`（不入库，参考副本见 `docs/refs/cfg/.clangd`）。

## 开发规范

代码设计理念、分层边界与编码规范见根目录 [code-rules.md](code-rules.md)：依赖只向下、第三方与内核只读（Wrapper > Hook > Copy）、运行时禁用动态分配、错误统一 `rt_err_t` 传播。

## 文档记录与清理规则

文档的记录与清理遵循 [docs-rules.md](docs-rules.md)：新增文档前过其记录门槛，改动代码后按其清理准则核查失实文档。

## 已知注意事项

- `compile_commands.json` 位于 `.vscode/`，不要假设根目录存在同名文件。
- `packages/` 不入库：克隆后先在仓库根目录用 RT-Thread Env 执行 `pkgs --update`，再构建。
- `rtconfig.h` 与 `.config` 由 `scons --menuconfig` 生成维护，直接手改会在下次 menuconfig 时丢失。
- 本地开发配置（`.vscode/`、`.clangd`）含机器相关绝对路径，不入库；改动后如需共享，同步更新 `docs/refs/cfg/` 参考副本。

## [重要] Commit 规范

- 格式：`<emoji> <type>(<scope>): <subject>`（中文主题，单行，scope 可选）。
- type 与 emoji 对应：`✨ feat` / `🐞 fix` / `⚡️ perf` / `🎨 refactor` / `🔧 chore` / `📝 docs`。
- 原则：单一粒度且可回滚，一个 commit 只做一件事，禁止混杂；涉及代码时每笔提交独立通过 `./run.sh rebuild`。
- 不添加 `Co-Authored-By` 及任何 AI 署名尾注。
