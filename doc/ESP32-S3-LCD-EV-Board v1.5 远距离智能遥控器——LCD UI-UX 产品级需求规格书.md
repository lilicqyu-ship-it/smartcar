# ESP32-S3-LCD-EV-Board v1.5 远距离智能遥控器

## LCD UI / UX 产品级需求规格书

**项目角色**

```text
ESP32-S3-LCD-EV-Board v1.5
= 主遥控器 / 主驾驶终端

ESP32-C6
= 小车无线通信网关

TC275
= 原有车辆控制核心

手机 Web
= 备用控制 + 调试 + OTA + 维护
```

**目标**

在不改变现有车辆核心控制架构的前提下，将 ESP32-S3-LCD-EV-Board v1.5 打造成一个具有：

```text
驾驶控制
+
车辆仪表
+
无线链路监控
+
安全告警
+
设备管理
+
诊断
```

能力的完整智能遥控终端。

---

# 1. UI 产品定位

S3 不能被设计成：

```text
一个“触摸按钮板”
```

而应该被设计成：

```text
一个“智能车辆遥控器”
```

用户在驾驶时应该能够：

```text
一眼知道车辆状态
一眼知道无线状态
一眼知道当前控制权
快速控制车辆
快速执行 STOP
快速识别故障
```

不应该要求用户进入多个页面才能确认：

```text
有没有连接
有没有控制权
当前速度多少
无线质量怎么样
车辆是不是处于安全状态
```

---

# 2. 核心 UX 原则

整个 UI 遵循以下原则：

## 2.1 驾驶优先

任何时候：

```text
车辆控制
>
无线状态
>
安全状态
>
车辆状态
>
高级信息
>
设置
```

---

## 2.2 重要信息永远可见

驾驶主界面必须长期显示：

```text
连接状态
控制权
速度
车辆状态
无线质量
STOP
```

---

## 2.3 大按钮

驾驶过程中所有高频操作按钮必须：

```text
大
清晰
容易触摸
有足够间距
避免误触
```

---

## 2.4 尽量少层级

驾驶过程中：

```text
1 次触摸
```

能够完成最重要的操作。

禁止把：

```text
STOP
```

隐藏在：

```text
Menu → Vehicle → Safety → Stop
```

之类的深层页面。

---

# 3. 屏幕基准

默认 UI 目标：

```text
480 × 480
```

适配 ESP32-S3-LCD-EV-Board 标准 3.95 英寸版本。

UI 必须采用：

```text
响应式布局
```

避免所有组件依赖固定像素坐标。

如果未来更换其他 LCD 分辨率：

```text
320×240
800×480
```

核心功能和页面结构应该仍然成立。

---

# 4. UI 信息架构

完整系统建议设计为：

```text
                    ┌──────────────┐
                    │   开机界面    │
                    └──────┬───────┘
                           │
                           ▼
                    ┌──────────────┐
                    │   HOME驾驶    │
                    │   主界面      │
                    └───┬────┬─────┘
                        │    │
             ┌──────────┘    └──────────┐
             ▼                           ▼
       Vehicle Page                Radio Page
       车辆信息                      无线信息
             │                           │
             └──────────┬────────────────┘
                        ▼
                    Status Page
                        │
                        ▼
                   Settings Page
                        │
              ┌─────────┼─────────┐
              ▼         ▼         ▼
            Radio     Display   System
            无线       显示       系统
```

此外还必须有：

```text
Emergency / Alert
```

作为系统级覆盖层。

---

# 5. 页面总览

至少设计以下页面：

```text
P0 Boot
P1 Home / Drive
P2 Vehicle
P3 Radio
P4 Diagnostics
P5 Settings
P6 Pairing
P7 Display
P8 About
P9 Alert Overlay
P10 Calibration
```

---

# 6. P0：启动界面

开机后显示：

```text
┌──────────────────────────────┐
│                              │
│          SMART CAR           │
│                              │
│             🚗               │
│                              │
│      REMOTE CONTROLLER       │
│                              │
│        SYSTEM STARTING       │
│                              │
│       █████████░░░           │
│                              │
│   Radio   Display   System   │
│      ✓       ✓        ✓      │
│                              │
└──────────────────────────────┘
```

启动过程中显示：

```text
LCD
Touch
Radio
System
```

初始化状态。

---

# 7. 启动页要求

不能显示大量：

```text
DEBUG LOG
```

普通用户不应该看到。

Debug 信息进入：

```text
Diagnostics
```

页面。

---

# 8. P1：驾驶主界面

这是整个系统最重要的 UI。

建议采用：

```text
顶部状态栏
+
中央驾驶控制区
+
底部车辆信息
```

结构：

```text
┌────────────────────────────────┐
│ 🚗 SMART CAR     ● S3 MASTER   │
│ WiFi/Radio  ████████  -61dBm   │
├────────────────────────────────┤
│                                │
│        SPEED                   │
│         1.25                   │
│        m/s                     │
│                                │
│           ┌────────┐           │
│           │        │           │
│           │   ●    │           │
│           │        │           │
│           └────────┘           │
│             JOYSTICK           │
│                                │
├────────────────────────────────┤
│ BAT 82%   MODE SPORT   READY   │
│                                │
│      [   STOP   ]              │
└────────────────────────────────┘
```

---

# 9. 驾驶主界面必须长期存在的信息

无论车辆处于什么状态，都尽量保持：

```text
Control Owner
Radio State
RSSI
Speed
Vehicle State
Battery
MODE
STOP
```

---

# 10. 顶部状态栏

顶部高度建议：

```text
约 50~65 px
```

包含：

```text
左：
车辆/项目名称

中：
连接状态

右：
控制权
```

例如：

```text
SMART CAR       ● CONNECTED
                S3 MASTER
```

---

# 11. 无线状态图标

无线状态不能只有：

```text
CONNECTED
```

建议至少：

```text
CONNECTING
CONNECTED
WEAK
LOST
```

UI 使用：

```text
无线图标
+
文字
```

双重表达。

不要只靠颜色。

---

# 12. 控制权显示

必须明确：

```text
S3 MASTER
```

或者：

```text
WEB MASTER
```

或者：

```text
NO CONTROL
```

如果当前为：

```text
S3 MASTER
```

手机 Web 在后台只能作为：

```text
Monitor
```

除非明确执行控制权切换。

---

# 13. 虚拟摇杆

虚拟摇杆是驾驶主界面的核心。

要求：

```text
中心死区
最大范围
平滑移动
视觉回弹
触摸轨迹
```

显示：

```text
Throttle
Steering
```

实时数值。

---

# 14. 摇杆视觉设计

建议采用：

```text
外圈
+
内圈
+
中心点
+
方向轨迹
```

例如：

```text
        ┌────────────┐
        │     ↑      │
        │   ↗ ●      │
        │            │
        │            │
        └────────────┘
```

中心：

```text
0 / 0
```

满行程：

```text
100 / -100
```

---

# 15. 摇杆死区

必须设计：

```text
Dead Zone
```

避免手指轻微抖动导致：

```text
车辆缓慢爬行
```

死区大小应该：

```text
可配置
```

默认值由实际驾驶测试确定。

---

# 16. 油门显示

建议主界面增加纵向速度/油门指示：

```text
THROTTLE

100%
│
│
│ █
│ █
│ █
│
0%
```

或者中央速度仪表。

重点是让用户知道：

```text
当前油门不是 0
```

---

# 17. 速度仪表

速度属于核心信息。

建议：

```text
大数字
+
单位
```

例如：

```text
1.25
m/s
```

而不是：

```text
Speed = 1.25
```

驾驶时大数字更容易读取。

---

# 18. STOP 按钮

STOP 必须是：

```text
大尺寸
高可见度
固定位置
```

不能随页面变化而移动。

建议：

```text
底部中央
```

保持固定区域。

---

# 19. STOP 操作

普通 STOP：

```text
一次触摸
→
立即停止
```

急停：

```text
触发
→
立即进入 Emergency Stop
```

解除急停：

```text
必须明确执行
+
车辆默认保持停止
```

不能因为触摸松开就自动恢复运动。

---

# 20. Emergency Overlay

出现严重情况时：

```text
全屏 Alert Overlay
```

例如：

```text
┌──────────────────────────────┐
│                              │
│           WARNING            │
│                              │
│       RADIO CONNECTION       │
│             LOST             │
│                              │
│          VEHICLE STOP        │
│                              │
│                              │
│        [ ACKNOWLEDGE ]       │
│                              │
└──────────────────────────────┘
```

---

# 21. 告警等级

至少定义：

```text
INFO
NOTICE
WARNING
CRITICAL
```

示例：

```text
INFO
Control Connected

NOTICE
RSSI Low

WARNING
Battery Low

CRITICAL
Radio Lost
```

---

# 22. 告警不能过度打扰

普通信息：

```text
不应该全屏弹窗
```

例如：

```text
RSSI -68dBm
```

只更新状态栏。

只有真正影响安全的事件才应该：

```text
全屏
+
明显提示
```

---

# 23. P2：Vehicle 页面

用于查看完整车辆状态。

建议：

```text
┌──────────────────────────────┐
│          VEHICLE             │
├──────────────────────────────┤
│ State          RUN           │
│ Speed          1.25 m/s      │
│ Throttle       35 %          │
│ Steering       -12 %         │
│ Mode           SPORT         │
│                              │
│ Battery        82 %          │
│ Temperature    36 °C         │
│                              │
│ Fault          NONE          │
├──────────────────────────────┤
│         [ BACK ]             │
└──────────────────────────────┘
```

---

# 24. P3：Radio 页面

专门观察远距离通信质量。

显示：

```text
Connection
RSSI
Latency
Packet Loss
RX Rate
TX Rate
Sequence
Last Packet
Peer ID
Channel
```

例如：

```text
┌──────────────────────────────┐
│           RADIO              │
├──────────────────────────────┤
│ ● CONNECTED                  │
│                              │
│ RSSI          -61 dBm        │
│ QUALITY       GOOD           │
│                              │
│ LATENCY       18 ms          │
│ LOSS          0.2 %          │
│                              │
│ TX            30 Hz          │
│ RX            10 Hz          │
│                              │
│ SEQ           12893          │
│ CHANNEL       6              │
└──────────────────────────────┘
```

---

# 25. 无线质量图形化

除了数字：

```text
-61 dBm
```

增加：

```text
████████░░
```

但数字仍必须保留。

因为：

```text
图形 = 快速判断

数字 = 工程诊断
```

---

# 26. Radio Quality

UI 定义：

```text
EXCELLENT
GOOD
FAIR
WEAK
CRITICAL
```

具体 RSSI 分界值由实际距离测试确定。

不能把未经实测的阈值当成最终产品标准。

---

# 27. 延迟显示

实时：

```text
18 ms
```

可以进一步显示：

```text
AVG
MIN
MAX
```

诊断页面可以：

```text
Average 18 ms
Minimum 12 ms
Maximum 46 ms
```

---

# 28. 丢包率显示

实时显示：

```text
0.2 %
```

诊断页面显示：

```text
Packets TX
Packets RX
Packets Lost
Loss %
```

帮助评估：

```text
实际远距离通信效果
```

---

# 29. P4：Diagnostics 页面

这是给开发人员和高级用户使用的。

包含：

```text
ESP32-S3
C6
Radio
Vehicle
Touch
LCD
Memory
Uptime
Firmware
```

例如：

```text
┌──────────────────────────────┐
│         DIAGNOSTICS          │
├──────────────────────────────┤
│ Controller                   │
│ FW      v0.1.0               │
│ Heap    xxxx KB              │
│ PSRAM   xxxx KB              │
│                              │
│ Radio                        │
│ RSSI    -61 dBm              │
│ Loss    0.2 %                │
│ Latency 18 ms                │
│                              │
│ Vehicle                      │
│ Link    OK                   │
│ TC275   READY                │
└──────────────────────────────┘
```

---

# 30. Diagnostics 不应该出现在驾驶首页

首页：

```text
简单
直观
面向驾驶
```

Diagnostics：

```text
详细
工程化
面向开发
```

两者必须分开。

---

# 31. P5：Settings 页面

建议分成：

```text
Control
Radio
Display
Sound
System
```

例如：

```text
┌──────────────────────────────┐
│           SETTINGS           │
├──────────────────────────────┤
│ Control              >       │
│ Radio                >       │
│ Display              >       │
│ Sound                >       │
│ System               >       │
│ About                >       │
└──────────────────────────────┘
```

---

# 32. Control Settings

允许配置：

```text
Joystick Dead Zone
Throttle Limit
Steering Limit
Direction Reverse
Sensitivity
Control Mode
```

---

# 33. 推荐提供驾驶模式

建议至少：

```text
ECO
NORMAL
SPORT
```

定义：

### ECO

```text
较低最大输出
较平缓加速
```

### NORMAL

```text
默认模式
```

### SPORT

```text
更高响应速度
更直接控制
```

具体参数必须最终通过车辆实际测试确定。

---

# 34. 模式切换必须有反馈

从：

```text
NORMAL
```

切换为：

```text
SPORT
```

必须显示：

```text
SPORT MODE
```

短暂确认。

不能出现：

```text
用户点击
→
车辆行为改变
→
屏幕没有任何反馈
```

---

# 35. Radio Settings

包含：

```text
Peer Status
Channel
LR Mode
Reconnect
Pairing
Statistics
```

第一阶段可以隐藏高级参数。

避免普通用户误改：

```text
Channel
Protocol
Peer
```

导致遥控器突然失联。

---

# 36. Display Settings

至少：

```text
Brightness
Auto Dim
Screen Timeout
Theme
Orientation
```

推荐：

```text
Brightness
20 / 40 / 60 / 80 / 100 %
```

---

# 37. 驾驶时不要自动黑屏

普通设备可以：

```text
几分钟无操作
→
Screen Off
```

但驾驶模式下：

```text
禁止自动进入深度屏保
```

或者提供：

```text
Driver Mode
```

保持屏幕持续可见。

---

# 38. 低亮度策略

如果未来使用电池供电：

```text
无触摸一段时间
→
降低亮度
```

但是：

```text
车辆运行中
```

不能彻底黑屏。

---

# 39. P6：Pairing 页面

第一阶段：

```text
固定 Peer
```

即可。

后期：

```text
Pairing
```

页面显示：

```text
Controller ID
Vehicle ID
Peer
Pair Status
Last Pair Time
```

---

# 40. 配对 UX

配对时：

```text
Searching vehicle...
```

找到：

```text
Vehicle Found

SMART-CAR-001

[ CONNECT ]
```

连接成功：

```text
CONNECTED
```

---

# 41. 配对失败页面

不能只显示：

```text
Error
```

必须告诉用户下一步。

例如：

```text
Vehicle not found

Check:
• Vehicle power
• Radio connection
• Pairing mode

[ RETRY ]
```

---

# 42. P7：Calibration 页面

控制体验非常依赖摇杆。

因此必须支持：

```text
Joystick Calibration
Touch Calibration
```

---

# 43. Joystick Calibration UX

设计成引导流程：

```text
Step 1

Release joystick

[ NEXT ]
```

然后：

```text
Step 2

Move to FULL UP
```

然后：

```text
FULL DOWN
```

然后：

```text
FULL LEFT
```

然后：

```text
FULL RIGHT
```

最终：

```text
Calibration Complete
```

---

# 44. Touch Calibration

只有触摸异常时使用。

正常用户不应该误触进入。

---

# 45. P8：About 页面

显示：

```text
SMART CAR REMOTE

Controller
ESP32-S3

Firmware
vX.X.X

Radio
ESP-NOW LR

Vehicle
TC275

Gateway
ESP32-C6
```

---

# 46. 页面导航

建议使用：

```text
Swipe
+
Back Button
```

而不是完全依赖实体按键。

但：

```text
驾驶主界面
```

不要频繁要求用户滑动切页。

---

# 47. 手势

推荐支持：

```text
单击
长按
左右滑动
上下滑动
```

但：

```text
驾驶功能
```

必须避免复杂手势。

---

# 48. 手势使用原则

例如：

```text
左滑
```

可以切换：

```text
Drive → Vehicle
```

而：

```text
STOP
```

必须使用明确按钮。

不要定义：

```text
双击左上角 = 急停
```

这种隐蔽操作。

---

# 49. 页面切换动画

允许：

```text
Fade
Slide
Scale
```

但不要过度。

目标：

```text
< 200~250ms
```

给用户：

```text
“设备很流畅”
```

的感觉。

---

# 50. 驾驶界面禁止复杂动画

驾驶主界面动画应该只保留：

```text
速度数字
摇杆
无线状态
仪表
告警
```

不要使用：

```text
大型背景动画
3D 特效
持续滚动文字
```

浪费 CPU 和屏幕刷新资源。

---

# 51. 颜色语义

整个 UI 必须统一颜色语义。

推荐定义：

```text
正常
= 中性/绿色系

警告
= 黄色/橙色系

严重
= 红色系

信息
= 蓝色系
```

但是：

> 颜色不能成为唯一的状态表达方式。

必须同时存在：

```text
图标
+
文字
+
颜色
```

例如：

```text
⚠ RADIO WEAK
```

而不是只显示一个黄色点。

---

# 52. 深色主题

默认推荐：

```text
Dark Theme
```

原因：

```text
驾驶环境
夜间
户外
高对比度
```

建议：

```text
深色背景
亮色数字
清晰分区
```

---

# 53. 白天/夜间

后期可增加：

```text
AUTO
DAY
NIGHT
```

其中：

```text
AUTO
```

可以根据时间或用户设置切换。

---

# 54. 字体层级

必须建立明确字体层级：

```text
一级：
速度 / 核心状态

二级：
RSSI / 电量 / 模式

三级：
说明文字

四级：
Diagnostics
```

速度不要和：

```text
Firmware Version
```

使用相同字号。

---

# 55. 图标

推荐为：

```text
Radio
Battery
Vehicle
Warning
Settings
Diagnostics
Control
```

建立统一图标风格。

不要一个页面用：

```text
emoji
```

另一个页面使用：

```text
Material icon
```

第三个页面使用：

```text
自制像素图标
```

要统一视觉语言。

---

# 56. 电量

如果未来 S3 供电支持电池检测，显示：

```text
82%
```

同时：

```text
Battery icon
```

低电量：

```text
LOW BATTERY
```

极低：

```text
CRITICAL BATTERY
```

---

# 57. 遥控器自身状态

首页应该能够知道：

```text
Controller Battery
Controller Temperature
Controller Firmware
```

如果硬件当前没有相关传感器：

```text
第一版可以隐藏
```

不要为了 UI 强行增加硬件依赖。

---

# 58. C6 与车辆状态的统一

S3 不应该只显示：

```text
Radio Connected
```

还应该区分：

```text
Controller ↔ C6
```

与：

```text
C6 ↔ TC275
```

例如：

```text
RADIO     ● CONNECTED
VEHICLE   ● READY
```

这样用户不会出现：

```text
S3 显示 Wi-Fi Connected
```

但实际上：

```text
TC275 Link Down
```

的误判。

---

# 59. 系统状态模型

建议至少：

```text
BOOT
CONNECTING
READY
CONTROL
WARNING
FAULT
STOPPED
EMERGENCY
```

UI 根据系统状态决定：

```text
哪个页面可以使用
哪些按钮可以操作
哪些告警必须显示
```

---

# 60. READY 状态

显示：

```text
CONNECTED
+
VEHICLE READY
```

但：

```text
油门 = 0
```

才允许进入运动状态。

---

# 61. CONTROL 状态

显示：

```text
S3 MASTER
```

并实时：

```text
速度
摇杆
模式
无线质量
```

---

# 62. FAULT 状态

必须优先显示：

```text
FAULT
```

并告诉用户：

```text
Fault Source
```

例如：

```text
VEHICLE FAULT

TC275 LINK ERROR
```

---

# 63. Emergency 状态

进入：

```text
EMERGENCY STOP
```

时：

```text
车辆停止
控制输入冻结
```

UI 必须明显变化。

例如：

```text
红色边框
+
Emergency Stop
+
明确解除操作
```

---

# 64. 页面访问权限

建议：

```text
普通页面
    ↓
直接进入

高级设置
    ↓
二级页面

危险设置
    ↓
长按/确认
```

例如：

```text
Restore Factory Settings
```

必须二次确认。

---

# 65. 防误触

以下操作必须二次确认：

```text
恢复出厂
删除配对
修改核心 Radio Channel
Firmware Upgrade
进入危险校准
```

---

# 66. 触摸反馈

每一次按钮操作都应有：

```text
视觉反馈
```

例如：

```text
Pressed
```

状态。

如果未来启用：

```text
蜂鸣器
```

则可以提供：

```text
Button click
Warning
Emergency
```

等不同反馈。

---

# 67. 音效不作为核心依赖

声音可以提升 UX。

但是系统不能要求：

```text
必须有声音
```

才能知道：

```text
STOP
Warning
Connected
```

因为用户可能：

```text
在嘈杂环境
戴耳机
关闭音量
```

---

# 68. HMI 反馈优先级

建议：

```text
Emergency
    ↓
视觉 + 声音

Critical
    ↓
视觉 + 可选声音

Warning
    ↓
视觉

Info
    ↓
状态栏
```

---

# 69. 首页信息密度

原则：

> 首页只放“驾驶时真正需要的信息”。

必须避免：

```text
把所有 Diagnostics 全部堆到首页
```

目标：

```text
3 秒内
用户能够读懂车辆当前状态
```

---

# 70. 推荐首页布局

最终推荐：

```text
┌────────────────────────────────┐
│ SMART CAR      ● CONNECTED     │
│               S3 MASTER        │
├────────────────────────────────┤
│                                │
│       SPEED                    │
│                                │
│         1.25                   │
│          m/s                   │
│                                │
│            ◉                   │
│       ┌──────────┐             │
│       │          │             │
│       │   ◉      │             │
│       │          │             │
│       └──────────┘             │
│                                │
│  -35% THROTTLE    +12% STEER  │
├────────────────────────────────┤
│ 🔋 82%    SPORT    ● READY     │
│ 📶 -61dBm      18ms     0.2%  │
│                                │
│            [ STOP ]            │
└────────────────────────────────┘
```

这个界面应该成为：

> **90% 驾驶时间的主界面。**

---

# 71. 快速设置抽屉

从驾驶页面向下滑：

```text
Quick Panel
```

显示：

```text
Brightness
Mode
Radio
Sound
Display
```

但不能放：

```text
危险操作
```

---

# 72. Quick Panel

例如：

```text
┌──────────────────────────────┐
│ QUICK CONTROL                │
├──────────────────────────────┤
│ MODE                         │
│ [ ECO ] [ NORMAL ] [ SPORT ] │
│                              │
│ BRIGHTNESS                   │
│ ─────────●──────             │
│                              │
│ RADIO                        │
│ ● CONNECTED                  │
│                              │
│ [ CLOSE ]                    │
└──────────────────────────────┘
```

---

# 73. 驾驶中的页面切换

不能因为误触：

```text
Home → Settings
```

导致车辆停止显示。

驾驶状态必须：

```text
持续运行
```

页面只是：

```text
信息视图切换
```

而不是：

```text
控制系统重置
```

---

# 74. Page Navigation 与车辆控制解耦

这是重要设计要求。

例如：

```text
用户打开 Radio Page
```

并不意味着：

```text
车辆控制任务停止
```

UI 页面只是：

```text
View
```

车辆控制是：

```text
Controller
```

二者分离。

---

# 75. UI 数据刷新

UI 数据应按不同优先级刷新。

例如：

```text
驾驶控制
30 Hz

速度
10~30 Hz

无线质量
5 Hz

电池
2~5 Hz

Firmware
静态
```

不要所有数据：

```text
50/60 Hz 全量刷新
```

---

# 76. UI 卡顿要求

要求：

```text
驾驶过程中不能因为：
Radio
Telemetry
Diagnostics
```

导致 UI：

```text
卡顿
掉帧
Touch 延迟
```

---

# 77. LVGL / UI 资源要求

如果现有 S3 工程使用 LVGL：

AI 应优先利用：

```text
现有 LVGL
现有 LCD driver
现有 Touch driver
现有 UI framework
```

避免引入：

```text
第二套 GUI Framework
```

---

# 78. 内存要求

UI 设计必须考虑：

```text
Flash
PSRAM
Frame Buffer
Image Assets
Font
Animation Buffer
```

大图片不能无限增加。

---

# 79. UI 资源管理

推荐：

```text
Fonts
Icons
Background
Vehicle Assets
Status Icons
```

统一管理。

不要：

```text
几十个重复 PNG
```

---

# 80. 图标设计

优先：

```text
SVG → 编译/转换资源
```

或者：

```text
LVGL vector/icon
```

具体方式由 AI 根据实际工程选择。

目标：

```text
清晰
轻量
统一
```

---

# 81. 启动速度

增加 UI 后不能明显拖慢：

```text
Boot
→
Ready
```

时间。

目标：

```text
启动后尽快进入可操作状态
```

---

# 82. 故障后的 UI 恢复

如果：

```text
Radio Lost
```

随后：

```text
Radio Recovered
```

UI 不应该一直停留在：

```text
Radio Lost
```

必须更新成：

```text
CONNECTED
```

同时保留历史事件记录。

---

# 83. Event History

第二阶段增加：

```text
EVENT LOG
```

例如：

```text
20:31:18
Radio Connected

20:31:43
RSSI Weak

20:31:55
Radio Lost

20:31:56
Vehicle STOP

20:32:03
Radio Recovered
```

这对长距离测试非常有价值。

---

# 84. 无线测试模式

建议增加：

```text
Radio Test
```

用于工程测试。

显示：

```text
RSSI
Latency
Packet Loss
Distance Test
TX/RX Rate
```

这样 S3 本身就可以成为：

> **无线链路测试仪表。**

---

# 85. Distance Test

工程模式可以记录：

```text
Start Test
```

然后持续统计：

```text
Time
RSSI
Latency
Packet Loss
```

结束后：

```text
Test Summary
```

这样以后可以直接进行：

```text
10m
50m
100m
200m
500m
```

等实地测试。

---

# 86. 用户模式

建议设计：

```text
USER MODE
ENGINEER MODE
```

普通用户：

```text
驾驶
车辆状态
基本设置
```

工程人员：

```text
Diagnostics
Radio
Calibration
Event Log
```

---

# 87. Engineer Mode

工程模式不能影响：

```text
正常驾驶稳定性
```

也不能默认开启。

可通过：

```text
连续点击 Firmware Version
```

或者其他明确方式进入。

具体进入方式由 AI 设计，但应避免普通用户误触。

---

# 88. 首屏目标

用户开机后的第一目标不是：

```text
“看起来很酷”
```

而是：

```text
我知道它有没有连接。
我知道现在谁拥有控制权。
我知道车辆是不是 READY。
我知道无线是否健康。
我能立即 STOP。
```

---

# 89. 第二目标

驾驶过程中：

```text
我要知道速度。
我要知道油门。
我要知道转向。
我要知道电池。
我要知道通信是否稳定。
```

---

# 90. 第三目标

遇到问题时：

```text
我应该知道出了什么问题。
我应该知道车辆有没有自动停止。
我应该知道如何恢复。
```

---

# 91. 产品体验目标

整个设备应该给用户这样的操作感受：

```text
开机
 ↓
快速连接
 ↓
一眼看到 READY
 ↓
进入驾驶
 ↓
大摇杆
 ↓
实时速度
 ↓
无线质量实时显示
 ↓
异常立即提示
 ↓
失联自动停止
 ↓
恢复后明确提示
```

而不是：

```text
开机
 ↓
等半天
 ↓
进入菜单
 ↓
找连接
 ↓
找车辆
 ↓
找控制
 ↓
才能开始
```

---

# 92. 分阶段 UI 开发

UI 也必须分阶段。

## UI Phase 1

实现：

```text
Boot
Home
基础 Touch
虚拟摇杆
STOP
连接状态
```

---

## UI Phase 2

增加：

```text
Speed
Mode
Battery
RSSI
Latency
Packet Loss
```

---

## UI Phase 3

增加：

```text
Vehicle
Radio
Diagnostics
```

---

## UI Phase 4

增加：

```text
Settings
Calibration
Pairing
Display
```

---

## UI Phase 5

增加：

```text
Event Log
Radio Test
Engineer Mode
```

---

## UI Phase 6

视觉升级：

```text
动画
图标
主题
过渡
微交互
音效
```

---

# 93. UI 开发优先级

严格按照：

```text
P0
STOP / 安全 / 状态

P1
驾驶

P2
连接质量

P3
车辆信息

P4
设置

P5
诊断

P6
视觉美化
```

不要反过来。

---

# 94. 第一版不要追求的东西

第一阶段禁止为了“炫酷”加入：

```text
3D 车辆
复杂动画
实时地图
视频
复杂天气
大量背景粒子
高频无意义动画
```

这些全部排在核心驾驶体验之后。

---

# 95. AI 实现要求

AI 在实现 UI 前必须：

1. 确认实际使用的 LCD 型号和分辨率。
2. 阅读现有 S3 工程。
3. 确认已有 LVGL / LCD / Touch 框架。
4. 确认输入设备。
5. 确认现有 C6 协议。
6. 确认可以从 C6 获取哪些 Telemetry。
7. 对无法获取的数据，不得伪造。
8. UI 与通信任务必须解耦。
9. UI 与车辆控制任务必须解耦。

---

# 96. AI 不得为了 UI 修改 TC275

LCD UI 增强不应该导致：

```text
TC275 重构
```

也不应该导致：

```text
TC275 增加 UI 相关协议
```

需要的信息：

```text
由 C6 Telemetry 提供
```

---

# 97. AI 不得为了 UI 重写 C6

C6 只提供：

```text
Command
Telemetry
Radio State
Vehicle State
```

UI 自己负责：

```text
如何显示
如何组织
如何动画
```

---

# 98. 推荐的软件数据层

S3 UI 不应该直接读取底层无线变量。

建议逻辑：

```text
Radio Layer
      ↓
Protocol Layer
      ↓
Vehicle State
      ↓
UI State
      ↓
Screen
```

例如：

```text
Radio:
RSSI=-61

        ↓

UI State:
radio_quality = GOOD

        ↓

Home:
📶 GOOD
```

---

# 99. UI State 设计

建议集中管理：

```text
connection_state
control_owner
vehicle_state
speed
throttle
steering
battery
rssi
latency
packet_loss
mode
fault
warning
```

这样：

```text
Home
Vehicle
Radio
Diagnostics
```

都读取同一份状态。

---

# 100. 页面必须共享同一状态源

禁止：

```text
Home 自己保存 RSSI
Radio 自己再保存 RSSI
Diagnostics 又保存一份 RSSI
```

应该：

```text
Single Source of Truth
```

---

# 101. 数据过期显示

任何 Telemetry 都应该有：

```text
timestamp
```

如果数据太久没有更新：

```text
不要继续显示旧数据冒充实时数据
```

例如：

```text
Battery
82%
```

如果数据过期：

```text
Battery
--
```

或者：

```text
STALE
```

---

# 102. Radio Lost 时首页

必须立即显示：

```text
RADIO LOST
```

同时：

```text
VEHICLE STOP
```

而不是继续显示：

```text
Speed 1.25m/s
```

让用户误认为系统正常。

---

# 103. 控制权丢失

如果：

```text
S3 不再是 Master
```

首页必须明显显示：

```text
NO CONTROL
```

或者：

```text
WEB MASTER
```

---

# 104. 手机接管时

例如：

```text
S3
 ↓
Control Released
 ↓
Phone
 ↓
WEB MASTER
```

S3 必须显示：

```text
WEB MASTER
```

并禁用：

```text
驾驶摇杆
```

防止用户误以为仍然拥有控制权。

---

# 105. 手机释放控制权后

S3 显示：

```text
READY
```

但不要自动恢复之前：

```text
Throttle = 60%
```

必须：

```text
用户重新操作
```

之后才恢复运动。

---

# 106. 防止“页面回到旧状态”

如果用户：

```text
进入 Settings
```

之后返回 Home：

必须读取：

```text
最新 UI State
```

不能显示进入 Settings 前的旧数据。

---

# 107. 视觉设计风格

建议整体风格：

```text
现代
科技
简洁
高对比
驾驶仪表盘
```

而不是：

```text
传统 MCU Demo
```

避免：

```text
边框过多
按钮过多
渐变过多
文字过多
```

---

# 108. 视觉参考方向

可以参考：

```text
汽车数字仪表盘
无人机遥控器
工业手持终端
机器人控制器
智能骑行码表
```

重点学习：

```text
信息层级
警告优先级
状态表达
驾驶时可读性
```

而不是直接复制某个商业产品界面。

---

# 109. UI 最终目标

最终应该形成：

```text
                 SMART CAR
                     │
            ┌────────┴────────┐
            │                 │
         CONTROL            STATUS
            │                 │
         Joystick          Speed
         Throttle          Battery
         Steering          Vehicle
         STOP              Radio
            │                 │
            └────────┬────────┘
                     │
                  SAFETY
                     │
              Alert / Fault
                     │
                     ▼
                Diagnostics
```

---

# 110. 最终产品级首页

最终推荐形成如下信息结构：

```text
┌──────────────────────────────────┐
│ 🚗 SMART CAR       ● CONNECTED   │
│                     S3 MASTER    │
├──────────────────────────────────┤
│                                  │
│              SPEED               │
│                                  │
│              1.25                │
│               m/s                │
│                                  │
│             ┌──────┐             │
│             │      │             │
│             │  ●   │             │
│             │      │             │
│             └──────┘             │
│            JOYSTICK               │
│                                  │
│ THROTTLE -35%     STEER +12%     │
├──────────────────────────────────┤
│ 🔋 82%     SPORT      ● READY    │
│                                  │
│ 📶 -61 dBm   18ms   LOSS 0.2%    │
│                                  │
│             [ STOP ]             │
└──────────────────────────────────┘
```

核心原则：

```text
顶部：连接和控制权
中央：驾驶
下部：车辆和无线状态
底部：安全停止
```

---

# 111. 最终 AI 验收标准

AI 完成 UI 后必须逐项检查：

```text
[ ] 480×480 正确布局
[ ] Touch 正常
[ ] Home 驾驶界面
[ ] 虚拟摇杆
[ ] STOP
[ ] Emergency Stop
[ ] Connection
[ ] Control Owner
[ ] Speed
[ ] Battery
[ ] RSSI
[ ] Latency
[ ] Packet Loss
[ ] Vehicle State
[ ] Radio Page
[ ] Vehicle Page
[ ] Diagnostics Page
[ ] Settings Page
[ ] Pairing Page
[ ] Calibration Page
[ ] Alert Overlay
[ ] Event Log
[ ] Dark Theme
[ ] UI 动画流畅
[ ] 页面切换不卡顿
[ ] Telemetry 不阻塞 UI
[ ] Radio 不阻塞 UI
[ ] 失联后状态正确
[ ] 手机接管后状态正确
[ ] 控制权切换无误触
```

---

# 112. UI 开发最终要求

整个 UI 不以：

```text
“功能很多”
```

作为完成标准。

而以：

```text
用户是否能快速理解
+
用户是否能快速操作
+
用户是否不容易误操作
+
异常是否能够立即理解
+
驾驶过程中是否持续流畅
```

作为完成标准。

---

# 113. 最终产品形态

最终 S3 应该从：

```text
ESP32-S3 开发板
```

变成：

```text
SMART CAR REMOTE
```

用户拿起来后的体验应该接近：

```text
开机
 ↓
自动连接车辆
 ↓
首页显示 READY
 ↓
触摸摇杆
 ↓
车辆运动
 ↓
实时看到速度
 ↓
实时看到无线质量
 ↓
异常自动告警
 ↓
失联自动停止
 ↓
恢复后自动提示
```

而不是：

```text
开机
 ↓
进入 Demo 页面
 ↓
手动找连接
 ↓
手动找控制
 ↓
调试信息满屏
```

---

# 114. 总体产品原则

最终系统应形成：

```text
ESP32-S3-LCD-EV-Board
        │
        │
        ▼
┌─────────────────────────┐
│  Professional HMI       │
│                         │
│  Drive                  │
│  Telemetry              │
│  Radio                  │
│  Safety                 │
│  Diagnostics            │
│  Settings               │
└────────────┬────────────┘
             │
        ESP-NOW LR
             │
             ▼
        ESP32-C6
             │
        Existing Bridge
             │
             ▼
           TC275
             │
             ▼
           Vehicle
```

最终目标不是“做一个 LCD 页面”，而是：

> **把 ESP32-S3-LCD-EV-Board v1.5 做成整个小车系统的人机交互中心。**

LCD 负责：

```text
看到
理解
控制
确认
告警
诊断
```

C6 负责：

```text
通信
桥接
联网
```

TC275 负责：

```text
车辆
实时控制
安全裁决
```

手机负责：

```text
备用
维护
OTA
调试
```

四者职责必须保持清晰。