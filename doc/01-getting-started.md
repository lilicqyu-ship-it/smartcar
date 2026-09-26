# 快速上手教程：从零到手机遥控

> 文档编号 **01** · 域 入口 · 状态：描述**当前代码（demo）**可跑通路 · 上级索引 [00-index.md](00-index.md)

本教程带你完成 AURIX SmartDrive 的完整搭建：编译烧录 TC275 固件 → 刷写 ESP32-C6 AT 固件 → 接线 → 手机网页遥控。预计 1~2 小时（不含采购）。

> **适用口径**：本文走的是**当前代码（demo，esp-at + UART）** 的最小可跑通路，用于把板子点亮、验证电机与安全逻辑。量产目标态（C6 自研固件 + SPI + SF 帧、闭环伺服、OTA/产测）的构建与验证流程按 [21-software-design.md](20-design/21-software-design.md) §15 里程碑落地，其中 SPI 链路需先通过 [22-link-spi-design.md](20-design/22-link-spi-design.md) §8 的 G1 台架门禁，**本文不提前描述未实现的流程**。UART 接线（第 4 步）在量产后仍是调试控制台与回退链路，需要保留。

## 你需要的东西

| 类别 | 要求 |
|---|---|
| 软件 | AURIX Development Studio（含 TASKING TriCore 编译器）、ESP-IDF v5.x（刷 esp-at 用）、串口终端（115200 8N1） |
| 硬件 | KIT-AURIX-TC275-LITE、ESP32-C6-DevKitC-1 V1.2、TB6612 驱动模块 ×2、直流电机 ×4、2S~3S 电池、DC-DC 降压 5V ≥1A、USB Type-C 线 ×2、杜邦线 |
| 前置知识 | 无需嵌入式经验；需要会插线 |

---

## 第 1 步：导入并编译 TC275 工程

1. 打开 AURIX Development Studio → `File > Import > General > Existing Projects into Workspace`，选择本仓库根目录（`myCar`）。
2. 选中工程 → `Project > Build Project`，等待输出 `Build finished`。

> 编译错误 `FreeRTOS.h not found` 时，检查 `FreeRtos/` 与 `Configurations/` 是否完整检出——它们是工程的一部分，不需要另外安装。

**看到什么算成功：** `TriCore Debug (TASKING)` 目录下生成 `.elf`。

## 第 2 步：烧录并验证最小系统

1. USB 连接 TC275 kit 的调试口，IDE 中点击 `Debug As > myCar TriCore Debug (TASKING)`。
2. 观察三件事：
   - **LED1（P00.5）以 250 ms 周期闪烁** —— FreeRTOS 调度正常；
   - 串口终端（kit 板载 FT2232 对应 COM 口，115200 8N1）打印 `UART initialized`；
   - 在终端里敲任意字符能**回显**（UART echo 任务在工作）。

**看到什么算成功：** 灯闪 + `UART initialized` + 回显，三者齐全。

## 第 3 步：给 ESP32-C6 刷 esp-at 固件（demo / 回退通道）

使用同机的 esp-at 工程（`C:\Code\TC275\AURIX-v1.10.36-workspace\esp-at`，已配置 target=esp32c6、module_esp32c6_default）：

1. 环境安装（仅首次）：运行仓库内 `run_install.bat`。
2. 构建：运行 `run_build.bat`（内部即 `idf.py -DSDKCONFIG_DEFAULTS=... build`）。
3. DevKitC-1 用 USB 线直连 PC（板载 USB-UART 桥口），执行 `idf.py flash`。
4. 验证：串口终端连同一 COM 口发 `AT`，收到 `OK`；再发 `AT+GMR` 能看到版本。

> 注意：esp-at 的 **AT 指令口是 UART1（GPIO6/7）**，不是刷录用的高亮日志口。第 2 步里 PC 直连 USB 测 AT 是走板载桥到 UART0 的下载/日志路径；上车载好后 AT 指令由 TC275 从 GPIO6/7 发出。开发期想用 PC 手动发 AT，需要杜邦线接 J1-5/J1-6 到 USB-UART。

## 第 4 步：按接线图接线

对照 [23-wiring.md](20-design/23-wiring.md) 完成三类接线，重点核对：

1. **TC275 ↔ ESP32-C6（UART，本教程用）**：`P15.0(TX) → GPIO6`、`P15.1(RX) ← GPIO7`、**必须共地**；TC275 侧物理位置在板载 **mikroBUS 插座 pin13(TX)/pin14(RX)**；DevKitC-1 侧 GPIO6/7 = J1-5/6，5V 接 J1-14（车载 DC-DC ≥2A）。量产主链路 SPI（QSPI3 ↔ C6 SPI2，**已按 23-wiring.md §9.1 实物接线**）目前固件未启用，所以本教程仍走 UART；**这组 UART 线保留不拆**（调试控制台 + 回退）。
2. **TC275 ↔ D24A**：8 根 PWM/方向线接 **J4（电机 A/B）/ J6（电机 C/D）**，见 23-wiring.md §3/§4 表格；STBY 直接短接同座 J4-1 的板载 3V3。
3. **电源**：电池 → D24A VIN（逻辑电源 D24A 板载稳压自产，无需外接 VCC）；DC-DC 5V → TC275 与 C6；全系统共地。

> 接线时先断电。所有信号线两端均为 3.3V 电平（D24A 逻辑输入 3.3V 已由实车验证）；**勿向 D24A 的 3V3/5V 输出脚反向灌电**。注意 P00.0（电机 C PWM）与板载 CAN 收发器输入并联，运行期间不要外接 CAN 总线（见 23-wiring.md §4 警示）。

## 第 5 步：上电，手机遥控

1. 全系统上电，等 3~5 秒，DevKitC-1 的 Wi-Fi AP 起来。
2. 手机连接 Wi-Fi：**SSID `AURIX-SmartDrive`，密码 `12345678`**。
3. 手机浏览器打开 **http://192.168.4.1:8080**，出现控制页面（UP/DOWN/LEFT/RIGHT/STOP + 速度滑条）。
4. 按住 UP：双轮前进；松手：50 ms 心跳停止后 100 ms 内自动停车。

**看到什么算成功：** 页面能控制电机，且松开按钮小车在 0.1 s 内停住——这条同时验证了心跳超时保护在工作。

## 验证清单

- [ ] LED1 闪烁、串口有日志
- [ ] `AT` 有 `OK`（PC 直连，或观察 TC275 侧日志：发命令打 `ESP-C6<- AT+...`，收到应答打 `ESP-C6<- OK` / 状态行，初始化过程有 `WIFI: ATE0 / AP mode / ...`）
- [ ] 手机能搜到 `AURIX-SmartDrive`
- [ ] 控制页四方向 + 速度滑条有效
- [ ] 松开按键 100 ms 内停车（心跳超时）
- [ ] 走直线时两侧轮速观感一致（否则检查 motor.c 方向翻转表）

## 故障排查

| 现象 | 大概率原因 | 处理 |
|---|---|---|
| 发 AT 无 OK | esp-at 默认开 RTS 流控，PC/TC275 未接该线 | 见 23-wiring.md §2：发 `AT+UART_CUR=115200,8,1,0,0` 或重编 esp-at 关闭 |
| TC275 日志 `WIFI: AT failed: AT+...` 或发 AT 无回包 | TX/RX 没交叉 / 没共地 | 核对 P15.0→GPIO6、P15.1←GPIO7、GND（TC275 侧在 mikroBUS pin13/14）|
| C6 反复重启（boot: 0x3 / brownout） | 5V/3.3V 供电不足 | 电机没转时正常、一加速就重启 → 电源裕量问题，加 ≥470 µF 电容、换 ≥1A 轨 |
| AP 搜不到 | AT 初始化失败 / C6 停在下载模式 | 复位 C6（RST 键），看 TC275 串口侧 AT 日志 |
| 网页能开但电机不动 | 机器人状态机在 FAULT 或心跳未刷新 | 查 10 ms robot 任务日志；发 STOP 后重试 |
| 一侧轮反转 | D24A 电机线序或 `g_dirInvert` | 按 23-wiring.md §4 注释处理，不要交叉猜测 |

## 下一步

- 想看量产目标态怎么设计（闭环/OTA/安全/产测）→ [21-software-design.md](20-design/21-software-design.md)（设计基准）
- 想看板间 SPI 链路与 SF 帧 → [22-link-spi-design.md](20-design/22-link-spi-design.md)；接线与引脚真源 → [23-wiring.md](20-design/23-wiring.md)
- 想理解**当前代码**怎么组织的 → [31-firmware-architecture.md](30-tc275/31-firmware-architecture.md)
- 想直接用二进制协议做 PC 上位机 → 31-firmware-architecture.md 的协议参考表（demo 帧；量产为 v2 + SF 两段，见 SDD §6）
- 想知道产品往哪走（编码器/PID/IMU） → [11-requirements.md](10-product/11-requirements.md)
