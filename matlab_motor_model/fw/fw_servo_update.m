function [duty, st] = fw_servo_update(st, targetPct10, measPct10, measValid, P)
% FW_SERVO_UPDATE  复刻 tc275_car/rt/servo.c SERVO_update()(1 kHz,每侧一个实例)
%
% 控制律(percent*10 域,增益无量纲):
%   e = target - meas
%   |e| > 3  → integ += Ki*e,并限幅 ±300(积分抗饱和)
%   u = FF*target + Kp*e + integ,限幅 ±1000
%   |u| < 5  → 输出 0(输出死区,防 TB6612 换向脚抖动)
%
% 开环回退:measValid=FALSE(编码器不 alive 或标定记录 src=0)时没有可信
% 测量,直接输出 duty=target,并把积分"清零"而不是冻结 —— 防止对着死编码器
% 积累的电荷在重新锁定瞬间踢轮子。
%
% 精度:固件全部用 float32,这里用 single 逐位对齐(运算顺序与 C 相同,
% 金标验证见 validate_fw_equivalence.m)。
%
% 输入
%   st          struct,integ 字段(single)
%   targetPct10 目标侧速度(percent*10,-1000..1000,已经过斜率限制)
%   measPct10   编码器测量侧速度(percent*10)
%   measValid   TRUE=闭环,FALSE=开环回退
%   P           motor_params()
% 输出
%   duty        本 tick 应施加的占空比(percent*10,-1000..1000)
%   st          更新后的状态

if ~measValid
    st.integ = single(0);
    duty = targetPct10;
    return
end

e = single(targetPct10) - single(measPct10);

% 误差死区外才积分:死区内是中值窗噪声,积进去只会让轮子在零附近游走
if (e > single(P.eDeadband)) || (e < single(-P.eDeadband))
    st.integ = st.integ + P.servoKi * e;
    if st.integ > single(P.integMax)
        st.integ = single(P.integMax);
    elseif st.integ < single(-P.integMax)
        st.integ = single(-P.integMax);
    end
end

u = (P.servoFf * single(targetPct10)) + (P.servoKp * e) + st.integ;

if u > single(P.outMax)
    u = single(P.outMax);
elseif u < single(-P.outMax)
    u = single(-P.outMax);
end

% 输出死区内是静摩擦噪声:滑行(0),让 I 项决定何时值得再推
if (u < single(P.outDeadband)) && (u > single(-P.outDeadband))
    duty = 0;
    return
end

duty = fix(u);   % C 的 (sint16) 截断,与 fix() 一致
end
