# 39 · TC275 诊断事务与传感器采集

2026-10-04，App v1.3.0。**TC275 已实现；C6 结构化入口/完整转发和 iOS 验收页面尚未实现。**
共享字节契约见 [事务协议](../../../contracts/link/transaction-v1.md)，编解码常量真源为 `contracts/link/diag_wire.h`。
本功能仅通过 SPI DIAG 接入，COM16 仍是日志口，不新增 UART 业务通道。

## 当前支持

HELLO、CAPS、STATUS、CANCEL、KEEPALIVE、CAPTURE、IMU_STATIC_CHECK、TOF_REFERENCE_CHECK。
request 为 16 B，SF payload 为 `0x53 + request`；EVT 0x28 控制、0x29 原始数据，单片最多 32 B。
IMU 原生 X/Y/Z 使用 mg、mdps、0.01°C；ToF 保留原始 status/targets，支持编译配置下的 16/64 区。
IMU 检查当前仅支持 policy_id=0：记录指定六面姿态，采集并核对健康/新鲜度，verdict 始终为未评定。
**IMU 精度/零偏/六面标定验收尚未实现，不能把采集完成解释为精度通过。**

ToF reference 使用工程检查 policy 1/version 1：指定单个区域与参考距离，至少 5 个有效帧、有效率至少 80%；均值偏差和总体标准差均不超过指定 tolerance_mm。
只有 status=5、targets>0、distance>0 才进入统计。驱动错误计数增加、丢片、样本不足或传感器过期均不通过验收；可靠性不足返回无法判定。
阈值比较使用 64 位精确矩，不先取整均值；报告均值/标准差为整数 mm 显示值。
这个判据适用于正对静止参考板的单区域检查，不等同于全视场/全距离/温漂的产品精度认证。
这里冻结测量判据，iOS 实现时必须提示工装距离、板面方向、区域编号和安装轴未标定状态。

## 实现边界

`app/diag_txn.[ch]`：纯 C99 状态机，固定容量、无动态分配，无硬件/FreeRTOS 依赖；处理去重、参数错误、会话租约、终态保留、TLV 和应用分片。
`mw/diag/diag_service.[ch]`：只在 CPU0 控制任务调用，读取 xcore 快照和车辆静止条件，组帧写出站队列。
CPU1 仍独占 IMU 和电机；IMU stampMs 为采样发布时的 STM 时间。
ToF 保留原有 RTOS stampMs 给融合使用，新增 sampleStampMs（STM）用于诊断，避免混用两种时基。
CPU2 每 5 ms 发布链路状态和 STM 时间。CPU0 超过 100 ms 看不到新发布，诊断按失联终止；这个诊断兜底不改现有 SPI ALIVE 500 ms 或驾驶心跳。

新增跨核队列：CPU2→CPU0 诊断请求深度 4（peek/consumed 后 pop），CPU0→CPU2 传感器事件深度 8；原来的驾驶命令深度 16、控制 EVT 深度 8 保持原值。
控制片先发，数据片后发；控制入队失败留存原片，数据入队失败终止任务并报告 OVERFLOW。
CPU0 每次最多输出 4 个控制片和 4 个数据片；每次只处理 1 个新诊断请求。
CPU2 对诊断请求满载/超长给关联 BUSY/BAD_LENGTH；传输队列也满载时回包可能发不出，但不会执行该请求，C6 必须遵守重试/查询规则。

任务启动前：robot 请求、跨核电机目标、实际 duty、实测轮速、jog duty、判向标定活动/待处理请求全部静止。
运动命令、非零速度/点动、判向标定、复位及标定记录变更先终止当前检查，再走原有命令路径。
不喂驾驶心跳、不发运动指令、不写 flash、不自动更改传感器配置。

会话租约 1000 ms；KEEPALIVE 用新 request_id 续租，重复原请求不会无限续约。
当前长任务以及最近 4 个终态保留至少 30 s，短事务最近 16 个缓存；缓存淘汰后高水位拒绝旧请求，避免重复执行。
长任务缓存未到期而全部被占用时，新任务返回 BUSY；终态即使暂时无法出队也保留在任务缓存中。
已完成任务 STATUS 查询重放不可变的原 RESULT，采集不重启。租约过期/物理链路失联清空当前会话。

## 验证与剩余工作

主机验证包含实际 engine、CPU0 adapter、CPU2 dispatcher、xcore 队列及黄金 SF 向量。
覆盖重复 ID/内容冲突、短事务缓存淘汰、任务终态保留、取消、链路失联、源数据冻结、队列满/丢片、4×4/8×8、时间回绕、ID 耗尽、超长参数和精确容差边界。
CPU0 adapter 测试使用传感器/机器人/xcore 边界桩；CPU2 dispatch 测试直接使用生产函数，主机桩不能证明真实三核时序。
CI 已加入上述测试。TASKING SCons 命令行构建可在本机运行，早期指南“只能 IDE 构建”已不适用于这台已配置许可证的环境。

当前未完成 C6/iOS 接入、整链路故障注入和压力门禁，也未执行传感器物理精度验收。
禁止在 iOS 页面仅看到普通遥测/link up 就展示“验收通过”；必须核对 capability、任务 RESULT、分片完整性和判据版本。
