/*
 * OnboardingView.swift — first-launch, three-step guidance: join the car's
 * Wi-Fi, open the pairing window, pair on the link page. Dismissal persists
 * via OnboardingState; Settings can replay it.
 */

import SwiftUI

struct OnboardingView: View {
    @Environment(AppState.self) private var app

    var body: some View {
        VStack(spacing: 20) {
            PageHeading(eyebrow: "GET STARTED", title: "三步开跑", subtitle: "第一次连接车辆？跟着做就行。")
            step(1, icon: "wifi", title: "加入车机 Wi-Fi",
                 detail: "系统设置里连接车辆热点，App 默认网关 192.168.4.1。")
            step(2, icon: "key.horizontal", title: "打开配对窗口",
                 detail: "长按车侧配对键 3 秒，窗口开启后 60 秒内完成配对。")
            step(3, icon: "gamecontroller", title: "配对并取得控制权",
                 detail: "到「连接」页点配对，token 自动保存，回驾驶页即可操控。")
            Label("想要实时画面？在「设置」开启相机（需 s3-gateway 视觉网关）。",
                  systemImage: "camera.on.rectangle")
                .font(.caption).foregroundStyle(Theme.dim)
            Button {
                Haptics.success()
                app.dismissOnboarding()
            } label: {
                Text("开始使用").font(.headline)
                    .frame(maxWidth: .infinity, minHeight: 50)
                    .foregroundStyle(.white)
                    .background(Theme.accent, in: RoundedRectangle(cornerRadius: 16))
            }.buttonStyle(.plain)
        }.padding(24)
    }

    private func step(_ index: Int, icon: String, title: String, detail: String) -> some View {
        HStack(alignment: .top, spacing: 14) {
            Text("\(index)")
                .font(Theme.mono(15, weight: .bold))
                .foregroundStyle(.white)
                .frame(width: 32, height: 32)
                .background(Theme.accent, in: Circle())
            Image(systemName: icon)
                .font(.system(size: 18, weight: .semibold))
                .foregroundStyle(Theme.accent)
                .frame(width: 30)
            VStack(alignment: .leading, spacing: 3) {
                Text(title).font(.subheadline.bold())
                Text(detail).font(.caption).foregroundStyle(Theme.dim)
            }
            Spacer(minLength: 0)
        }.padding(14)
            .background(Theme.panel, in: RoundedRectangle(cornerRadius: 18))
            .overlay(RoundedRectangle(cornerRadius: 18).strokeBorder(Theme.panelStroke, lineWidth: 1))
    }
}
