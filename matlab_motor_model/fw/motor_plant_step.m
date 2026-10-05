function [st, out] = motor_plant_step(st, dutyPct10, mode, tauLoad, P)
% MOTOR_PLANT_STEP  被控对象单步(1 kHz):MG310 直流电机 + TB6612 + 48 mm 轮
%
% 结构图(平均值 PWM 模型,20 kHz 开关远高于 1 kHz 控制环):
%   电气: L di/dt = V_eff - R i - Ke w        (w = 电机角速度)
%   机械: J dw/dt = Kt i - B w - tauC*sgn(w) - tauLoad
%   传动: w_wheel = w / 20.409,v = w_wheel * r_wheel(24 mm)
%
% 侧 = 同侧两台电机同占空比驱动、平分负载。两台并联同压驱动的方程与
% "单台电机参数"的方程逐项相同(README §3),所以这里按每台电机的参数
% 仿真一台,i 即每台电机的电流。
%
% TB6612 工作模式(注意:固件函数名与电气行为是反的,见 README §5):
%   mode='drive' 对应 MOTOR_setSpeed:方向脚 + PWM,平均值 V_eff = duty/1000*Vbat
%   mode='stop'  对应 MOTOR_stop   :IN1=IN2=L → TB6612 短接刹车(能耗制动,最硬)
%   mode='brake' 对应 MOTOR_brake  :IN1=IN2=H → TB6612 STANDBY=高阻,自由滑行
%
% 线性部分(电气+粘性机械+负载)用 ZOH 精确离散(Ad,Bd 由 expm 得出,
% 见 motor_params);电气时间常数(~60 us)<< Ts,电流一步内即到准稳态。
% 库仑摩擦/静摩擦显式处理:转动时按符号扣除且不过零;近零时推力矩不超过
% 静摩擦阈值则保持停转。
%
% 输入
%   st        状态(首次调用传 motor_plant_step('init', P))
%   dutyPct10 percent*10 占空比(-1000..1000),mode='drive' 时有效
%   mode      'drive' | 'stop' | 'brake'
%   tauLoad   外部负载力矩(N·m,每台电机份额,正=阻碍前进)
% 输出
%   st        更新后的状态
%   out       vMmS(真实轮速 mm/s), i(每台电机电流 A), omegaM(rad/s),
%             countDelta(本 ms 该轮编码器计数增量,52 counts/电机转)

switch nargin
    case 1  % init: st = motor_plant_step(P)
        P = st;
        st = struct('omegaM', 0, 'i', 0, 'phiWheel', 0, 'count', 0);
        out = struct('vMmS', 0, 'i', 0, 'omegaM', 0, 'countDelta', 0);
        return
end

% ---- TB6612 模式 → 等效端电压与电流路径 ----
switch mode
    case 'drive'
        d = min(max(dutyPct10, -P.bspClamp), P.bspClamp);   % BSP ±1000 再限幅
        V = d / 1000 * P.Vnom;
        openCircuit = false;          % duty=0 与 stop 同为短接刹车(MOTOR_setSpeed 走 MOTOR_stop)
    case 'stop'                       % 短接刹车:V=0,电流可在 L-R-Ke 回路流动
        V = 0; openCircuit = false;
    case 'brake'                      % 高阻:无电流路径,自由滑行
        V = 0; openCircuit = true;
    otherwise
        error('mode 必须是 drive|stop|brake');
end

% ---- ZOH 线性部分(电气 + 粘性机械;库仑摩擦随后显式处理) ----
if openCircuit
    st.i = 0;                                     % 高阻:无电流路径,保持 0
    x = P.Ad * [0; st.omegaM] + P.Bd * [0; -tauLoad];
    st.omegaM = x(2);
else
    x = P.Ad * [st.i; st.omegaM] + P.Bd * [V; -tauLoad];
    st.i = x(1);
    st.omegaM = x(2);
end

% ---- 库仑摩擦 + 静摩擦 ----
if abs(st.omegaM) > P.omegaThresh
    dOmega = sign(st.omegaM) * (P.tauC / P.J) * P.Ts;
    if abs(dOmega) > abs(st.omegaM)
        st.omegaM = 0;                            % 不穿越零
    else
        st.omegaM = st.omegaM - dOmega;
    end
else
    tauThrust = P.Kt * st.i - tauLoad;
    if abs(tauThrust) <= P.tauC
        st.omegaM = 0;                            % 静摩擦:保持停转
    end
    % 推力矩足以克服静摩擦时保留线性结果,库仑项从下一拍起生效
end

% ---- 位置与编码器计数(52 counts/电机转,齿轮比 20.409) ----
omegaWheel = st.omegaM / P.gearRatio;
st.phiWheel = st.phiWheel + omegaWheel * P.Ts;
count = floor(st.phiWheel * P.gearRatio / (2 * pi) * (P.encPpr * 4));
out.countDelta = count - st.count;
st.count = count;

out.vMmS   = omegaWheel * P.wheelR_m * 1000;
out.i      = st.i;
out.omegaM = st.omegaM;
end
