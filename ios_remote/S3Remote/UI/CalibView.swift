/*
 * CalibView.swift — 台架标定页（TC275 DPT 0x70~0x74 的 iOS 端）。
 * 与 C6 Web 标定页 /calib.html 同一份流程契约（esp32c6_car doc/17 §9）：
 *   ① 安全前提（连接/控制权/TC 在线/四轮离地确认）→ ② 判向标定（一次确认
 *   一帧、3 s 回执窗口、迟到回执仍更新）→ ③ 点动复核（按住即转 + 编码器
 *   计数反馈）→ ④ 生效参数与持久化（REC_GET/SET/CLEAR + 落库一致性判据）。
 * 常驻 STOP 不受任何互锁限制。
 */

import SwiftUI

struct CalibView: View {
    @Environment(AppState.self) private var app
    @State private var confirmRun = false
    @State private var confirmClear = false
    // ④ 的编辑缓冲：仅在回执与上次种子不同时回填，避免打断输入
    @State private var fsText = ""
    @State private var wdText = ""
    @State private var pos: [Int] = WheelChannel.defaultPos
    @State private var seededRecord: CalibRecord?

    var body: some View {
        Page {
            ScrollView {
                VStack(spacing: 18) {
                    PageHeading(eyebrow: "CALIB / TC275 · DPT",
                                title: "台架标定",
                                subtitle: "编码器判向 · 点动复核 · 参数持久化（0x70–0x74）")
                    safetyBar
                    prereqCard
                    directionCard
                    jogCard
                    recordCard
                }
                .padding(20)
                .frame(maxWidth: 680)
                .frame(maxWidth: .infinity)
            }
        }
        .task {
            app.calibRecGet() // 进入页面读回当前生效参数（doc 17 §8.3）
            seedFromRecord()
        }
        .onChange(of: app.calib.record) { _, _ in seedFromRecord() }
        .onDisappear { app.calibLeave() } // 离页先停点动
        .confirmationDialog("开始判向标定？", isPresented: $confirmRun, titleVisibility: .visible) {
            Button("开始（车轮将逐个转动）") {
                Haptics.medium()
                app.startDirectionCalib()
            }
            Button("取消", role: .cancel) {}
        } message: {
            Text("每轮将施加 250 ms 脉冲，共约 1.4 秒。请确认车辆四轮离地、周围无人。")
        }
        .confirmationDialog("擦除标定记录？", isPresented: $confirmClear, titleVisibility: .visible) {
            Button("擦除并恢复默认", role: .destructive) {
                Haptics.medium()
                app.calibRecClear()
            }
            Button("取消", role: .cancel) {}
        } message: {
            Text("TC275 将擦除 DFlash 记录并回到默认值，闭环使能门随之关闭（开环等价），需重新判向。")
        }
    }

    // ---- 常驻安全条 ------------------------------------------------------------

    private var safetyBar: some View {
        HStack(spacing: 14) {
            VStack(alignment: .leading, spacing: 3) {
                Text(app.calibPrereqMissing.isEmpty
                     ? "前提齐备 · 可以上台架操作"
                     : "缺 \(app.calibPrereqMissing.count) 项：\(app.calibPrereqMissing.joined(separator: "、"))")
                    .font(.subheadline.weight(.semibold))
                Text("STOP 任何状态可用 · 标定时请勿同时操作其他控制端")
                    .font(.caption)
                    .foregroundStyle(Theme.dim)
            }
            Spacer(minLength: 0)
            Button {
                Haptics.medium()
                app.calibStop()
            } label: {
                Text("STOP")
                    .font(Theme.mono(13)).tracking(1)
                    .foregroundStyle(.white)
                    .frame(minWidth: 88, minHeight: 46)
                    .background(Theme.stopGradient, in: RoundedRectangle(cornerRadius: 15))
            }
            .buttonStyle(.plain)
            .accessibilityLabel("停止车辆")
        }
        .padding(14)
        .background(Theme.stopRed.opacity(0.08), in: RoundedRectangle(cornerRadius: 20))
        .overlay(RoundedRectangle(cornerRadius: 20).strokeBorder(Theme.stopRed.opacity(0.25)))
    }

    // ---- ① 安全前提 -------------------------------------------------------------

    private var missingSuffix: String {
        app.calibPrereqMissing.isEmpty ? "" : " · 缺 \(app.calibPrereqMissing.count) 项"
    }

    private var prereqCard: some View {
        Panel {
            VStack(alignment: .leading, spacing: 12) {
                Label("① 安全前提\(missingSuffix)", systemImage: "checklist")
                    .font(.headline)
                checkRow("WebSocket 已连接", ok: app.connState == .connected)
                checkRow("控制权（CTRL 角色）", ok: app.ctrlRole)
                checkRow("TC275 车端在线", ok: app.tcUp)
                Divider()
                Toggle(isOn: Binding(
                    get: { app.calib.wheelsOffConfirmed },
                    set: { app.setWheelsOffConfirmed($0) })) {
                    Text("我已确认四轮离地（标定将逐轮转动车轮）")
                        .font(.subheadline)
                }
                .tint(Theme.accent)
            }
        }
    }

    private func checkRow(_ text: String, ok: Bool) -> some View {
        HStack(spacing: 10) {
            Image(systemName: ok ? "checkmark.circle.fill" : "circle")
                .foregroundStyle(ok ? Theme.live : Theme.dim)
            Text(text).font(.subheadline)
            Spacer()
            Text(ok ? "就绪" : "未就绪")
                .font(.caption.weight(.semibold))
                .foregroundStyle(ok ? Theme.live : Theme.dim)
        }
    }

    // ---- ② 判向标定 ---------------------------------------------------------------

    private var directionCard: some View {
        Panel {
            VStack(alignment: .leading, spacing: 14) {
                Label("② 编码器判向标定", systemImage: "arrow.triangle.2.circlepath")
                    .font(.headline)
                Text("每轮 250 ms 脉冲共约 1.4 s；完成后 TC275 自动写入 DFlash 并打开闭环使能门。")
                    .font(.caption)
                    .foregroundStyle(Theme.dim)

                Button {
                    Haptics.light()
                    confirmRun = true // 二次确认：一次确认只发一帧（防连点靠窗口互斥）
                } label: {
                    Text(app.calib.windowOpen ? "标定进行中…" : "开始判向标定")
                        .font(.subheadline.weight(.heavy))
                        .foregroundStyle(.white)
                        .frame(maxWidth: .infinity, minHeight: 46)
                        .background(app.calibStartGateOpen
                                    ? AnyShapeStyle(Theme.accentGradient)
                                    : AnyShapeStyle(Theme.dim.opacity(0.35)),
                                    in: RoundedRectangle(cornerRadius: 14))
                }
                .buttonStyle(.plain)
                .disabled(!app.calibStartGateOpen)

                if app.calib.windowOpen {
                    VStack(alignment: .leading, spacing: 6) {
                        ProgressView(value: app.calib.progress)
                            .tint(Theme.accent)
                        Text("标定中 · 车轮将逐个转动，请保持四轮离地")
                            .font(.caption)
                            .foregroundStyle(Theme.warn)
                    }
                } else if app.calib.awaitingLateReceipt {
                    Text("尚未收到最终标定与保存回执，保存状态待确认。请保持车辆静止；回执到达后会自动更新，也可查看 TC275 串口。")
                        .font(.caption)
                        .foregroundStyle(Theme.warn)
                } else if let result = app.calib.result {
                    HStack(spacing: 10) {
                        Text(result.statusText)
                            .font(.subheadline.weight(.bold))
                            .foregroundStyle(resultColor(result))
                        Text(result.savedText)
                            .font(.caption.weight(.semibold))
                            .foregroundStyle(savedColor(result))
                        Spacer()
                    }
                }

                resultTable
            }
        }
    }

    private func resultColor(_ r: CalibResult) -> Color {
        switch r.status {
        case 0: r.saved == 2 ? Theme.crit : Theme.live
        case 1: Theme.crit
        default: Theme.warn
        }
    }

    private func savedColor(_ r: CalibResult) -> Color {
        switch r.saved {
        case 1: Theme.live
        case 2: Theme.crit
        default: Theme.warn
        }
    }

    /// 结果表：轮 / 位置 / delta / 计数方向 / 结论（doc 17 §4.2 文案）
    private var resultTable: some View {
        VStack(spacing: 6) {
            HStack {
                tableCell("轮", width: 30)
                tableCell("位置", width: 46)
                Spacer()
                tableCell("delta", width: 64)
                tableCell("结论", width: 108)
            }
            .font(.caption2.weight(.semibold))
            .foregroundStyle(Theme.dim)

            ForEach(0..<4, id: \.self) { i in
                HStack(spacing: 0) {
                    Text(WheelChannel.letters[i])
                        .font(Theme.mono(12, weight: .bold))
                        .foregroundStyle(Theme.info)
                        .frame(width: 30, height: 26)
                        .background(Theme.info.opacity(0.12), in: Circle())
                        .padding(.trailing, 0)
                    Text(WheelChannel.posLabel(posIndex(i)))
                        .font(.caption)
                        .frame(width: 76, alignment: .leading)
                        .padding(.leading, 6)
                    Spacer()
                    Text(app.calib.result.map { String($0.delta[i]) } ?? "--")
                        .font(Theme.mono(13))
                        .frame(width: 64, alignment: .trailing)
                    verdictChip(i)
                        .frame(width: 108, alignment: .trailing)
                }
            }
        }
    }

    private func posIndex(_ channel: Int) -> Int {
        app.calib.record?.pos[channel] ?? WheelChannel.defaultPos[channel]
    }

    private func verdictChip(_ channel: Int) -> some View {
        let verdict: (text: String, kind: CalibVerdictKind)
        if let res = app.calib.result {
            verdict = calibVerdict(delta: res.delta[channel], runComplete: res.status == 0)
        } else {
            verdict = ("待标定", .untested)
        }
        let color: Color = switch verdict.kind {
        case .ok: Theme.live
        case .flipped: Theme.warn
        case .dead: Theme.crit
        case .untested: Theme.dim
        }
        let invert = app.calib.result.map { $0.invert[channel] < 0 ? "−1" : "+1" } ?? "--"
        return HStack(spacing: 5) {
            Text(invert).font(Theme.mono(11))
            Text(verdict.text).font(.caption2.weight(.semibold))
        }
        .foregroundStyle(color)
    }

    private func tableCell(_ text: String, width: CGFloat) -> some View {
        Text(text).frame(width: width, alignment: text == "delta" || text == "结论" ? .trailing : .leading)
    }

    // ---- ③ 点动复核 ----------------------------------------------------------------

    private var jogCard: some View {
        Panel {
            VStack(alignment: .leading, spacing: 14) {
                Label("③ 点动复核 · 车辆实时", systemImage: "hand.tap.fill")
                    .font(.headline)
                Text("按住即转（开环、不过伺服），松手即停；轮下的编码器计数增量用于复核②的方向判定。")
                    .font(.caption)
                    .foregroundStyle(Theme.dim)

                BenchCarView(jogChannel: app.calib.jogChannel,
                             pos: posIndexAll,
                             faultGated: app.jogFaultGated)

                sideGauge("左", target: liveTele?.vTargetL, meas: liveTele?.vMeasL)
                sideGauge("右", target: liveTele?.vTargetR, meas: liveTele?.vMeasR)
                Divider()
                ForEach(0..<4, id: \.self) { i in
                    jogRow(i)
                    if i < 3 { Divider() }
                }
            }
        }
    }

    private var posIndexAll: [Int] {
        (0..<4).map { posIndex($0) }
    }

    private var liveTele: Telemetry? { app.teleFresh ? app.telemetry : nil }

    private func sideGauge(_ side: String, target: Int16?, meas: Int16?) -> some View {
        HStack {
            Text(side)
                .font(Theme.mono(13, weight: .bold))
                .foregroundStyle(Theme.info)
                .frame(width: 30, height: 30)
                .background(Theme.info.opacity(0.12), in: Circle())
            Text("目标 \(target.map { String($0) } ?? "--")")
                .font(Theme.mono(14))
            Spacer()
            Text("实测 \(meas.map { String($0) } ?? "--")")
                .font(Theme.mono(14, weight: .bold))
                .foregroundStyle(Theme.accent)
        }
    }

    private func jogRow(_ channel: Int) -> some View {
        let active = app.calib.jogChannel == channel
        return VStack(spacing: 6) {
            HStack(spacing: 10) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("\(WheelChannel.letters[channel]) · \(WheelChannel.posLabel(posIndex(channel)))")
                        .font(.subheadline.weight(.semibold))
                    Text(flippedNote(channel))
                        .font(.caption2)
                        .foregroundStyle(Theme.dim)
                }
                Spacer(minLength: 0)
                if active {
                    Text(app.calib.jogDutyNow > 0 ? "+50 % duty" : "−50 % duty")
                        .font(Theme.mono(11, weight: .bold))
                        .foregroundStyle(Theme.accent)
                }
                holdButton("backward", symbol: "arrow.down", channel: channel, forward: false)
                holdButton("forward", symbol: "arrow.up", channel: channel, forward: true)
            }
            if active {
                Text("编码器计数 Δ \(app.calib.jogCounts[channel])")
                    .font(Theme.mono(11))
                    .foregroundStyle(Theme.dim)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
    }

    /// 复核行下的②结论（web `#jv0..3` 同源语义）
    private func flippedNote(_ channel: Int) -> String {
        guard let res = app.calib.result, res.status == 0 else { return "尚未标定" }
        let v = calibVerdict(delta: res.delta[channel], runComplete: true)
        return "② \(v.text)"
    }

    private func holdButton(_ label: String, symbol: String,
                            channel: Int, forward: Bool) -> some View {
        let active = app.calib.jogChannel == channel
            && app.calib.jogDutyNow == (forward ? CalibSession.jogDuty : -CalibSession.jogDuty)
        return Image(systemName: symbol)
            .font(.system(size: 15, weight: .bold))
            .frame(width: 46, height: 46)
            .background(active ? Theme.accent : Theme.bgLift,
                        in: RoundedRectangle(cornerRadius: 13))
            .foregroundStyle(active ? Color.white : Theme.text)
            .overlay(RoundedRectangle(cornerRadius: 13)
                .strokeBorder(Theme.panelStroke.opacity(0.6)))
            .onLongPressGesture(minimumDuration: .infinity, maximumDistance: 60,
                                pressing: { pressing in
                                    if pressing {
                                        Haptics.light()
                                        app.jogPress(channel: channel, forward: forward)
                                    } else {
                                        app.jogRelease()
                                    }
                                }, perform: {})
            .disabled(!app.jogGateOpen && !active)
            .opacity(app.jogGateOpen || active ? 1 : 0.45)
            .accessibilityLabel("\(WheelChannel.letters[channel]) 通道\(forward ? "向前" : "向后")点动")
    }

    // ---- ④ 生效参数与持久化 --------------------------------------------------------

    private var recordCard: some View {
        Panel {
            VStack(alignment: .leading, spacing: 14) {
                HStack {
                    Label("④ 生效参数与持久化", systemImage: "internaldrive")
                        .font(.headline)
                    Spacer()
                    Button {
                        Haptics.light()
                        app.calibRecGet()
                    } label: {
                        Label("读回", systemImage: "arrow.clockwise")
                            .font(.caption.weight(.semibold))
                    }
                    .buttonStyle(.borderless)
                    .disabled(app.connState != .connected || !app.ctrlRole)
                }

                if let rec = app.calib.record {
                    HStack(spacing: 8) {
                        chip("来源：\(rec.srcText)", color: srcColor(rec))
                        chip(rec.crcOk ? "记录校验通过" : "DFlash 校验失败 · 已回落默认",
                             color: rec.crcOk ? Theme.live : Theme.crit)
                        Spacer(minLength: 0)
                    }
                    consistencyLine
                    paramRows(rec)
                    editorFields(rec)
                    actionButtons(rec)
                    Text("REC_SET 回执为参数生效回显，最终写入以标定回执 saved 为准；位置仅是记录元数据，不交换驱动通道。")
                        .font(.caption2)
                        .foregroundStyle(Theme.dim)
                } else {
                    Text("尚未读到生效参数（REC_GET 回执 0x23 到达后显示）。")
                        .font(.caption)
                        .foregroundStyle(Theme.dim)
                }
            }
        }
    }

    private func srcColor(_ rec: CalibRecord) -> Color {
        switch rec.src {
        case 1: Theme.accent
        case 2: Theme.info
        default: Theme.dim
        }
    }

    /// recVsCalib：0x23.invert 与最近一轮 0x22.invert 逐轮比对
    @ViewBuilder
    private var consistencyLine: some View {
        let bad = app.calib.invertMismatches
        if !bad.isEmpty {
            Text("与最近一次标定不一致 ⚠（\(bad.map { WheelChannel.letters[$0] }.joined(separator: "、"))）— 标定可能未落库")
                .font(.caption.weight(.semibold))
                .foregroundStyle(Theme.crit)
        } else if app.calib.result?.status == 0 {
            Text("与最近一次标定一致")
                .font(.caption.weight(.semibold))
                .foregroundStyle(Theme.live)
        } else {
            Text("完成一次判向后可在此对照落库结果")
                .font(.caption)
                .foregroundStyle(Theme.dim)
        }
    }

    private func paramRows(_ rec: CalibRecord) -> some View {
        VStack(spacing: 8) {
            ForEach(0..<4, id: \.self) { i in
                HStack {
                    Text("\(WheelChannel.letters[i]) · \(WheelChannel.posLabel(rec.pos[i]))")
                        .font(.subheadline)
                        .frame(width: 92, alignment: .leading)
                    Spacer()
                    Menu {
                        ForEach(0..<4, id: \.self) { p in
                            Button(WheelChannel.posLabels[p]) { pos[i] = p }
                        }
                    } label: {
                        HStack(spacing: 4) {
                            Text(WheelChannel.posLabel(pos.count == 4 ? pos[i] : rec.pos[i]))
                            Image(systemName: "chevron.up.chevron.down")
                                .font(.caption2)
                        }
                        .font(.caption.weight(.semibold))
                        .padding(.horizontal, 10)
                        .padding(.vertical, 6)
                        .background(Theme.bgLift, in: RoundedRectangle(cornerRadius: 9))
                    }
                    Text(rec.invert[i] < 0 ? "−1 已翻转" : "+1")
                        .font(Theme.mono(12))
                        .foregroundStyle(rec.invert[i] < 0 ? Theme.warn : Theme.dim)
                        .frame(width: 78, alignment: .trailing)
                }
            }
            if Set(pos).count != 4 {
                Text("四个位置必须唯一，否则无法写入")
                    .font(.caption2.weight(.semibold))
                    .foregroundStyle(Theme.warn)
            }
        }
    }

    private func editorFields(_ rec: CalibRecord) -> some View {
        HStack(spacing: 12) {
            field("满量程 mm/s", text: $fsText, hint: "100..5000",
                  ok: CalibSession.fullScaleRange.contains(Int(fsText) ?? 0))
            field("轮径 mm", text: $wdText, hint: "30..200",
                  ok: CalibSession.wheelDiaRange.contains(Int(wdText) ?? 0))
        }
    }

    private func field(_ title: String, text: Binding<String>, hint: String, ok: Bool) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(title).font(.caption).foregroundStyle(Theme.dim)
            TextField(hint, text: text)
                .keyboardType(.numberPad)
                .font(Theme.mono(15))
                .padding(.horizontal, 12)
                .frame(height: 40)
                .background(Theme.bgLift, in: RoundedRectangle(cornerRadius: 10))
                .overlay(RoundedRectangle(cornerRadius: 10)
                    .strokeBorder(ok ? Theme.panelStroke : Theme.warn, lineWidth: 1))
        }
        .frame(maxWidth: .infinity)
    }

    private func actionButtons(_ rec: CalibRecord) -> some View {
        HStack(spacing: 12) {
            Button {
                Haptics.medium()
                app.calibRecSet(pos: pos, invert: rec.invert,
                                fullScaleMmS: Int(fsText) ?? 0,
                                wheelDiaMm: Int(wdText) ?? 0)
            } label: {
                Text("写入参数")
                    .font(.subheadline.weight(.heavy))
                    .foregroundStyle(.white)
                    .frame(maxWidth: .infinity, minHeight: 44)
                    .background(canWrite(rec)
                                ? AnyShapeStyle(Theme.accentGradient)
                                : AnyShapeStyle(Theme.dim.opacity(0.35)),
                                in: RoundedRectangle(cornerRadius: 13))
            }
            .buttonStyle(.plain)
            .disabled(!canWrite(rec))

            Button {
                Haptics.light()
                confirmClear = true
            } label: {
                Text("恢复默认")
                    .font(.subheadline.weight(.heavy))
                    .foregroundStyle(Theme.stopRed)
                    .frame(maxWidth: .infinity, minHeight: 44)
                    .background(Theme.stopRed.opacity(0.08), in: RoundedRectangle(cornerRadius: 13))
                    .overlay(RoundedRectangle(cornerRadius: 13)
                        .strokeBorder(Theme.stopRed.opacity(0.3)))
            }
            .buttonStyle(.plain)
            .disabled(app.connState != .connected || !app.ctrlRole)
        }
    }

    private func canWrite(_ rec: CalibRecord) -> Bool {
        app.connState == .connected && app.ctrlRole
            && CalibWire.recSetValid(pos: pos, invert: rec.invert,
                                     fullScaleMmS: Int(fsText) ?? 0,
                                     wheelDiaMm: Int(wdText) ?? 0)
    }

    private func chip(_ text: String, color: Color) -> some View {
        Text(text)
            .font(.caption2.weight(.semibold))
            .padding(.horizontal, 9)
            .padding(.vertical, 5)
            .background(color.opacity(0.12), in: Capsule())
            .foregroundStyle(color)
    }

    private func seedFromRecord() {
        guard let rec = app.calib.record, rec != seededRecord else { return }
        seededRecord = rec
        fsText = String(rec.fullScaleMmS)
        wdText = String(rec.wheelDiaMm)
        pos = rec.pos
    }
}

// ---- 车辆俯视图（jog 高亮，doc 17 §8.2 的 iOS 简化版） ---------------------------

struct BenchCarView: View {
    let jogChannel: Int?
    /// 通道 → 位置值（0 前左 / 1 前右 / 2 后左 / 3 后右）
    let pos: [Int]
    let faultGated: Bool

    /// 位置值 → 画布角落（车头朝上）
    private static let corner: [CGPoint] = [
        CGPoint(x: 0, y: 0), // 前左
        CGPoint(x: 1, y: 0), // 前右
        CGPoint(x: 0, y: 1), // 后左
        CGPoint(x: 1, y: 1), // 后右
    ]

    var body: some View {
        HStack(spacing: 18) {
            car
            VStack(alignment: .leading, spacing: 5) {
                Text("车头朝上").font(.caption2).foregroundStyle(Theme.dim)
                ForEach(0..<4, id: \.self) { i in
                    HStack(spacing: 5) {
                        Circle()
                            .fill(jogChannel == i ? Theme.accent : Theme.panelStroke.opacity(0.7))
                            .frame(width: 7, height: 7)
                        Text("\(WheelChannel.letters[i]) \(WheelChannel.posLabel(pos.indices.contains(i) ? pos[i] : 0))")
                            .font(.caption2)
                            .foregroundStyle(jogChannel == i ? Theme.accent : Theme.text)
                    }
                }
                if faultGated {
                    Text("⚠ 故障锁存：车端会拒绝点动")
                        .font(.caption2.weight(.semibold))
                        .foregroundStyle(Theme.warn)
                }
            }
            Spacer(minLength: 0)
        }
        .frame(maxWidth: .infinity)
        .padding(14)
        .background(Theme.bgLift.opacity(0.6), in: RoundedRectangle(cornerRadius: 16))
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("车辆俯视图；点动中的通道已高亮")
    }

    private var car: some View {
        let W: CGFloat = 190, H: CGFloat = 116
        let wheelW: CGFloat = 20, wheelH: CGFloat = 36
        return ZStack {
            RoundedRectangle(cornerRadius: 18)
                .fill(Theme.panel)
                .overlay(RoundedRectangle(cornerRadius: 18)
                    .strokeBorder(faultGated ? Theme.crit : Theme.panelStroke, lineWidth: faultGated ? 2 : 1))
                .frame(width: W, height: H)
            // 挡风玻璃示意（车头朝上）
            RoundedRectangle(cornerRadius: 6)
                .fill(Theme.info.opacity(0.14))
                .frame(width: W * 0.56, height: 18)
                .offset(y: -H * 0.18)
            ForEach(0..<4, id: \.self) { i in
                let c = Self.corner[pos.indices.contains(i) ? pos[i] : 0]
                let active = jogChannel == i
                RoundedRectangle(cornerRadius: 5)
                    .fill(active ? Theme.accent : Theme.ink.opacity(0.82))
                    .frame(width: wheelW, height: wheelH)
                    .overlay(RoundedRectangle(cornerRadius: 5)
                        .strokeBorder(active ? Theme.accentDeep : .clear, lineWidth: 2))
                    .offset(x: c.x == 0 ? -W / 2 : W / 2,
                            y: c.y == 0 ? -H / 2 : H / 2)
                    .overlay {
                        Text(WheelChannel.letters[i])
                            .font(Theme.mono(9, weight: .bold))
                            .foregroundStyle(active ? .white : Theme.panel)
                            .offset(x: c.x == 0 ? -W / 2 : W / 2,
                                    y: c.y == 0 ? -H / 2 : H / 2)
                    }
            }
            Text("▲")
                .font(.caption2)
                .foregroundStyle(Theme.dim)
                .offset(y: -H / 2 - 10)
        }
        .frame(width: W + wheelW, height: H + wheelH)
    }
}
