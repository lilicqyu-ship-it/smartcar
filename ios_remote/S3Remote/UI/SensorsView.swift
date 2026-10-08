/*
 * SensorsView.swift — 传感器实况（sensor tab）: the car's raw IMU stream and
 * ToF 8×8 zone map, the fusion attitude instruments and the driving-guard
 * flag wall. Display-only by contract: every value here arrives via the
 * diagnostic JSON side-channel and nothing feeds back into driving.
 *
 * The attitude is the primary instrument; acceleration, raw axes and trends
 * follow it. The ToF card can also show the fresh fusion distance summary
 * while the optional 64-zone stream is unavailable.
 */

import SwiftUI

struct SensorsView: View {
    @Environment(AppState.self) private var app
    @State private var selectedSensor: SensorPage = .imu
    @State private var levelCapture: [Double]?
    @State private var noseCapture: [Double]?
    @State private var trackMmText = ""
    @State private var calibrationHint = ""

    private enum SensorPage: CaseIterable {
        case imu, tof

        var title: String {
            switch self { case .imu: "IMU 姿态"; case .tof: "ToF 测距" }
        }

        var symbol: String {
            switch self { case .imu: "rotate.3d"; case .tof: "camera.metering.matrix" }
        }
    }

    var body: some View {
        Page {
            ScrollViewReader { proxy in
                ScrollView {
                    VStack(spacing: 18) {
                        PageHeading(eyebrow: "LIVE TELEMETRY", title: "传感器实况",
                                    subtitle: "IMU 动态与前方距离 · 实时观察")
                            .id("sensor-top")
                        sensorSwitcher
                        streamBar
                        if selectedSensor == .imu {
                            imu3DPanel
                                .id("imu-panel")
                            imuCalibrationPanel
                            imuWavePanel
                        } else {
                            tofPanel
                            guardPanel
                        }
                    }
                    .padding(20)
                    .frame(maxWidth: 680)
                    .frame(maxWidth: .infinity)
                }
                .onChange(of: selectedSensor) { _, _ in
                    withAnimation(.easeInOut(duration: 0.2)) {
                        proxy.scrollTo("sensor-top", anchor: .top)
                    }
                }
                .onAppear {
                    app.calibRecGet()
                    // dev/screenshot hooks, same family as --tab/--no-alert
                    let args = ProcessInfo.processInfo.arguments
                    if args.contains("--sensors-bottom") {
                        selectedSensor = .tof
                        DispatchQueue.main.asyncAfter(deadline: .now() + 0.15) {
                            proxy.scrollTo("guard-panel", anchor: .bottom)
                        }
                    } else if args.contains("--sensors-tof") {
                        selectedSensor = .tof
                    } else if args.contains("--sensors-mid") {
                        proxy.scrollTo("imu-panel", anchor: .top)
                    }
                }
                .onChange(of: app.connState) { _, state in
                    if state == .connected { app.calibRecGet() }
                }
                .onChange(of: app.ctrlRole) { _, ownsControl in
                    if ownsControl { app.calibRecGet() }
                }
            }
        }
    }

    // ---- stream status + pause -------------------------------------------------

    private var sensorSwitcher: some View {
        HStack(spacing: 5) {
            ForEach(SensorPage.allCases, id: \.self) { page in
                Button {
                    Haptics.selection()
                    selectedSensor = page
                } label: {
                    HStack(spacing: 7) {
                        Image(systemName: page.symbol)
                            .font(.subheadline.weight(.semibold))
                        Text(page.title)
                            .font(.subheadline.weight(.semibold))
                    }
                    .foregroundStyle(selectedSensor == page ? Theme.text : Theme.dim)
                    .frame(maxWidth: .infinity, minHeight: 42)
                    .background(selectedSensor == page ? Theme.panel : Color.clear,
                                in: RoundedRectangle(cornerRadius: 13))
                }
                .buttonStyle(.plain)
                .accessibilityLabel("显示\(page.title)界面")
            }
        }
        .padding(5)
        .background(Theme.bgLift, in: RoundedRectangle(cornerRadius: 18))
    }

    private var streamBar: some View {
        HStack(spacing: 10) {
            if selectedSensor == .imu {
                streamChip(title: "IMU", detail: app.imuRateHz.map { String(format: "%.0f Hz", $0) } ?? "--",
                           fresh: app.imuFresh && !app.sensorPaused)
            } else {
                streamChip(title: "ToF", detail: "8×8 区域",
                           fresh: tofDisplayMap != nil && !app.sensorPaused)
            }
            Spacer()
            Button {
                Haptics.light()
                app.sensorPaused.toggle()
            } label: {
                Label(app.sensorPaused ? "已暂停" : "暂停",
                      systemImage: app.sensorPaused ? "play.fill" : "pause.fill")
                    .font(.subheadline.weight(.semibold))
                    .padding(.horizontal, 14)
                    .padding(.vertical, 8)
                    .background(app.sensorPaused ? Theme.warn.opacity(0.14) : Theme.bgLift,
                                in: Capsule())
                    .overlay(Capsule().strokeBorder(Theme.panelStroke, lineWidth: 1))
                    .foregroundStyle(app.sensorPaused ? Theme.warn : Theme.text)
            }
            .buttonStyle(.plain)
            .accessibilityLabel(app.sensorPaused ? "继续接收传感器数据" : "暂停传感器图表")
        }
    }

    private func streamChip(title: String, detail: String, fresh: Bool) -> some View {
        HStack(spacing: 6) {
            Circle()
                .fill(fresh ? Theme.live : Theme.dim.opacity(0.4))
                .frame(width: 8, height: 8)
            Text(title)
                .font(.subheadline.weight(.semibold))
                .foregroundStyle(Theme.text)
            Text(detail)
                .font(Theme.mono(12))
                .foregroundStyle(Theme.dim)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .background(Theme.panel, in: Capsule())
        .overlay(Capsule().strokeBorder(Theme.panelStroke, lineWidth: 1))
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("\(title) 数据流")
        .accessibilityValue(fresh ? "实时，\(detail)" : "无数据或已暂停")
    }

    // ---- ToF panel ---------------------------------------------------------------

    private var tofPanel: some View {
        Panel { VStack(alignment: .leading, spacing: 14) {
            HStack {
                Label("ToF 测距", systemImage: "camera.metering.matrix")
                    .font(.subheadline.weight(.semibold))
                    .foregroundStyle(Theme.dim)
                let scene = tofUsingFusionSummary
                    ? ("摘要", Theme.warn)
                    : Self.tofScene(tofDisplayMap?.mode, fresh: tofDisplayMap != nil)
                Text(scene.0)
                    .font(Theme.mono(12, weight: .bold))
                    .padding(.horizontal, 10)
                    .padding(.vertical, 4)
                    .background(scene.1.opacity(0.14), in: Capsule())
                    .foregroundStyle(scene.1)
                Spacer()
                Text(tofZoneSummary)
                    .font(Theme.mono(12))
                    .foregroundStyle(Theme.dim)
            }
            HStack(alignment: .top, spacing: 16) {
                TofHeatmapView(zones: tofDisplayMap?.zones,
                               fresh: tofDisplayMap != nil && !app.sensorPaused)
                    .frame(width: 168, height: 168)
                VStack(alignment: .leading, spacing: 10) {
                    VStack(alignment: .leading, spacing: 0) {
                        Text("最近障碍")
                            .font(.caption2.weight(.semibold))
                            .foregroundStyle(Theme.dim)
                        HStack(alignment: .firstTextBaseline, spacing: 4) {
                            Text(tofNearestMm.map { String(format: "%.2f", Double($0) / 1000) } ?? "--")
                                .font(Theme.display(34))
                                .foregroundStyle(Theme.text)
                                .monospacedDigit()
                                .contentTransition(.numericText())
                            Text("m")
                                .font(.subheadline)
                                .foregroundStyle(Theme.dim)
                        }
                    }
                    NearTrendSparkline(history: app.tofNearHistory)
                        .frame(height: 46)
                    Text(tofUsingFusionSummary ? "融合摘要 · 区域图未收到" : "近距走势 · 最近 150 帧")
                        .font(.caption2)
                        .foregroundStyle(tofUsingFusionSummary ? Theme.warn : Theme.dim)
                    Spacer(minLength: 0)
                }
            }
            Text(tofExplanation)
                .font(.caption2)
                .foregroundStyle(Theme.dim)
        } }
    }

    private var tofExplanation: String {
        if tofUsingFusionSummary {
            return "正在显示融合层的最近距离；区域分片未到达，请检查传感器数据流。"
        }
        guard let map = tofDisplayMap else { return "等待 ToF 区域分片；连接车辆后将显示 8×8 热力图。" }
        if map.receivedFragments < TofZoneFrame.fragmentCount {
            return "区域分片不完整，灰格为未收到或未信任区域；最近距离仍来自当前帧。"
        }
        return "热力图为车前 ~45° 视场：越红越近，越蓝越远，灰格为未信任区。"
    }

    private var tofDisplayMap: TofZoneFrame? {
        if app.sensorPaused { return app.tofMap ?? app.tofPartialMap }
        if app.tofMapFresh { return app.tofMap }
        if app.tofPartialFresh { return app.tofPartialMap }
        return nil
    }

    private var tofUsingFusionSummary: Bool {
        tofDisplayMap == nil && app.fusionFresh && ((app.fusion?.flags ?? 0) & 0x81) != 0
    }

    private var tofNearestMm: Int? {
        if let map = tofDisplayMap, map.validZones > 0 { return map.nearestMm }
        if tofUsingFusionSummary, let fusion = app.fusion, fusion.validZones > 0 {
            return fusion.distance
        }
        return nil
    }

    private var tofZoneSummary: String {
        if let map = tofDisplayMap {
            return map.receivedFragments == TofZoneFrame.fragmentCount
                ? "可信区 \(map.validZones)/64"
                : "区域 \(map.receivedFragments)/3 片"
        }
        if tofUsingFusionSummary, let fusion = app.fusion { return "融合 \(fusion.validZones) 区" }
        return "等待区域数据"
    }

    /// (label, color) for the FUSION_MODE_* scene classification.
    private static func tofScene(_ mode: Int?, fresh: Bool) -> (String, Color) {
        guard fresh else { return (mode == nil ? "未收到" : "过期", Theme.dim) }
        switch mode {
        case 0: return ("失明", Theme.crit)
        case 1: return ("跟踪", Theme.live)
        case 2: return ("降级", Theme.warn)
        case 3: return ("开阔", Theme.info)
        default: return ("等待", Theme.dim)
        }
    }

    // ---- IMU 3D panel -----------------------------------------------------------

    /// One large attitude instrument, then two tilt gauges and a separate
    /// acceleration vector. The model needs the full phone width to be legible.
    private var imu3DPanel: some View {
        Panel { VStack(alignment: .leading, spacing: 13) {
            HStack {
                VStack(alignment: .leading, spacing: 3) {
                    Text(carFrameAttitude ? "车体姿态" : "IMU 姿态预览")
                        .font(Theme.display(22))
                    Text("IMU / FUSION")
                        .font(Theme.mono(10))
                        .tracking(2)
                        .foregroundStyle(Theme.dim)
                }
                Spacer()
                Text(calibratedAttitude ? "车体融合" :
                     (displayCalibrated ? "显示标定" : (attitudeAvailable ? "原始 IMU" : "等待 IMU")))
                    .font(.caption2.weight(.bold))
                    .foregroundStyle(attitudeAvailable ? Theme.live : Theme.dim)
                    .padding(.horizontal, 10)
                    .padding(.vertical, 6)
                    .background(attitudeAvailable ? Theme.live.opacity(0.1) : Theme.bgLift,
                                in: Capsule())
            }
            Car3DView(headingDeg: attitudeAvailable ? headingDeg : 0,
                      rollDeg: attitudeAvailable ? rollDeg : 0,
                      pitchDeg: attitudeAvailable ? pitchDeg : 0,
                      hasData: attitudeAvailable,
                      sourceLabel: calibratedAttitude ? "已标定 · 车体融合" :
                          (displayCalibrated ? "iOS 标定 · 等待车端确认" : "未标定 · 传感器坐标"))
                .frame(height: 228)
            if attitudeAvailable && !calibratedAttitude {
                Text(displayCalibrated
                     ? "iOS 轴向已标定，车端尚未确认；模型仅供预览，倾斜保护仍以车端融合状态为准。"
                     : "轴向未标定：模型跟随 IMU 原始倾角；旋转为陀螺仪积分的相对变化，不代表车头实际方向。")
                    .font(.caption2)
                    .foregroundStyle(Theme.warn)
            }
            HStack(spacing: 10) {
                TiltGaugeView(title: "横滚", degrees: rollDeg, hasData: attitudeAvailable,
                              showsProtection: calibratedAttitude)
                TiltGaugeView(title: "俯仰", degrees: pitchDeg, hasData: attitudeAvailable,
                              showsProtection: calibratedAttitude)
            }
            HStack(spacing: 12) {
                headingReadout(title: "相对旋转", text: headingText, unit: "°")
                Rectangle().fill(Theme.panelStroke).frame(width: 1, height: 18)
                headingReadout(title: "旋转速率", text: yawRateText, unit: "°/s")
            }
            Divider()
            HStack(alignment: .center, spacing: 15) {
                GBallView(fwd: gBallFwd, lat: gBallLat,
                          trail: app.accelTrail,
                          compensated: attitudeAvailable && imuDisplayAvailable,
                          carFrame: carFrameAttitude,
                          peakG: app.peakHorizontalG)
                    .frame(width: 128)
                VStack(alignment: .leading, spacing: 10) {
                    Text("水平加速度")
                        .font(.subheadline.weight(.semibold))
                    Text(carFrameAttitude ? "圆点偏移即车辆加速方向" : "传感器 X/Y 方向 · 重力近似扣除")
                        .font(.caption2)
                        .foregroundStyle(Theme.dim)
                    accelerationReadout(carFrameAttitude ? "前后" : "X 轴", value: gBallFwd)
                    accelerationReadout(carFrameAttitude ? "左右" : "Y 轴", value: gBallLat)
                    Text("仅用于观察，不参与控制")
                        .font(.caption2)
                        .foregroundStyle(Theme.dim)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
        } }
    }

    private var imuCalibrationPanel: some View {
        Panel { VStack(alignment: .leading, spacing: 12) {
            HStack {
                Label("IMU 安装标定", systemImage: "scope")
                    .font(.subheadline.weight(.semibold))
                Spacer()
                Button("读取车端") { app.calibRecGet() }
                    .font(.caption.weight(.semibold))
                    .buttonStyle(.plain)
                    .foregroundStyle(Theme.accentDeep)
            }
            Text("车停稳后记录水平姿态，再抬起车头约 20–30° 并停稳记录。由两次重力方向推算车体前/左/上轴；轮距需实测。")
                .font(.caption2)
                .foregroundStyle(Theme.dim)
            HStack(spacing: 8) {
                calibrationButton(levelCapture == nil ? "① 记录水平" : "① 重录水平",
                                  ready: levelCapture != nil) {
                    levelCapture = captureStableImu()
                    noseCapture = nil
                    if levelCapture != nil { calibrationHint = "已记录水平姿态；请抬起车头后记录第二姿态。" }
                }
                calibrationButton(noseCapture == nil ? "② 记录抬头" : "② 重录抬头",
                                  ready: noseCapture != nil) {
                    noseCapture = captureStableImu()
                    if let levelCapture, let noseCapture {
                        calibrationHint = ImuAxisCalibration.derive(level: levelCapture, noseUp: noseCapture) == nil
                            ? "两次姿态区分不够：检查车身水平，抬头至少 20° 后停稳重录。"
                            : "轴向推算完成；输入实测左右轮距，再写入 TC275。"
                    }
                }
            }
            HStack(spacing: 10) {
                Text("左右轮距")
                    .font(.caption.weight(.semibold))
                TextField("实测 mm", text: $trackMmText)
                    .keyboardType(.numberPad)
                    .font(Theme.mono(14))
                    .multilineTextAlignment(.trailing)
                Text("mm · 80–600")
                    .font(.caption2)
                    .foregroundStyle(Theme.dim)
            }
            .padding(10)
            .background(Theme.bgLift, in: RoundedRectangle(cornerRadius: 10))
            Button {
                guard let axisCalibration, let track = Int(trackMmText) else { return }
                app.setImuDisplayCalibration(axisCalibration, trackMm: track)
                calibrationHint = "标定已发送；等待车端写入回执，写入后可重新读取确认。"
            } label: {
                Text("写入车端并保存")
                    .font(.subheadline.weight(.semibold))
                    .frame(maxWidth: .infinity, minHeight: 42)
                    .background(calibrationReady ? Theme.accent : Theme.bgLift,
                                in: RoundedRectangle(cornerRadius: 11))
                    .foregroundStyle(calibrationReady ? .white : Theme.dim)
            }
            .buttonStyle(.plain)
            .disabled(!calibrationReady)
            if !calibrationHint.isEmpty {
                Text(calibrationHint).font(.caption2).foregroundStyle(Theme.dim)
            }
            if axisCalibration != nil && !calibrationReady {
                Text("写入需要：控制权、TC275 在线、IMU 实时流、未暂停，以及 80–600 mm 的实测轮距。")
                    .font(.caption2)
                    .foregroundStyle(Theme.warn)
            }
            Text(imuCalibrationStatus)
                .font(.caption2.weight(.semibold))
                .foregroundStyle(calibratedAttitude ? Theme.live : Theme.warn)
        } }
    }

    private func calibrationButton(_ title: String, ready: Bool,
                                   action: @escaping () -> Void) -> some View {
        Button(action: action) {
            HStack {
                Text(title)
                if ready { Image(systemName: "checkmark.circle.fill") }
            }
            .font(.caption.weight(.semibold))
            .frame(maxWidth: .infinity, minHeight: 38)
            .background(Theme.bgLift, in: RoundedRectangle(cornerRadius: 10))
            .foregroundStyle(ready ? Theme.live : Theme.text)
        }
        .buttonStyle(.plain)
    }

    private var axisCalibration: ImuAxisCalibration? {
        guard let levelCapture, let noseCapture else { return nil }
        return ImuAxisCalibration.derive(level: levelCapture, noseUp: noseCapture)
    }

    private var calibrationReady: Bool {
        axisCalibration != nil && (Int(trackMmText).map { (80...600).contains($0) } ?? false)
            && app.connState == .connected && app.ctrlRole && app.tcUp
            && app.imuFresh && !app.sensorPaused
    }

    private var imuCalibrationStatus: String {
        if app.imuCalTimedOut { return "等待车端回执超时：确认 C6 与 TC275 固件均支持 0x7A。" }
        switch app.imuCalSaveStatus {
        case .pending: return "已发送到车端，等待静止保存与 DFlash 回读。"
        case .failed: return "已在本次运行生效，但 DFlash 保存失败；重启后不会保留。"
        case .rejected: return "车端拒绝标定：确认停车、IMU 在线、轴向与轮距有效。"
        case .saved: break
        case .idle: break
        }
        guard let record = app.calib.record else { return "车端记录未读取；需要控制权与 TC275 在线。" }
        if record.imuSaved == 3 { return "车端拒绝标定：确认停车、IMU 在线、轴向与轮距有效。" }
        if record.imuSaved == 2 { return "已在本次运行生效，但 DFlash 保存失败；重启后不会保留。" }
        if record.imuSaved == 1 { return "车端 DFlash 保存成功 · 轴向 \(record.imuAxis) · 轮距 \(record.trackMm) mm" }
        if record.imuAxis.contains(where: { $0 != 0 }) {
            return "车端轴向 \(record.imuAxis) · 轮距 \(record.trackMm) mm · \(calibratedAttitude ? "融合已生效" : "等待融合状态")"
        }
        return "车端 IMU 轴向尚未标定"
    }

    private func captureStableImu() -> [Double]? {
        let samples = Array(app.imuHistory.suffix(10))
        guard app.imuFresh, !app.sensorPaused, samples.count == 10,
              let first = samples.first, let last = samples.last,
              (300...900).contains(last.stampMs - first.stampMs),
              samples.allSatisfy({ $0.gyroMdps.allSatisfy { abs($0) < 15_000 } }) else {
            calibrationHint = "等待稳定的 IMU 数据：保持姿态静止约 0.5 秒后重试。"
            return nil
        }
        return (0..<3).map { axis in
            Double(samples.reduce(0) { $0 + $1.accMg[axis] }) / Double(samples.count)
        }
    }

    private func accelerationReadout(_ title: String, value: Double) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 3) {
            Text(title).font(.caption2).foregroundStyle(Theme.dim)
            Spacer(minLength: 2)
            Text(imuDisplayAvailable && attitudeAvailable ? String(format: "%+.2f", value) : "--")
                .font(Theme.mono(15))
                .monospacedDigit()
            Text("g").font(.caption2).foregroundStyle(Theme.dim)
        }
    }

    private var rollDeg: Double {
        calibratedAttitude ? Double(app.fusion?.rollCdeg ?? 0) / 100
                           : (app.rawImuAttitude?.rollDeg ?? 0)
    }
    private var pitchDeg: Double {
        calibratedAttitude ? Double(app.fusion?.pitchCdeg ?? 0) / 100
                           : (app.rawImuAttitude?.pitchDeg ?? 0)
    }
    private var headingDeg: Double {
        calibratedAttitude ? Double(app.fusion?.headingCdeg ?? 0) / 100
                           : (app.rawImuAttitude?.relativeYawDeg ?? 0)
    }
    private var calibratedAttitude: Bool {
        app.fusionFresh && ((app.fusion?.flags ?? 0) & 0x0a) == 0x0a
    }
    private var displayCalibrated: Bool { app.settings.imuDisplayCalibration != nil }
    private var carFrameAttitude: Bool { calibratedAttitude || displayCalibrated }
    private var attitudeAvailable: Bool {
        calibratedAttitude || (imuDisplayAvailable && app.rawImuAttitude != nil)
    }
    private var headingText: String {
        attitudeAvailable ? String(format: "%+.1f", headingDeg) : "--"
    }
    private var yawRateText: String {
        guard attitudeAvailable else { return "--" }
        let rate = calibratedAttitude ? Double(app.fusion?.yawRateCdegS ?? 0) / 100
                                      : (app.rawImuAttitude?.yawRateDegS ?? 0)
        return String(format: "%+.1f", rate)
    }
    private var gBallFwd: Double { app.accelTrail.last?.fwd ?? 0 }
    private var gBallLat: Double { app.accelTrail.last?.lat ?? 0 }
    private var imuDisplayAvailable: Bool {
        app.sensorPaused ? !app.imuHistory.isEmpty : app.imuFresh
    }

    private func headingReadout(title: String, text: String, unit: String) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 3) {
            Text(title)
                .font(.caption2.weight(.semibold))
                .foregroundStyle(Theme.dim)
            Spacer()
            Text(text)
                .font(Theme.mono(15))
                .foregroundStyle(Theme.text)
                .monospacedDigit()
            Text(unit)
                .font(.caption2)
                .foregroundStyle(Theme.dim)
        }
    }

    /// Six-axis live values, one column per axis, colored to match the
    /// strip-chart traces: accel in g, angular rate in °/s.
    private var axisValueGrid: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("六轴瞬时值")
                .font(.caption.weight(.semibold))
                .foregroundStyle(Theme.dim)
            LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: 8), count: 3),
                      spacing: 8) {
                ForEach(axisValueItems, id: \.label) { item in
                    VStack(alignment: .leading, spacing: 5) {
                        HStack(spacing: 5) {
                            Circle().fill(item.color).frame(width: 6, height: 6)
                            Text(item.label)
                                .font(.caption2.weight(.semibold))
                                .foregroundStyle(Theme.dim)
                        }
                        HStack(alignment: .firstTextBaseline, spacing: 2) {
                            Text(item.value ?? "--")
                                .font(Theme.mono(15))
                                .foregroundStyle(Theme.text)
                                .monospacedDigit()
                                .lineLimit(1)
                                .minimumScaleFactor(0.75)
                            Text(item.unit)
                                .font(Theme.mono(9))
                                .foregroundStyle(Theme.dim)
                        }
                    }
                    .padding(10)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .background(Theme.bgLift.opacity(0.65), in: RoundedRectangle(cornerRadius: 12))
                }
            }
        }
        .accessibilityElement(children: .combine)
        .accessibilityLabel("六轴实时数值")
    }

    private var axisValueItems: [(label: String, color: Color, value: String?, unit: String)] {
        let sample = imuDisplayAvailable ? app.imuHistory.last : nil
        let acc = AxisLegend.series.map { axis in
            (label: "a\(axis.label.lowercased())", color: axis.color,
             value: sample.map { String(format: "%+.2f", Double($0.accMg[axis.index]) / 1000) }, unit: "g")
        }
        let gyro = AxisLegend.series.map { axis in
            (label: "ω\(axis.label.lowercased())", color: axis.color,
             value: sample.map { String(format: "%+.1f", Double($0.gyroMdps[axis.index]) / 1000) }, unit: "°/s")
        }
        return acc + gyro
    }

    // ---- IMU waveform panel ----------------------------------------------------------

    private var imuWavePanel: some View {
        Panel { VStack(alignment: .leading, spacing: 12) {
            HStack {
                Label("IMU 波形", systemImage: "waveform")
                    .font(.subheadline.weight(.semibold))
                    .foregroundStyle(Theme.dim)
                Spacer()
                Text(app.imuRateHz.map { String(format: "%.0f Hz", $0) } ?? "-- Hz")
                    .font(Theme.mono(12))
                    .foregroundStyle(Theme.dim)
            }
            axisValueGrid
            // Series stay in wire units (mg / mdps); autoRange + the legend
            // present them in g / °/s.
            let accRange = ImuKinematics.autoRange(values: accSeries.map(\.values),
                                                   minimumSpan: 500)
            MultiStripChart(series: accSeries, range: accRange)
                .frame(height: 78)
            legendRow(series: accSeries, live: accLiveValues,
                      rangeText: String(format: "%@ g · 自适应 ±%.2f",
                                        "加速度", accRange.upperBound / 1000))
            let gyroRange = ImuKinematics.autoRange(values: gyroSeries.map(\.values),
                                                    minimumSpan: 20_000)
            MultiStripChart(series: gyroSeries, range: gyroRange)
                .frame(height: 78)
            legendRow(series: gyroSeries, live: gyroLiveValues,
                      rangeText: String(format: "%@ °/s · 自适应 ±%.1f",
                                        "角速度", gyroRange.upperBound / 1000))
            HStack {
                Text("传感器物理量程 ±4 g · ±500 dps")
                    .font(.caption2)
                    .foregroundStyle(Theme.dim)
                Spacer()
                Text("芯片温度")
                    .font(.caption2.weight(.semibold))
                    .foregroundStyle(Theme.dim)
                Text(app.imuHistory.last.map {
                    String(format: "%.2f °C", Double($0.tempCentiC) / 100)
                } ?? "--")
                    .font(Theme.mono(13))
                    .foregroundStyle(Theme.text)
                    .monospacedDigit()
            }
        } }
    }

    private var accSeries: [MultiStripChart.Series] {
        AxisLegend.series.map { axis in
            MultiStripChart.Series(label: axis.label, color: axis.color,
                                   values: app.imuHistory.map { Double($0.accMg[axis.index]) })
        }
    }

    private var gyroSeries: [MultiStripChart.Series] {
        AxisLegend.series.map { axis in
            MultiStripChart.Series(label: axis.label, color: axis.color,
                                   values: app.imuHistory.map { Double($0.gyroMdps[axis.index]) })
        }
    }

    private var accLiveValues: [Double?] {
        AxisLegend.series.map { axis in
            app.imuHistory.last.map { Double($0.accMg[axis.index]) / 1000 }
        }
    }

    private var gyroLiveValues: [Double?] {
        AxisLegend.series.map { axis in
            app.imuHistory.last.map { Double($0.gyroMdps[axis.index]) / 1000 }
        }
    }

    /// Chart legend with per-axis live values ("X +0.98") and the current
    /// auto-range note on the right.
    private func legendRow(series: [MultiStripChart.Series], live: [Double?],
                           rangeText: String) -> some View {
        HStack(spacing: 10) {
            ForEach(Array(series.enumerated()), id: \.element.label) { item in
                HStack(spacing: 3) {
                    Circle().fill(item.element.color).frame(width: 6, height: 6)
                    Text(item.element.label)
                        .font(.caption2)
                        .foregroundStyle(Theme.dim)
                    Text(item.offset < live.count
                         ? live[item.offset].map { String(format: "%+.2f", $0) } ?? "--"
                         : "--")
                        .font(Theme.mono(11))
                        .foregroundStyle(Theme.text)
                        .monospacedDigit()
                }
            }
            Spacer()
            Text(rangeText)
                .font(.caption2)
                .foregroundStyle(Theme.dim)
        }
        .accessibilityElement(children: .combine)
    }

    // ---- driving guard panel --------------------------------------------------------

    private var guardPanel: some View {
        Panel { VStack(alignment: .leading, spacing: 12) {
            HStack {
                Label("驾驶防护", systemImage: "shield.lefthalf.filled")
                    .font(.subheadline.weight(.semibold))
                    .foregroundStyle(Theme.dim)
                Spacer()
                if app.fusion?.brake == 1 {
                    Text("制动中")
                        .font(Theme.mono(12, weight: .bold))
                        .padding(.horizontal, 10)
                        .padding(.vertical, 4)
                        .background(Theme.crit.opacity(0.14), in: Capsule())
                        .foregroundStyle(Theme.crit)
                }
            }
            HStack(alignment: .firstTextBaseline, spacing: 6) {
                Text(Self.reasonText(app.fusion?.reason))
                    .font(Theme.display(20))
                    .foregroundStyle(Theme.text)
                Spacer()
                if let cap = app.fusion?.cap {
                    Text(String(format: "前进上限 %.2f km/h", Double(cap) * 0.0036))
                        .font(Theme.mono(12))
                        .foregroundStyle(Theme.dim)
                }
            }
            ForEach(flagChips.indices, id: \.self) { row in
                HStack(spacing: 8) {
                    ForEach(flagChips[row], id: \.title) { chip in chipView(chip) }
                }
            }
        } }
        .id("guard-panel")
    }

    /// 8 guard bits (fusion JSON flags), as two rows of four chips.
    private var flagChips: [[(title: String, on: Bool)]] {
        let flags = app.fusion?.flags
        let defs: [(bit: Int, title: String)] = [
            (0, "ToF OK"), (1, "IMU OK"), (2, "编码器 OK"), (3, "已标定"),
            (4, "打滑"), (5, "偏置就绪"), (6, "需回中"), (7, "覆盖受限"),
        ]
        let flat = defs.map { def in
            (title: def.title, on: flags.map { $0 & (1 << def.bit) != 0 } ?? false)
        }
        return stride(from: 0, to: flat.count, by: 4).map { Array(flat[$0..<min($0 + 4, flat.count)]) }
    }

    private func chipView(_ chip: (title: String, on: Bool)) -> some View {
        Text(chip.title)
            .font(Theme.mono(11, weight: .semibold))
            .lineLimit(1)
            .minimumScaleFactor(0.7)
            .padding(.horizontal, 10)
            .padding(.vertical, 6)
            .frame(maxWidth: .infinity)
            .background(chip.on ? Theme.live.opacity(0.12) : Theme.bgLift, in: Capsule())
            .overlay(Capsule().strokeBorder(chip.on ? Theme.live.opacity(0.5) : Theme.panelStroke, lineWidth: 1))
            .foregroundStyle(chip.on ? Theme.live : Theme.dim)
            .accessibilityLabel(chip.title)
            .accessibilityValue(chip.on ? "正常" : "未就绪")
    }

    private static func reasonText(_ reason: Int?) -> String {
        switch reason {
        case 0: return "自由行驶"
        case 1: return "接近障碍 · 限速中"
        case 2: return "障碍停车"
        case 3: return "测距失效 · 停车"
        case 4: return "倾斜保护"
        case 5: return "编码器丢失"
        default: return "等待融合状态"
        }
    }
}

// ---- ToF heatmap ---------------------------------------------------------------------

/// 8×8 ToF zone map. The grid is the sensor's forward field of view (~45°);
/// rows/cols map to the car frame per the physical mount, which only a bench
/// check can confirm — `rowFlip` is the reserved calibration constant.
struct TofHeatmapView: View {
    let zones: [Int?]?
    var fresh: Bool = true

    private static let rowFlip = false

    /// Near = crit red → mid = warn amber → far = info blue over 0.1–2.5 m.
    /// RGB values mirror Theme's stops (kept in sync by hand — Theme colors
    /// are frozen constants, and interpolating raw values beats a UIColor
    /// round-trip inside the 64-cell Canvas loop).
    private static let scale: [(t: Double, rgb: (Double, Double, Double))] = [
        (0.0, (0.76, 0.15, 0.18)),  // Theme.crit
        (0.5, (0.60, 0.36, 0.02)),  // Theme.warn
        (1.0, (0.17, 0.36, 0.53)),  // Theme.info
    ]

    static func color(forDistance mm: Int) -> Color {
        let u = min(max(Double(mm), 100), 2500)
        let norm = (u - 100) / 2400
        let upperIdx = scale.firstIndex(where: { $0.t >= norm }) ?? scale.count - 1
        let lowerIdx = max(0, upperIdx - 1)
        let lower = scale[lowerIdx], upper = scale[upperIdx]
        let span = upper.t - lower.t
        let k = span > 0 ? (norm - lower.t) / span : 0
        return Color(red: lower.rgb.0 + (upper.rgb.0 - lower.rgb.0) * k,
                     green: lower.rgb.1 + (upper.rgb.1 - lower.rgb.1) * k,
                     blue: lower.rgb.2 + (upper.rgb.2 - lower.rgb.2) * k)
    }

    var body: some View {
        Canvas { ctx, size in
            let grid = 8
            let inset: CGFloat = 1.5
            let cw = size.width / CGFloat(grid)
            let ch = size.height / CGFloat(grid)
            let count = zones?.count ?? TofZoneFrame.zoneCount
            for i in 0..<count {
                let row = i / grid, col = i % grid
                let drawRow = Self.rowFlip ? (grid - 1 - row) : row
                let rect = CGRect(x: CGFloat(col) * cw + inset,
                                  y: CGFloat(drawRow) * ch + inset,
                                  width: cw - inset * 2, height: ch - inset * 2)
                let path = Path(roundedRect: rect, cornerRadius: 3)
                let cell: Int? = zones.flatMap { zs in i < zs.count ? zs[i] : nil }
                guard let mm = cell else {
                    // untrusted zone (or no frame yet): hatched-out grey
                    ctx.fill(path, with: .color(Theme.dim.opacity(fresh ? 0.10 : 0.06)))
                    ctx.stroke(path, with: .color(Theme.panelStroke.opacity(0.7)), lineWidth: 0.7)
                    continue
                }
                var color = Self.color(forDistance: mm)
                if !fresh { color = color.opacity(0.35) }
                ctx.fill(path, with: .color(color))
            }
            // boresight cross at the centre of the field
            let cx = size.width / 2, cy = size.height / 2
            var cross = Path()
            cross.move(to: CGPoint(x: cx - 5, y: cy))
            cross.addLine(to: CGPoint(x: cx + 5, y: cy))
            cross.move(to: CGPoint(x: cx, y: cy - 5))
            cross.addLine(to: CGPoint(x: cx, y: cy + 5))
            ctx.stroke(cross, with: .color(Theme.text.opacity(0.55)), lineWidth: 1)
        }
        .aspectRatio(1, contentMode: .fit)
        .background(Theme.bgLift, in: RoundedRectangle(cornerRadius: 14))
        .overlay {
            if zones == nil {
                Text("区域图未收到")
                    .font(.caption2.weight(.semibold))
                    .foregroundStyle(Theme.dim)
                    .padding(8)
                    .background(Theme.panel.opacity(0.9), in: Capsule())
            }
        }
        .overlay(RoundedRectangle(cornerRadius: 14)
            .strokeBorder(Theme.panelStroke.opacity(0.6), lineWidth: 1))
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("ToF 八乘八区域热力图")
        .accessibilityValue(zones.map { zs in
            let valid = zs.compactMap { $0 }
            return valid.isEmpty ? "无有效读数"
                : "最近 \(valid.min() ?? 0) 毫米，共 \(valid.count) 个有效区域"
        } ?? "等待数据")
    }
}

// ---- sparkline + strip charts ----------------------------------------------------------

/// Nearest-distance trend on a fixed 0–2.5 m scale.
struct NearTrendSparkline: View {
    let history: [Int]

    var body: some View {
        Canvas { ctx, size in
            let maxMm = 2500.0
            guard !history.isEmpty else {
                var base = Path()
                base.move(to: CGPoint(x: 0, y: size.height))
                base.addLine(to: CGPoint(x: size.width, y: size.height))
                ctx.stroke(base, with: .color(Theme.dim.opacity(0.3)), lineWidth: 1)
                return
            }
            var path = Path()
            let last = history.count - 1
            for (i, mm) in history.enumerated() {
                let x = size.width * CGFloat(i) / CGFloat(max(1, last))
                let y = size.height * (1 - CGFloat(min(Double(mm), maxMm) / maxMm))
                if i == 0 { path.move(to: CGPoint(x: x, y: y)) }
                else { path.addLine(to: CGPoint(x: x, y: y)) }
            }
            ctx.stroke(path, with: .color(Theme.accent),
                       style: StrokeStyle(lineWidth: 2, lineCap: .round))
            ctx.stroke(path, with: .color(Theme.accent.opacity(0.18)), lineWidth: 6)
        }
        .accessibilityLabel("最近障碍距离走势")
        .accessibilityValue(history.last.map { "当前 \($0) 毫米" } ?? "暂无读数")
    }
}

/// Multi-series rolling strip chart over a caller-computed (auto) range
/// with a zero line.
struct MultiStripChart: View {
    struct Series: Identifiable {
        let label: String
        let color: Color
        let values: [Double]
        var id: String { label }
    }

    let series: [Series]
    let range: ClosedRange<Double>

    var body: some View {
        Canvas { ctx, size in
            let span = range.upperBound - range.lowerBound
            func yPos(_ v: Double) -> CGFloat {
                let clamped = min(max(v, range.lowerBound), range.upperBound)
                return size.height * (1 - CGFloat((clamped - range.lowerBound) / span))
            }
            var zero = Path()
            zero.move(to: CGPoint(x: 0, y: yPos(0)))
            zero.addLine(to: CGPoint(x: size.width, y: yPos(0)))
            ctx.stroke(zero, with: .color(Theme.dim.opacity(0.25)),
                       style: StrokeStyle(lineWidth: 1, dash: [3, 4]))
            for s in series where s.values.count > 1 {
                var path = Path()
                let last = s.values.count - 1
                for (i, v) in s.values.enumerated() {
                    let point = CGPoint(x: size.width * CGFloat(i) / CGFloat(last),
                                        y: yPos(v))
                    if i == 0 { path.move(to: point) } else { path.addLine(to: point) }
                }
                ctx.stroke(path, with: .color(s.color),
                           style: StrokeStyle(lineWidth: 1.6, lineCap: .round))
            }
        }
        .background(Theme.bgLift, in: RoundedRectangle(cornerRadius: 12))
        .overlay(RoundedRectangle(cornerRadius: 12)
            .strokeBorder(Theme.panelStroke.opacity(0.5), lineWidth: 1))
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(series.map(\.label).joined(separator: "、") + "轴波形")
    }
}

/// X/Y/Z legend identity shared by the IMU charts.
enum AxisLegend {
    static let series: [(index: Int, label: String, color: Color)] = [
        (0, "X", Theme.crit), (1, "Y", Theme.warn), (2, "Z", Theme.info),
    ]
}

// ---- g-ball ----------------------------------------------------------------------------

/// Horizontal-acceleration bubble: the ball sits at the gravity-compensated
/// (fwd, lat) vector, 1 g at the rim — accelerating runs it up (车头方向),
/// braking down, cornering sideways. The fading trail is the last ~6 s of
/// positions. In an uncalibrated installation the axes are sensor X/Y.
struct GBallView: View {
    let fwd: Double
    let lat: Double
    let trail: [(fwd: Double, lat: Double)]
    var compensated: Bool = true
    var carFrame: Bool = true
    var peakG: Double = 0

    var body: some View {
        VStack(spacing: 5) {
            Canvas { ctx, size in
                let centre = CGPoint(x: size.width / 2, y: size.height / 2)
                let radius = min(size.width, size.height) / 2 - 8
                for ring in [0.25, 0.5, 1.0] {
                    let rr = radius * CGFloat(ring)
                    let rect = CGRect(x: centre.x - rr, y: centre.y - rr, width: rr * 2, height: rr * 2)
                    ctx.stroke(Path(ellipseIn: rect),
                               with: .color(Theme.dim.opacity(ring == 1.0 ? 0.45 : 0.22)),
                               style: StrokeStyle(lineWidth: 1, dash: ring == 1.0 ? [] : [2, 3]))
                }
                var cross = Path()
                cross.move(to: CGPoint(x: centre.x - radius, y: centre.y))
                cross.addLine(to: CGPoint(x: centre.x + radius, y: centre.y))
                cross.move(to: CGPoint(x: centre.x, y: centre.y - radius))
                cross.addLine(to: CGPoint(x: centre.x, y: centre.y + radius))
                ctx.stroke(cross, with: .color(Theme.dim.opacity(0.3)),
                           style: StrokeStyle(lineWidth: 0.7, dash: [1, 3]))
                // Orientation hints follow the active coordinate system.
                ctx.draw(Text(carFrame ? "前" : "X").font(.system(size: 9, weight: .semibold))
                            .foregroundStyle(Theme.dim),
                         at: CGPoint(x: centre.x, y: centre.y - radius + 9))
                ctx.draw(Text(carFrame ? "左" : "Y").font(.system(size: 9, weight: .semibold))
                            .foregroundStyle(Theme.dim.opacity(0.7)),
                         at: CGPoint(x: centre.x - radius + 10, y: centre.y))
                ctx.draw(Text("1g").font(Theme.mono(8)).foregroundStyle(Theme.dim.opacity(0.6)),
                         at: CGPoint(x: centre.x + radius - 11, y: centre.y + radius - 9))
                // fading trail, oldest first
                let count = trail.count
                for (i, p) in trail.enumerated() {
                    let point = Self.point(fwd: p.fwd, lat: p.lat,
                                           centre: centre, radius: radius)
                    let age = Double(i) / Double(max(1, count - 1))
                    let dot = CGRect(x: point.x - 1.75, y: point.y - 1.75, width: 3.5, height: 3.5)
                    ctx.fill(Path(ellipseIn: dot),
                             with: .color(Theme.accent.opacity((0.04 + 0.30 * age) * (compensated ? 1 : 0.35))))
                }
                // the ball, clamped just inside the rim
                let ballPoint = Self.point(fwd: fwd, lat: lat, centre: centre, radius: radius)
                let ballRect = CGRect(x: ballPoint.x - 7, y: ballPoint.y - 7, width: 14, height: 14)
                let ballColor = compensated ? Theme.accent : Theme.dim
                ctx.fill(Path(ellipseIn: ballRect), with: .color(ballColor))
                ctx.stroke(Path(ellipseIn: ballRect), with: .color(.white.opacity(0.65)), lineWidth: 1)
                if compensated {
                    let highlight = CGRect(x: ballPoint.x - 3.5, y: ballPoint.y - 4.5,
                                           width: 4, height: 4)
                    ctx.fill(Path(ellipseIn: highlight), with: .color(.white.opacity(0.8)))
                }
            }
            .aspectRatio(1, contentMode: .fit)
            Text(compensated ? String(format: "峰值 %.2f g", peakG) : "未补偿")
                .font(Theme.mono(10))
                .foregroundStyle(compensated ? Theme.dim : Theme.warn)
            Text(carFrame ? "水平加速度" : "传感器水平分量")
                .font(.caption2)
                .foregroundStyle(Theme.dim)
        }
        .background(Theme.bgLift, in: RoundedRectangle(cornerRadius: 14))
        .overlay(RoundedRectangle(cornerRadius: 14)
            .strokeBorder(Theme.panelStroke.opacity(0.6), lineWidth: 1))
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(carFrame ? "水平加速度矢量球" : "传感器 X Y 加速度矢量球")
        .accessibilityValue(compensated
            ? String(format: carFrame ? "前向 %.2f g，侧向 %.2f g，峰值 %.2f g"
                                      : "X 轴 %.2f g，Y 轴 %.2f g，峰值 %.2f g", fwd, lat, peakG)
            : "姿态缺失，未补偿")
    }

    /// g → screen point: forward up, left screen-left, clamped to 0.92 r.
    private static func point(fwd: Double, lat: Double,
                              centre: CGPoint, radius: CGFloat) -> CGPoint {
        var x = CGFloat(-lat)
        var y = CGFloat(-fwd)
        let length = (x * x + y * y).squareRoot()
        if length > 0.92 {
            x *= 0.92 / length
            y *= 0.92 / length
        }
        return CGPoint(x: centre.x + x * radius, y: centre.y + y * radius)
    }
}

// ---- tilt gauges -------------------------------------------------------------------------

/// Linear inclinometer, ±60° full scale, drawn entirely in one Canvas (a
/// GeometryReader wrapper greedily eats vertical space in the panel). The
/// red bands start at ±45° — the TC275's FUSION_TILT hard-stop threshold
/// (|roll| > 45 or |pitch| > 45 in tc275_car app/fusion.c), so "needle in
/// red" means "car stops, tilt guard".
struct TiltGaugeView: View {
    let title: String
    let degrees: Double
    var hasData: Bool = true
    var showsProtection: Bool = true
    var spanDeg: Double = 60
    var limitDeg: Double = 45

    private var valueColor: Color {
        if showsProtection && abs(degrees) > limitDeg { return Theme.crit }
        if showsProtection && abs(degrees) > limitDeg - 10 { return Theme.warn }
        return Theme.text
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(alignment: .firstTextBaseline) {
                Text(title)
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(Theme.dim)
                Spacer()
                Text(hasData ? String(format: "%+.1f°", degrees) : "--°")
                    .font(Theme.mono(17))
                    .monospacedDigit()
                    .foregroundStyle(valueColor)
            }
            Canvas { ctx, size in
                let mid = size.width / 2
                let half = size.width / 2 - 3
                let cy = size.height / 2
                // track
                let track = CGRect(x: 0, y: cy - 5, width: size.width, height: 10)
                ctx.fill(Path(roundedRect: track, cornerRadius: 5), with: .color(Theme.bgLift))
                // red stop zones beyond ±limitDeg
                if showsProtection {
                    let zoneWidth = half * (1 - limitDeg / spanDeg)
                    for side in [0.0, 1.0] {
                        let zone = CGRect(x: side == 0 ? 0 : size.width - zoneWidth,
                                          y: cy - 5, width: zoneWidth, height: 10)
                        ctx.fill(Path(roundedRect: zone, cornerRadius: 5),
                                 with: .color(Theme.crit.opacity(0.22)))
                    }
                }
                // ticks every 15°, majors at 0 and ±limit
                var deg = -spanDeg
                while deg <= spanDeg + 0.01 {
                    let x = mid + CGFloat(deg / spanDeg) * half
                    let isLimit = showsProtection && abs(abs(deg) - limitDeg) < 0.01
                    let major = abs(deg) < 0.01 || isLimit
                    var tick = Path()
                    tick.move(to: CGPoint(x: x, y: cy - (major ? 8 : 6)))
                    tick.addLine(to: CGPoint(x: x, y: cy + 8))
                    ctx.stroke(tick, with: .color(isLimit
                        ? Theme.crit.opacity(0.85)
                        : Theme.text.opacity(major ? 0.55 : 0.22)), lineWidth: major ? 1.5 : 1)
                    deg += 15
                }
                // needle
                let fraction = max(-1, min(1, degrees / spanDeg))
                let nx = mid + CGFloat(fraction) * half
                var needle = Path()
                needle.move(to: CGPoint(x: nx, y: cy - 10))
                needle.addLine(to: CGPoint(x: nx, y: cy + 10))
                ctx.stroke(needle, with: .color(hasData ? (showsProtection && abs(degrees) > limitDeg ? Theme.crit : Theme.accent) : Theme.dim),
                           style: StrokeStyle(lineWidth: 3.5, lineCap: .round))
                var cap = Path()
                cap.addEllipse(in: CGRect(x: nx - 2.5, y: cy - 2.5, width: 5, height: 5))
                ctx.fill(cap, with: .color(hasData ? Theme.accent : Theme.dim))
            }
            .frame(height: 22)
            .animation(.easeInOut(duration: 0.2), value: degrees)
            Text(showsProtection ? "±45° 倾斜保护" : "传感器参考角")
                .font(Theme.mono(9))
                .foregroundStyle(Theme.dim)
        }
        .padding(11)
        .frame(maxWidth: .infinity)
        .background(Theme.bgLift.opacity(0.72), in: RoundedRectangle(cornerRadius: 13))
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("\(title)倾角")
        .accessibilityValue(hasData
            ? String(format: showsProtection ? "%.1f 度，保护阈值 45 度" : "传感器参考角 %.1f 度", degrees)
            : "等待数据")
    }
}
