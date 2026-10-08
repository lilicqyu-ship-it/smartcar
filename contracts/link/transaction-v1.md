# TC275 ↔ C6 事务与传感器验收协议

版本：设计基线 1.0，2026-10-04。状态：**TC275 v1.3.0 已实现；C6/iOS 接入尚未实现，端到端门禁未通过**。
本文件是 21 号软件设计基线新增事务层的跨工程契约；现行 SPI 电气、SF 编解码及驾驶语义仍由 22 号文档和源码定义。
验收入口为 **iOS Remote → 已鉴权 C6 WebSocket → SPI → TC275**。串口仅用于开发日志。

## 1. 分层与职责

| 层 | 规则 |
|---|---|
| SPI | TC275 QSPI3 主机、C6 SPI2 半双工从机，沿用现有寄存器、DMA、IRQ 与 ALIVE 判活 |
| SF | 沿用 VER=1、8 B 头、CRC16-CCITT-FALSE、CRC 高字节在前、4 B 对齐；FLAGS=0 |
| 现有业务 | 驾驶、急停、38 B 遥测、OTA、已实现标定命令保持原字节与语义 |
| 新事务层 | DIAG 0x53 的 sub=0x60；独立 schema=1、会话、请求 ID、回执、查询、结果与应用分片 |
| C6 | 鉴权、单控制者仲裁、会话管理、转发、流量控制与链路状态；不伪造 TC275 执行成功 |
| TC275 CPU2 | 校验 SF、通信收发、可观察的队列拒绝；不得执行传感器验收或 flash 写入 |
| TC275 CPU0 | 事务去重、参数校验、有限时任务、结果缓存；读取跨核快照 |
| TC275 CPU1 | 电机与 IMU 实时属主；验收读取快照，不跨核操作 IMU 硬件 |
| iOS | 验收流程、参考值/姿态提示、图表、记录导出、明确的通过/失败/无法判定结果 |

SF SEQ 只负责传输窗口，**不能**用作请求 ID，也不能把 SF 接收成功当成业务成功。
新命令不得依赖当前大端/小端假设：所有业务多字节字段显式小端、有符号数为补码，禁止直接发送 C struct。

## 2. 分配与兼容

新请求使用 `TYPE=CMD(0x01), CID=DIAG(0x03)`，SF payload 为 `53 + request[16]`，LEN=17。
CPU0 收到剥离 0x53 后的 16 B，适配现有 `PROTO_MAX_PAYLOAD=16`，不增加跨核命令尺寸。

新回包均为 `TYPE=EVT(0x05)`：CID **0x28=事务控制/结果**，**0x29=传感器数据**；SF payload 为 18～32 B。
适配现有跨核事件上限 32 B。现有 EVT 0x22～0x27 不变。
这些是本设计的新分配，必须在实现阶段同步两端常量、C6 桥接与 iOS 解析器。
禁止占用已有 0x75～0x79：这些命令已有历史预留含义，即使目前 TC275 未实现也不能重用。
2026-10-08：IMU 安装轴向标定分配独立 DPT op `0x7A`（5 B：axis i8×3 +
trackMm u16LE），不占用上述保留码；其持久化回显扩展既有 EVT `0x23`，
不改变本诊断事务的 EVT `0x28/0x29` 分配。

新能力必须通过 CAPS 显式宣告。已知旧固件不支持时，C6 标记 `diagSupported=false`，iOS 显示“当前固件不支持验收”；未确认固件的 HELLO 超时显示“验收协议协商失败”，不能把拥塞推断为不支持。驾驶仍按现有能力运行。
事务超时不得让正常 SPI 链路显示 link down；分别暴露物理链路、事务会话和传感器健康状态。
外层 SF VER、事务 schema、固件版本、验收判据版本是四个独立版本。
schema 主版本不匹配必须拒绝；同一主版本只允许新增 opcode、记录种类、能力位与可跳过 TLV，禁止更改已分配字段的单位/符号/含义。

## 3. 请求：固定 16 B

| 偏移 | 类型 | 字段 | 约束 |
|---|---|---|---|
| 0 | u8 | sub | 固定 0x60 |
| 1 | u8 | schema | 固定 1 |
| 2 | u8 | opcode | 下表 |
| 3 | u8 | flags | v1 必须 0；未知位拒绝 |
| 4 | u32 | session_id | 非零，由 C6 生成；HELLO 提议值，其余须与 TC275 当前会话一致 |
| 8 | u16 | request_id | 非零，会话内不复用；C6 分配 |
| 10 | u8[6] | args | 按 opcode 精确解码；未使用字节必须 0 |

| opcode | 名称 | args[0..5] |
|---|---|---|
| 0x01 | HELLO | min_schema:u8, max_schema:u8, reserved[4]=0；v1 两个版本均为 1 |
| 0x02 | GET_CAPS | 全 0 |
| 0x03 | GET_STATUS | target_request:u16, scope:u8, reserved[3]=0；scope=0 当前状态且 target=0，scope=1 查询指定任务 |
| 0x04 | CANCEL | target_request:u16, reason:u8, reserved[3]=0；reason=0 用户取消、1 客户端退出 |
| 0x05 | KEEPALIVE | 全 0；独立诊断租约，不喂驾驶心跳 |
| 0x10 | CAPTURE | sensors:u8, imu_hz:u8, duration_ms:u16, reserved[2]=0 |
| 0x11 | IMU_STATIC_CHECK | sensors=1:u8, imu_hz:u8, duration_ms:u16, face:u8, policy_id:u8 |
| 0x12 | TOF_REFERENCE_CHECK | reference_mm:u16, tolerance_mm:u16, duration_s:u8, zone:u8 |

sensor mask：bit0=IMU、bit1=ToF，其他位必须 0，CAPTURE 必须非零。
IMU 请求频率 10～100 Hz 且必须为能力中支持的值；只采 ToF 时 imu_hz=0。
CAPTURE/IMU_STATIC_CHECK 时长 100～60000 ms。ToF 帧率为实际驱动配置，由 ACK/CAPS 明确返回，不把 IMU 频率套在 ToF 上。
TOF_REFERENCE_CHECK 距离 20～4000 mm、容差 1～1000 mm、时长 1～60 s、zone 必须小于当前分辨率。
IMU face：0=仅检查静止与模长，1～6=+X/-X/+Y/-Y/+Z/-Z 朝上；这是传感器原生轴，不假定安装轴已标定。
policy_id 必须存在于 CAPS 中；0 表示只采集/统计、不输出精度通过结论。
SNAPSHOT 后续可新增独立 opcode，v1 不用 duration=0 的隐式特例。

## 4. 回包、关联和分片

| 偏移 | 类型 | 字段 |
|---|---|---|
| 0 | u8 | schema=1 |
| 1 | u8 | kind |
| 2 | u8 | code |
| 3 | u8 | flags，v1=0 |
| 4 | u32 | session_id，回显所属会话 |
| 8 | u16 | request_id，回显所属请求；采集记录始终使用启动请求 ID |
| 10 | u16 | record_seq，单个请求内逻辑记录编号，0 起递增 |
| 12 | u32 | stamp_ms，TC275 单调时基，采样记录为采样时刻、其他为生成时刻 |
| 16 | u8 | fragment_index，0 起 |
| 17 | u8 | fragment_count，1～32 |
| 18.. | bytes | body，单片最多 14 B |

kind：0x01 ACK、0x02 PROGRESS、0x03 RESULT、0x04 CAPS、0x05 STATUS、0x10 IMU、0x11 TOF。
ACK 表示已完成参数/权限/队列校验并接纳任务，**不表示验收通过**；任务必须有 RESULT。
HELLO、GET_CAPS、GET_STATUS、KEEPALIVE、CANCEL 为短事务，也必须有 RESULT；可跳过中间 ACK，直接给终态。
GET_CAPS 的 CAPS、GET_STATUS 的 STATUS 与最终 RESULT 分属不同 record_seq。
ACK/PROGRESS/RESULT/CAPS/STATUS 使用 TLV；传感器数据使用下节固定格式。

分片使用事务 body 的 index/count，**不使用或改写 SF FLAGS**，避免与现有传输分片预留冲突。
非末片 body 必须恰为 14 B，末片 1～14 B；空 body 允许单片长度 0。
同一逻辑记录的 schema/kind/code/flags/session/request/seq/stamp/count 必须一致。
接收者按 `(session,request,record_seq)` 重组；允许片乱序、相同片重复；同 index 内容不同则记录损坏。
重组上限 448 B、每会话最多 4 条未完成记录、超时 500 ms。超时/超限计入缺失，不允许拼接不同 ToF 帧。
record_seq 在一个请求内不得回绕；60 s 采集和规定频率保证不达到 65536，扩展长任务必须另起请求。
末帧 CRC 正确只能证明该片正确，不能证明整个验收记录完整。

## 5. 数据与 TLV

IMU 逻辑 body 为 20 B：`acc_mg:i16[3], gyro_mdps:i32[3], temp_centiC:i16`，轴序 X/Y/Z，分成 2 片。
sample stamp 必须由 CPU1 在成功采样发布时记录；禁止用 CPU0 读取时刻替代。
静态 WHOAMI、量程、配置 ODR、实际发布频率、驱动错误计数、轴标定状态通过 CAPS/STATUS 提供。

ToF body：`resolution:u8, zone_count:u8, alive:u8, reserved:u8=0`，之后 zone_count 条
`distance_mm:i16, status:u8, targets:u8`，按驱动原生 zone 编号递增。
v1 支持 resolution=4/zone_count=16（68 B，5 片）及 resolution=8/zone_count=64（260 B，19 片），必须成对匹配。
每条 ToF 记录取自同一个完整快照，stamp 是该帧获取时刻。status/targets 保留原值，不能把无目标改成“距离=0 的障碍”。
有效规则、ROI 和算法版本须记录在验收报告；默认精度统计只使用 status=5 且 targets>0，其他状态单独统计。
IMU/ToF 的 record_seq 是输出逻辑记录编号，不是驱动原始帧序号；驱动 seq/缺失发布数另放 STATUS。

控制类 TLV 格式：`tag:u8, len:u8, value[len]`。数值小端；未知 tag 按 len 跳过并保存原始值。
TLV 不可重复；越界/固定 tag 长度错误则整个记录无效。已知必需 tag 缺失则结果无法判定。
单个 value 最长 255 B；大块资料采用新增记录类型，不以重复 tag 隐式拼接。

| tag | 名称 / 类型 | 使用位置 |
|---|---|---|
| 0x01 | capability_bits:u32 | CAPS 必需；bit0 capture，1 IMU static，2 ToF reference，3 cancel，4 query，5 result retention |
| 0x02 | app_version:u32 | CAPS 必需，沿用现有版本打包规则 |
| 0x03 | sensor_mask:u8 | CAPS/ACK 必需 |
| 0x04 | max_duration_ms:u16 | CAPS 必需 |
| 0x05 | imu_rates_hz:bytes | CAPS 必需，去重升序列表 |
| 0x06 | tof_config:{resolution:u8,hz:u8} | CAPS 必需 |
| 0x07 | policy_versions:{id:u8,version:u16}[] | CAPS；列表长度必须为 3 的倍数 |
| 0x08 | granted_config:{imu_hz:u8,tof_hz:u8,duration_ms:u16} | 启动 ACK 必需；不能静默降频/缩时，无法满足时拒绝 |
| 0x09 | task_state:u8 | STATUS/RESULT 必需；0 idle，1 running，2 completed，3 canceled，4 failed，5 unknown |
| 0x0A | sensor_health_bits:u16 | STATUS/长任务 RESULT 必需；bit0 IMU alive，1 IMU fresh，2 ToF alive，3 ToF fresh |
| 0x0B | imu_records:u32 | 采集 RESULT 必需，TC275 生成的逻辑记录数，包括未成功入队的记录 |
| 0x0C | tof_records:u32 | 同上 |
| 0x0D | dropped_fragments:u32 | 长任务 RESULT 必需，TC275 已知的出站数据丢片数与缺少的采样槽位对应片数 |
| 0x0E | elapsed_ms:u32 | 长任务 RESULT 必需 |
| 0x0F | verdict:u8 | 检查 RESULT 必需；0 未评定，1 通过，2 失败，3 无法判定 |
| 0x10 | policy_version:u16 | 检查 RESULT 必需；policy_id 随报告记录，不用固件版本代替判据版本 |
| 0x11 | target_request:u16 | CANCEL/任务查询必需，指向原任务 |
| 0x12 | imu_source_seq:u32 | STATUS 可选 |
| 0x13 | tof_source_seq:u32 | STATUS 可选 |
| 0x14 | driver_errors:{imu:u32,tof:u32} | STATUS/长任务 RESULT 必需 |
| 0x15 | completed_record_seq:u16 | 长任务 RESULT 必需，终态自身的逻辑序号；已完成任务 STATUS 可回显原终态序号 |
| 0x16 | reference:{reference_mm:u16,tolerance_mm:u16,zone:u8,policy_id:u8} | ToF 检查 RESULT 必需 |
| 0x17 | tof_statistics:{total:u32,valid:u32,mean_mm:i32,std_mm:u32} | ToF 检查 RESULT 必需；显示值取整数，阈值比较不取整 |
| 0x18 | imu_face:u8 | IMU static RESULT 必需 |
| 0x19 | imu_info:{expected_who:u8,observed_who:u8,odr_code:u8,xl_fs_code:u8,gy_fs_code:u8,body_axis_calibrated:u8,nominal_publish_ms:u16} | CAPS/STATUS 必需；LSM6DSV16BX 配置码，当前车体轴未标定 |

CAPS 未报告的功能不得调用；健康位表示检测状态，不能直接当作精度合格。
接纳前拒绝的 RESULT 不代表开始过长任务，只要求关联头、非 OK code、task_state=failed、verdict=inconclusive；不要求采集/统计 TLV。
future tags 0x1A～0xEF 为统一登记区，0xF0～0xFF 为厂商试验区，不进入量产验收必需字段。
TC275 v1.3.0：IMU static 只支持 policy_id=0（采集、未评定），非零判据拒绝；CAPS 的 policy 1/version 1 只属于 ToF reference。
ToF policy 1 要求静止参考板、指定单区域至少 5 个有效帧、有效率至少 80%、均值偏差与总体标准差都不超过请求容差。
统计使用 status=5、targets>0、distance>0；丢片/新鲜度不满足、驱动错误增加或样本不足不能 PASS。
完整实现与实测边界见 [39](../../tc275_car/doc/30-tc275/39-diagnostic-transactions.md)。健康位不能直接当作精度 PASS。

## 6. 错误码与状态机

code：0 OK、1 BAD_LENGTH、2 UNSUPPORTED_SCHEMA、3 UNSUPPORTED_OPCODE、4 BAD_ARGUMENT、
5 BUSY、6 NOT_READY、7 STALE_SENSOR、8 ID_CONFLICT、9 SESSION_LOST、10 CANCELED、
11 LINK_LOST、12 BUFFER_OVERFLOW、13 INTERNAL_ERROR、14 RESULT_EXPIRED、15 MOTION_ACTIVE、16 UNAUTHORIZED。
code 描述事务是否完成；verdict 描述验收结论。完成测量但偏差超标为 code=OK/verdict=FAIL。
不能可信评定的数据丢失/传感器失联为非 OK 或 verdict=INCONCLUSIVE，绝不能 PASS。
SF CRC/格式错误没有可信 request_id：丢弃并计数，不能猜 ID 发送执行回执。
schema/opcode/长度错误只有在固定关联字段可完整读取时才返回拒绝结果，否则由 C6 超时分类。

长任务：`idle → accepted → running → completed/canceled/failed`；每个启动请求最多一次执行。
短事务没有 running 阶段。CANCEL 自身 RESULT 和原任务 CANCELED RESULT 分别关联各自请求。
重复 CANCEL 返回之前结果；对已完成任务取消不修改原结果，CANCEL 返回 OK 并携带 target 状态。
单会话只允许一个长任务；状态查询、取消、KEEPALIVE 不占长任务槽。
开始检查前要求运动请求、实际轮速、点动/判向标定状态均静止；期间检测运动操作立即终止检查并返回 MOTION_ACTIVE。
验收不喂驾驶心跳、不发运动指令、不自动写标定或改变已校准参数；急停随时有效。
运动与验收的仲裁必须落在 TC275，不能只靠 iOS 隐藏按钮。将来标定写入使用独立能力与 prepare/commit 操作，不复用采集命令。

## 7. 会话、重试与断连

C6 为控制者生成非零随机 session_id，HELLO 成功后才能发送事务；身份鉴权使用现有配对/控制权限，session_id 本身不是授权凭据。
TC275 复位后会话为空，除 HELLO 外拒绝旧会话；C6 复位后生成新 session_id，不能恢复执行旧待发任务。
活动会话中同 session 的相同 HELLO 可以重放回包；不同 session 的 HELLO 在旧租约有效时拒绝 BUSY。
TC275 对无会话 HELLO 接纳提议值。C6 若发现 SESSION_LOST，清空旧待发队列、将旧操作标记 UNKNOWN，重新协商并使用新 session_id。
32 位 session 是有限命名空间：C6 不复用当前及缓存会话 ID，随机碰撞重选；它不构成跨掉电永久唯一标识。

request_id 在一个会话内单调分配，KEEPALIVE 也使用新 ID；耗尽前结束会话、待长任务结束后重新 HELLO，禁止回绕复用。
去重键为 `(session_id,request_id)`：相同请求全部 16 B 相同则重放已缓存 ACK/RESULT，不重启任务；内容不同返回 ID_CONFLICT。
TC275 至少保留最近 16 个短事务结果和当前/最近 4 个长任务终态，长任务终态保留至少 30 s。
即使缓存淘汰，接收方也保存请求高水位：低于高水位且查无缓存的请求返回 RESULT_EXPIRED，不再次执行。
C6 保证同一时间只提交一个新的短事务，按 ID 顺序提交；长任务接纳后可以穿插新的状态/租约/取消请求。
仅重传当前尚未确认的请求使用原 ID；停止重传后才能递交下一条新请求，避免乱序触发高水位拒绝。

接纳回包目标超时 500 ms；C6 最多 2 次重试（总发送 3 次），必须原样重发应用请求，SF SEQ 每次由正常编码器更新。
超时后先 GET_STATUS 查询已有启动请求，不用新 ID 重启采集。查询也无法确认则 UI 显示 UNKNOWN，不能写成成功或失败测量。
已 ACK 长任务绝不自动重新启动；执行超时为协商 duration+1000 ms，随后查询一次并必要时 CANCEL。
TC275 每 500 ms 产生进度，RESULT 必须保留到查询缓存，不能因为出站队列满丢掉终态。

TC275 会话租约 1000 ms；C6 每 250 ms KEEPALIVE，只有拥有该会话的鉴权 iOS 连接及其应用心跳有效时才续租。
iOS 应用心跳每 500 ms，C6 1000 ms 未收到则停止续租；因此客户端静默冻结的终止上限是 2000 ms。
SPI ALIVE 的现有 500 ms 判活不变；链路失联立即终止当前检查。仅短瞬断且会话仍有效可查询终态，不恢复旧采集。
iOS 正常退出验收主动 CANCEL；异常断网走租约兜底。重连且原会话租约仍有效时，可 GET_STATUS 查询原请求。
会话已过期/两板复位后，新会话不得用旧 request_id 查询 TC275；只能取 C6 已收到且仍缓存的旧终态报告，没有可信终态则显示无法判定。
租约过期先结束任务并保存旧会话终态，再清空当前会话；旧终态不迁移成新会话下的成功结果。
协议承诺的是**同一有效会话内的有界去重执行**，不能宣称跨掉电“恰好执行一次”。

## 8. 流量、优先级与完整性

优先级：急停/最新驾驶目标与心跳 > 关键故障/事务 ACK、终态 > 周期遥测 > 进度 > 原始传感器流 > 开发日志。
队列只能预留现有容量并采用有界操作，禁止为了诊断阻塞 CPU1 或 CPU0 等待 SPI 空闲。
接受任务前检查终态缓存及控制回包槽位；资源不足返回 BUSY。CPU2 入站满载拒绝必须关联请求，不能静默入队失败。
控制回包采用独立待发状态和有界重试；数据帧拥塞允许丢失并计数，过载终止检查，不能无限扩展队列。
每次 CPU0 10 ms tick 最多产生 4 个诊断数据片（控制回包不计入此额度）；CPU2 按优先级发送，原始采集不得挤占驾驶队列。
64 区 15 Hz ToF 需要 285 片/s，IMU 50 Hz 需要 100 片/s，共 385 片/s；因此默认双传感器 50 Hz，100 Hz IMU 仅限 4×4 或 IMU 单独模式。
CAPS 与 ACK 必须报告实际受支持组合；不能把理论 SPI 带宽等同于可用 CPU0/队列预算。
32 B SF payload 的数据片占 44 B 对齐长度，400 片/s 约 17.6 kB/s，另加遥测/控制/寄存器/半双工开销。
该数值是设计预算；1 MHz 物理时钟继续沿用，调度实测达标前不得宣称吞吐或时延门禁通过。

每个产生的逻辑记录都分配 record_seq，包括后来丢失的记录。C6/iOS 结合序号、分片与 RESULT 计数检查完整性。
任何缺片、序号缺口、重复内容冲突、生成数与接收数不符，验收报告必须含缺失量与 INCONCLUSIVE。
终态的已知 drop=0 不等于 iOS 收齐，iOS 仍要核对接收完整性；串口日志节流不影响验收数据流。
控制优先级可让 RESULT 先于已入队的数据片到达；iOS 须等待至多 500 ms 的重组宽限并核对终态计数，再给出完整性结论，不能收到 RESULT 就立即误判缺片或 PASS。
TC275 只采集接纳后新发布的样本；IMU 缺少请求频率要求的采样槽位计入丢片与序号缺口，不能把短时冻结解释成完整采集。
stamp_ms 使用 TC275 时基，32 位回绕用无符号差处理；不与 iPhone 墙钟直接相减。
跨会话禁止拼接时间轴；iOS 另存接收时间用于通信时延观察。

## 9. iOS ↔ C6 边界

iOS 使用现有鉴权 WebSocket，新增文本提交：
`{"t":"diag_req","schema":1,"operation":"UUID","opcode":16,"args_hex":"033210270000","retry":false}`。
operation 是一次用户操作的 client_operation_id（规范小写 UUID），args_hex 必须为 6 B 的 12 个小写十六进制字符。
C6 独占分配 session/request，返回
`{"t":"diag_bound","schema":1,"operation":"UUID","session":305419896,"request":2}`，随后编码 proto v2 CMD=0x53、16 B DATA，再走现有 SF 适配。
diag_bound 只表示 C6 已建立关联，不能当作 TC275 ACK。
重试必须原样提交 operation/opcode/args，retry=true；已存在的 UUID 返回同一绑定键，不重复启动。
同 UUID 内容不同返回 ID_CONFLICT；retry=true 但映射不存在返回 UNKNOWN，禁止新建任务。
C6 在鉴权控制者范围内保留当前操作和至少 32 条关联，终态关联至少保留 30 s；重启后映射为空，iOS 不能自动以 retry=false 重启旧操作。
C6 本地拒绝使用 `{"t":"diag_error","schema":1,"operation":"UUID","origin":"c6","reason":"UNKNOWN"}`，reason 为 UNAUTHORIZED/BUSY/BAD_ARGUMENT/ID_CONFLICT/UNKNOWN/LINK_DOWN/UNSUPPORTED。
重连查询 C6 缓存使用 `{"t":"diag_lookup","schema":1,"operation":"UUID"}`；身份须与原控制者匹配，只回放已保存的 diag_bound/RESULT 或返回 UNKNOWN，不触发设备执行。
TC275 拒绝仍通过原始 RESULT 返回，不混淆本地错误与设备执行结果。
新事务仅使用结构化入口；原有驾驶等二进制接口保持不变。禁止从客户端二进制帧直接提交 sub=0x60，从而避免两处分配 ID 或越权访问会话。

回传为 `{ "t":"diag", "schema":1, "operation":"UUID", "cid":40或41, "payload_hex":"完整SF载荷的小写十六进制" }`。
operation 由 C6 的会话/请求关联映射补充；无法匹配的旧会话事件只计数记录，不向当前验收操作转发。
C6 必须保留完整载荷，不得只输出现有 `{t:evt,cid,n}`；验证长度后转发，字段解析可作为附加显示而不能替代原始内容。
hex 长度须为实际载荷长度的两倍，上限 64 字符；没有接收者也不能把转发当作验收已收到。
每个报告记录 client_operation_id、session/request、两板固件版本、schema、判据版本、参数、完整性计数、统计量、结论与原因。
用户界面区分“接纳/执行/等待数据/通过/失败/取消/无法判定”；断线前显示通过后不得被陈旧新会话数据覆盖。

## 10. 实施与验收门禁

TC275 编码阶段已落地；本批不变更驾驶控制，不声称 C6/iOS 验收功能或端到端门禁已完成。
实施顺序：共享 codec/常量和双端黄金向量 → 会话/队列/回执 → 传感器任务 → C6 完整转发 → iOS 验收页面及报告。
每一步均同步文档和副本校验；未知操作必须有明确拒绝，未知事件必须可完整传输。

必须验证：大小端黄金向量、CRC 错误/半包/连续帧、精确长度/保留位、未知版本/操作、相同 ID 重试与内容冲突、队列满、丢 ACK、丢 RESULT 后查询、失联租约、两板分别复位、请求 ID 耗尽、时钟回绕、分片乱序/重复/缺失/冲突、4×4/8×8、同时驾驶拒绝、急停、旧固件兼容、iOS 后台/重连。
台架压力门禁：1 MHz，双传感器采集+正常 50 Hz 遥测连续 30 min，控制请求接纳延迟 P99≤100 ms；故障注入不能误 PASS；急停响应不得劣于现有基线。
100 ms 是设计验收目标，不是当前实测保证。物理传感器精度另用工装/参考尺/六面姿态验证，不由通信 CRC 代替。

机器可读登记见 [transaction-v1.json](transaction-v1.json)，请求黄金向量也登记于其中。
