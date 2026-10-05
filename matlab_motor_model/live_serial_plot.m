function live_serial_plot(port, baud)
%LIVE_SERIAL_PLOT TC275 速度环串口实时示波器(100 Hz SRVB 台架流)
%
% 与固件的握手:TC275 上 ASCLIN0 控制台(115200, P14.0/P14.1 USB-串口)
% 收一行命令 "BENCH on"(app/console.c)后,以 100 Hz 推出紧凑遥测行,
% 本脚本实时解析并绘制,替代"上车试参数"式的盲调。关闭:
% 串口终端发 "BENCH off",或直接断开。
%
% 用法:
%   live_serial_plot                          % 自动探测串口(usbserial/COM)
%   live_serial_plot("/dev/tty.usbserial-A50285BI")
%   live_serial_plot("COM7", 115200)
%
% 窗口四格:左/右轮速度(目标 vs 实测)、左右 duty、左右积分项(真实值,
% 固件按 x10 整数编码)。关闭窗口结束,样本自动存 results/live_serial_*.csv
% (uptime 列保留固件时间戳,可与 sim_closed_loop 场景对比分析)。
%
% SRVB 行格式(XCORE_logi,值均为整数):
%   SRVB <uptime_ms> <tgL> <msL> <dutyL> <intL*10> <tgR> <msR> <dutyR> <intR*10>
% 全部在 percent*10 域。其余行(启动横幅/融合日志/控制台回复)只计数不解析。
% 115200 baud 下 ~60 B/行 * 100 Hz ≈ 6 KB/s,低于 ~11.5 KB/s 线速,但日志环
% 满载时固件会整行丢弃 —— 断缝直接反映为 uptime 跳变,CSV 里可复现。
%
% 依赖:基础 MATLAB(serialport,R2020b+),无需工具箱。

if nargin < 1 || isempty(port), port = autoPort(); end
if nargin < 2 || isempty(baud), baud = 115200; end

sp = serialport(port, baud);
configureTerminator(sp, "CR/LF");
sp.Timeout = 1;
fprintf('已连接 %s @ %d baud —— 串口终端发送 BENCH on 开始推流\n', port, baud);

fig = figure('Name', sprintf('TC275 速度环 — %s @ %d baud', port, baud), ...
             'NumberTitle', 'off', 'Color', 'w');
tlo = tiledlayout(fig, 2, 2, 'TileSpacing', 'compact', 'Padding', 'compact');
title(tlo, 'TC275 速度环 SRVB 流(percent*10 域)');

axL = nexttile; hold(axL, 'on'); grid(axL, 'on');
title(axL, '左轮速度'); xlabel(axL, 't / s'); ylabel(axL, 'pct*10');
alTl = animatedline(axL, 'Color', [0.25 0.45 0.85], 'LineStyle', '--', ...
                    'MaximumPoints', 6000, 'DisplayName', '目标');
alMl = animatedline(axL, 'Color', [0.85 0.30 0.10], 'LineWidth', 1.2, ...
                    'MaximumPoints', 6000, 'DisplayName', '实测');
legend(axL, 'show', 'Location', 'northwest');

axR = nexttile; hold(axR, 'on'); grid(axR, 'on');
title(axR, '右轮速度'); xlabel(axR, 't / s'); ylabel(axR, 'pct*10');
alTr = animatedline(axR, 'Color', [0.25 0.45 0.85], 'LineStyle', '--', ...
                    'MaximumPoints', 6000, 'DisplayName', '目标');
alMr = animatedline(axR, 'Color', [0.85 0.30 0.10], 'LineWidth', 1.2, ...
                    'MaximumPoints', 6000, 'DisplayName', '实测');
legend(axR, 'show', 'Location', 'northwest');

axD = nexttile; hold(axD, 'on'); grid(axD, 'on');
title(axD, '输出 duty'); xlabel(axD, 't / s'); ylabel(axD, 'pct*10');
alDl = animatedline(axD, 'Color', [0.10 0.60 0.30], 'MaximumPoints', 6000, ...
                    'DisplayName', '左 duty');
alDr = animatedline(axD, 'Color', [0.55 0.25 0.70], 'MaximumPoints', 6000, ...
                    'DisplayName', '右 duty');
legend(axD, 'show', 'Location', 'northwest');

axI = nexttile; hold(axI, 'on'); grid(axI, 'on');
title(axI, '积分项(÷10 还原)'); xlabel(axI, 't / s'); ylabel(axI, 'pct*10');
alIl = animatedline(axI, 'Color', [0.10 0.60 0.30], 'MaximumPoints', 6000, ...
                    'DisplayName', '左 I');
alIr = animatedline(axI, 'Color', [0.55 0.25 0.70], 'MaximumPoints', 6000, ...
                    'DisplayName', '右 I');
legend(axI, 'show', 'Location', 'northwest');

% 记录缓冲:uptime_ms + 8 列,不够时倍增
data = zeros(100000, 9);
nRec = 0;
others = 0;      % 非 SRVB 行(控制台回复、常规日志)
badParse = 0;    % SRVB 前缀但字段数不对

    function onLine(src, ~)
        line = char(readline(src));
        if startsWith(line, 'SRVB')
            v = sscanf(line(6:end), '%d');
            if numel(v) == 9
                nRec = nRec + 1;
                if nRec > size(data, 1)
                    data = [data; zeros(size(data, 1), 9)];
                end
                data(nRec, :) = v';
                t = v(1) / 1000;                      % 固件 uptime,秒
                addpoints(alTl, t, v(2)); addpoints(alMl, t, v(3));
                addpoints(alTr, t, v(6)); addpoints(alMr, t, v(7));
                addpoints(alDl, t, v(4)); addpoints(alDr, t, v(8));
                addpoints(alIl, t, v(5)/10); addpoints(alIr, t, v(9)/10);
            else
                badParse = badParse + 1;
            end
        else
            others = others + 1;
        end
    end

configureCallback(sp, "terminator", @onLine);

try
    while isvalid(fig) && isvalid(sp)
        drawnow limitrate;
        pause(0.02);
    end
catch
    % 窗口关闭 / Ctrl-C / 设备拔出 —— 统一落到保存与总结
end

if isvalid(sp)
    configureCallback(sp, "off");
    sp = [];                          %#ok<NASGU> 删除对象即释放串口
end

if nRec > 0
    outDir = fullfile(fileparts(mfilename('fullpath')), 'results');
    if ~exist(outDir, 'dir'), mkdir(outDir); end
    outFile = fullfile(outDir, ['live_serial_' ...
        char(datetime('now', 'Format', 'yyyymmdd_HHMMSS')) '.csv']);
    T = array2table(data(1:nRec, :), 'VariableNames', ...
        {'uptime_ms', 'target_l', 'meas_l', 'duty_l', 'int_l_x10', ...
         'target_r', 'meas_r', 'duty_r', 'int_r_x10'});
    writetable(T, outFile);
    fprintf('已记录 %d 样本(其他行 %d,解析失败 %d)→ %s\n', ...
            nRec, others, badParse, outFile);
else
    fprintf('未收到 SRVB 样本(其他行 %d)—— 确认已在串口终端发送 BENCH on\n', ...
            others);
end
end

function p = autoPort()
%AUTOPORT 优先挑 USB 转串口(macOS: usbserial/usbmodem;Windows: COM)
ports = serialportlist("available");
if isempty(ports)
    error('live_serial_plot:noPort', '未发现可用串口 —— 检查 USB 线与驱动');
end
cand = ports(contains(ports, {'usbserial', 'usbmodem'}));
if isempty(cand) && ispc
    cand = ports(startsWith(ports, 'COM'));
end
if isempty(cand), cand = ports; end
if numel(cand) > 1
    error('live_serial_plot:ambiguous', ...
          '发现多个候选串口,请显式指定其一:\n%s', strjoin(cand, newline));
end
p = char(cand(1));
end
