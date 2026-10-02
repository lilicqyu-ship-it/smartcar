# 更新日志

本项目的所有显著变更都将记录在此文件中。

格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
并且本项目遵循[语义化版本](https://semver.org/lang/zh-CN/)。

## [未发布]

### 变更
- `mw/app_version.h` 升至 1.1.0（整车 1.1 基线，迎接 S3-CAM 替换 C6；1.1.0 已涵盖下方 1.0.4~1.0.11 的 DFlash 擦除极性与页装载字节序修复）

## [1.0.10] - 2026-10-02

### 修复
- DFlash 页装载字节序改为小端：台架读回实证页装载缓冲 LSB 落低地址——大端 packWord 把每组 4 字节写反（'SDC1' 被写成 '1CDS'），回读校验必败（step 4）；v1.0.1 的 CALIBREC_getI32 小端本就是对的
- 至此保存链路全通：台架复跑 0x70 四轮 delta 全正零翻转、读回记录逐字节正确、闭环门自动开启、S3 显示 FR/RR INVERTED（镜像右半桥的正确标定态）

## [1.0.9] - 2026-10-02

### 修复
- **擦除确认极性修正：DFlash0 擦除态读 0x00 而非 0xFF**（与 PFlash 相反；Infineon TC275 训练材料原话"memory viewer will show either 0s or 0xEEEEEEEE"）——v1.0.2 的"首字节 0xFF 才算擦除"门把擦好的扇区当没擦，永远拒绝编程（SAVE failed 的直接原因）；页填充 0xFF→0x00 随擦除态；撤除基于"读全 0=未映射"误判的探针与自动选槽（槽位定回 0xAF01E000，与 SBL ota_layout 契约一致）

## [1.0.8] - 2026-10-02

### 新增
- 每次保存打印 `CALSEC=`（当前槽位地址）——启动探针行易错过，诊断随每次保存输出

## [1.0.7] - 2026-10-02

### 新增/修复
- DF0 边界探针改字节读 + UART 直发（v1.0.6 的字读探针闯入 HSM 属地触发 trap 致无声挂死，已撤）+ 槽位自动选择（后依 1.0.9 极性结论撤销）

## [1.0.6] - 2026-10-02

### 新增
- DFlash 地址扫描探针（诊断版；字读探针有 trap 风险，1.0.7 重做）

## [1.0.5] - 2026-10-02

### 修复
- **挂死根因修复：撤销 __disable() 内的阵列轮询读**——flash 阵列 busy 期间读取会总线停等，喂狗链断裂 → 看门狗复位进 FMU 卡死态（热复位不清、断电才恢复）；改用 flash_ota.c 的"仅命令发出相位关中断"模式，等待全程开中断喂狗
- busy 轮询改 FABUSY|D0BUSY 双位；新增 `CALSLOT0/1=` 槽位内容台架日志（其"全 0"读数后被证实=擦除态，是解开极性问题的关键数据）

## [1.0.4] - 2026-10-02

### 新增
- 三时点 FSR 快照（fsr0 操作前/fsr1 命令后/fsr2 失败判定）——其 fsr1 的 D0BUSY 置位证明擦除命令实际被接受执行

## [1.0.3] - 2026-10-02

### 修复
- ENDINIT 解锁改自实现有界等待版（最多 2 万次读回轮询，被拒记 step=5），取代 iLLD clear/setSafetyEndinit 内联的无界自旋（1.0.1 台架挂死的直接死点）

## [1.0.2] - 2026-10-02

### 修复
- 撤销 1.0.1 的 Safety ENDINIT 包装（无界自旋死点）；恢复擦除确认 + 3 次重试上限
- 引入两项误判后由 1.0.9/1.0.10 纠正：0xFF 擦除确认门（极性反）、大端装页（字节序反）

## [1.0.1] - 2026-10-01

### 修复
- 擦除前补 clearStatus、每步查 FSR 错误位、擦除独立 2s 预算、`CALSAVE FAIL step/FSR=` 定位行

## [1.0.4] - 2026-10-02

### 新增
- 保存路径诊断版（未烧台架）：擦除失败时记录 fsr0（本次操作前）/fsr1（命令刚发完）/fsr2（失败判定）三个完整 FSR 快照（`CALSAVE FAIL st/FSR0/1/2=`），擦除确认改为字节轮询 2s（记录擦除是否/何时落地）——一次标定即可裁决"FMU 异常态 / regdef 位名错位 / 竞态"三类嫌疑

## [1.0.3] - 2026-10-02

### 修复
- ENDINIT 解锁改为自实现有界等待版（最多 2 万次读回轮询，被拒记 step=5 干净失败），取代 iLLD `clear/setSafetyEndinit` 内联——后者的无界自旋是 1.0.1 台架挂死的直接死点
- 台架结果：与 1.0.2 失败逐字节相同（step=1，FSR=0x01000100）——ENDINIT 解锁不改变结局，擦除命令仍被静默丢弃，根因转入 1.0.4 诊断

## [1.0.2] - 2026-10-02

### 修复
- DFlash 页装载字节序修正：页装载缓冲 MSB 落最低字节（TriCore 惯例，SBL `flashota_packWord` 同款），1.0.1 用小端 `CALIBREC_getI32` 装载使每个 4 字节组写反、回读校验必败
- 撤销 1.0.1 的 Safety ENDINIT 包装（iLLD 内联无界自旋，在保存路径 `__disable()` 不喂狗窗口里被拒即永久自旋 → CPU0 断喂死亡——1.0.1 仍复现"标定后卡死"的直接死点）
- 恢复擦除确认（扇区首字节读 0xFF 才允许编程，杜绝对未擦除扇区二次编程造 ECC 损坏页）与写失败重试上限（3 次）；页填充 0x00→0xFF（DFlash 擦除态）
- 台架结果：不再挂死（优雅失败生效），但保存仍失败——`CALSAVE FAIL step=1 FSR=0x01000100`：无 OPER/SQER/PROER、D0BUSY 从未置位、三次重试后字节仍非 0xFF = 擦除命令被静默丢弃

## [1.0.1] - 2026-10-01

### 修复
- 标定存储写序列首轮加固：擦除前补 `clearStatus`（FSR 错误锁存会让 FMU 静默拒擦而逐页编程照常执行）、每步擦/编后查 `FSR.OPER/PROER/SQER`、擦除独立 2 s 超时预算、失败打 `CALSAVE FAIL step/FSR=` 定位行
- （本轮误引入两项后由 1.0.2/1.0.3 处置：Safety ENDINIT 包装、小端装页字节序）

## [1.0.0] - 2026-10-01

首个稳定版，对齐 `mw/app_version.h` 1.0.0。

### 新增
- 按需版本信标：DIAG 0x53/0x24 请求即答 EVT 0x24/0x25（PROTO 消费 CID_DIAG，app_ver 请求标志 CPU0 同轮取走，不再等 5 s 周期；配合 S3 About 页 tap 刷新）

## [0.2.2] - 2026-10-01

### 新增
- 固件版本号落地：`mw/app_version.h` 唯一真源，产物名携带版本，启动横幅打印（APPFW tc275_car vX.Y.Z）
- SBL 版本直读：固定地址 0x80007E00 + magic 校验；版本号上链路（遥测 fwVer + EVT 0x24/0x25 版本信标）
- SBL 接入：槽 A 构建 + OTA 接收接入 + 自检确认（doc/24，整包烧录 merge_hex.py）
- F02 速度闭环 servo + 0x70 台架自动判向
- F04 电池电压采集：VADC G0 CH4 采样经 xcore 出遥测
- 34 号标定/DPT：结果回传、DFlash 记录、0x71~0x74 与 EVT 0x22/0x23
- CPU0/CPU1 硬件看门狗，栈溢出钩子改为上报后断喂复位

### 变更
- SCons 命令行构建入库（SConstruct + site_scons，与 tc275_sbl 同源副本），构建统一 SCons
- 工程名 myCar → tc275_car，统一仓库名/工程名/产物名
- 统一 LF 换行（.gitattributes），保证 contracts 跨平台逐字节校验一致
- SDD 升至 V1.8（同步 F02 闭环与看门狗）

### 修复
- 编码器刻度按实物更正：13 PPR、1061.27 计数/轮转、默认轮径 48
- 测速 8 ms 窗由中值改均值；0x70 判向标定改为按当前符号取反
- 调试 launch 的 SVD 路径对齐本机 ADS 1.10.40

## [0.2.1] - 2026-09-27

### 新增
- 车速显示（F03）：遥测填 vMeasL/R（mm/s）+ odoSession 真值，SPD= 台架行，线格式不变
- CI：GitHub Actions——主机单测与 tag 驱动发布

### 修复
- 链路泵简化：单读快照、LOST 只看 ALIVE、移除重同步活锁

## [0.2.0] - 2026-09-27

板间链路换向 SPI/SF 代码闭合。

### 新增
- CPU2 量产 SPI 链路（QSPI3 主机 + SF 帧）与 USE_SPI_LINK 双构建
- CPU1 霍尔编码器 ×4 测速（GTM TIM0 TIEM 边沿中断 + 软件正交）
- 0x50 DRIVE 摇杆混控为轮速百分比，驾驶命令兼作心跳
- CPU2 链路诊断输出 LINK_diagPrint 与 XCORE_logu

### 变更
- Wi-Fi 驱动由 esp8266 模块换成 CPU2 wifi_at，AT UART 移至 P11.12/P11.10
- 工程目录按 SDD §3.4 重排，doc 体系以 SDD 为唯一设计基准重建

### 修复
- SPI 契约逐行对齐已烧录 C6 固件；相位改 trailing 边沿采样并降慢 CS/数据沿速率
- HTTP keep-alive 回复固定到 +IPD 来源链路

[未发布]: https://github.com/lilicqyu-ship-it/tc275_car/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/lilicqyu-ship-it/tc275_car/compare/v0.2.2...v1.0.0
[0.2.2]: https://github.com/lilicqyu-ship-it/tc275_car/compare/v0.2.1...v0.2.2
[0.2.1]: https://github.com/lilicqyu-ship-it/tc275_car/compare/v0.2.0...v0.2.1
[0.2.0]: https://github.com/lilicqyu-ship-it/tc275_car/releases/tag/v0.2.0
