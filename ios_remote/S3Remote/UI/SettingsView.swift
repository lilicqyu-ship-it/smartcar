import SwiftUI

struct SettingsView: View {
    @Environment(AppState.self) private var app
    @State private var confirmReset = false

    var body: some View {
        Page {
            ScrollView {
                VStack(spacing: 18) {
                    PageHeading(eyebrow: "PREFERENCES", title: "按你的习惯驾驶", subtitle: "连接、操控与偏好，集中管理。")
                    linkPanel
                    controlPanel
                    aboutPanel
                }
                .padding(20)
                .frame(maxWidth: 680)
                .frame(maxWidth: .infinity)
            }
            .scrollDismissesKeyboard(.interactively)
        }
    }

    private func sectionTitle(_ text: String, icon: String) -> some View {
        Label(text, systemImage: icon)
            .font(.subheadline.weight(.semibold))
            .foregroundStyle(Theme.dim)
    }

    private var linkPanel: some View {
        Panel { VStack(alignment: .leading, spacing: 10) {
            sectionTitle("网关连接", icon: "antenna.rtl")
            LabeledField("网关地址") {
                TextField("192.168.4.1", text: bind(\.host))
                    .keyboardType(.URL)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .font(Theme.mono(15))
                    .textFieldStyle(.roundedBorder)
            }
            LabeledField("配对凭证") {
                SecureField("配对后自动获得", text: bind(\.token))
                    .font(Theme.mono(13))
                    .textFieldStyle(.roundedBorder)
                    .autocorrectionDisabled()
                    .textInputAutocapitalization(.never)
            }
            HStack(spacing: 10) {
                Button {
                    Haptics.light()
                    app.link?.reconnect()
                } label: {
                    Text("应用并重连").frame(maxWidth: .infinity, minHeight: 42)
                }
                .buttonStyle(.borderedProminent)
                .foregroundStyle(.white)
                Button(role: .destructive) {
                    confirmReset = true
                } label: {
                    Text("重置配对").frame(maxWidth: .infinity, minHeight: 42)
                }
                .buttonStyle(.bordered)
            }
            .alert("清空配对 Token？", isPresented: $confirmReset) {
                Button("清空", role: .destructive) {
                    app.settings.token = ""
                    app.link?.reconnect()
                }
                Button("取消", role: .cancel) {}
            } message: {
                Text("清空后需要重新配对才能取得控制权。")
            }
        } }
    }

    private var controlPanel: some View {
        Panel { VStack(alignment: .leading, spacing: 10) {
            sectionTitle("操控", icon: "hand.tap.fill")
            HStack {
                Text("摇杆死区").font(.subheadline)
                Spacer()
                Text(String(format: "%.0f %%", app.settings.deadzone * 100))
                    .font(Theme.mono(14)).foregroundStyle(Theme.accent)
            }
            Slider(value: bind(\.deadzone), in: 0...0.3, step: 0.01)
                .tint(Theme.accent)
            HStack {
                Text("默认模式限幅").font(.subheadline)
                Spacer()
                Picker("模式", selection: bind(\.mode)) {
                    ForEach(DriveMode.allCases) { m in
                        Text(m.label).tag(m)
                    }
                }
                .pickerStyle(.menu)
                .tint(Theme.accent)
            }
        } }
    }

    private var aboutPanel: some View {
        Panel { VStack(spacing: 10) {
            sectionTitle("关于", icon: "info.circle.fill")
            row("App 版本", appVersion)
            row("操控方式", "上下前后 · 左右转向")
            row("紧急停止", "长按停止按钮 1.2 秒")
            row("连接恢复", "断开后自动尝试重连")
        } }
    }

    /// 真源是 pbxproj 的 MARKETING_VERSION（just ios-version 升版），这里读构建信息
    private var appVersion: String {
        let v = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String
        let b = Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String
        return "\(v ?? "?") (\(b ?? "?"))"
    }

    private func row(_ label: String, _ value: String) -> some View {
        HStack {
            Text(label).font(.subheadline).foregroundStyle(Theme.dim)
            Spacer()
            Text(value).font(Theme.mono(11)).foregroundStyle(Theme.text)
        }
    }

    private func LabeledField<Content: View>(_ label: String, @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(label).font(.caption).foregroundStyle(Theme.dim)
            content()
        }
    }

    private func bind(_ key: WritableKeyPath<AppSettings, String>) -> Binding<String> {
        Binding(
            get: { app.settings[keyPath: key] },
            set: { app.settings[keyPath: key] = $0 })
    }

    private func bind(_ key: WritableKeyPath<AppSettings, Double>) -> Binding<Double> {
        Binding(
            get: { app.settings[keyPath: key] },
            set: { app.settings[keyPath: key] = $0 })
    }

    private func bind(_ key: WritableKeyPath<AppSettings, DriveMode>) -> Binding<DriveMode> {
        Binding(
            get: { app.settings[keyPath: key] },
            set: { app.settings[keyPath: key] = $0 })
    }
}
