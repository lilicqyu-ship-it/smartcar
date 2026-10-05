function target = fw_step_toward(current, target_cmd, P)
% FW_STEP_TOWARD  复刻 tc275_car/rt/motor_algo.c MOTOR_ALGO_stepToward()
%
% 目标斜率限制:每 1 ms tick 最多变化 ±MOTOR_ALGO_MAX_STEP(=2 pct*10),
% 所以 0..1000 的阶跃被整形为 0.5 s 的斜坡 —— 伺服看到的永远是斜坡不是台阶。
% 纯整数运算,与固件 sint16 语义逐位一致。

diff = target_cmd - current;
if diff > P.maxStep
    diff = P.maxStep;
elseif diff < -P.maxStep
    diff = -P.maxStep;
end
target = current + diff;
end
