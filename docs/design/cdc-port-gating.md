# CDC 串口转发门控：主机未打开端口时不转发

| 项 | 内容 |
| ---- | ---- |
| 定位 | UART→USB 方向「端口打开才转发」机制与两条设计边界的当前设计事实 |
| 代码 | [applications/usb_bridge/bridge_pump.c](../../applications/usb_bridge/bridge_pump.c)（probe / retract / 空闲丢弃均在此文件） |
| 不覆盖 | 泵线程结构与吞吐设计（见 [usb-bridge.md](usb-bridge.md)）；性能基线（见 [performance-report.md](../../performance-report.md)） |

## 背景与决策

CDC ACM 协议没有「端口已打开」信号。DTR 依赖上位机行为——部分工具不置位甚至主动拉低（DTR 接复位的烧录场景），不能作为门控依据。采用的判据：主流主机串口驱动（Linux `cdc_acm`、Windows `usbser`、macOS）仅在端口打开期间提交 IN URB，因此「IN 传输是否被取走」是端口开关在设备侧的可观测事实，与上位机应用的具体行为无关。

行为目标对齐市面 USB 转串口（FT232 / CP210x / CH340）：端口未打开期间数据不缓存，打开时不回放历史。

## 机制

- **开检测（probe）**：空闲态在 IN 端点挂 1 个 ZLP。端口打开后驱动提交的第一个 IN URB 取走该 ZLP，完成回调即「端口已打开」证据，置 `host_reading` 开始转发。ZLP 对上位机应用不可见（tty 层丢弃 0 字节读）。
- **关检测（retract）**：转发态中一笔 IN 传输超过 500 ms（`PUMP_HOST_IDLE_MS`）无完成，判定 URB 流已停止，`usbd_ep_close`/`usbd_ep_open` 撤回该传输（dwc2 标准 EPDIS 流程，open 时 flush TX FIFO），回空闲态。
- **空闲丢弃**：`host_reading` 为 0 期间（含 USB 未接入），UART RX 数据从 DMA 环读出即丢弃。

## 设计边界

### 边界 1：关闭后 500 ms 内快速重开，会交付窗口内数据

关检测触发前重开端口，固件仍视为同一会话，关闭窗口内抵达的数据（龄期 < 500 ms）照常交付。实测：关闭后立即灌入 2048 B、250 ms 后重开，交付 1920 B。重开间隔超过 500 ms 则交付 0 B。窗口宽度由 `PUMP_HOST_IDLE_MS` 控制。

### 边界 2：端口打开但持续不读，超过 500 ms 后丢弃，恢复读取自动续传

应用长时间不 `read()` 时，主机内核 tty 缓冲（约 64 KB）打满后驱动节流、停止提交 IN URB——设备侧与「端口关闭」不可区分，500 ms 后按关闭处理并丢弃后续数据。应用恢复读取后 URB 恢复、probe 完成，转发自动恢复，无需重开端口。

该场景下 FT232 同样丢数据（芯片 RX 缓冲 256 B，115200 下约 22 ms 即溢出），属无流控串口桥的共性而非本机制引入。需要该场景不丢数据须 RTS/CTS 硬件流控，当前未实现（RTS 仅记录，见 usb-bridge.md 协议要点）。

## 已验证（2026-07-28，闭环：通道 B ttyACM1 ↔ FT232 ttyUSB0，DFU 升级后实测）

- 未打开期间灌入 2.1 KB → 打开后读取 0 B；关闭后持续灌入 2.8 KB → 1 s 后重开读取 0 B，实时转发恢复且 byte-exact。
- 28 轮开关循环（每轮夹带脏数据注入 + 512 B 双向校验）全部干净。
- 并发读模式下 921600 波特率 256 KiB × 3 轮 × 双向、2 Mbaud 64 KiB 双向全部 byte-exact——门控不影响端口打开期间的持续吞吐。
- 节流场景复现：64 KiB 写入且无并发读，交付 5719 B 后触发 retract（数值确定性复现），恢复读取后 16 KiB 传输 byte-exact，自愈无后遗症。
