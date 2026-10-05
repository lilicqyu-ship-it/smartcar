function tune_pid_grid()
% TUNE_PID_GRID  Kp/Ki 网格扫描 —— 为台架整定(doc 21 SS15.3)做的仿真预扫
%
% 场景:0.3 s 时 500 pct*10 阶跃,1.5~2.0 s 加 0.010 N·m 负载扰动。
% 指标:超调、阶跃段 IAE(0.3–1.5 s)、扰动段 IAE(1.5–2.5 s)、
% 稳态 duty 抖动。排名:先给"超调 ≤5%"约束下的最优(阶跃品质优先),
% 同时列纯 IAE 总量排名 —— 高 Ki 抗扰好但阶跃超调大,权衡要摆到台面上。

here = fileparts(mfilename('fullpath'));
addpath(fullfile(here, 'fw'));
P0 = motor_params();
outDir = fullfile(here, P0.figDir);
if ~exist(outDir, 'dir'), mkdir(outDir); end

kpList = [0.4 0.6 0.8 1.0 1.2 1.5];
kiList = [0.005 0.01 0.02 0.04 0.08];
N  = 3000;
cmd = zeros(1, N);  cmd(301:end) = 500;
tau = zeros(1, N);  tau(1501:2000) = 0.010;

[KP, KI] = meshgrid(kpList, kiList);
overs = nan(size(KP));   % 阶跃段超调(0.3–1.5 s,扰动到来之前)
kicks = nan(size(KP));   % 扰动释放后的速度反弹(积分充能的代价)
iaeStep = nan(size(KP));  iaeDist = nan(size(KP));
chatters = nan(size(KP));

for r = 1:numel(KP)
    P = P0;
    P.servoKp = single(KP(r));
    P.servoKi = single(KI(r));
    L = sim_run_side(P, cmd, struct('tauLoad', tau));

    ref = 500;  v = L.measPct;
    overs(r) = max(0, (max(v(301:1500)) - ref) / ref * 100);
    kicks(r) = max(0, (max(v(2001:2600)) - ref) / ref * 100);
    iaeStep(r) = sum(abs(ref - v(301:1500))) * P.Ts;
    iaeDist(r) = sum(abs(ref - v(1501:2500))) * P.Ts;
    chatters(r) = std(diff(L.duty(2600:end)));
end
iaeAll = iaeStep + iaeDist;

%% ---- 排名表 ---------------------------------------------------------------
fprintf('== Kp/Ki 扫描(阶跃500 + 1.5–2.0s 扰动)==\n\n');
[~, idx] = sort(iaeAll(:));
fprintf('-- 纯 IAE 总量前 5 --\n');
fprintf('%4s %6s %8s %7s %8s %12s %12s %10s %8s\n', ...
        '名次', 'Kp', 'Ki', '超调%', '反弹%', 'IAE阶跃', 'IAE扰动', '抖动std', '备注');
shown = 0;
for i = 1:numel(idx)
    r = idx(i);
    mark = '';
    if abs(KP(r) - 0.8) < eps && abs(KI(r) - 0.01) < eps, mark = '<- 固件现值'; end
    fprintf('%4d %6.2f %8.3f %7.1f %8.1f %12.4f %12.4f %10.2f %8s\n', ...
            i, KP(r), KI(r), overs(r), kicks(r), iaeStep(r), iaeDist(r), chatters(r), mark);
    shown = shown + 1;
    if shown >= 5, break; end
end

good = find(overs(:) <= 5);
fprintf('\n-- 阶跃超调 ≤5%% 约束下 IAE 总量最优的 5 组 --\n');
fprintf('%4s %6s %8s %7s %8s %12s %12s %10s %8s\n', ...
        '名次', 'Kp', 'Ki', '超调%', '反弹%', 'IAE阶跃', 'IAE扰动', '抖动std', '备注');
if isempty(good)
    fprintf('   (网格内没有满足超调≤5%%的组合 —— 需要减小 Ki 或加大 Kp,或接受当前超调)\n');
else
    [~, gIdx] = sort(iaeAll(good));
    for i = 1:min(5, numel(gIdx))
        r = good(gIdx(i));
        mark = '';
        if abs(KP(r) - 0.8) < eps && abs(KI(r) - 0.01) < eps, mark = '<- 固件现值'; end
        fprintf('%4d %6.2f %8.3f %7.1f %8.1f %12.4f %12.4f %10.2f %8s\n', ...
                i, KP(r), KI(r), overs(r), kicks(r), iaeStep(r), iaeDist(r), chatters(r), mark);
    end
end

%% ---- 热图 ------------------------------------------------------------------
fig = figure('Visible', 'off', 'Position', [0 0 1280 480], 'Color', 'w');
tiledlayout(fig, 1, 2, 'TileSpacing', 'compact', 'Padding', 'compact');
nexttile;
imagesc(kpList, kiList, iaeAll);  set(gca, 'YDir', 'normal');
colorbar;  hold on;
plot(0.8, 0.01, 'wo', 'MarkerSize', 14, 'LineWidth', 2);
grid on; xlabel('K_p'); ylabel('K_i');
title('IAE [pct*10·s] (lower is better)');
text(0.8, 0.01, '  firmware', 'Color', 'w', 'VerticalAlignment', 'bottom');
nexttile;
imagesc(kpList, kiList, overs);  set(gca, 'YDir', 'normal');
colorbar;  hold on;
plot(0.8, 0.01, 'wo', 'MarkerSize', 14, 'LineWidth', 2);
grid on; xlabel('K_p'); ylabel('K_i');
title('overshoot [%] (step 500)');
exportgraphics(fig, fullfile(outDir, 'h_tune_grid.png'), 'Resolution', 150);
fprintf('\n热图已输出到 %s/h_tune_grid.png\n', P0.figDir);
end
