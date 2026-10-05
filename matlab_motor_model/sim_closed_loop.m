function sim_closed_loop()
% SIM_CLOSED_LOOP  闭环伺服主仿真:阶跃/斜坡/负载扰动/编码器断线/急停
%
% 五个场景,每个都跑完整链路(sim_run_side,1 kHz,与 TC275 CPU1 同构):
%   a. 500 pct*10 阶跃,闭环 vs 开环对比(伺服的价值一目了然)
%   b. 斜率限制整形出的 0.5 s 斜坡跟踪(MOTOR_ALGO_MAX_STEP=2/ms)
%   c. 稳态下的负载扰动(积分项的抗扰作用)
%   d. 编码器断线 1 s(开环回退 + 积分清零,恢复无踢轮)
%   e. 急停:MOTOR_brake(高阻滑行,固件实际行为)vs MOTOR_stop(短接刹车)
%      —— 量化 TB6612 "brake 名不副实" 的代价
% 图与指标输出到 results/。

here = fileparts(mfilename('fullpath'));
addpath(fullfile(here, 'fw'));
P = motor_params();
outDir = fullfile(here, P.figDir);
if ~exist(outDir, 'dir'), mkdir(outDir); end
N = round(P.simTimeS / P.Ts);

%% ---- a. 500 pct*10 阶跃:闭环 vs 开环 ------------------------------------
cmd = zeros(1, N);  cmd(round(0.3 * 1000) + 1:end) = 500;
Lc = sim_run_side(P, cmd);
Lo = sim_run_side(P, cmd, struct('closedLoop', false));

mA = metrics(Lc, 500);  mO = metrics(Lo, 500);
fprintf('== a. 阶跃 500 pct*10(=50%% 满量程)==\n');
printMetrics('闭环', mA);  printMetrics('开环', mO);

fig = figure('Visible', 'off', 'Position', [0 0 1280 720], 'Color', 'w');
tiledlayout(fig, 2, 1, 'TileSpacing', 'compact', 'Padding', 'compact');
nexttile; hold on;
plot(Lc.t, Lc.target, '--', 'Color', [.4 .4 .4], 'LineWidth', 1);
plot(Lc.t, Lc.measPct, 'b', 'LineWidth', 1.2);
plot(Lo.t, Lo.measPct, 'r', 'LineWidth', 1.2);
grid on; ylabel('speed [pct*10]');
legend('target (slewed)', 'closed loop', 'open loop', 'Location', 'southeast');
title('a) Step 500 pct*10: closed-loop vs open-loop');
nexttile; hold on;
plot(Lc.t, Lc.duty, 'b', 'LineWidth', 1.2);
plot(Lo.t, Lo.duty, 'r--', 'LineWidth', 1);
grid on; ylabel('duty [pct*10]'); xlabel('t [s]');
legend('closed loop', 'open loop', 'Location', 'southeast');
exportgraphics(fig, fullfile(outDir, 'a_step500.png'), 'Resolution', 150);

%% ---- b. 斜坡跟踪(斜率限制整形) ------------------------------------------
Lb = sim_run_side(P, cmd);                     % 同 a 的闭环
fprintf('\n== b. 0.5 s 斜坡跟踪(斜率限制整形,目标 500)==\n');
fprintf('   斜坡期间最大滞后:%.1f pct*10(%.1f mm/s)\n', ...
        max(Lb.target(300:800) - Lb.measPct(300:800)), ...
        max(Lb.target(300:800) - Lb.measPct(300:800)) * P.fullScaleMmS / 1000);

fig = figure('Visible', 'off', 'Position', [0 0 1280 480], 'Color', 'w');
tiledlayout(fig, 1, 2, 'TileSpacing', 'compact', 'Padding', 'compact');
nexttile; hold on;
plot(Lb.t, Lb.cmd, ':', 'Color', [.6 .6 .6], 'LineWidth', 1);
plot(Lb.t, Lb.target, '--', 'Color', [.4 .4 .4], 'LineWidth', 1.2);
plot(Lb.t, Lb.measPct, 'b', 'LineWidth', 1.2);
grid on; ylabel('speed [pct*10]'); xlabel('t [s]');
legend('command (CPU0)', 'target after slew', 'measured', 'Location', 'southeast');
title('b) Slew-limited ramp (2 pct*10/ms = 0.5 s full scale)');
nexttile; hold on;
plot(Lb.t, Lb.duty, 'b', 'LineWidth', 1.2);
plot(Lb.t, Lb.integ, 'm', 'LineWidth', 1);
grid on; ylabel('duty / integral [pct*10]'); xlabel('t [s]');
legend('duty out', 'integral term', 'Location', 'southeast');
exportgraphics(fig, fullfile(outDir, 'b_ramp.png'), 'Resolution', 150);

%% ---- c. 负载扰动 ---------------------------------------------------------
tau = zeros(1, N);  tau(round(1.5 * 1000) + 1:round(2.0 * 1000)) = 0.010;  % ≈25% 堵转
Ld = sim_run_side(P, cmd, struct('tauLoad', tau));
dip = max(Lc.measPct(round(1.5 * 1000) + 1:round(2.0 * 1000)) - 500);
fprintf('\n== c. 负载扰动(1.5~2.0 s 加 0.010 N·m ≈ 25%% 堵转)==\n');
if dip < 20
    fprintf('   最大速降:%.1f pct*10(未越出 ±2%% 带,PI 抗扰良好)\n', dip);
else
    fprintf('   最大速降:%.1f pct*10,恢复时间(回到 ±2%%):%.0f ms\n', dip, ...
            recoverMs(Ld.measPct, 500, 20, round(1.5 * 1000)));
end

fig = figure('Visible', 'off', 'Position', [0 0 1280 720], 'Color', 'w');
tiledlayout(fig, 2, 1, 'TileSpacing', 'compact', 'Padding', 'compact');
nexttile; hold on;
plot(Ld.t, Ld.target, '--', 'Color', [.4 .4 .4], 'LineWidth', 1);
plot(Ld.t, Ld.measPct, 'b', 'LineWidth', 1.2);
grid on; ylabel('speed [pct*10]');
title('c) Load disturbance 0.010 N·m @1.5–2.0 s');
legend('target', 'measured', 'Location', 'southeast');
nexttile; hold on;
plot(Ld.t, Ld.duty, 'b', 'LineWidth', 1.2);
plot(Ld.t, Ld.integ, 'm', 'LineWidth', 1);
grid on; ylabel('duty / integral [pct*10]'); xlabel('t [s]');
legend('duty out', 'integral term', 'Location', 'southeast');
exportgraphics(fig, fullfile(outDir, 'c_disturbance.png'), 'Resolution', 150);

%% ---- d. 编码器断线 1 s ----------------------------------------------------
encLive = true(1, N);  encLive(round(1.5 * 1000) + 1:round(2.5 * 1000)) = false;
Le = sim_run_side(P, cmd, struct('encLive', encLive));
fprintf('\n== d. 编码器断线(1.5~2.5 s)==\n');
fprintf('   死亡后前 500 ms(alive 未过期,meas 冻结 0):duty 打满 %d,真实轮速峰值 %.0f mm/s(超速!)\n', ...
        max(Le.duty(round(1.5 * 1000):round(2.0 * 1000))), ...
        max(Le.vTrue(round(1.5 * 1000):round(2.0 * 1000))));
fprintf('   alive 判死后(开环 duty=target=500):真实轮速 ≈ %.0f mm/s;\n', ...
        mean(Le.vTrue(round(2.05 * 1000):round(2.45 * 1000))));
fprintf('   恢复瞬间 duty 跳变:%.1f pct*10(积分已清零,无踢轮)\n', ...
        max(abs(diff(Le.duty(round(2.5 * 1000) + (1:20))))));

fig = figure('Visible', 'off', 'Position', [0 0 1280 720], 'Color', 'w');
tiledlayout(fig, 2, 1, 'TileSpacing', 'compact', 'Padding', 'compact');
nexttile; hold on;
plot(Le.t, Le.target, '--', 'Color', [.4 .4 .4], 'LineWidth', 1);
plot(Le.t, Le.measPct, 'b', 'LineWidth', 1.2);
plot(Le.t, Le.vTrue / P.fullScaleMmS * 1000, 'r', 'LineWidth', 1);
xline(1.5, 'k:');  xline(2.5, 'k:');
grid on; ylabel('speed [pct*10]');
legend('target', 'measured (frozen when dead)', 'true wheel speed', 'Location', 'south');
title('d) Encoder dropout 1.5–2.5 s: open-loop fallback');
nexttile; hold on;
plot(Le.t, Le.duty, 'b', 'LineWidth', 1.2);
plot(Le.t, Le.integ, 'm', 'LineWidth', 1);
plot(Le.t, Le.alive * 200, 'g', 'LineWidth', 1);
xline(1.5, 'k:');  xline(2.5, 'k:');
grid on; ylabel('duty / integral / alive'); xlabel('t [s]');
legend('duty out', 'integral term', 'encoder alive (x200)', 'Location', 'south');
exportgraphics(fig, fullfile(outDir, 'd_encoder_dropout.png'), 'Resolution', 150);

%% ---- e. 急停:高阻"brake" vs 短接"stop" -----------------------------------
estopAt = round(2.0 * 1000);
Lbr = sim_run_side(P, cmd, struct('estopAt', estopAt));                        % 固件实际
Lst = sim_run_side(P, cmd, struct('estopAt', estopAt, 'estopMode', 'stop'));   % 假如短接
v0 = Lbr.measPct(estopAt - 1);
tBr = settleBelow(Lbr.vTrue, 50, estopAt) * P.Ts * 1000;      % 减到 50 mm/s 的毫秒数
tSt = settleBelow(Lst.vTrue, 50, estopAt) * P.Ts * 1000;
fprintf('\n== e. 急停(从 %.0f pct*10)==\n', v0);
fprintf('   MOTOR_brake(高阻滑行,固件实际):减到 50 mm/s 需 %.0f ms\n', tBr);
fprintf('   MOTOR_stop(短接刹车,更硬)   :减到 50 mm/s 需 %.0f ms\n', tSt);

fig = figure('Visible', 'off', 'Position', [0 0 1280 480], 'Color', 'w');
tiledlayout(fig, 1, 2, 'TileSpacing', 'compact', 'Padding', 'compact');
nexttile; hold on;
plot(Lbr.t, Lbr.vTrue, 'b', 'LineWidth', 1.2);
plot(Lst.t, Lst.vTrue, 'r--', 'LineWidth', 1.2);
xline(2.0, 'k:');
grid on; ylabel('true wheel speed [mm/s]'); xlabel('t [s]');
legend('MOTOR\_brake (high-Z coast, firmware)', 'MOTOR\_stop (short brake)', 'Location', 'northeast');
title('e) E-stop deceleration from 50% speed');
nexttile; hold on;
plot(Lbr.t, Lbr.iMotor, 'b', 'LineWidth', 1.2);
plot(Lst.t, Lst.iMotor, 'r--', 'LineWidth', 1.2);
xline(2.0, 'k:');
grid on; ylabel('motor current [A]'); xlabel('t [s]');
legend('MOTOR\_brake', 'MOTOR\_stop', 'Location', 'northeast');
exportgraphics(fig, fullfile(outDir, 'e_estop.png'), 'Resolution', 150);

fprintf('\n图已输出到 %s/\n', P.figDir);
end

%% ---- 指标 -----------------------------------------------------------------
function m = metrics(L, ref)
k0 = round(0.3 * 1000) + 1;              % 命令生效之后
v = L.measPct;
m.rise = (find(v(k0:end) >= 0.9 * ref, 1) + k0 - 1 - k0) * 1e-3;
peak = max(v(k0:end));
m.overshoot = max(0, (peak - ref) / ref * 100);
band = 0.02 * ref;
outside = find(abs(v(k0:end) - ref) > band, 1, 'last');
m.settle = ((outside + k0 - 1) - k0) * 1e-3;
m.steadyErr = mean(v(round(2.5 * 1000):end) - ref);
tail = round(2.5 * 1000):numel(v);
m.chatter = std(diff(L.duty(tail)));
end

function printMetrics(name, m)
if isempty(m.rise)
    riseTxt = sprintf('%8s', 'n/a');
else
    riseTxt = sprintf('%4.0f ms', m.rise * 1000);
end
fprintf('   %s:上升(0→90%%)%s,超调 %.1f%%,整定(±2%%)%.0f ms,稳态误差 %.1f,稳态抖动 std(Δduty)=%.2f\n', ...
        name, riseTxt, m.overshoot, m.settle * 1000, m.steadyErr, m.chatter);
end

function ms = recoverMs(v, ref, band, kEvent)
k = kEvent + find(abs(v(kEvent + 1:end) - ref) <= band, 1);
ms = (k - kEvent) * 1e-3 * 1000;
end

function dk = settleBelow(v, vLim, kFrom)
% 相对 kFrom 的拍数(第一次低于 vLim)
dk = find(v(kFrom + 1:end) < vLim, 1);
if isempty(dk), dk = numel(v) - kFrom; end
end
