import SwiftUI

/// Tab indices — single source of truth (also used by the `--tab` dev hook
/// and HomeView's pairing-guide jump).
enum Tab: Int {
    case drive = 0, play, vehicle, sensors, calib, topology, link, settings
    static let all: [Tab] = [.drive, .play, .vehicle, .sensors, .calib, .topology, .link, .settings]
}

@main
struct S3RemoteApp: App {
    @State private var app = AppState()
    @State private var link: LinkEngine?

    var body: some Scene {
        WindowGroup {
            RootView()
                .environment(app)
                .preferredColorScheme(.light)
                .task {
                    if link == nil {
                        let engine = LinkEngine(app: app)
                        app.link = engine
                        link = engine
                        engine.start()
                    }
                }
        }
    }
}

struct RootView: View {
    @Environment(AppState.self) private var app
    @Environment(\.scenePhase) private var scenePhase
    // dev/screenshot hook: `--tab 0..7` picks the initial tab
    @State private var selection = RootView.initialTab

    static let initialTab: Int = {
        let args = ProcessInfo.processInfo.arguments
        guard let i = args.firstIndex(of: "--tab"), i + 1 < args.count,
              let n = Int(args[i + 1]), (0...7).contains(n) else { return 0 }
        return n
    }()

    var body: some View {
        @Bindable var app = app
        ZStack {
            Theme.bg.ignoresSafeArea()
            // 横幅用 VStack 占位而非 safeAreaInset：TabView（UIKit 背板）不
            // 可靠传播祖先附加安全区，VStack 能保证各页内容不被横幅遮挡。
            VStack(spacing: 0) {
                RadioLostBanner(selection: $selection)
                TabView(selection: $selection) {
                    HomeView(selection: $selection)
                        .tabItem { Label("驾驶", systemImage: "gamecontroller.fill") }
                        .tag(Tab.drive.rawValue)
                    PlayView()
                        .tabItem { Label("玩法", systemImage: "party.popper.fill") }
                        .tag(Tab.play.rawValue)
                    VehicleView()
                        .tabItem { Label("车辆", systemImage: "car.fill") }
                        .tag(Tab.vehicle.rawValue)
                    SensorsView()
                        .tabItem { Label("传感器", systemImage: "gauge.with.needle") }
                        .tag(Tab.sensors.rawValue)
                    CalibView()
                        .tabItem { Label("标定", systemImage: "wrench.and.screwdriver.fill") }
                        .tag(Tab.calib.rawValue)
                    TopologyView()
                        .tabItem { Label("拓扑", systemImage: "network") }
                        .tag(Tab.topology.rawValue)
                    DiagView()
                        .tabItem { Label("连接", systemImage: "waveform.path.ecg") }
                        .tag(Tab.link.rawValue)
                    SettingsView()
                        .tabItem { Label("设置", systemImage: "gearshape.fill") }
                        .tag(Tab.settings.rawValue)
                }
            }
            AlertOverlayView()

        }
        .tint(Theme.accent)
        .animation(.easeInOut(duration: 0.25), value: app.alert)
        .animation(.easeInOut(duration: 0.25), value: app.radioLostActive)
        .sheet(isPresented: $app.showOnboarding) {
            OnboardingView()
                .presentationDetents([.large])
        }
        .task(id: selection) { app.syncCamera(activeTab: selection) }
        .onChange(of: selection) { oldValue, newValue in
            if oldValue == Tab.drive.rawValue && newValue != Tab.drive.rawValue { app.joystickMoved(v: 0, w: 0) }
            if oldValue == Tab.calib.rawValue && newValue != Tab.calib.rawValue { app.calibLeave() }
        }
        .onChange(of: app.settings.cameraEnabled) { _, _ in
            app.syncCamera(activeTab: selection)
        }
        .onChange(of: scenePhase) { _, phase in
            if phase != .active {
                app.handleBackground()
            } else {
                app.syncCamera(activeTab: selection)
            }
        }
    }
}

// ---- full-screen alert overlay (P9, spec 20-22) -------------------------------
// radioLost 在此只留空分支：它降级为顶部横幅 RadioLostBanner（离线不锁 UI）。
// 其余类型（急停/故障/电量）均为刻意设计的可确认全屏遮罩。

struct AlertOverlayView: View {
    @Environment(AppState.self) private var app

    var body: some View {
        if ProcessInfo.processInfo.arguments.contains("--no-alert") {
            EmptyView() // dev/screenshot hook
        } else if app.emergActive {
            overlay(
                title: "紧急停止",
                subtitle: "车辆已锁定。解除急停后，仍需触摸摇杆恢复驾驶。",
                color: Theme.stopRed,
                icon: "exclamationmark.octagon.fill",
                buttonTitle: "解除急停",
                buttonAction: { app.emergencyRelease() })
        } else if let alert = app.alert {
            switch alert.kind {
            case .radioLost:
                // 失联改为顶部非阻断横幅（RadioLostBanner），不再全屏锁 UI
                EmptyView()
            case .vehicleFault:
                overlay(title: "车辆需要检查",
                        subtitle: "\(alert.detail) — 确认后可继续观察",
                        color: Theme.warn, icon: "gear.badge.questionmark",
                        buttonTitle: "我知道了", buttonAction: { app.ackAlert() })
            case .criticalBattery:
                overlay(title: "电量即将耗尽", subtitle: "电量 \(alert.detail)",
                        color: Theme.crit, icon: "battery.25percent",
                        buttonTitle: "我知道了", buttonAction: { app.ackAlert() })
            case .lowBattery:
                overlay(title: "电量偏低", subtitle: "电量 \(alert.detail)",
                        color: Theme.warn, icon: "battery.50percent",
                        buttonTitle: "我知道了", buttonAction: { app.ackAlert() })
            case .emergency:
                EmptyView()
            }
        }
    }

    private func overlay(title: String, subtitle: String, color: Color,
                         icon: String, buttonTitle: String?, buttonAction: (() -> Void)?) -> some View {
        ZStack {
            Rectangle()
                .fill(.ultraThinMaterial)
                .ignoresSafeArea()
            Theme.bg.opacity(0.55)
                .ignoresSafeArea()

            VStack(spacing: 22) {
                Image(systemName: icon)
                    .font(.system(size: 58, weight: .bold))
                    .foregroundStyle(color)
                    .glow(color, radius: 10, opacity: 0.55)
                Text(title)
                    .font(Theme.display(32, weight: .heavy))
                    .foregroundStyle(color)
                    .kerning(1.5)
                Text(subtitle)
                    .font(.subheadline)
                    .foregroundStyle(Theme.text)
                    .multilineTextAlignment(.center)
                    .padding(.horizontal, 36)
                if let buttonTitle, let buttonAction {
                    Button {
                        Haptics.heavy()
                        buttonAction()
                    } label: {
                        Text(buttonTitle)
                            .font(.title3.weight(.heavy))
                            .foregroundStyle(.white)
                            .frame(minWidth: 200, minHeight: 54)
                            .background(
                                LinearGradient(colors: [color, color.opacity(0.75)],
                                               startPoint: .top, endPoint: .bottom),
                                in: RoundedRectangle(cornerRadius: 16))
                            .glow(color, radius: 10, opacity: 0.45)
                    }
                    .buttonStyle(.plain)
                    .padding(.top, 6)
                }
            }
        }
        .contentShape(Rectangle())
        .transition(.scale(scale: 0.96).combined(with: .opacity))
    }
}

// ---- non-blocking radio-lost banner --------------------------------------------
/// 失联提示横幅：radioLost 从全屏遮罩降级而来（离线也要能正常操作）。
/// 持续显示、不拦截任何触摸、点击直达「连接」页，链路恢复即自动消失。
/// 触发条件仍是 SafetyMonitor 的 1.2 s 去抖（含冷启动从未连接的场景）。
struct RadioLostBanner: View {
    @Environment(AppState.self) private var app
    @Binding var selection: Int

    var body: some View {
        if ProcessInfo.processInfo.arguments.contains("--no-alert") {
            EmptyView() // dev/screenshot hook, same as AlertOverlayView
        } else if app.radioLostActive {
            Button {
                Haptics.selection()
                selection = Tab.link.rawValue
            } label: {
                HStack(spacing: 10) {
                    Image(systemName: "wifi.exclamationmark")
                        .font(.subheadline.weight(.bold))
                    Text(app.hasConnected ? "连接已中断 · 请检查 Wi-Fi" : "车辆未连接 · 请检查 Wi-Fi")
                        .font(.subheadline.bold())
                    Spacer()
                    Text("查看").font(.caption.bold())
                    Image(systemName: "chevron.right").font(.caption2.weight(.bold))
                }
                .foregroundStyle(Theme.crit)
                .padding(.horizontal, 16)
                .padding(.vertical, 10)
                .background(Theme.crit.opacity(0.12), in: RoundedRectangle(cornerRadius: 14))
                .overlay(RoundedRectangle(cornerRadius: 14)
                    .strokeBorder(Theme.crit.opacity(0.35), lineWidth: 1))
                .padding(.horizontal, 20)
                .padding(.vertical, 8)
            }
            .buttonStyle(.plain)
            .accessibilityLabel(app.hasConnected ? "连接已中断，前往连接页" : "车辆未连接，前往连接页")
            .transition(.move(edge: .top).combined(with: .opacity))
        }
    }
}
