# ailink

基于 RT-Thread 的 STM32F446 固件工程：板载 USB OTG_FS 枚举为标准复合设备，单根 USB 线同时提供两路免驱 CDC 串口（波特率上限 11.25 M）、8 路 GPIO 与 CMSIS-DAP v2 调试器，并支持免调试器的 DFU 固件升级。

| 项 | 内容 |
| --- | --- |
| MCU | STM32F446RET6（LQFP64，Cortex-M4F，512 KB Flash / 128 KB RAM，180 MHz） |
| RTOS | RT-Thread v5.2.2（USB 栈为其内置 CherryUSB，要求 ≥ 1.5.x） |
| 设备身份 | 双 CDC ACM + vendor 复合设备，VID/PID `1209:0010`（pid.codes 测试号） |
| 目标平台 | Linux（已验证）；Windows 10+ 按免驱设计（usbser + MS OS 2.0 自动绑 WinUSB），尚未实测 |
| 许可 | Apache-2.0；`1209:0010` 为 pid.codes 测试用途分配，不可用于产品销售 |

## 功能

| 通道 | 主机侧 | 设备侧 | 说明 |
| --- | --- | --- | --- |
| 串口 A | `/dev/ttyACM*`（by-id `if00`） | USART6，PC6=TX / PC7=RX | 上限 11.25 M；≤4 M 回环实测达线速 99%，≥6 M 受 USB FS 总线限制约 568 KB/s |
| 串口 B | `/dev/ttyACM*`（by-id `if02`） | USART1，PA9=TX / PA10=RX | 同上（两路同为 APB2 90 MHz 时钟域） |
| GPIO | `ailink-gpio.py` | bit 0–7 = PB0 / PB1 / PB2 / PB8 / PB9 / PB10 / PA1 / PA8 | EP0 vendor request，不占用串口与调试通道 |
| 调试器 | pyOCD / OpenOCD | SWCLK=PA4 / SWDIO=PA5 / nRESET=PA6 | CMSIS-DAP v2，实测 SWD 2 MHz 下 halt / 读写内存通过（10 cm 杜邦线连外部 STM32F407 目标） |

- 四路通道可并发使用；固件控制台独立走 UART2（PA2=TX / PA3=RX，115200 8N1），不参与桥接。
- 序列号取 MCU 96-bit UID，`/dev/serial/by-id/` 路径跨板稳定。
- 固件升级免调试器：`ailink-ota.py flash` 一条命令走 ROM DFU 直刷（约 20–30 s，期间设备离线）；升级中断不变砖，自研 boot 校验失败自动回落 DFU，重跑命令即救回。
- 已知边界：串口无 RTS/CTS 硬件流控，主机连续写入需限制在飞字节数（见 `bench.py --window`，设备侧 RX 环每通道 8 KB）；USB FS 总线有效载荷实测约 1.14 MB/s 聚合，双路双向同时打满会超出物理带宽。

端点映射、描述符布局、FIFO 分区与完整性能数据见 [docs/design/usb-bridge.md](docs/design/usb-bridge.md)；升级与救砖链路见 [docs/design/ota.md](docs/design/ota.md)。

## 构建与烧录

| 依赖 | 说明 |
| --- | --- |
| ARM GNU Toolchain 13.3.rel1 | `arm-none-eabi-*`，`bin` 路径由 `RTT_EXEC_PATH` 指定（默认 `~/toolchain/arm-eabi-toolchain/bin`） |
| RT-Thread v5.2.2 源码 | 根目录由 `RTT_ROOT` 指定（默认 `~/SDK/rt-thread`） |
| RT-Thread Env | 执行 `pkgs --update` 拉取 `packages/`（不入库） |
| Python 3 + python3-venv | scons / kconfiglib 版本锁定于 `requirements.txt`，首次构建自动装入 `.venv/` |
| bear、OpenOCD | 分别用于生成 `compile_commands.json` 与烧录（默认 ST-Link） |

```bash
pkgs --update            # 首次克隆后拉取 packages/
./run.sh build           # 构建 app，默认 Release
./run.sh build-boot      # 构建 bootloader
./run.sh flash-all       # 空板首刷：boot（0x08000000）+ app（0x08010000）
./run.sh rebuild-flash   # 清理、构建并烧录 app（boot 稳定后日常够用）
./run.sh help            # 完整命令与参数
```

产物为 `build/ailink.elf` / `.bin` / `.map` 与 `bootloader/build/ailink-boot.bin`；app 链接后自动回填 `.fw_info` 元数据并输出内存占用报告。

注意：核心板实装晶振为 16 MHz（与 `docs/refs/sche` 原理图标注的 8 MHz 不符），时钟树由 `board/CubeMX_Config/Inc/stm32f4xx_hal_conf.h` 的 `HSE_VALUE` 推导；更换晶振后只需改该宏。

## 主机侧使用

```bash
sudo tools/host/install.sh                   # 安装 udev 规则，执行一次
pip install -r tools/host/requirements.txt   # pyserial >= 3.5、pyusb >= 1.2
```

udev 规则将 `1209:0010` 与 `0483:df11`（ROM DFU）加入 plugdev 组，使 pyusb 工具、pyOCD 与 dfu-util 免 sudo。不安装规则时两路串口仍可直接使用（cdc_acm 内核驱动自动绑定），代价是 GPIO / OTA / 调试工具需 sudo。

```bash
tools/host/ailink-gpio.py dir 0xFF 0xFF    # 8 路 GPIO 全部配置为输出
tools/host/ailink-gpio.py set 0x0F 0x05    # bit0/bit2 置高，bit1/bit3 置低
tools/host/bench.py /dev/ttyACM1 4000000   # 回环吞吐测试，需 TX-RX 短接（A 路 PC6-PC7）
pyocd list                                 # 应识别出 CMSIS-DAP v2 调试器
tools/host/ailink-ota.py flash build/ailink.bin  # OTA 升级（需 dfu-util >= 0.9）
tools/host/ailink-ota.py info              # 查询设备运行中的固件版本
```

## 目录结构

```text
applications/  应用层（usb_bridge/ 为复合设备实现，ota/ 为升级触发与镜像元数据）
board/         板级初始化、CubeMX 配置、链接脚本、ports/ 板级件
bootloader/    自研裸机 bootloader（DFU 跳板、镜像校验、变砖兜底）
libraries/     RT-Thread 设备框架对接 STM32 HAL 的适配驱动
docs/          设计文档 design/、长期备忘 notes/、外部参考 refs/
tools/         构建辅助脚本 build/ 与主机工具 host/
run.sh         构建与烧录入口
```

## 相关文档

- [docs/design/usb-bridge.md](docs/design/usb-bridge.md)：USB 复合设备设计事实（协议、端点、性能边界）。
- [docs/design/ota.md](docs/design/ota.md)：固件升级与救砖链路（分区、`.fw_info`、boot 行为、恢复路径）。
- [AGENTS.md](AGENTS.md)：AI 协作速查（环境、命令、目录、注意事项）。
- [code-rules.md](code-rules.md) / [docs-rules.md](docs-rules.md)：编码规范与文档治理规则。

## 许可

工程代码以 [Apache-2.0](LICENSE) 发布。`applications/usb_bridge/dap/` 为 ARM CMSIS-DAP 参考实现逐字副本（Apache-2.0）。
