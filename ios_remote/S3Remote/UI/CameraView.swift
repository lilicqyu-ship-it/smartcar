/*
 * CameraView.swift — s3-gateway live video card: compact 16:9-ish card on
 * the drive page with a state chip, plus a fullscreen sheet. Single-viewer
 * etiquette lives in CameraClient (detach on leave/background); the card
 * itself never keeps the stream alive off-tab (RootView owns start/stop).
 */

import SwiftUI

struct CameraCard: View {
    @Environment(AppState.self) private var app
    @State private var fullscreen = false

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Label("实时画面", systemImage: "camera.on.rectangle")
                    .font(.subheadline.bold())
                Spacer()
                stateChip
                Button {
                    Haptics.light()
                    fullscreen = true
                } label: {
                    Image(systemName: "arrow.up.left.and.arrow.down.right")
                        .font(.system(size: 13, weight: .bold))
                        .frame(width: 32, height: 32)
                        .background(Theme.bgLift, in: RoundedRectangle(cornerRadius: 9))
                }.buttonStyle(.plain)
                    .disabled(app.camera.state != .live)
                    .accessibilityLabel("全屏查看相机画面")
            }
            videoArea
        }.padding(12)
            .background(Theme.panel, in: RoundedRectangle(cornerRadius: 22))
            .overlay(RoundedRectangle(cornerRadius: 22).strokeBorder(Theme.panelStroke, lineWidth: 1))
            .sheet(isPresented: $fullscreen) { CameraFullscreenView() }
    }

    private var stateChipText: (String, Color) {
        switch app.camera.state {
        case .idle: return ("OFF", Theme.dim)
        case .connecting: return ("CONNECTING", Theme.warn)
        case .live: return ("LIVE \(app.camera.fps) FPS", Theme.live)
        case .busy: return ("BUSY", Theme.warn)
        case .failed: return ("ERROR", Theme.crit)
        }
    }

    private var stateChip: some View {
        let (text, color) = stateChipText
        return Text(text)
            .font(Theme.mono(11, weight: .bold))
            .padding(.horizontal, 9).padding(.vertical, 4)
            .background(color.opacity(0.13), in: Capsule())
            .foregroundStyle(color)
    }

    @ViewBuilder private var videoArea: some View {
        switch app.camera.state {
        case .live:
            if let f = app.camera.frame {
                Image(uiImage: f)
                    .resizable().scaledToFit()
                    .frame(maxWidth: .infinity, minHeight: 180)
                    .background(Theme.ink)
                    .clipShape(RoundedRectangle(cornerRadius: 14))
                    .accessibilityLabel("车辆实时画面")
            } else {
                placeholder("等待首帧…", icon: "photo")
            }
        case .connecting:
            placeholder("正在连接相机网关…", icon: "hourglass")
        case .busy:
            placeholder("查看器被占用 — 手持机或其它页面正在观看，对方退出后自动接入", icon: "person.2")
        case .failed(let message):
            placeholder("相机不可用（\(message)）", icon: "exclamationmark.triangle")
        case .idle:
            placeholder("相机未开启 — 到「设置」打开实时画面", icon: "video.slash")
        }
    }

    private func placeholder(_ text: String, icon: String) -> some View {
        VStack(spacing: 8) {
            Image(systemName: icon).font(.system(size: 22, weight: .light))
            Text(text).font(.caption).multilineTextAlignment(.center)
        }.foregroundStyle(.white.opacity(0.45))
            .frame(maxWidth: .infinity, minHeight: 180)
            .background(Theme.ink)
            .clipShape(RoundedRectangle(cornerRadius: 14))
    }
}

struct CameraFullscreenView: View {
    @Environment(AppState.self) private var app
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            if let f = app.camera.frame {
                Image(uiImage: f)
                    .resizable().scaledToFit()
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .accessibilityLabel("车辆实时画面")
            } else {
                Text("等待画面…").foregroundStyle(.white.opacity(0.4))
            }
            VStack {
                HStack {
                    if app.camera.state == .live {
                        Text("LIVE · \(app.camera.fps) FPS")
                            .font(Theme.mono(12, weight: .bold))
                            .foregroundStyle(Theme.live)
                    }
                    Spacer()
                    Button {
                        dismiss()
                    } label: {
                        Image(systemName: "xmark")
                            .font(.system(size: 15, weight: .bold))
                            .frame(width: 40, height: 40)
                            .background(.white.opacity(0.14), in: Circle())
                            .foregroundStyle(.white)
                    }.accessibilityLabel("关闭全屏画面")
                }.padding(18)
                Spacer()
            }
        }
        .onTapGesture { dismiss() }
    }
}
