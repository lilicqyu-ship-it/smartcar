/*
 * CalibSession.swift — TC275 编码器判向标定台架会话（纯逻辑，无 I/O）。
 * 协议两端同源契约：tc275_car doc/34 §8/§9 ↔ esp32c6_car doc/17 §8/§9。
 *
 *   - 判向运行窗口：一次确认只发一帧 0x70；车端逐轮 250 ms 脉冲共约 1.4 s；
 *     回执窗口 3 s，超窗只显示"待确认"，迟到的 0x22 回执仍更新结果
 *     （DFlash 等静止/重试可能远超 3 s，doc 17 §2.4）；
 *   - 逐电机开环点动：按住以 30 Hz 发 0x71 {motor, duty ±500}，松手补一帧
 *     duty=0；固件侧 300 ms 无刷新自动停是兜底（doc 34 §9.3）；
 *   - REC_SET 12 B 载荷编码 + 前端范围校验（fullScale 100..5000、
 *     wheelDia 30..200、pos 0..3 且四值唯一）；
 *   - recVsCalib 一致性判据：0x23.invert 与最近一轮 0x22.invert 逐轮比对
 *     （doc 17 §9，"标定到底有没有落库"的软件侧证据）。
 *
 * 发送路径与 JSON 接收路径由 AppState/LinkEngine 持有；本结构体只产出
 * 要发送的载荷与供 UI 读取的派生状态。
 */

import Foundation

// ---- wire models (C6 bridge JSON ↔ Swift) -----------------------------------

/// EVT 0x22 → `{"t":"cal","status":n,"saved":n,"invert":[..],"delta":[..]}`
public struct CalibResult: Equatable, Sendable {
    /// 0 完成 / 1 急停中止 / 2 忙（已有标定在跑）
    public let status: Int
    /// 0 未持久化 / 1 已写 DFlash / 2 写失败；nil = 缺失或非法 → 显示待确认
    public let saved: Int?
    /// 各轮符号终值，下标 = 轮号 A..D（非位置）
    public let invert: [Int]
    /// 各轮脉冲计数 delta；status != 0 时未测轮为 0
    public let delta: [Int]

    public init(status: Int, saved: Int?, invert: [Int], delta: [Int]) {
        self.status = status
        self.saved = saved
        self.invert = invert
        self.delta = delta
    }

    public var statusText: String {
        switch status {
        case 0: "标定完成"
        case 1: "急停中止"
        case 2: "忙（已有标定进行中）"
        default: "状态 \(status)"
        }
    }

    public var savedText: String {
        switch saved {
        case 1: "已写入 DFlash"
        case 0: "未持久化"
        case 2: "写入失败"
        default: "保存状态待确认"
        }
    }

    public var savedOK: Bool { saved == 1 }
    public var savedBad: Bool { saved == 2 }
}

/// EVT 0x23 → `{"t":"rec","ver":n,"src":n,"pos":[..],"invert":[..],
///              "fullScale":n,"wheelDia":n,"crcOk":n}`
public struct CalibRecord: Equatable, Sendable {
    /// 记录布局版本（CALIB_REC_VER）
    public let ver: Int
    /// 0 默认值 / 1 DFlash / 2 在线设置
    public let src: Int
    /// 下标 = 轮号 A..D，值 = 位置（0 前左 / 1 前右 / 2 后左 / 3 后右）
    public let pos: [Int]
    /// 当前生效计数方向符号（下标 = 轮号 A..D）
    public let invert: [Int]
    public let fullScaleMmS: Int
    public let wheelDiaMm: Int
    public let imuAxis: [Int]
    public let trackMm: Int
    /// 0 = 普通回显/写入等待，1 = DFlash 已验证，2 = 失败，3 = 拒绝。
    public let imuSaved: Int?
    /// 独立车轮判向标志；旧固件无此字段时仅按旧记录来源兼容推断。
    public let wheelCalibrated: Bool
    /// 0 = DFlash 校验/范围失败已回落默认
    public let crcOk: Bool

    public init(ver: Int, src: Int, pos: [Int], invert: [Int],
                fullScaleMmS: Int, wheelDiaMm: Int, crcOk: Bool,
                imuAxis: [Int] = [0, 0, 0], trackMm: Int = 0, imuSaved: Int? = nil,
                wheelCalibrated: Bool? = nil) {
        self.ver = ver
        self.src = src
        self.pos = pos
        self.invert = invert
        self.fullScaleMmS = fullScaleMmS
        self.wheelDiaMm = wheelDiaMm
        self.crcOk = crcOk
        self.imuAxis = imuAxis
        self.trackMm = trackMm
        self.imuSaved = imuSaved
        self.wheelCalibrated = wheelCalibrated ?? (src != 0)
    }

    public var srcText: String {
        switch src {
        case 1: "DFlash"
        case 2: "在线设置"
        default: "默认值"
        }
    }
}

/// 轮号 A..D（线协议 motor 0..3，驱动通道）与物理位置的词汇表。
public enum WheelChannel {
    public static let letters = ["A", "B", "C", "D"]
    public static let posLabels = ["前左", "前右", "后左", "后右"]

    /// 编译期默认位置表（tc275 doc/23 §3 / calib_record.c fillDefaults）：
    /// A 前左、B 后左、C 后右、D 前右。仅作 REC_GET 到达前的占位。
    public static let defaultPos = [0, 2, 3, 1]

    public static func posLabel(_ pos: Int) -> String {
        posLabels.indices.contains(pos) ? posLabels[pos] : "位置\(pos)"
    }
}

/// 单轮判读（doc 34 §5.2 与页面文案共用）：delta<0 已自动翻转（正常）；
/// delta==0 无计数（查接线，禁止手改 invert 掩盖）。
public func calibVerdict(delta: Int, runComplete: Bool) -> (text: String, kind: CalibVerdictKind) {
    if delta < 0 { return ("已翻转 −1", .flipped) }
    if delta == 0 { return runComplete ? ("无计数 · 查接线", .dead) : ("未测", .untested) }
    return ("正常", .ok)
}

public enum CalibVerdictKind: Equatable, Sendable {
    case ok, flipped, dead, untested
}

// ---- session ----------------------------------------------------------------

/// 按住即转的一帧点动指令（0x71 载荷 = {motor u8, duty i16LE}，op 由 C6 前置）。
public struct JogFrame: Equatable, Sendable {
    public let channel: Int
    public let duty: Int16

    /// 载荷与 TC275 `CALIBREC_jogDecode` 对齐：motor@0，duty i16LE@1..2，
    /// 双端各钳 ±500（percent×10 = ±50 %）。
    public var payload: [UInt8] {
        let d = max(-500, min(500, Int(duty)))
        return [UInt8(channel)] + Wire.putI16(Int16(d))
    }
}

public struct CalibSession: Sendable {
    /// 车端脉冲总时长：4 × (250 ms 脉冲 + 80 ms 停顿) ≈ 1.4 s
    public static let pulseTrainS: Double = 1.4
    /// 回执窗口：窗口内禁再触发（防连点 + 互锁），超窗转"待确认"文案
    public static let receiptWindowS: Double = 3.0
    /// 点动刷新率（与控制流同拍）
    public static let jogRateHz: Int = 30
    public static let jogDuty: Int16 = 500 // percent×10 = ±50 %
    public static let fullScaleRange = 100...5000
    public static let wheelDiaRange = 30...200

    /// 最近一次 tick 的时间基准（UI 进度条与窗口判定的时钟）
    public private(set) var nowMs: Double = 0
    /// 本端发出 0x70 的时刻；0 = 空闲
    public private(set) var sentAtMs: Double = 0
    /// 已发出 0x70 且尚未收到任何 0x22 回执（含迟到回执场景）
    public private(set) var pending = false
    /// 最近一次判向结果（EVT 0x22）
    public private(set) var result: CalibResult?
    /// 最近一次生效参数（EVT 0x23）
    public private(set) var record: CalibRecord?
    /// 四轮离地确认（进入运行窗口的前置之一；断开连接时由 AppState 复位）
    public var wheelsOffConfirmed = false

    // ---- jog ----
    /// 按住中的通道（nil = 无）；同一时刻只允许一轮
    public private(set) var jogChannel: Int?
    public private(set) var jogDutyNow: Int16 = 0
    /// 本轮会话用过点动（流程③完成标记，与 web `refreshFlow` 同义）
    public private(set) var jogUsed = false
    /// jogcnt 事件（EVT 0x26，10 Hz）：按压期间各通道编码器计数增量
    public private(set) var jogCountOn = false
    public private(set) var jogCounts: [Int] = [0, 0, 0, 0]

    public init() {}

    // ---- calibration run ----------------------------------------------------

    /// 运行窗口：发出后 3 s 内（按钮锁存 + 防连点 + DRIVE 抑制）
    public var windowOpen: Bool {
        sentAtMs > 0 && nowMs - sentAtMs < Self.receiptWindowS * 1000
    }

    /// 窗口已关但回执未到 → "待确认"文案（不报失败、不复位，迟到回执仍更新）
    public var awaitingLateReceipt: Bool {
        pending && !windowOpen
    }

    /// 进度 0...1（1.4 s 脉冲列车）；窗口外恒 1
    public var progress: Double {
        guard windowOpen else { return 1 }
        return min(1, (nowMs - sentAtMs) / (Self.pulseTrainS * 1000))
    }

    public mutating func setWheelsOffConfirmed(_ on: Bool) {
        wheelsOffConfirmed = on
    }

    /// 开始一次判向：调用方先过门（连接 + CTRL + TC 在线 + 离地确认 +
    /// 窗口互斥），这里只负责记账。一次调用对应恰好一帧 0x70。
    public mutating func start(nowMs: Double) {
        self.nowMs = nowMs
        sentAtMs = nowMs
        pending = true
        result = nil // 新一轮替换旧结果表（doc 34 §9.4：旧 DONE 不再代表本轮）
    }

    /// EVT 0x22 到达：任何 status 都终结等待；迟到的回执同样更新结果。
    public mutating func receive(result: CalibResult, nowMs: Double) {
        self.nowMs = nowMs
        self.result = result
        pending = false
    }

    /// EVT 0x23 到达：当前生效参数回显（REC_GET/SET/CLEAR 共用）。
    public mutating func receive(record: CalibRecord) {
        self.record = record
    }

    public mutating func receiveJogCount(on: Bool, deltas: [Int]) {
        jogCountOn = on
        if deltas.count == 4 { jogCounts = deltas }
    }

    /// 30 Hz 心跳：推进窗口/进度时钟
    public mutating func tick(nowMs: Double) {
        self.nowMs = nowMs
    }

    /// 链路断开：车端会因失联中止标定，本地不再等待回执；点动状态就地清除
    ///（发不出帧，纯状态清理）。结果表保留供查看。
    public mutating func linkDown() {
        pending = false
        sentAtMs = 0
        jogChannel = nil
        jogDutyNow = 0
    }

    // ---- jog engine ---------------------------------------------------------

    /// 按住即转：返回要立即发出的 0x71 帧。互斥与门禁由调用方把守。
    public mutating func jogPress(channel: Int, forward: Bool, nowMs: Double) -> JogFrame? {
        guard (0...3).contains(channel) else { return nil }
        self.nowMs = nowMs
        jogChannel = channel
        jogDutyNow = forward ? Self.jogDuty : -Self.jogDuty
        jogUsed = true
        return JogFrame(channel: channel, duty: jogDutyNow)
    }

    /// 松手：补一帧 duty=0（newest-wins；固件 300 ms 超时兜底）
    public mutating func jogRelease() -> JogFrame? {
        guard let channel = jogChannel else { return nil }
        jogChannel = nil
        jogDutyNow = 0
        return JogFrame(channel: channel, duty: 0)
    }

    /// 30 Hz 续期帧（按住期间每个控制拍调用一次）
    public func jogKeepalive() -> JogFrame? {
        guard let channel = jogChannel else { return nil }
        return JogFrame(channel: channel, duty: jogDutyNow)
    }

    // ---- record ↔ result consistency (doc 17 §9 recVsCalib) -------------------

    /// 0x23.invert 与最近一轮 0x22.invert 不一致的轮号（A..D 下标）；
    /// 任一侧缺失返回空（无判据时不谎报一致）。
    public var invertMismatches: [Int] {
        guard let rec = record, let res = result, res.status == 0 else { return [] }
        guard rec.invert.count == 4, res.invert.count == 4 else { return [] }
        return (0..<4).filter { rec.invert[$0] != res.invert[$0] }
    }
}

// ---- payload builders ---------------------------------------------------------

public enum CalibWire {
    /// 0x73 REC_SET 12 B：pos u8×4 @0..3、invert i8×4 @4..7、
    /// fullScale i16LE @8..9、wheelDia i16LE @10..11（ver/src 由固件填写，
    /// 不在线上载荷里）。
    public static func recSet(pos: [Int], invert: [Int],
                              fullScaleMmS: Int, wheelDiaMm: Int) -> [UInt8] {
        var p = [UInt8](repeating: 0, count: 12)
        for i in 0..<4 {
            p[i] = UInt8(clamping: pos.count == 4 ? pos[i] : 0)
            p[4 + i] = UInt8(bitPattern: Int8(clamping: invert.count == 4 ? invert[i] : 1))
        }
        p.replaceSubrange(8..<10, with: Wire.putI16(Int16(clamping: fullScaleMmS)))
        p.replaceSubrange(10..<12, with: Wire.putI16(Int16(clamping: wheelDiaMm)))
        return p
    }

    /// REC_SET 前端校验（doc 17 §8.3）：范围 + pos 四值唯一 + invert ∈ {±1}。
    public static func recSetValid(pos: [Int], invert: [Int],
                                   fullScaleMmS: Int, wheelDiaMm: Int) -> Bool {
        guard pos.count == 4, invert.count == 4 else { return false }
        guard Set(pos).count == 4, pos.allSatisfy({ (0...3).contains($0) }) else { return false }
        guard invert.allSatisfy({ $0 == 1 || $0 == -1 }) else { return false }
        return CalibSession.fullScaleRange.contains(fullScaleMmS)
            && CalibSession.wheelDiaRange.contains(wheelDiaMm)
    }
}
