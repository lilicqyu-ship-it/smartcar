function [out1, alive, st] = fw_encoder_measure(st, sideDelta, nowMs, P)
% (init 调用 st = fw_encoder_measure(P) 时,状态从第一个输出返回)
% FW_ENCODER_MEASURE  复刻 tc275_car/rt/encoder.c ENCODER_task() 的测量链
% (每侧一个输入,4x 软件四倍频解码本身不建模 —— 见 README §3)
%
% 固件链路:每 ms 取各轮计数的增量 → 同侧两轮求和 → 写入 8 ms 滑动窗口 →
% 窗口均值(注意是均值不是中值,doc 21 §5.1 V1.13 的教训:中值被整数量化
% 钉死,1 m/s 以下大部分时刻读 0)→ counts/ms 换算 mm/s(float32,截断为
% 整数)→ percent*10 域(整数除法,±1000 饱和)。
%
% alive 判定:500 ms 内任一侧有过边沿(ENC_ALIVE_WINDOW_MS)。
%
% 输入
%   st         测量链状态(首次调用 st = fw_encoder_measure(P) 初始化)
%   sideDelta  [dLeft dRight],本 ms 同侧两轮计数增量之和(整数,可负)
%   nowMs      当前 tick(1 kHz 计数)
%   P          motor_params()
% 输出
%   measPct    [pctLeft pctRight] percent*10 域测量速度(±1000 饱和)
%   alive      TRUE = 500 ms 内见过边沿
%   st         更新后的状态
%
% 换算系数:侧增量计的是两轮之和,一轮 = 2*ENCODER_COUNTS_WHEEL_REV
% (=2*1061=2122 counts,整数宏);周长用 3.14159265f*轮径,与固件同字面量。

switch nargin
    case 1  % init: st = fw_encoder_measure(P)
        P = st;
        st = struct();
        st.win      = zeros(2, P.windowMs);   % [winL; winR] 滑动窗口
        st.winIdx   = 0;
        st.winFull  = false;
        st.lastMove = 0;
        st.speedMmS = [0 0];
        st.odoAcc   = single([0 0]);
        st.odoMm    = [0 0];
        st.first    = true;
        out1 = st; alive = false;
        return
end

dl = sideDelta(1); dr = sideDelta(2);

% alive:任一侧动了就刷新时间戳
if (dl ~= 0) || (dr ~= 0)
    st.lastMove = nowMs;
end
alive = (nowMs - st.lastMove) < P.aliveWindowMs;
if st.first
    st.first = false;   % 与固件一致:首个 tick 不判死亡
    st.lastMove = nowMs;
    alive = true;
end

% 滑动窗口写入
st.winIdx = st.winIdx + 1;
st.win(:, st.winIdx) = [dl; dr];
if st.winIdx == P.windowMs
    st.winIdx = 0;
    st.winFull = true;
end

n = P.windowMs * st.winFull + st.winIdx * ~st.winFull;   % winFull ? 8 : winIdx
if n > 0
    countsPerWheelRev = single(2 * P.countsWheelRev);    % 2122,与固件整数宏一致
    circMm = single(3.14159265) * single(P.wheelDiaMm);  % 3.14159265f*48
    meanL = single(sum(st.win(1, 1:n))) / single(n);
    meanR = single(sum(st.win(2, 1:n))) / single(n);
    mmPerS = (meanL * single(1000) / countsPerWheelRev) * circMm;
    st.speedMmS(1) = fix(double(mmPerS));                % (sint32) 截断
    mmPerS = (meanR * single(1000) / countsPerWheelRev) * circMm;
    st.speedMmS(2) = fix(double(mmPerS));

    % 里程计(绝对值累加,与固件一致)
    st.odoAcc = st.odoAcc + single([abs(dl) abs(dr)]);
    mmPerCount = single(P.wheelDiaMm) * single(3.14159265) / countsPerWheelRev;
    st.odoMm = fix(double(st.odoAcc * mmPerCount));
end

% mm/s → percent*10:C 整数除法(向零截断),±1000 饱和
out1 = fix(st.speedMmS * 1000 / P.fullScaleMmS);
out1 = min(max(out1, -1000), 1000);
end
