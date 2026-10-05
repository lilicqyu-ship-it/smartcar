function sim_low_speed_quantization()
% SIM_LOW_SPEED_QUANTIZATION  低速测量量化 与 低速闭环静摩擦极限环
%
% 背景固件事实(doc 21 §5.1 V1.13):
%   1 count/ms(单槽)= 71.06 mm/s,整数量化粗;8 ms 窗口均值下
%   分辨率 = 71.06/8 ≈ 8.9 mm/s。这就是当年从"中值"改成"窗口均值"的原因
%   (中值在大多数槽为 0 时直接读 0,低速全盲)。
%
% 内容:
%   a. 开环恒占空比,真实轮速 ≈ 20 / 50 / 120 / 300 mm/s:看量化楼梯
%   b. 闭环目标 30 pct*10:静摩擦 + 死区 + 积分的低速极限环(游走/抖振)

here = fileparts(mfilename('fullpath'));
addpath(fullfile(here, 'fw'));
P = motor_params();
outDir = fullfile(here, P.figDir);
if ~exist(outDir, 'dir'), mkdir(outDir); end
N = 1500;                                   % 1.5 s

%% ---- a. 开环量化楼梯 ------------------------------------------------------
targets = [100 150 250 400];   % 静摩擦阈值 ~87 pct*10 之下的占空比轮子不转
logs = cell(1, 4);
for i = 1:4
    cmd = zeros(1, N);  cmd(201:end) = targets(i);
    logs{i} = sim_run_side(P, cmd, struct('closedLoop', false));
end

stictionDuty = P.tauC * P.R / P.Kt / P.Vnom * 1000;
fprintf('== a. 开环恒占空比下的量化(测量 = 8 ms 窗口均值 + 整数截断)==\n');
fprintf('   静摩擦阈值:占空比 <%.0f pct*10 时轮子不转(占位摩擦参数)\n', stictionDuty);
fprintf('%8s %12s %14s %12s\n', '目标', '真实mm/s', '测量mm/s(均值)', '测量抖动std');
for i = 1:4
    k = 800:N;
    fprintf('%8d %12.1f %14.1f %12.2f\n', targets(i), mean(logs{i}.vTrue(k)), ...
            mean(logs{i}.measMmS(k)), std(logs{i}.measMmS(k)));
end

fig = figure('Visible', 'off', 'Position', [0 0 1280 800], 'Color', 'w');
tiledlayout(fig, 2, 2, 'TileSpacing', 'compact', 'Padding', 'compact');
for i = 1:4
    nexttile; hold on;
    plot(logs{i}.t, logs{i}.vTrue, 'r', 'LineWidth', 1.2);
    plot(logs{i}.t, logs{i}.measMmS, 'b', 'LineWidth', 1);
    yline(mean(logs{i}.measMmS(800:N)), 'k:');
    grid on; xlabel('t [s]'); ylabel('speed [mm/s]');
    title(sprintf('open loop, duty = %d pct*10', targets(i)));
    legend('true wheel speed', 'measured (8 ms window mean)', 'Location', 'southeast');
    ylim([-30 max(logs{i}.vTrue) * 1.5 + 30]);
end
exportgraphics(fig, fullfile(outDir, 'f_low_speed_openloop.png'), 'Resolution', 150);

%% ---- b. 低速闭环极限环 ----------------------------------------------------
cmd = zeros(1, 5000);  cmd(201:end) = 30;    % 5 s:极限环周期在秒级
Lg = sim_run_side(P, cmd);
moving = Lg.vTrue > 10;
edges = diff(moving);
nStart = sum(edges == 1);
fprintf('\n== b. 闭环目标 30 pct*10(3 mm/s)==\n');
fprintf('   4.8 s 内起停次数:%d(静摩擦极限环;固件注释"让 I 项决定何时值得再推")\n', nStart);
fprintf('   duty 范围:[%d, %d],测量速度范围:[%d, %d] mm/s\n', ...
        min(Lg.duty(300:end)), max(Lg.duty(300:end)), ...
        min(Lg.measMmS(300:end)), max(Lg.measMmS(300:end)));

fig = figure('Visible', 'off', 'Position', [0 0 1280 720], 'Color', 'w');
tiledlayout(fig, 2, 1, 'TileSpacing', 'compact', 'Padding', 'compact');
nexttile; hold on;
plot(Lg.t, Lg.cmd, ':', 'Color', [.6 .6 .6], 'LineWidth', 1.5);
plot(Lg.t, Lg.vTrue, 'r', 'LineWidth', 1.2);
plot(Lg.t, Lg.measMmS, 'b', 'LineWidth', 1);
grid on; ylabel('speed [mm/s]');
legend('target 30 pct*10 = 3 mm/s', 'true wheel speed', 'measured', 'Location', 'northeast');
title('closed loop at 3 mm/s: stiction limit cycle');
nexttile; hold on;
plot(Lg.t, Lg.duty, 'b', 'LineWidth', 1.2);
plot(Lg.t, Lg.integ, 'm', 'LineWidth', 1);
yline(P.tauC * P.R / P.Kt / P.Vnom * 1000, 'k:', 'stiction duty');
grid on; ylabel('duty / integral [pct*10]'); xlabel('t [s]');
legend('duty out', 'integral term', 'static-friction duty threshold', 'Location', 'east');
exportgraphics(fig, fullfile(outDir, 'g_low_speed_closedloop.png'), 'Resolution', 150);

fprintf('\n图已输出到 %s/(f_low_speed_openloop / g_low_speed_closedloop)\n', P.figDir);
end
