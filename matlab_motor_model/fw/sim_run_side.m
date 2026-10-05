function log = sim_run_side(P, cmd, opts)
% SIM_RUN_SIDE  单侧(左或右)完整链路仿真:1 kHz,与 TC275 CPU1 主循环同构
%
% 每拍执行(与 MOTOR_ALGO_task 的 controlStep 路径同时序):
%   1. ENCODER_task      消费上一拍内 ISR 累计的计数增量 → fw_encoder_measure
%   2. stepToward        目标斜率限制(±2 pct*10/ms)
%   3. SERVO_update      前馈+PI(闭环)或开环回退
%   4. MOTOR_setSpeed    本拍占空比在 (k, k+1] 区间作用于电机(一拍推进)
%
% 输入
%   P     motor_params()
%   cmd   1×N 目标序列(percent*10)
%   opts  可选字段(缺省如下):
%     .closedLoop  true      —— 标定记录 src≠0 的等价开关(g_closedLoopOk)
%     .encLive     true(N)   —— false 的拍编码器无输出(断线/死传感器)
%     .estopAt     Inf(拍号) —— 急停:之后走 brakeAll 路径(积分清零+输出停)
%     .estopMode   'brake'   —— 'brake'=MOTOR_brake(高阻滑行,固件实际行为)
%                               'stop' =MOTOR_stop(短接刹车,能耗制动)
%     .tauLoad     0(N)      —— 外部负载力矩(N·m,每台电机份额)
% 输出
%   log   struct:t, cmd, target, measPct, measMmS, alive, duty, integ,
%         vTrue, iMotor, openLoop

if nargin < 3, opts = struct(); end
o.closedLoop = true;
o.encLive    = true(1, numel(cmd));
o.estopAt    = Inf;
o.estopMode  = 'brake';
o.tauLoad    = zeros(1, numel(cmd));
fn = fieldnames(opts);
for ii = 1:numel(fn), o.(fn{ii}) = opts.(fn{ii}); end

N = numel(cmd);
encSt = fw_encoder_measure(P);             % init
srvSt = struct('integ', single(0));
pltSt = motor_plant_step(P);               % init
lastCount = 0;

log.t = (1:N) * P.Ts;  log.cmd = cmd;
log.target   = zeros(1, N);
log.measPct  = zeros(1, N);
log.measMmS  = zeros(1, N);
log.alive    = false(1, N);
log.duty     = zeros(1, N);
log.integ    = zeros(1, N);
log.vTrue    = zeros(1, N);
log.iMotor   = zeros(1, N);
log.openLoop = false(1, N);

target = 0;
for k = 1:N
    % ---- 急停:brakeAll = 目标清零 + MOTOR_brake(高阻)+ SERVO_reset ----
    if k >= o.estopAt
        [measPct, alive, encSt] = fw_encoder_measure(encSt, [0 0], k, P);
        srvSt.integ = single(0);
        [pltSt, out] = motor_plant_step(pltSt, 0, o.estopMode, o.tauLoad(k), P);
        lastCount = pltSt.count;
        log.target(k) = 0; log.measPct(k) = measPct(1); log.measMmS(k) = encSt.speedMmS(1);
        log.alive(k) = alive; log.duty(k) = 0; log.integ(k) = 0;
        log.vTrue(k) = out.vMmS; log.iMotor(k) = out.i; log.openLoop(k) = true;
        continue
    end

    % ---- 1. 编码器测量(消费上一拍计数;失效拍无计数) ----
    if o.encLive(k)
        dSide = 2 * (pltSt.count - lastCount);    % 同侧两轮计数和
    else
        dSide = 0;
    end
    lastCount = pltSt.count;
    [measPct, alive, encSt] = fw_encoder_measure(encSt, [dSide 0], k, P);
    log.measPct(k) = measPct(1);
    log.measMmS(k) = encSt.speedMmS(1);
    log.alive(k)   = alive;

    % ---- 2. 斜率限制 ----
    target = fw_step_toward(target, cmd(k), P);
    log.target(k) = target;

    % ---- 3. 伺服 ----
    measOk = alive && o.closedLoop;
    [duty, srvSt] = fw_servo_update(srvSt, target, measPct(1), measOk, P);
    log.duty(k) = duty;
    log.integ(k) = double(srvSt.integ);
    log.openLoop(k) = ~measOk;

    % ---- 4. 电机推进一拍 ----
    [pltSt, out] = motor_plant_step(pltSt, duty, 'drive', o.tauLoad(k), P);
    log.vTrue(k)  = out.vMmS;
    log.iMotor(k) = out.i;
end
end
