function validate_fw_equivalence()
% VALIDATE_FW_EQUIVALENCE  MATLAB 模型 ↔ 固件 C 代码等价性验证
%
% 两层:
%   A. 金标回放:tools/golden_servo.csv 由真实固件源码
%      (tc275_car/rt/servo.c,cc -ffp-contract=off 编译)生成,逐行喂给
%      MATLAB 移植版,比较输出占空比和积分项的 float32 位模式 —— 必须
%      逐位一致(IEEE single,非容差比较)。
%   B. 结构不变量:编码器测量链(8 ms 窗口均值、mm/s 截断、pct 换算、
%      ±1000 饱和、alive 窗口)按第一性公式独立复算后断言。
%
% 重新生成金标(改了固件或编译器后):
%   cc -O2 -ffp-contract=off -I ../tc275_car -I tools/stub \
%      tools/gen_golden.c ../tc275_car/rt/servo.c -o tools/gen_golden
%   ./tools/gen_golden > tools/golden_servo.csv

here = fileparts(mfilename('fullpath'));
addpath(fullfile(here, 'fw'));
P = motor_params();
csv = fullfile(here, 'tools', 'golden_servo.csv');
assert(isfile(csv), '缺少金标文件 %s(先按文件头注释用 cc 生成)', csv);

%% ---- A. 金标回放 ---------------------------------------------------------
raw = fileread(csv);
lines = regexp(raw, '\r?\n', 'split');
lines = lines(~cellfun('isempty', lines));

srvSt = struct();           % 按侧索引的积分状态
nSrv = 0;  nSlew = 0;  nBad = 0;
fprintf('== A. 固件金标回放(%d 行,源:tc275_car/rt/servo.c)==\n', numel(lines));
fprintf('%4s %5s %5s %5s %6s %6s %6s %10s  %s\n', ...
        'seq', 'side', 'tgt', 'meas', 'valid', 'C输出', 'M输出', 'integ(C/M)', '判定');

for i = 1:numel(lines)
    v = sscanf(lines{i}, '%d,%d,%d,%d,%d,%d,%x')';
    seq = v(1); side = v(2); tgt = v(3); meas = v(4); valid = v(5);
    dutyC = v(6); bitsC = v(7);

    if valid == 2                       % stepToward 行
        dutyM = fw_step_toward(tgt, meas, P);
        nSlew = nSlew + 1;
        ok = (dutyM == dutyC);
        integTxt = sprintf('%10s', '-');
    elseif valid == 3                   % SERVO_reset 标记行
        if isfield(srvSt, sprintf('s%d', side))
            srvSt = rmfield(srvSt, sprintf('s%d', side));
        end
        continue
    else                                % SERVO_update 行
        if ~isfield(srvSt, sprintf('s%d', side))
            srvSt.(sprintf('s%d', side)) = struct('integ', single(0));
        end
        st = srvSt.(sprintf('s%d', side));
        [dutyM, st] = fw_servo_update(st, tgt, meas, logical(valid), P);
        srvSt.(sprintf('s%d', side)) = st;
        nSrv = nSrv + 1;
        bitsM = double(typecast(single(st.integ), 'uint32'));
        ok = (dutyM == dutyC) && (bitsM == bitsC);
        integTxt = sprintf('%5.3f/%5.3f', typecast(uint32(bitsC), 'single'), double(st.integ));
    end
    if ~ok
        nBad = nBad + 1;
        fprintf('%4d %5d %5d %5d %6d %6d %6d %10s  不一致!\n', ...
                seq, side, tgt, meas, valid, dutyC, dutyM, integTxt);
    end
end
fprintf('   SERVO_update 比对 %d 行,stepToward 比对 %d 行,不一致 %d 行\n', nSrv, nSlew, nBad);
assert(nBad == 0, '金标回放存在不一致,见上表');

%% ---- B. 结构不变量(编码器测量链) ---------------------------------------
fprintf('== B. 编码器测量链不变量 ==\n');

% 独立的第一性复算:14 counts/ms(≈1 m/s,固件注释的标定点)
dlRef = 14;
circMm = 3.14159265 * P.wheelDiaMm;                      % 双精度参考
vRef = (dlRef * 1000 / (2 * P.countsWheelRev)) * circMm; % ≈ 994.9 mm/s
pctRef = fix(vRef * 1000 / P.fullScaleMmS);

st = fw_encoder_measure(P);              % init
now = 0;
[measPct, alive, st] = fw_encoder_measure(st, [dlRef 0], now + 1, P);
chk(abs(st.speedMmS(1) - vRef) <= 1, '恒速下 mm/s 均值:期望 %.1f,实际 %d', vRef, st.speedMmS(1));
chk(measPct(1) == pctRef, 'pct*10 换算:期望 %d,实际 %d', pctRef, measPct(1));
chk(measPct(2) == 0, '未喂计数的右侧应保持 0');

% 部分窗口(n=1 时就出值 —— 均值而非"等满 8 拍"是设计意图)
chk(st.speedMmS(1) ~= 0, '第 1 拍即应输出(窗口均值按现有样本数)');

% 撤掉计数:8 ms 窗口内均值单调衰减到 0
prev = inf;  mono = true;
for k = 2:10
    [~, ~, st] = fw_encoder_measure(st, [0 0], now + k, P);
    if st.speedMmS(1) > prev, mono = false; end
    prev = st.speedMmS(1);
end
chk(prev == 0 && mono, '8 ms 窗口均值应在 8 拍内单调衰减到 0(最后 %d)', prev);

% alive 窗口:499 ms 内仍活,500 ms 判死
st = fw_encoder_measure(P);
[~, alive, st] = fw_encoder_measure(st, [dlRef 0], 1, P);
chk(alive, '有边沿应 alive');
[~, alive, st] = fw_encoder_measure(st, [0 0], 1 + 499, P);
chk(alive, '距最后边沿 498 ms 应 alive');
[~, alive, st] = fw_encoder_measure(st, [0 0], 1 + 501, P);
chk(~alive, '距最后边沿 500 ms 应判死(ENC_ALIVE_WINDOW_MS)');

% 饱和与符号(200 counts/ms ≈ 14.2 m/s,物理可达的上界,不触发 C 侧溢出)
st = fw_encoder_measure(P);
[measPct, ~, st] = fw_encoder_measure(st, [200 0], 1, P);
chk(measPct(1) == 1000, '超量程应饱和到 +1000(实际 %d)', measPct(1));
st = fw_encoder_measure(P);
[measPct, ~, st] = fw_encoder_measure(st, [-200 0], 1, P);
chk(measPct(1) == -1000, '负向超量程应饱和到 -1000(实际 %d)', measPct(1));
st = fw_encoder_measure(P);
[measPct, ~, st] = fw_encoder_measure(st, [-dlRef 0], 1, P);
chk(measPct(1) == -pctRef, '负速对称:期望 %d,实际 %d', -pctRef, measPct(1));

% 分辨率注释自检:1 count(窗口内)= 71.06 mm/s,8 ms 均值下 8.9 mm/s
resOneCount = 1000 / (2 * P.countsWheelRev) * circMm;
chk(abs(resOneCount - 71.06) < 0.1, '每 count 换算应 ≈71.06 mm/s(实际 %.2f)', resOneCount);
fprintf('   测量分辨率:1 count(单槽)= %.2f mm/s;8 ms 均值 = %.2f mm/s\n', ...
        resOneCount, resOneCount / P.windowMs);

fprintf('\n全部通过:A 金标 %d+%d 行逐位一致,B 不变量 %d 项通过。\n', nSrv, nSlew, 9);

    function chk(cond, varargin)
        assert(cond, varargin{:});
        fprintf('   [通过] %s\n', sprintf(varargin{:}));
    end
end
