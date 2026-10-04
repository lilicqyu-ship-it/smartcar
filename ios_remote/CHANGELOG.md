# 更新日志

本项目的所有显著变更都将记录在此文件中。

格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
并且本项目遵循[语义化版本](https://semver.org/lang/zh-CN/)。

## [未发布]

### 新增
- **实时相机视频面**（车端零改动）：s3-gateway MJPEG 流 `:81/stream` 接入——
  驾驶页实时画面卡（LIVE/BUSY/ERROR 状态章 + 全屏页）、拓扑页网关节点转实时
  状态、设置里独立开关与地址（留空跟随控制网关）；单查看者礼让（离页/退后台
  自动断流）、503"被占用"态、4s 短超时 + 0.8s→4s 指数退避重连 + 2s 帧停滞
  自愈；multipart 解析按 Content-Length 切帧（`Camera/CameraClient.swift`）
- **新手引导**：首启三步引导 sheet（加 Wi-Fi → 长按配对键 → PAIR 取控），
  设置里可重看；`--no-onboard` 截图钩子
- **事件日志导出**：诊断页一键分享/复制全量事件文本；玩法页轨迹图快照分享
- **告警无障碍**：全屏告警出现时向 VoiceOver 播报（标题+详情），弹层过渡动画
  真正生效（此前 withAnimation 缺失瞬现）
- **启动屏品牌化**：UILaunchScreen 使用暖白纸底色（LaunchBackground 资产）

### 变更
- **配对 token 迁移 Keychain**（收口 README 已知限制）：运行时值仍走
  settings.token 供 WS URL 使用，持久化改由 Keychain 保存；首启自动迁移
  v1.2 遗留 blob 并从 UserDefaults 剔除；重置配对同步清两层
- **glow() 真实现**：双层 shadow 辉光（原为 return self 空实现，拓扑节点/
  链路脉冲/告警层/电量环等 7 处效果全部失效）
- 数值动画补齐：车速大字/圈速表/纪录墙/电量环 contentTransition(.numericText())
- 无障碍补课：信号条/电量环/RTT 走势/日志导出按钮补 VoiceOver 标签；
  车辆页任务状态与故障码中文化（FaultText，未知码保留 hex）
- 版式统一：驾驶页对齐其余页面的 680/padding(20)；删除未挂载死组件
  SpeedGaugeView/LinkDotView/RTTChip

## [1.2.0] - 2026-10-04

### 新增
- **Logo 仪表环**：驾驶页头部徽标改为 iOS 主屏电池小组件风格的双圆弧——
  外圈 = 电量百分比（>20 % 绿 / ≤20 % 琥珀 / ≤10 % 红，与 C6 原始电量胶囊
  同档位），内圈 = 信号强度（≥3 格陶橙 / 2 格琥珀 / 1 格红），12 点方向
  顺时针填充，数值变化 0.4 s 缓动；离线/无数据时仅显示轨道环
  （`UI/Components.swift` LogoGaugeView）
- **玩法 Tab**（插在驾驶页后）：特技动作库、实时轨迹、圈速挑战、纪录墙
  四大玩法集中入口
- **特技动作库**：8 个预设一键动作（原地左/右旋、8 字巡航、S 形绕桩、
  弹射起步、漂移甩尾、舞蹈串烧、往返冲刺），关键帧线性插值经 30 Hz 控制流
  走摇杆同一条 DRIVE 通道；STOP/急停/摇杆接管/失联/退后台即时中止，
  执行中显示进度、再点中止（`Control/StuntSequencer.swift`）
- **体感驾驶**：CoreMotion 重力映射，前倾=油门、左右倾斜=转向；死区+expo+
  灵敏度可调+一键校准；驾驶页气泡姿态指示，开启后摇杆让位，STOP 锁存时
  体感条提供"继续"恢复入口（`Control/TiltDriver.swift` + `Motion/MotionSource.swift`）
- **实时轨迹**：左右轮速差速航位推算（ω=(vR−vL)/轮距），Canvas 实时绘制、
  最近段高亮、1500 点自动抽稀、遥测断流 >2 s 自动重画；轮距设置可调
  （`Control/OdometryTracker.swift`）
- **竞速与纪录**：圈速秒表（开始/打圈/结束/重置+最近圈列表）与纪录墙
  （极速/单程最远/最快圈速），本地持久化（`Control/RecordsTracker.swift`）
- **音效包**：AVAudioEngine 实时合成引擎嗡鸣（音高随实际输出速度）、双音喇叭、
  特技启动音；ambient 会话尊重静音键，设置默认关（`Audio/SoundEngine.swift`）
- **整车拓扑页**：iPhone → Wi-Fi/WS proto v2 → C6 → SPI/SF 帧 → TC275 节点链，
  链路状态实时点亮（WS/TV 通道、两级 RTT、SPI 误码），TC275 外设芯片
  （xcore 命令队列/DFlash 标定/电机编码器）与 s3-gateway 视觉网关虚线占位
- **芯片版本清单**：本 App（Bundle）/ C6 固件（hello ver）/ TC275 App（tcver
  信标，遥测 fw_ver 兜底）/ TC275 SBL（tcver）/ 硬件 rev——零固件改动；
  AppState 新增 tcver 存储并随断链清零

### 修复
- **实时车速波动剧烈**：驾驶页大字此前直接显示 50 Hz 原始轮速均值且带两位
  小数（0.01 km/h 分辨率），数字狂跳。移植 C6 renderSpeed 同款显示平滑——
  dt 感知 EMA（τ=150 ms），首帧/断流 >400 ms/进出静止（<30 mm/s ≈ 0.1 km/h）
  时吸附而非爬坡，显示改为 1 位小数、静止读 0.0；轨迹/纪录/安全仍用原始遥测
  （`Control/SpeedDisplayFilter.swift`）
- **特技点击不动**：特技启动会使驾驶页摇杆禁用，摇杆的"禁用即归零"复位
  无条件发出 `onChange(0,0)`，被 `joystickMoved` 当成"摇杆接管"把刚启动的
  特技立刻中止——轮子完全不动。双层修复：摇杆 `reset()` 仅在有实际输入时
  才发归零回调；`joystickMoved` 在特技执行中忽略零值写入（非零推杆仍即时
  接管），补 4 个接管语义回归测试
- **充电后电量显示恢复**：电池显示防抖原为"会话内只降不升"，充电后显示值
  永久锁死在低电量。新增两条恢复通道——① 遥测 uptime 回退（整车重启，
  即充电断电重开）时双平面重置并立即重播种；② 车边充边连时，电量中值
  持续 ≥+2 % 达 10 s 步进上调（最终回 100 %），电压平面加 50 mV 上行迟滞
  跟随回升；原有 1.5 s 下降确认、中值尖峰吸收、≤2 Hz 显示闸等防抖特性全部
  保留
- **摇杆转向反转**：`JoystickInput` 右推此前输出 w>0，而固件混控约定 w>0 为
  左转（C6 Web 页 joyW=−dx 同源），实驾"推右左拐"；已翻正并补方向回归测试
- **设置前向兼容**：AppSettings 自定义解码全部 `decodeIfPresent`，旧版 JSON
  缺新字段时不再整体解码失败（否则会回退默认值、清空已保存的 host/token）
- Tab 魔法数字重构为 `Tab` 枚举（含 `--tab` dev hook 与 HomeView 配对跳转）
- App 版本号链路：Info.plist 的 CFBundleShortVersionString 改用
  `$(MARKETING_VERSION)` 占位（此前硬编码 1.0.0，`just ios-version` 升版到不了包内）

## [1.1.0] - 2026-10-03

### 新增
- **座舱 HUD UI 大改版**：弧线速度表盘（渐变弧/刻度/红区/状态章）、遥测呼吸环摇杆
  （外环按链路健康脉动、死区环可视化）、T/S/v/ω 芯片行、图标化模式选择器、
  危险斜纹 STOP（长按进度横扫）、径向 SOC 仪表、RTT 走势图、呼吸辉光告警覆盖层；
  全局触感反馈（STOP 按压升级/模式选择/急停 error 震动）
- **电池显示防抖**（对齐 C6 cee1189 策略）：电压 5 点中值 + 500 ms EMA + ≤2 Hz
  显示闸 + 20 mV 下降迟滞；电量 5 点中值 + 持续 1.5 s 下降确认；会话内只降不升、
  重连不解除、零电压视为未就绪；告警与颜色仍用真实遥测
- **信号强度显示**（两级）：网关提供 hello `rssi` 字段或 `{"t":"rssi","dbm":N}`
  时显示真实 dBm（分档 −60/−67/−75/−85 与 S3 遥控器 Kconfig 同源）；否则按
  RTT + 遥测丢包合成 4 格预估（UI 标注"预估"）
- **App 图标**（CoreGraphics 脚本绘制：发光摇杆 + 底座 LED + 信号弧），
  `ios_remote/tools/gen_icon.swift` 可复现，`just ios-icon` 一键重生成

### 变更
- 版本管理接入 monorepo 口径：版本真源 pbxproj `MARKETING_VERSION`（`just ios-version`
  升版），关于页读 Bundle；tag 前缀 `ios/vX.Y.Z`，Release workflow 复用 CI 门禁

## [1.0.0] - 2026-10-03

### 新增
- 首版：S3 遥控器（smartcar_remote）核心功能的 iOS 复刻——proto v2 帧编解码
  （CRC16 黄金向量与 C 实现字节级对齐）、WS 链路（hello/tc/pong/err 文本面、
  10 s 静默看门狗、3 s 退避）、配对 REST（403/409/504/503 → 动作提示）、
  30 Hz DRIVE 心跳与 ECO/NORMAL/SPORT 限幅、STOP/急停双锁存语义（对齐
  scr_ctrl.c）、失联 1.2 s 去抖 + 电池 20/10 % 回差告警、遥测丢包统计；
  驾驶/车辆/诊断/设置四页 + 全屏告警覆盖层；60 项主机单测
