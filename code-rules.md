# code-rules — 代码设计与编码规则

| 项 | 内容 |
| ---- | ---- |
| 定位 | 代码的设计理念、分层边界与编码底线；约束「怎么写是对的」，不罗列实现细节 |
| 不覆盖 | 构建与烧录流程、commit 规范（见 [AGENTS.md](AGENTS.md)）；文档治理（见 [docs-rules.md](docs-rules.md)） |

## 1. 设计理念

- **资源确定性优先**：运行时逻辑禁用动态内存分配（`malloc` / `rt_malloc`），内存在编译期或初始化期一次成型——`static` 分配或受监控的栈；只读数据（查找表、配置）加 `const` 入 ROM。
- **失败必须可见**：系统/驱动调用的返回值必须检查；错误以 `rt_err_t` 沿调用链向上传播，不静默吞掉，错误路径保持日志可追踪。
- **简单优于通用**：不为假想需求引入抽象；一个模块只做一件事，公共 API 面越小越好；文件内私有函数/变量一律 `static`。
- **敏感数据即用即清**：密钥、口令使用后立即从栈/缓冲区清零。

## 2. 分层边界

| 层 | 目录 | 职责 |
| ---- | ---- | ---- |
| 应用层 | `applications/` | 业务逻辑、协议、应用服务 |
| 板级层 | `board/` | 板级初始化、CubeMX 配置、链接脚本 |
| 适配层 | `libraries/HAL_Drivers/` | RT-Thread 设备框架对接 STM32 HAL 外设库（`drv_*`） |
| 第三方 | `packages/` | CMSIS-Core 与 STM32F4 CMSIS/HAL 驱动包（`pkgs` 拉取，不入库），只读 |
| RTOS 内核 | 外部 `$RTT_ROOT`（默认 `~/SDK/rt-thread`） | RT-Thread 内核与组件，只读，不在仓库内 |

- 依赖只向下：应用层 → 板级/适配层 → 第三方/内核；禁止反向依赖与跨层直达。
- 应用层不直接访问寄存器，硬件访问只经 `board/` 或 RT-Thread 设备框架。
- 第三方与内核只读；确需改变行为，按优先级选择：**Wrapper**（在自有层封装）> **Hook**（RT-Thread 钩子/回调）> **Copy**（复制到自有层改副本并重命名）。
- 新功能默认落在 `applications/`，归属存疑时向上表中职责对齐。

## 3. 编码规范

- 命名：标识符统一 `snake_case`；函数按 `module_action_object()`（如 `ble_start_advertising`）；结构体 typedef 以 `_t` 结尾；宏 `UPPER_CASE`；具体规则以根目录 `.clang-tidy` 为准。
- 类型：使用 `<stdint.h>` 类型（`uint8_t`、`int32_t`）代替裸 `char`、`int`；头文件用 `#pragma once` 或 include guard。
- 错误码：统一 `rt_err_t`（`RT_EOK` 成功、`-RT_ERROR` 等负值错误），跨模块传递不丢失原始错误语义；开发期不可恢复错误用 `RT_ASSERT()`。
- ISR：极简短，禁止阻塞调用与 `printf`，复杂流程经信号量/事件下放线程。
- 注释：代码与注释使用英文；公共 API 用 Doxygen 风格（`@brief` / `@param` / `@return`）；解释为什么这样做，而不仅是做了什么。
- 文件头：英文块注释，含模块名、简述与 `SPDX-License-Identifier: Apache-2.0`（与仓库 LICENSE 一致）。
- 格式化：以根目录 `.clang-format` 为准，不手工偏离。
- 交付底线：编码后无 lint error/warning，每笔提交独立通过 `./run.sh rebuild`。
