# OTA 升级（ROM DFU 直刷）

| 项 | 内容 |
| ---- | ---- |
| 定位 | 固件升级与救砖链路的当前拍板设计：自研 bootloader（跳板 + 校验 + 兜底）、ROM DFU 日常升级通道、`.fw_info` 镜像元数据、EP0 触发请求 |
| 状态 | 2026-07-21 自 ailink-f407 移植合入（该工程 2026-07-20 全链路验收通过）；本板端到端验证见「已验证与未决」 |
| 代码 | `bootloader/`、`applications/ota/`、`tools/build/fwinfo.py`、`tools/host/ailink-ota.py` |
| 决策依据 | 候选方案对比与否决理由见 ailink-f407 工程 `docs/notes/ota-decision.md`（F446 沿用其结论，无新增决策点） |
| 不覆盖 | 构建与烧录命令（见 [AGENTS.md](../../AGENTS.md)）；USB 桥本体设计（见 [usb-bridge.md](usb-bridge.md)） |

## 片内 Flash 布局

STM32F446RET6 片内 512 KB，扇区结构 4×16 K + 1×64 K + 3×128 K（与 F407 相同）：

| 区域 | 地址 | 大小 | 说明 |
| ---- | ---- | ---- | ---- |
| boot | 0x08000000 | 64 KB（扇区 0–3） | 自研裸机 bootloader（`bootloader/`，独立 Makefile 工程），OTA 永不改写；实占 <2 KB |
| app | 0x08010000 | 448 KB（扇区 4–7） | RT-Thread 应用固件，dfu-util 擦写目标 |

外挂 W25Q64 不参与升级链路，整片维持 elmFAT 文件系统（设备 `norflash0`）不变。

## `.fw_info` 镜像元数据

64 B 结构体，位于 app 基址 +0x200（向量表 0x188 B 之后的首个 512 B 对齐位置）。C 侧布局唯一事实源为 `applications/ota/fw_info.h`（boot 经 include 路径共享）；链接期由 `applications/ota/fw_info.c` 占位（仅编译期身份字段，header_crc32 为 0，未回填镜像天然过不了 boot 校验），构建后经 `tools/build/fwinfo.py patch` 同时回填 `.bin` 与 ELF——OpenOCD/dfu-util 烧 `.bin` 与调试器载 ELF（`debug-load`）携带同一份有效元数据。

| 偏移 | 字段 | 值/说明 |
| ---- | ---- | ---- |
| 0x00 | magic `u32` | 0x57464C41（"ALFW"，小端） |
| 0x04 | hdr_ver `u16` | 1 |
| 0x06 | hdr_size `u16` | 64 |
| 0x08 | board_id `u32` | 0x0A17F446（区别于 f407 的 0x0A17F407，防两板镜像互刷） |
| 0x0C | image_size `u32` | 整镜像字节数（含向量表与本结构） |
| 0x10 | image_crc32 `u32` | zlib CRC32，覆盖 [0, 0x200) ∪ [0x240, image_size)，即跳过本结构 64 B |
| 0x14 | build_time `u32` | 构建时刻 Unix 时间戳（UTC） |
| 0x18 | fw_version `char[32]` | `git describe --tags --dirty --always`，NUL 填充；固件版本的单一事实源 |
| 0x38 | reserved `u32` | 0 |
| 0x3C | header_crc32 `u32` | zlib CRC32，覆盖本结构前 60 B |

boot 校验顺序：magic → header_crc32 → board_id → image_size ∈ (0x240, 448 K] → image_crc32 两段比对。任一步失败判定 app 无效。表驱动 CRC32（1 KB 查表，boot 启动时生成于 RAM）在 HSI 16 MHz 下校验百余 KB 镜像的耗时不构成可感知启动延迟。

## boot 行为

全程 HSI 16 MHz、不开中断、不碰外设（保持近复位环境，为 ROM DFU 跳转与 app 时钟接管提供前提）。上电/复位后依次：

1. 读 RTC 备份寄存器 BKP0R（PWR 时钟 + DBP 解锁）：值为 0x5AFEB007 时清标志（跳板恰好触发一次，避免 DFU 复位后死循环）、跳 ROM DFU；
2. 按上节顺序就地校验 app：通过则设 `SCB->VTOR = 0x08010000`、置 MSP、跳 app 复位向量；
3. 校验失败（含 dfu-util 传输中断留下的截断镜像）：点亮 LED1（PC0，低有效，作为「恢复模式」指示常亮贯穿 DFU 会话）后跳 ROM DFU——app 变砖场景仅凭 USB 线即可救回。

跳 ROM DFU 的姿势：置 SYSCFG_MEMRMP=1 将系统存储别名到 0x0（复现 BOOT 引脚自系统存储启动的映射）、置 MSP 为 `*(u32*)0x1FFF0000`、跳 `*(u32*)0x1FFF0004`（F446 系统存储地址与 F407 相同）。

USART2（PA2，115200 8N1，TX 轮询，与 app 的 finsh 控制台共用外接排针）输出启动决策日志；两条离开路径（跳 app / 跳 ROM DFU）前均等待发送移位器排空并将 USART2、GPIOA 经 RCC 复位线还原，维持近复位外设状态（GPIOA 兼载 ROM bootloader 所属的 OTG_FS 引脚）。boot 不依赖 `packages/`（自带寄存器定义），实占 <2 KB。

## VTOR 双保险

app 进入路径有两条，VTOR 各自覆盖：

- **经 boot 跳转**（正常上电）：boot 跳转前已设 VTOR = 0x08010000；
- **经 dfu-util `:leave` 直跳**（升级完成时）：DfuSe leave 语义为直接跳转下载地址，不经芯片复位、不过 boot，此时 VTOR 仍指向 ROM。为此 app 在板级初始化最早点（`board.c` 的 `SystemClock_Config` 开头）自设 `SCB->VTOR = &g_pfnVectors`（即 0x08010000）。

CMSIS 包内 `SystemInit` 未定义 `USER_VECT_TAB_ADDRESS`，不触碰 VTOR，`packages/` 保持零改动。

## EP0 vendor 请求

请求码续接 GPIO 的 0x60–0x62：

| bRequest | 方向 | 参数 | 行为 |
| ---- | ---- | ---- | ---- |
| 0x63 OTA_REBOOT | OUT，无数据 | wValue：0=正常复位；1=写 BKP0R 后复位，经 boot 进 ROM DFU | 一次性内核定时器延时约 100 ms 执行复位，等 EP0 状态阶段完成后再断线 |
| 0x64 OTA_VERSION | IN，48 B | — | 返回运行中固件 `.fw_info` 的 fw_version[32] + build_time u32 + board_id u32 + hdr_ver u16 + 保留 u16 + image_crc32 u32（小端；主机以 image_crc32 做升级后比对） |

挂接机制：CherryUSB `usbd_core.c` 对 vendor 请求按接口注册顺序链式尝试各 `vendor_handler`，返回 0 即认领。`usb_bridge` 暴露通用扩展注册接口 `usb_bridge_register_vendor_ext()`（单槽先到先得，挂入 `cdc_proto.c` 的分发链尾），OTA 模块（`applications/ota/ota_ep0.c`）初始化时自注册，仅认领 0x63–0x64，其余返回 -1 交还分发链。依赖方向 ota → usb_bridge，CDC 协议层对 OTA 无感知、零改动。

## ROM DFU 通道事实

- 入口 0x1FFF0004（系统存储，AN2606），要求近复位环境——由 boot 入口跳转天然满足；不从运行中的 RT-Thread 环境直接跳转；
- USB DFU 时钟依赖 HSE 自动检测（4–26 MHz），板载 16 MHz 晶振满足；
- DFU 态枚举为 `0483:df11`（DfuSe 协议），擦写命令：`dfu-util -a 0 -s 0x08010000:leave -D build/ailink.bin`（按镜像大小逐扇区擦除，写毕直跳 app）；
- RDP 需维持 Level 0（当前默认）；DFU 可写全片内 Flash，误操作可覆盖 boot——主机封装脚本固定 `-s` 地址以规避。

## 恢复路径分层

| 场景 | 通道 | 需要 |
| ---- | ---- | ---- |
| 日常升级 | 0x63(wValue=1) → ROM DFU → dfu-util | 一根 USB 线，一条命令 |
| app 变砖（含升级中断电截断） | boot 校验失败自动落入 ROM DFU | 一根 USB 线，重跑 dfu-util |
| boot 损坏（OTA 链路永不触碰 boot，风险面极小） | SWD 或 BOOT0 拉高进 ROM DFU | 物理接触；可选 WRP 写保护扇区 0–3 进一步加固 |

升级窗口（进 DFU → 擦写 → 回枚举）约 20–30 s，期间双路串口与 DAP 全部离线，升级前需结束占用会话。

## 主机侧

`tools/host/ailink-ota.py` 三个子命令：`flash <bin>`（完整升级）、`info`（0x64 查询运行版本）、`verify <bin>`（仅本地校验镜像）。`flash` 流程：校验目标 `.bin` 的 `.fw_info`（magic/board_id/CRC，防误刷）→ 发 0x63(wValue=1) → 等待 `0483:df11` 枚举 → 调 dfu-util → 等待 `1209:0010` 回枚举 → 发 0x64 按 image_crc32 比对。两个快捷路径：设备已处于 DFU 态（截断救回场景）则跳过触发直接刷写；设备已运行同 CRC 镜像则直接返回（`--force` 强制重刷）。依赖系统安装 dfu-util（≥0.9）。

DFU 设备权限由 `tools/host/99-ailink.rules` 解决（`0483:df11` 归 plugdev），经 `install.sh` 安装。

## 已验证与未决

- 已验证：boot 校验通过后跳 app（启动日志经 PA2 输出）、`.fw_info` 构建期回填、boot+app 组合烧录（`./run.sh flash-all`）。
- 待验证：0x63/0x64 端到端升级回环、截断救砖，见 [usb-bridge.md](usb-bridge.md) 验证清单同批执行。
