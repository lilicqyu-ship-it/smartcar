import SwiftUI

struct HomeView: View {
    @Environment(AppState.self) private var app
    @Binding var selection: Int

    var body: some View {
        Page {
            ScrollView {
                VStack(spacing: 14) {
                    HStack(alignment: .center) {
                        VStack(alignment: .leading, spacing: 6) {
                            Text("S3 / REMOTE CONTROL").font(Theme.mono(9)).tracking(2).foregroundStyle(Theme.accent)
                            Text("每一程，尽在掌握").font(.system(size: 25, weight: .bold))
                            Text("连接你的车，探索下一程。").font(.caption).foregroundStyle(Theme.dim)
                        }.frame(maxWidth: .infinity, alignment: .leading)
                        LogoGaugeView()
                    }
                    StatusDeck()
                    if let warning = app.fusionWarning {
                        Label(warning, systemImage: "exclamationmark.triangle.fill")
                            .font(.subheadline).foregroundStyle(Theme.warn)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .padding(14)
                            .background(Theme.warn.opacity(0.12), in: RoundedRectangle(cornerRadius: 16))
                            .accessibilityLabel(warning)
                    }
                    if app.settings.cameraEnabled {
                        CameraCard()
                    }
                    drivingCard
                    ModeSelector()
                    TiltBar()
                    if !app.ctrlRole {
                        Button { selection = app.connState == .connected ? Tab.vehicle.rawValue : Tab.link.rawValue } label: {
                            HStack(spacing: 12) {
                                Image(systemName: "link")
                                VStack(alignment: .leading, spacing: 4) {
                                    Text(app.connState == .connected ? "配对后开启驾驶" : "先连接，再出发").font(.subheadline.bold())
                                    Text(app.connState == .connected ? "前往连接页完成车辆配对" : "加入车辆 Wi-Fi，检查网关地址").font(.caption)
                                }
                                Spacer()
                                Image(systemName: "arrow.up.right")
                            }.foregroundStyle(Theme.text).padding(16)
                                .background(Theme.panel, in: RoundedRectangle(cornerRadius: 20))
                        }.buttonStyle(.plain)
                    }
                }.padding(20)
                    .frame(maxWidth: 680)
                    .frame(maxWidth: .infinity)
            }
            .safeAreaInset(edge: .bottom, spacing: 0) {
                StopButton().padding(.horizontal, 20).padding(.vertical, 10)
                    .frame(maxWidth: 660).frame(maxWidth: .infinity)
                    .background(Theme.bg)
            }
        }
    }

    private var drivingCard: some View {
        VStack(spacing: 8) {
            HStack(alignment: .top) {
                VStack(alignment: .leading, spacing: 0) {
                    Text("实时车速").font(.caption).foregroundStyle(.white.opacity(0.65))
                    HStack(alignment: .firstTextBaseline, spacing: 8) {
                        Text(speedText).font(Theme.display(40)).monospacedDigit()
                            .contentTransition(.numericText())
                            .animation(.easeInOut(duration: 0.3), value: speedText)
                        Text("km/h").font(Theme.mono(12)).foregroundStyle(.white.opacity(0.6))
                    }
                }
                Spacer()
                VStack(alignment: .trailing, spacing: 8) {
                    if app.settings.soundEnabled {
                        HornButton()
                    }
                    Text(app.stopLatched ? "已驻停" : app.ctrlRole ? "操控就绪" : app.connState == .connected ? "只读观察" : "等待连接")
                        .font(.caption.bold()).padding(.horizontal, 12).padding(.vertical, 7)
                        .background(.white.opacity(0.09), in: Capsule())
                    Text(driveHint).font(.caption2).foregroundStyle(.white.opacity(0.55))
                }
            }
            JoystickView(deadzone: app.settings.deadzone,
                         enabled: app.ctrlRole && !app.emergActive && !app.tiltEnabled && !app.sequencer.active,
                         onTouch: { app.joystickTouch() },
                         onChange: { v, w in app.joystickMoved(v: v, w: w) })
            HStack {
                driveValue("油门", value: app.controller.joyV)
                Spacer()
                Text("拖动以操控").font(.caption).foregroundStyle(.white.opacity(0.55))
                Spacer()
                driveValue("转向", value: app.controller.joyW)
            }
            HStack {
                Text("输出速度  \(app.outV) mm/s")
                Spacer()
                Text("转速  \(app.outW) °/s")
            }.font(Theme.mono(10)).foregroundStyle(.white.opacity(0.6))
        }.foregroundStyle(.white).padding(20)
            .background(Theme.ink, in: RoundedRectangle(cornerRadius: 30))
    }

    private var driveHint: String {
        if app.connState == .connected && !app.tcUp { return "车控未就绪" }
        if app.sequencer.active { return "特技执行中 · STOP 可中止" }
        if app.tiltEnabled { return "体感驾驶中 · 倾斜手机操控" }
        return "松手自动回中"
    }

    /// Smoothed display speed (C6 renderSpeed port): dt-aware EMA in
    /// AppState, shown with one 0.1 km/h digit; below ~0.1 km/h reads 0.0.
    private var speedText: String {
        guard app.teleFresh else { return "—" }
        if abs(app.displaySpeedMmS) < SpeedDisplayFilter.stopMmS { return "0.0" }
        return String(format: "%.1f", abs(app.displaySpeedMmS) * 0.0036)
    }
    private func driveValue(_ title: String, value: Double) -> some View {
        VStack(spacing: 4) {
            Text(String(format: "%+.0f%%", value * 100)).font(Theme.mono(16)).monospacedDigit()
            Text(title).font(.caption2).foregroundStyle(.white.opacity(0.6))
        }
    }
}

struct StatusDeck: View {
    @Environment(AppState.self) private var app
    var body: some View {
        HStack(spacing: 8) {
            Circle().fill(app.connState == .connected ? Theme.live : Theme.dim).frame(width: 7, height: 7)
            Text(app.connState == .connected ? "车辆已连接" : app.connState == .connecting ? "正在连接" : "车辆未连接")
                .font(.caption.bold())
            Spacer(minLength: 4)
            Label(app.teleFresh ? app.batteryDisplayPct.map { "\($0)%" } ?? "—" : "—", systemImage: "battery.75percent")
                .font(Theme.mono(12))
            Text(app.connState == .connected && app.rttLast > 0 ? "\(app.rttLast) ms" : "— ms")
                .font(Theme.mono(11)).foregroundStyle(Theme.dim)
        }
        .accessibilityElement(children: .combine)
    }
}

struct ModeSelector: View {
    @Environment(AppState.self) private var app
    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text("驾驶模式").font(.subheadline.bold())
                Spacer()
                Text("动力上限 \(app.controller.mode.pct)%").font(.caption).foregroundStyle(Theme.dim)
            }
            HStack(spacing: 6) {
                ForEach(DriveMode.allCases) { mode in
                    let selected = app.controller.mode == mode
                    Button {
                        Haptics.selection(); app.setMode(mode)
                    } label: {
                        VStack(spacing: 5) {
                            Text(mode == .eco ? "轻行" : mode == .normal ? "标准" : "运动").font(.subheadline.bold())
                            Text(mode.label).font(Theme.mono(9)).tracking(1)
                        }.frame(maxWidth: .infinity, minHeight: 56)
                            .foregroundStyle(selected ? .white : Theme.dim)
                            .background(selected ? Theme.accent : .clear, in: RoundedRectangle(cornerRadius: 15))
                    }.buttonStyle(.plain)
                        .accessibilityAddTraits(selected ? [.isSelected] : [])
                }
            }.padding(5).background(Theme.bgLift, in: RoundedRectangle(cornerRadius: 20))
                .disabled(!app.ctrlRole)
        }
    }
}

/// Tilt-steering switch row: toggle + bubble-level indicator + calibration.
struct TiltBar: View {
    @Environment(AppState.self) private var app

    var body: some View {
        HStack(spacing: 12) {
            Button {
                Haptics.selection()
                app.setTiltEnabled(!app.tiltEnabled)
            } label: {
                HStack(spacing: 10) {
                    Image(systemName: "move.3d")
                        .font(.system(size: 16, weight: .bold))
                        .foregroundStyle(app.tiltEnabled ? .white : Theme.accent)
                        .frame(width: 34, height: 34)
                        .background(app.tiltEnabled ? Theme.accent : Theme.accent.opacity(0.10),
                                    in: RoundedRectangle(cornerRadius: 10))
                    VStack(alignment: .leading, spacing: 2) {
                        Text("体感驾驶").font(.subheadline.bold()).foregroundStyle(Theme.text)
                        Text(app.tiltEnabled ? "前倾加速 · 左右倾斜转向" : "像握方向盘一样开车")
                            .font(.caption2).foregroundStyle(Theme.dim)
                    }
                    Spacer(minLength: 4)
                    Text(app.tiltEnabled ? "ON" : "OFF")
                        .font(Theme.mono(11)).tracking(1)
                        .foregroundStyle(app.tiltEnabled ? .white : Theme.dim)
                        .padding(.horizontal, 10).padding(.vertical, 6)
                        .background(app.tiltEnabled ? Theme.accent : Theme.bgLift, in: Capsule())
                }.padding(10)
                    .background(Theme.panel, in: RoundedRectangle(cornerRadius: 18))
                    .overlay(RoundedRectangle(cornerRadius: 18).strokeBorder(Theme.panelStroke, lineWidth: 1))
            }.buttonStyle(.plain)
                .disabled(!app.ctrlRole)
                .opacity(app.ctrlRole ? 1 : 0.55)

            if app.tiltEnabled {
                bubbleLevel
                Button {
                    Haptics.light()
                    app.calibrateTilt()
                } label: {
                    Text("校准").font(.caption.bold())
                        .padding(.horizontal, 12).padding(.vertical, 8)
                        .background(Theme.bgLift, in: Capsule())
                }.buttonStyle(.plain)
                if app.stopLatched && !app.emergActive {
                    // joystick is inert while tilt drives — give the STOP
                    // latch a touch target here
                    Button {
                        Haptics.medium()
                        app.joystickTouch()
                    } label: {
                        Label("继续", systemImage: "hand.tap.fill")
                            .font(.caption.bold())
                            .padding(.horizontal, 12).padding(.vertical, 8)
                            .background(Theme.accent, in: Capsule())
                            .foregroundStyle(.white)
                    }.buttonStyle(.plain)
                }
            }
        }
    }

    /// Bubble level: dot shows the current tilt (right/down = tilted).
    private var bubbleLevel: some View {
        Circle()
            .strokeBorder(Theme.panelStroke, lineWidth: 1.5)
            .background(Theme.panel, in: Circle())
            .frame(width: 42, height: 42)
            .overlay {
                Circle()
                    .fill(app.tiltAxes.v == 0 && app.tiltAxes.w == 0 ? Theme.accent : Theme.warn)
                    .frame(width: 11, height: 11)
                    .offset(x: CGFloat((app.gravity?.x ?? 0) * 15),
                            y: CGFloat((app.gravity?.z ?? 0) * 15))
            }
            .accessibilityLabel("姿态指示")
    }
}

struct StopButton: View {
    @Environment(AppState.self) private var app
    @State private var longFired = false
    @State private var pressProgress: Double = 0
    @State private var pressTask: Task<Void, Never>?
    @State private var pressed = false

    private static let longPressS: Double = 1.2

    var body: some View {
        HStack(spacing: 14) {
            Image(systemName: "stop.fill")
                .font(.title3).frame(width: 42, height: 42)
                .background(.white.opacity(0.15), in: RoundedRectangle(cornerRadius: 13))
            VStack(alignment: .leading, spacing: 4) {
                Text(app.emergActive ? "紧急停止" : app.stopLatched ? "已停止" : "停止车辆")
                    .font(.headline)
                Text(app.stopLatched ? "触摸摇杆恢复操控" : "轻点停车 · 长按 1.2 秒急停")
                    .font(.caption).opacity(0.85)
            }
            Spacer(minLength: 0)
            Text("STOP").font(Theme.mono(12)).tracking(1)
        }
            .foregroundStyle(.white)
            .padding(.horizontal, 18)
            .frame(maxWidth: .infinity, minHeight: 76)
            .background(Theme.stopRed, in: RoundedRectangle(cornerRadius: 22))
            .overlay(progressWipe)
            .clipShape(RoundedRectangle(cornerRadius: 22))
            .contentShape(RoundedRectangle(cornerRadius: 22))
            .accessibilityElement(children: .ignore)
            .accessibilityLabel("停止车辆")
            .accessibilityHint("轻点停车，长按一秒二触发急停")
            .accessibilityAddTraits(.isButton)
            .accessibilityAction { app.stopPressed() }
            .accessibilityAction(named: "紧急停止") { app.emergencyTriggered() }
            .onDisappear { pressTask?.cancel(); pressTask = nil; pressProgress = 0 }
            .onLongPressGesture(
                minimumDuration: Self.longPressS,
                maximumDistance: 60,
                perform: {
                    longFired = true
                    Haptics.error()
                    app.emergencyTriggered()
                },
                onPressingChanged: { pressing in
                    pressed = pressing
                    if pressing {
                        longFired = false
                        Haptics.light()
                        startProgress()
                    } else {
                        pressTask?.cancel()
                        pressTask = nil
                        withAnimation(.easeOut(duration: 0.15)) { pressProgress = 0 }
                        if !longFired {
                            Haptics.medium()
                            app.stopPressed() // click: immediate DRIVE(0,0) + latch
                        }
                    }
                })
    }

    /// Long-press progress: warn-colored wipe across the button.
    private var progressWipe: some View {
        GeometryReader { geo in
            if pressProgress > 0 {
                Rectangle()
                    .fill(Theme.warn.opacity(0.30))
                    .frame(width: geo.size.width * pressProgress)
                    .clipShape(RoundedRectangle(cornerRadius: 20))
                    .allowsHitTesting(false)
            }
        }
    }

    private func startProgress() {
        pressTask = Task {
            let start = ContinuousClock.now
            while !Task.isCancelled {
                let elapsed = ContinuousClock.now - start
                let p = Double(elapsed.components.seconds) + Double(elapsed.components.attoseconds) / 1e18
                pressProgress = min(p / Self.longPressS, 1)
                if pressProgress >= 1 { break }
                try? await Task.sleep(for: .milliseconds(33))
            }
        }
    }
}
