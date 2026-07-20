# ailink 性能测试报告

| 项 | 内容 |
| --- | --- |
| 定位 | ailink 当前有效性能基线，覆盖 DAPLink、双路串口与固件升级 3 条主路径 |
| 最新测试日期 | 2026-07-21（Asia/Shanghai） |
| 被测固件 | `8bfcebe53375`，Release，镜像 123,796 B，CRC32 `0xD789A288` |
| 测试状态 | 3 个章节的有效性判据均通过 |
| 更新方式 | 原位替换测试环境、方法、数据与结论；历史结果仅由 Git 保留 |

本报告用于回答“当前固件在当前基准环境中的性能”。固件实现、测试工具或关键硬件路径变化后，应重新执行受影响章节，不得将不同固件、不同测试环境的数据拼接为同一组基线。

## 维护约定

- 每次更新必须记录测试日期、固件提交、构建模式、镜像大小、镜像 CRC、样本数量、主机环境、工具版本与物理连接。
- 吞吐量统一使用十进制单位 `kB/s`（1 kB = 1,000 B）；串口波特率使用 `Mbaud`（1 Mbaud = 1,000,000 baud）；耗时使用秒。
- 表内“均值（范围）”由同一工况的全部有效轮次计算。任何超时、长度不符、字节不一致、工具非零退出或设备未按期重枚举，均判定该轮无效并单独排查，不得从统计中静默删除。
- 性能数据仅保留最新有效基线。旧固件结果、优化前后对比与测试过程不在文件内累积，按 [docs-rules.md](docs-rules.md) 由 Git 历史承载。
- 本基线来自 1 台 ailink 样机与 1 台外部目标板，不代表批量硬件分布；Windows、跨主机差异、温度与电源扰动、EMC、连续运行数日的可靠性不在本次覆盖范围内。

## 共用测试环境

| 项 | 配置 |
| --- | --- |
| ailink 硬件 | STM32F446RET6，180 MHz，USB OTG_FS，设备 VID/PID `1209:0010` |
| 被测样机 | USB iSerial `203934305030500C0009001A`，样本数量 1 台 |
| 固件构建 | `./run.sh rebuild release`；Arm GNU Toolchain 13.3.Rel1（GCC 13.3.1） |
| 固件元数据 | 版本 `8bfcebe`；构建时间 2026-07-20 19:44:49 UTC；镜像 123,796 B；CRC32 `0xD789A288` |
| 主机 | Ubuntu 24.04，Linux 7.0.0-28-generic，x86_64 |
| USB 拓扑 | xHCI 根控制器 → 两级 USB 2.0 Hub → ailink；设备以 USB Full Speed 12 Mbit/s 枚举 |
| 主机工具 | OpenOCD 0.12.0、dfu-util 0.11、Python 3.12.3、pyserial 3.5、PyUSB 1.3.1 |
| 并发负载 | 除双串口并发用例外，其他复合设备通道保持空闲 |

## 1. DAPLink（CMSIS-DAP v2）

### 1.1 测试对象与边界

本项目未集成 Arm DAPLink 固件；本章沿用常用称呼“DAPLink”，实际被测对象为 ailink 的 CMSIS-DAP v2 bulk 调试接口。测试覆盖主机文件与目标 RAM 之间的端到端读写、目标识别、halt、reset 与 resume，不覆盖目标 Flash 擦写、GDB 单步延迟、SWO 或 JTAG。

外部目标为 STM32F407，使用约 10 cm 杜邦线连接 `SWCLK=PA4`、`SWDIO=PA5`、`nRESET=PA6` 与公共 GND。OpenOCD 识别结果为 DPIDR `0x2BA01477`、Cortex-M4 r0p1。测试只改写目标易失性 RAM `0x20000000–0x2000FFFF`，每个频率档结束后执行 `reset run`，不改写目标 Flash。

### 1.2 测试方法

1. 使用 `interface/cmsis-dap.cfg`、USB bulk 后端与 `target/stm32f4x.cfg` 建立 SWD 会话。
2. 分别配置 1 / 2 / 4 / 8 / 12 MHz；每档执行 `reset halt` 并核对 DPIDR 与 CPU 类型。
3. 取 `build/ailink.bin` 前 65,536 B 作为固定载荷，写入目标 RAM，再将同一地址范围读回主机。写入和读回分别用 OpenOCD Jim Tcl `time` 计时，每档重复 5 轮。
4. 每轮使用 `cmp -n 65536` 逐字节比对。载荷及全部 25 份回读文件的 SHA-256 均为 `4a99ef9fe9847b9011c0fa30fbdd2ad2a08303bdff469f77b85c04a8dfff377f`。
5. 任一轮出现 OpenOCD 非零退出、目标身份变化、读写错误或字节不一致，即判定该频率档未通过。

单轮命令模板如下；将频率、轮次与临时文件名按矩阵展开：

```bash
openocd -f interface/cmsis-dap.cfg \
  -c 'cmsis_dap_backend usb_bulk' \
  -f target/stm32f4x.cfg \
  -c 'adapter speed 4000; init; reset halt; \
      echo WRITE_US=[time {load_image build/ailink.bin 0x20000000 bin 0x20000000 0x10000} 1]; \
      echo READ_US=[time {dump_image /tmp/ailink-dap-4000-1.bin 0x20000000 0x10000} 1]; \
      reset run; shutdown'
cmp -n 65536 build/ailink.bin /tmp/ailink-dap-4000-1.bin
```

该口径包含 OpenOCD、USB 传输、SWD 访问与主机文件 I/O 开销，属于实际工具链的端到端吞吐，不等同于 SWD 裸线理论带宽。

### 1.3 测试结果

| SWD 时钟 | RAM 写入，kB/s | RAM 读回，kB/s | 完整性 | 目标控制 |
| --- | ---: | ---: | --- | --- |
| 1 MHz | 95.88（95.84–95.92） | 96.17（96.14–96.21） | 5/5 轮一致 | 通过 |
| 2 MHz | 95.90（95.89–95.92） | 96.18（96.08–96.23） | 5/5 轮一致 | 通过 |
| 4 MHz | 95.87（95.84–95.90） | 96.08（96.02–96.14） | 5/5 轮一致 | 通过 |
| 8 MHz | 95.89（95.85–95.91） | 96.14（96.00–96.28） | 5/5 轮一致 | 通过 |
| 12 MHz | 95.89（95.86–95.92） | 96.17（96.07–96.23） | 5/5 轮一致 | 通过 |

1–12 MHz 的端到端 RAM 吞吐基本不随 SWD 时钟变化。结合 CMSIS-DAP 包长 64 B 与当前固件单响应在途的实现，可判断本工况主要受 USB 命令往返和主机工具调度限制，而非 SWD 时钟限制。12 MHz 代表当前样机与连接条件下已验证的最高测试档位，不作为其他目标板或更长连线的稳定性承诺。

## 2. 双路串口回环压测

### 2.1 测试对象与边界

通道 A 为 USART6（PC6=TX、PC7=RX），通道 B 为 USART1（PA9=TX、PA10=RX）；两路均使用独立 TX-RX 短接线构成硬件回环。测试采用 8N1，无 RTS/CTS。`tools/host/bench.py` 为每轮生成随机载荷，并限制在飞窗口为 2,048 B，低于设备侧每通道 8 KB RX 环。

回环路径为主机 → USB OUT → UART TX → UART RX → USB IN → 主机，每个有效载荷字节经过 USB 总线两次。因此，本章结果代表双向回环吞吐，不等同于单向 USB→UART 或 UART→USB 的独立上限。

### 2.2 测试方法

单通道基线在另一通道空闲时执行。A、B 两路分别覆盖 1 / 2 / 4 / 6 / 8 / 11.25 Mbaud，每档 1,000,000 B、重复 3 轮。理论线速按 8N1 的 10 bit/B 计算，达线速比例为“实测吞吐 ÷（波特率 ÷ 10）”。

```bash
ls -l /dev/serial/by-id/*ailink*
serial_port_a=/dev/serial/by-id/usb-ailink_ailink_USB_bridge_203934305030500C0009001A-if00
serial_port_b=/dev/serial/by-id/usb-ailink_ailink_USB_bridge_203934305030500C0009001A-if02
.venv/bin/python tools/host/bench.py "$serial_port_a" 4000000 \
  -n 1000000 --window 2048
```

双通道饱和用例在两路均配置 11.25 Mbaud 时同步启动，每路每轮发送 5,000,000 B，重复 3 轮：

```bash
serial_port_a=/dev/serial/by-id/usb-ailink_ailink_USB_bridge_203934305030500C0009001A-if00
serial_port_b=/dev/serial/by-id/usb-ailink_ailink_USB_bridge_203934305030500C0009001A-if02
.venv/bin/python tools/host/bench.py "$serial_port_a" 11250000 \
  -n 5000000 --window 2048 &
serial_pid_a=$!
.venv/bin/python tools/host/bench.py "$serial_port_b" 11250000 \
  -n 5000000 --window 2048 &
serial_pid_b=$!
wait "$serial_pid_a"
wait "$serial_pid_b"
```

有效性判据为：进程退出码为 0、接收长度等于发送长度、随机载荷逐字节一致、测试期间无超时。当前固件上的单通道与双通道用例累计校验 66,000,000 B。

### 2.3 单通道结果

| 波特率 | 理论线速，kB/s | 通道 A，kB/s | A 达线速 | 通道 B，kB/s | B 达线速 | 完整性 |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| 1 Mbaud | 100 | 99.30（99.3–99.3） | 99.30% | 99.30（99.3–99.3） | 99.30% | 两路均 3/3 轮一致 |
| 2 Mbaud | 200 | 197.30（197.3–197.3） | 98.65% | 197.30（197.3–197.3） | 98.65% | 两路均 3/3 轮一致 |
| 4 Mbaud | 400 | 397.33（397.0–397.8） | 99.33% | 397.30（396.8–397.9） | 99.33% | 两路均 3/3 轮一致 |
| 6 Mbaud | 600 | 569.27（568.8–569.6） | 94.88% | 568.63（568.4–568.9） | 94.77% | 两路均 3/3 轮一致 |
| 8 Mbaud | 800 | 566.47（566.3–566.7） | 70.81% | 566.77（566.4–567.0） | 70.85% | 两路均 3/3 轮一致 |
| 11.25 Mbaud | 1,125 | 568.10（567.8–568.3） | 50.50% | 568.13（567.9–568.6） | 50.50% | 两路均 3/3 轮一致 |

1–4 Mbaud 基本由 UART 线速决定；6 Mbaud 起进入 USB FS 带宽平台区，继续提高波特率不再增加持续回环吞吐。单通道最高均值为 569.27 kB/s，对应约 1.139 MB/s 的 USB OUT + IN 有效载荷。

### 2.4 双通道并发结果

| 轮次 | 通道 A，kB/s | 通道 B，kB/s | 两路有效载荷合计，kB/s | USB OUT + IN，kB/s | 完整性 |
| --- | ---: | ---: | ---: | ---: | --- |
| 1 | 297.6 | 290.0 | 587.6 | 1,175.2 | 两路一致 |
| 2 | 297.5 | 290.1 | 587.6 | 1,175.2 | 两路一致 |
| 3 | 297.7 | 290.0 | 587.7 | 1,175.4 | 两路一致 |
| 均值 | 297.60 | 290.03 | 587.63 | 1,175.27 | 3/3 轮通过 |

双路同时饱和时未出现丢字节、错序或通道停摆。较低通道与较高通道的吞吐比为 97.5%，未观察到通道饥饿。两路并发比单路回环更充分地利用多个 USB 端点，因此总 USB 有效载荷略高于单路结果。

## 3. 固件升级性能

### 3.1 测试对象与边界

测试覆盖 `ailink-ota.py flash` 的完整用户路径：校验本地 `.fw_info` → 查询运行版本 → EP0 请求重启 → ROM DFU 枚举 → 擦除与下载 app 镜像 → `:leave` → ailink 重新枚举 → 读取版本与 CRC 复核。

端到端耗时由 `/usr/bin/time` 在主机进程外测量，包含镜像校验、USB 重枚举、dfu-util 擦写、脚本内固定等待与升级后 CRC 查询。因此，折算速率用于描述实际命令体验，不代表 ROM DFU 的裸下载速度。本章不测量断电恢复耗时、损坏镜像救砖耗时、不同镜像尺寸的缩放关系或外挂文件系统读写性能。

### 3.2 测试方法

1. 执行 `./run.sh rebuild release`，使用 `ailink-ota.py verify` 核对镜像元数据。
2. 首轮从上一有效固件升级至 `8bfcebe`；后 4 轮使用 `--force` 重刷同一镜像，确保完整链路不会因 CRC 相同而提前返回。
3. 每轮设置单阶段默认枚举超时 30 s。成功判据为 dfu-util 下载完成、设备重新枚举为 `1209:0010`、运行中 CRC 等于 `0xD789A288`，且主机工具退出码为 0。

```bash
.venv/bin/python tools/host/ailink-ota.py verify build/ailink.bin
/usr/bin/time -f 'OTA_WALL_S=%e' \
  .venv/bin/python tools/host/ailink-ota.py flash \
  build/ailink.bin --force
```

### 3.3 测试结果

| 轮次 | 镜像大小 | 端到端耗时 | 端到端折算速率 | 结果 |
| --- | ---: | ---: | ---: | --- |
| 1 | 123,796 B | 19.99 s | 6.19 kB/s | 成功，版本与 CRC 一致 |
| 2 | 123,796 B | 19.99 s | 6.19 kB/s | 成功，版本与 CRC 一致 |
| 3 | 123,796 B | 19.98 s | 6.20 kB/s | 成功，版本与 CRC 一致 |
| 4 | 123,796 B | 19.98 s | 6.20 kB/s | 成功，版本与 CRC 一致 |
| 5 | 123,796 B | 19.93 s | 6.21 kB/s | 成功，版本与 CRC 一致 |
| 统计 | 123,796 B | 均值 19.974 s；中位数 19.98 s；范围 19.93–19.99 s | 均值 6.198 kB/s | 5/5 轮通过 |

当前镜像与主机环境下，完整升级命令耗时稳定在约 20 s，5 轮极差为 0.06 s。升级期间双路 CDC 与 CMSIS-DAP 接口均离线，性能数据不应解释为业务通道可用时间；需要无中断通信的场景应在上层流程中安排维护窗口。
