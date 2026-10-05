# matlab_motor_model — 智能车电机算法 MATLAB 建模

对 `tc275_car` CPU1 上 1 kHz 电机闭环算法的 MATLAB/Simulink 建模:控制律与
固件 **逐位一致**(编译真实 `servo.c` 生成金标向量验证),被控对象为
MG310 直流电机 + TB6612 + 48 mm 轮的物理模型。用于整定预扫、控制行为研究
和回归参照,代替"上车试参数"。

## 快速开始

仓库根目录的 justfile 已收录全部入口(推荐):

```bash
just matlab-validate    # 固件等价性(金标回放)
just matlab-sim         # 主仿真:阶跃/斜坡/扰动/断线/急停,图到 results/
just matlab-lowspeed    # 低速量化 + 静摩擦极限环
just matlab-tune        # Kp/Ki 扫描
just matlab-all         # 上面四个一键跑完
just matlab-simulink    # 重建并验证 Simulink 模型
just matlab-golden      # 改固件 servo.c/h 后重新生成金标(再跑 validate)
```

等价的直接调用(无 just 时):

```bash
cd matlab_motor_model
/Applications/MATLAB_R2024a.app/bin/matlab -batch "validate_fw_equivalence"
/Applications/MATLAB_R2024a.app/bin/matlab -batch "sim_closed_loop"
/Applications/MATLAB_R2024a.app/bin/matlab -batch "sim_low_speed_quantization"
/Applications/MATLAB_R2024a.app/bin/matlab -batch "tune_pid_grid"
/Applications/MATLAB_R2024a.app/bin/matlab -batch "build_simulink_model"
```

图形输出在 `results/`。也可以在 MATLAB 桌面里逐个运行同名脚本。

## 目录结构

| 文件 | 作用 | 固件对应 |
|---|---|---|
| `motor_params.m` | 全部参数:固件常量 1:1 + 电机物理参数(占位) | servo.h / motor_algo.h / encoder.h |
| `fw/fw_step_toward.m` | 目标斜率限制 ±2/ms | `MOTOR_ALGO_stepToward()` |
| `fw/fw_servo_update.m` | 前馈+PI 伺服(single 精度逐位复刻) | `SERVO_update()` |
| `fw/fw_encoder_measure.m` | 8 ms 窗口均值 → mm/s → pct*10、alive 判定 | `ENCODER_task()` 测量链 |
| `fw/motor_plant_step.m` | 电机物理模型 + TB6612 三模式 + 编码器计数 | (被控对象,固件无对应) |
| `fw/sim_run_side.m` | 单侧 1 kHz 主循环(与 `MOTOR_ALGO_task` 同构) | `MOTOR_ALGO_task()` |
| `validate_fw_equivalence.m` | 金标回放 + 测量链不变量 | — |
| `sim_closed_loop.m` | 阶跃/斜坡/扰动/编码器断线/急停 | — |
| `sim_low_speed_quantization.m` | 低速量化楼梯 + 静摩擦极限环 | — |
| `tune_pid_grid.m` | Kp/Ki 网格扫描(阶跃/扰动指标分开) | doc 21 SS15.3 台架整定 |
| `build_simulink_model.m` | 程序化搭建 `motor_algo_sim.slx` 并交叉验证 | — |
| `tools/gen_golden.c` | 编译真实 servo.c 生成金标 CSV | `tc275_car/rt/servo.c` |
| `tools/stub/Ifx_Types.h` | 金标编译用的最小类型桩 | — |
| `tools/golden_servo.csv` | 金标向量(118 行 SERVO_update + 5 行 stepToward) | — |

## 1. 等价性:模型 ↔ 固件

`SERVO_update` 的 MATLAB 移植不是"照着写的另一份实现",而是与 **编译出来的
固件代码逐位比对过**的:

- `tools/gen_golden.c` 以 `cc -ffp-contract=off` 直接编译
  `tc275_car/rt/servo.c`,跑过全部分支(积分充放/限幅、误差死区边界、
  输出死区、开环回退、reset、双侧独立性、负向饱和),输出每步的占空比和
  积分项 float32 **位模式**;
- `validate_fw_equivalence.m` 用 MATLAB 移植版回放金标:118 行
  SERVO_update + 5 行 stepToward 全部一致(IEEE single,零容差);
- 编码器测量链按第一性公式独立复算(窗口均值、截断、±1000 饱和、
  500 ms alive 窗口、71.06/8.88 mm·s⁻¹ 分辨率),9 项不变量全部通过。

改了 `servo.h` 的增益/死区后:重编译金标(`gen_golden.c` 文件头有命令)→
重跑 `validate_fw_equivalence`,模型即同步到新固件。

## 2. 控制律(percent*10 域,1 kHz)

```
目标斜率限制:   |target[k]-target[k-1]| ≤ 2            (0..1000 需 0.5 s)
误差:          e = target - meas
积分(死区外):  |e| > 3 时 integ += 0.01·e,限幅 ±300
输出:          u = 1.0·target + 0.8·e + integ,限幅 ±1000
输出死区:      |u| < 5 → 0(TB6612 换向脚防抖)
开环回退:      测量无效 → duty = target,积分清零(不冻结)
测量链:        8 ms 窗口均值 → mm/s(截断)→ pct(整数除法,±1000)
```

## 3. 被控对象模型

电气 `L di/dt = V−Ri−Keω`,机械 `J dω/dt = Kt i − Bω − τc·sgnω − τload`,
线性部分 ZOH 精确离散(`expm`,不依赖工具箱),库仑/静摩擦显式处理;
`v = ω/(20.409)·r_wheel`;编码器计数按 52 counts/电机转的确定性楼梯生成,
再走固件同款 8 ms 窗口。**同侧两台电机同占空比、平分负载,与"每台电机
参数"的单机方程逐项相同,故按单机建模**。

标定点:V_nom=12 V 下 100% 占空比 ≈ 1050 mm/s 空载,即固件 `FF_GAIN=1.0`
的"100% duty ≈ 满量程"假设。

### ⚠ 占位参数(上车台架辨识后替换 `motor_params.m`)

`iNoLoad=0.4 A`、`iStall=3 A`(→R)、`L=0.25 mH`、`τc=0.0035 N·m`、
`Jrotor=1.5e-6`、整车 1.5 kg、V_nom=12 V。控制律部分与这些无关;响应
数值(上升/整定时间、静摩擦阈值 ~87 pct*10 等)会随实车参数变化。

## 4. 仿真结论摘要(占位参数下)

| 场景 | 结果 |
|---|---|
| 阶跃 500(闭环) | 上升 244 ms,超调 3.0%,稳态误差 −2.9,duty 抖动 std 4.5 |
| 阶跃 500(开环对照) | 稳态停在 ~385(工厂增益 ~0.77)—— I 项的必要性实证 |
| 斜坡跟踪 | 斜率限制整形后最大滞后 ~99 pct*10 |
| 负载扰动 0.010 N·m | 速降仅 15 pct*10,未越出 ±2% 带 |
| 急停 | 高阻滑行 60 ms vs 短接刹车 58 ms(占位摩擦下差距小) |
| 低速测量 | 窗口均值分辨率 8.9 mm/s;抖动 std ≈ 8(与分辨率吻合) |
| 低速闭环(3 mm/s 目标) | duty 在 81~132 间振荡的静摩擦极限环 |
| Kp/Ki 扫描 | 现值 (0.8, 0.01) 在"超调≤5%"约束下近优;Ki 提到 0.02–0.08 抗扰快 4 倍但阶跃超调 6–17% |

## 5. 模型发现的固件事实(建议复核)

1. **编码器死亡后的 500 ms 盲跑**:`ENC_ALIVE_WINDOW_MS=500` 未过期时
   `alive` 仍真、`meas` 已冻结为 0,闭环把 duty 打满 +1000(仿真中真实
   轮速冲到 ~845 mm/s)。可评估:缩短 alive 窗口,或"目标≠0 且 meas≈0
   持续 N ms"即降级开环。
2. **`MOTOR_brake` 名不副实**:TB6612 的 IN1=IN2=H 是 STANDBY(高阻),
   所以急停走的 `MOTOR_brake()` 实际是**自由滑行**;真正的短接刹车是
   `MOTOR_stop()`(IN1=IN2=L)。两者若要交换,只需对调两处引脚电平。
3. **低速 P 项放大量化噪声**:E 死区(±3)只挡住了积分项;Kp=0.8 直接
   放大 ±8.9 mm/s 的量化台阶 → 稳态 duty 抖动 std ≈ 4.5 pct*10。
4. **重锁暂态**:断线恢复后第一个测量值被 8 ms 窗口里的死期零样本拉低,
   duty 短暂冲高(纯 Kp 作用,~8 ms,无积分踢轮)——设计如此,但台架
   复现时别误认为积分问题。

## 6. Simulink 模型

`build_simulink_model.m` 由 `motor_params()` 的数值**烘焙**出三个
MATLAB Function 模块(控制律/被控对象/编码器链,代码与 .m 同源),
1 kHz 离散步长,反馈回路按固件时序放一拍 Unit Delay。已与 .m 逐拍模型
交叉验证:阶跃 500 场景峰值/稳态**逐点一致**(0.0 mm/s 差异)。
生成的 `motor_algo_sim.slx` 可直接打开加扰动/换增益做实验;改动参数后
重跑该脚本即可再生成。

## 7. 已知简化

- PWM 取平均值模型(20 kHz ≫ 1 kHz,合理);电池恒压,无内阻跌落;
- 未建模 CPU0 的 v/w → 左右混控、目标帧超时(150 ms)归零路径;
- 编码器 ISR 的 4x 解码本身不建模,从整数计数楼梯开始;
- 摩擦模型是粘性+库仑+静摩擦的常规近似,非辨识结果。
