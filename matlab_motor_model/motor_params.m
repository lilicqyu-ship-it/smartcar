function P = motor_params()
% MOTOR_PARAMS  智能车电机算法建模参数(固件常量 1:1 + 被控对象物理参数)
%
% 固件常量来源(tc275_car,与源码逐字对齐,注释里给出文件与宏名):
%   tc275_car/rt/servo.h       SERVO_*
%   tc275_car/rt/motor_algo.h  MOTOR_ALGO_*
%   tc275_car/rt/encoder.h     ENCODER_*
%   tc275_car/rt/encoder.c     ENC_WINDOW_MS / ENC_ALIVE_WINDOW_MS
%   tc275_car/bsp/motor.c      MOTOR_PWM_FREQUENCY
%
% 被控对象(MG310 + TB6612 + 48 mm 轮)的物理参数是"占位估计值",量纲与
% 结构真实,数值待台架辨识后替换(见 README §4)。标定点:V_nom 下
% 100% 占空比 ≈ 1050 mm/s 空载轮速,与固件的设计假设一致
% (servo.h 注释:"100% duty ~ full-scale speed",FF_GAIN=1.0)。
%
% 用法:P = motor_params();

%% ---- 控制回路(1 kHz,percent*10 域) ------------------------------------
P.Ts            = 1e-3;            % MOTOR_ALGO_PERIOD_MS = 1
P.maxStep       = 2;               % MOTOR_ALGO_MAX_STEP:每 ms 目标最多变 2(0..1000 需 0.5 s)
P.servoKp       = single(0.8);     % SERVO_KP
P.servoKi       = single(0.01);    % SERVO_KI(每 ms)
P.servoFf       = single(1.0);     % SERVO_FF_GAIN
P.eDeadband     = 3;               % SERVO_E_DEADBAND:|e|<=3 不积分(0.3% 满量程)
P.integMax      = 300;             % SERVO_INTEGRAL_MAX:积分限幅 ±30% 占空比
P.outMax        = 1000;            % SERVO_OUT_MAX:输出限幅(BSP 同值,"三层一致")
P.outDeadband   = 5;               % SERVO_OUT_DEADBAND:|u|<5 输出 0,防 TB6612 换向抖动
P.bspClamp      = 1000;            % MOTOR_setSpeed 的 ±1000 再限幅

%% ---- 编码器测量链 --------------------------------------------------------
P.encPpr        = 13;              % ENCODER_PPR:霍尔每电机转 13 脉冲
P.encGearNum    = 20409;           % ENCODER_GEAR_NUM:减速比 1:20.409
P.encGearDen    = 1000;            % ENCODER_GEAR_DEN
% C 里是整数除法:52*20409/1000 截断 = 1061(注释承认 -0.03% 偏差)
P.countsWheelRev = floor(P.encPpr * 4 * P.encGearNum / P.encGearDen);  % = 1061
P.windowMs      = 8;               % ENC_WINDOW_MS:滑动窗口均值
P.aliveWindowMs = 500;             % ENC_ALIVE_WINDOW_MS:500 ms 无边沿判死亡
P.fullScaleMmS  = 1000;            % ENCODER_FULL_SCALE_MM_S:pct*10=1000 对应的轮速
P.wheelDiaMm    = 48;              % ENCODER_WHEEL_DIA_MM
P.wheelRmm      = P.wheelDiaMm / 2;

%% ---- 被控对象:MG310 电机 + TB6612 驱动 + 电池 ---------------------------
% 侧 = 同侧 2 台电机同占空比驱动,机械对称。等价单机:用"每台电机的
% 参数"仿真一台即可(两台并联同压驱动、平分负载,与单机方程完全等价,
% 推导见 README §3)。
P.Vnom          = 12.0;            % 电池标称电压(V,3S 中点估计;encoder.c 提到推荐上限 13 V)
P.pwmHz         = 20000;           % MOTOR_PWM_FREQUENCY(20 kHz,远高于 1 kHz,取平均值模型)
P.gearRatio     = P.encGearNum / P.encGearDen;    % 20.409
P.vFullScale    = 1050;            % 标定点:V_nom 下 100% 占空比空载轮速 mm/s
P.iNoLoad       = 0.40;            % 空载电流(含减速箱搅油摩擦,A)——待辨识
P.iStall        = 3.0;             % 堵转电流(A,R = Vnom/iStall)——待辨识
P.L             = 0.25e-3;         % 绕组电感(H)——待辨识
P.tauC          = 0.0035;          % 库仑摩擦力矩(N·m,每台电机)——待辨识
P.omegaThresh   = 0.05;            % 静摩擦判定的角速度阈值(rad/s)

% 由标定点推导(不要直接改这三行,改上面的标定量):
P.wheelR_m      = P.wheelRmm * 1e-3;
P.omegaWheelNl  = (P.vFullScale * 1e-3) / P.wheelR_m;   % 空载轮角速度 rad/s(mm/s→m/s)
P.omegaMnl      = P.omegaWheelNl * P.gearRatio;         % 空载电机角速度 rad/s
P.Ke            = P.Vnom / P.omegaMnl;                  % 反电动势系数 V·s/rad
P.Kt            = P.Ke;                                 % SI 制下 Kt=Ke(N·m/A)
P.R             = P.Vnom / P.iStall;                    % 绕组电阻 Ω
P.Bvis          = P.Kt * P.iNoLoad / P.omegaMnl;        % 粘性摩擦 N·m·s/rad
% 折算到电机轴的惯量:转子 ~1.5e-6 + 车体 1/4(每轮)经减速比反折
mCar = 1.5;     % 整车 kg——待实测
Jrotor = 1.5e-6;% 单台电机转子+齿轮箱 kg·m²——待辨识
P.J             = Jrotor + (mCar * P.wheelR_m^2 / 4) / P.gearRatio^2;

% 离散化:ZOH 精确离散(expm,不依赖工具箱)。状态 x=[i; omega_m],
% 输入 u=[V_eff; tau_load_ext]。库仑摩擦在 plant 步进函数里显式处理。
A = [-P.R/P.L, -P.Ke/P.L;
      P.Kt/P.J, -P.Bvis/P.J];
Bu = [1/P.L, 0;
      0,    -1/P.J];
% 标准 ZOH 增广:[A B; 0 0] → expm(T) = [Ad Bd; 0 I]
M  = expm([A, Bu; zeros(2, 4)] * P.Ts);
P.Ad = M(1:2, 1:2);
P.Bd = M(1:2, 3:4);

%% ---- 仿真默认值 ----------------------------------------------------------
P.simTimeS      = 3.0;
P.figDir        = 'results';
end
