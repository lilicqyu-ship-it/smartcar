import SwiftUI

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
    // dev/screenshot hook: `--tab 0..4` picks the initial tab
    @State private var selection = RootView.initialTab

    static let initialTab: Int = {
        let args = ProcessInfo.processInfo.arguments
        guard let i = args.firstIndex(of: "--tab"), i + 1 < args.count,
              let n = Int(args[i + 1]), (0...4).contains(n) else { return 0 }
        return n
    }()

    var body: some View {
        ZStack {
            TabView(selection: $selection) {
                HomeView(selection: $selection)
                    .tabItem { Label("驾驶", systemImage: "gamecontroller.fill") }
                    .tag(0)
                VehicleView()
                    .tabItem { Label("车辆", systemImage: "car.fill") }
                    .tag(1)
                TopologyView()
                    .tabItem { Label("拓扑", systemImage: "network") }
                    .tag(2)
                DiagView()
                    .tabItem { Label("连接", systemImage: "waveform.path.ecg") }
                    .tag(3)
                SettingsView()
                    .tabItem { Label("设置", systemImage: "gearshape.fill") }
                    .tag(4)
            }
            AlertOverlayView()

        }
        .tint(Theme.accent)
        .onChange(of: selection) { oldValue, newValue in
            if oldValue == 0 && newValue != 0 { app.joystickMoved(v: 0, w: 0) }
        }
    }
}

// ---- full-screen alert overlay (P9, spec 20-22) -------------------------------

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
                overlay(title: "连接已中断",
                        subtitle: "请靠近车辆并检查 Wi-Fi。连接恢复后将自动返回。",
                        color: Theme.crit, icon: "wifi.exclamationmark",
                        buttonTitle: nil, buttonAction: nil)
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
