# USB 复合设备：双 CDC 串口 + GPIO + CMSIS-DAP v2

| 项 | 内容 |
| ---- | ---- |
| 定位 | OTG_FS 上的标准复合设备：双路 CDC ACM 免驱串口 + EP0 GPIO + CMSIS-DAP v2 调试器的当前设计事实 |
| 代码 | [applications/usb_bridge/](../../applications/usb_bridge/)、[board/ports/usb_config.h](../../board/ports/usb_config.h)、[board/ports/usbd_fs_port.c](../../board/ports/usbd_fs_port.c)、[board/ports/DAP_config.h](../../board/ports/DAP_config.h) |
| 主机工具 | [tools/host/](../../tools/host/) |
| 参考实现 | 架构承接 ailink-f407 的 FT2232H 克隆方案；F446 端点翻倍后改标准 CDC，弃用 FTDI 伪装 |
| 不覆盖 | 构建与烧录（见 [AGENTS.md](../../AGENTS.md)）；实现过程与验证记录（见 git log） |

## 设备身份

- VID/PID `1209:0010`（pid.codes 测试号，宏定义于 `bridge_desc.c` 可改），bcdUSB 2.1（枚举 BOS），设备类 `EF/02/01`（IAD 复合）。
- 五接口：0/1 = CDC ACM 函数 0（通道 A）、2/3 = CDC ACM 函数 1（通道 B）、4 = vendor（CMSIS-DAP v2，iInterface 含 "CMSIS-DAP" 供 pyOCD/OpenOCD 识别）。
- 免驱：Linux `cdc_acm` 绑出两个 `/dev/ttyACM*`；Windows 10+ `usbser` 按 IAD 绑双 COM；DAP 接口经 BOS + MS OS 2.0 描述符（配置子集 → 接口 4 函数子集 → CompatibleID "WINUSB" + DeviceInterfaceGUIDs {CDB3B5AD-293B-4663-AA36-1AAE46463776}）自动绑 WinUSB。
- 序列号取 MCU 96-bit UID，`/dev/serial/by-id/` 路径跨板稳定。

## 端点与通道映射

F446 OTG_FS 有 EP0 + 5 对端点，全部用满：

| 接口 | 端点 | 功能 | UART / 引脚 | 波特率上限 |
| ---- | ---- | ---- | ---- | ---- |
| 0+1 | EP1 bulk 对 + EP4 int IN（notify，闲置） | CDC ACM 通道 A | USART6，PC6/PC7（APB2 90 MHz）+ DMA | 11.25 M |
| 2+3 | EP2 bulk 对 + EP5 int IN（notify，闲置） | CDC ACM 通道 B | USART1，PA9/PA10（APB2 90 MHz）+ DMA | 11.25 M |
| 4 | EP3 bulk 对 | CMSIS-DAP v2 | SWCLK=PA4 / SWDIO=PA5 / nRESET=PA6 | SWD 1–4 M |
| — | EP0 | CDC 类请求 + GPIO/OTA vendor request | GPIO 引脚表（见下） | — |

- 两路桥全用 APB2 串口以对齐 11.25 M 上限（OVER8 时 90 MHz / 8）；控制台为此让位迁至 `uart2`（PA2/PA3，APB1，115200）。
- 11.25 M 为线速能力（短突发/固定高波特率场景）；USB FS bulk 持续吞吐约 1.2 MB/s，双通道 + DAP 共享。
- GPIO 控制组引脚表（`bridge_gpio.c`，LQFP64 无 PE 口，改散点引脚）：bit 0..7 = PB0、PB1、PB2、PB8、PB9、PB10、PA1、PA8。
- DMA 选流：UART1_RX=DMA2_S2/Ch4、UART1_TX=DMA2_S7/Ch4、UART6_RX=DMA2_S1/Ch5、UART6_TX=DMA2_S6/Ch5（`dma_config.h` 链式分配结果）。

## 协议要点（实现依据）

- 串口控制面为标准 CDC ACM 类请求：`SET/GET_LINE_CODING`（波特率/数据位/校验/停止位）与 `SET_CONTROL_LINE_STATE`（DTR/RTS，仅记录），由 CherryUSB `usbd_cdc_acm.c` 解码后回调 `cdc_proto.c`；回调在 USB 中断上下文，仅记录参数，由泵线程应用（`HAL_UART_Init` 不进 ISR）。
- 数据面纯透传（CDC 无逐包状态头）；IN 传输落在 64 B 整包边界时补发 ZLP，使按大 URB 读取的主机（Windows usbser）立即完成传输。
- GPIO 走 EP0 vendor request `0x60`（CONFIG 方向）/ `0x61`（WRITE mask+value）/ `0x62`（READ），协议与 ailink-f407 完全一致，主机工具跨板复用。
- MS OS 2.0 vendor code 为 `0x20`，与 GPIO/OTA 的 `0x60–0x64` 无冲突；CherryUSB 核心在 vendor 分发链前置处理 MSOSv2 描述符请求。
- CherryUSB 挂接点：CDC 类请求走 `usbd_cdc_acm_init_intf` 的 class handler；设备级 vendor 请求（GPIO/OTA）挂在接口 0 的 `vendor_handler`（`usbd_core.c` 按接口链式尝试），不改内核代码。

## 固件模块（全静态分配，错误 `rt_err_t` 上抛）

- `bridge_desc.c`：设备/配置/BOS/MSOSv2/字符串描述符，五接口枚举与初始化入口（`INIT_APP_EXPORT`）。
- `cdc_proto.c`：CDC 线路参数回调（记录 + seq 递增）、EP0 vendor 分发（GPIO 转发 + OTA 扩展槽 `usb_bridge_register_vendor_ext()`）、msh 诊断命令。
- `bridge_pump.c`：每通道 1 线程的双向泵。UART→USB 侧从 serial DMA 环形收（每通道 8 KB），单次最多 512 B `usbd_ep_start_write`；USB→UART 侧 8 槽 64 B 环，OUT 完成 ISR 立即重挂空闲槽，满槽时靠 USB NAK 反压；就绪槽由泵线程合批进 512 B 暂存区后以**单节点**提交 UART TX DMA（serial v1 的 TX data queue 在多节点并发下存在 push 与 DMADONE 竞态，队列深度 >1 时完成事件丢失会永久停摆——实测 2 M+ 触发，单节点化后消除），波特率重配延迟到节点间隙执行。溢出计入 `usbbr_stat`。
- `bridge_gpio.c`：8 引脚映射表 `static const`，经 `rt_pin_*` 操作，协议处理在 USB ISR 内（寄存器级操作，微秒量级）。
- `bridge_dap.c`：EP3 命令-响应泵线程，调用 CMSIS-DAP 参考实现 `DAP_ExecuteCommand`，并提供 `DAP_Info` 标识串回调。
- `dap/`：ARM CMSIS-DAP 参考源码（`DAP.c`/`SW_DP.c`/`DAP.h`，Apache-2.0）逐字副本，只读；改动只经 `board/ports/DAP_config.h`（SWD 位操作，`DAP_JTAG=0`、`SWO=0`）。
- msh 命令：`usbbr_stat`（线路参数/泵计数/ZLP/溢出）、`usbbr_ep`（OTG_FS 端点寄存器）。

## 关键约束与本地改动

- 48 MHz 时钟：主 PLL 维持 180 MHz（PLLQ 无法出 48 M），USB 时钟由 PLLSAI 提供（VCO 2 M×192=384 M，/8=48 M）经 `CK48MSEL` 选择，`board.c` 增量配置。
- `board/ports/usb_config.h`：CherryUSB 裁剪配置（device only、FS PHY、slave 模式、无 data cache），启用 `CONFIG_USB_DWC2_CUSTOM_FIFO`。
- `board/ports/usbd_fs_port.c`：OTG_FS 320 word（1.25 KB）FIFO 分区——RX 共享 128 / EP0 TX 16 / EP1 TX 48 / EP2 TX 48 / EP3 TX 48 / EP4 TX 16 / EP5 TX 16（合计 320 word）。
- `libraries/HAL_Drivers/drivers/drv_usart.c`：OVER8 切换阈值由固定 `>5 M` 改为 `baud*16 > pclk`（按实例取 PCLK1/PCLK2），解锁 pclk/16–pclk/8 区间。属适配层 Wrapper 改动。
- USB 传输缓冲需 4 字节对齐（dwc2 断言要求）。
- 泵线程调用 `usbd_ep_start_read/write` 时关中断，避免与 USB ISR 竞争 `DIEPEMPMSK` 读改写导致 IN 传输永久停摆。
- F446 OTG_FS 无 VBUS 检测脚约束：CherryUSB ST glue 按 `STM32F446xx` 自动置 `b_session_valid_override`，无需板级处理。

## 性能边界（2026-07-21 实测）

单通道自环（`bench.py`，8N1，window 2048，另一通道空闲）两路结果一致：

| 波特率 | 吞吐 | 达线速 | 瓶颈 |
| ---- | ---- | ---- | ---- |
| 1 M | 99.3 KB/s | 99% | 线速 |
| 2 M | 197.3 KB/s | 99% | 线速 |
| 4 M | 397.5 KB/s | 99% | 线速 |
| 6 M | 569.6 KB/s | 95% | 线速→USB 过渡 |
| 8 M / 11.25 M | ≈568 KB/s | — | USB FS 总线 |

- 回环路径每字节过总线两次（OUT+IN），≈568 KB/s×2 ≈ 1.14 MB/s 聚合即 FS bulk 实际上限，故 ≥6 M 后吞吐平台化；window 加大到 4096/6144 无增益（瓶颈在总线不在窗口）。单向应用（只收或只发）理论可获得更高单通道速率。
- 11.25 M 下 5 MB 长稳无丢字节无错序；请求 12 M 被 clamp 到 11.25 M（计数器可见），自环下完整性不受影响。
- 无串口硬件流控（RTS/CTS 不引出）：主机侧连续写需窗口限流（在飞字节数低于设备 RX 环 8 KB），否则设备 RX 环溢出丢数据，`bench.py --window` 即为此设。

## 主机侧交付物（tools/host/）

- `ailink-gpio.py`（pyusb）：`dir` / `set`（带 mask）/ `get` / `pulse --ms`；EP0 控制传输无需 claim 接口，与 `cdc_acm` 共存。GPIO 引脚需先 `dir` 配置为输出再 `set`。
- `bench.py`（pyserial）：回环吞吐 + 完整性统计，`--window` 限制在飞字节数。
- `99-ailink.rules`：`1209:0010` 与 `0483:df11` 免 sudo 权限（CDC 接口由 cdc_acm 自行接管，无需解绑规则）。
- `install.sh`：一键安装上述 udev 规则（`sudo ./install.sh` 执行一次）。
- `requirements.txt`：pyserial ≥ 3.5、pyusb ≥ 1.2。

## 已验证与未决

2026-07-21 上板验证（Linux 主机）：

- 枚举：`1209:0010` 五接口复合设备，`cdc_acm` 绑出两个 ttyACM（`/dev/serial/by-id/` 含 UID 序列号），BOS/MSOSv2 描述符读取正常，FIFO 六分区按预期生效（fifo1–5 offset 连续），dwc2 报告 6 端点。
- CDC 控制面：stty 设 921600（A）/2 M（B）经 SET_LINE_CODING 到达泵线程（`usbbr_stat` 线路参数与 seq 递增一致）；host→device 数据路（OUT 槽→UART TX DMA）计数正确、无 NAK 反压残留。
- CMSIS-DAP：pyOCD 识别为 CMSIS-DAP v2 探针；经 SWD（PA4/PA5/PA6）连外部 STM32F407 目标（IDCODE 0x10076413），1 MHz 与 2 MHz 下 halt / 读 CPUID（0x410fc241）/ RAM 读写 / resume 全通过，OTA 重枚举后复测正常。
- GPIO：`ailink-gpio.py dir/set/get` 全通过（方向掩码与电平回读一致）。
- OTA 与四路共存：完整升级回环 + 截断救砖通过（详见 [ota.md](ota.md)）。
- 串口回环：PC6↔PC7、PA9↔PA10 自环下双通道全阶梯（115200→11.25 M）完整性与吞吐通过，数据见「性能边界」；期间修复 serial v1 TX DMA 队列竞态（2 M+ 停摆，见 `bridge_pump.c` 模块说明）。
- 待验证（遗留）：Windows 侧免驱枚举与 WinUSB 自动绑定（本机无 Windows）。
