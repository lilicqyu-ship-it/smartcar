/*
 * TopologyView.swift — vehicle network topology (P3): iPhone → C6 → TC275
 * node chain with live link states, plus a chip-version inventory.
 *
 * Data sources (all already on the wire — no firmware changes):
 *   iPhone–C6  link: connState / hello pair window / RTT (pong) / RSSI or estimate
 *   C6 node:   hello "ver" (C6 firmware)
 *   C6–TC275 link: tc{on} up-state, telemetry link_rtt_ms (C6-side ping)
 *                  and link_err_rate (0.1 % units)
 *   TC275 node: telemetry fw_ver (0x00MMmmpp) + hw_rev, and the
 *               {"t":"tcver","app","sbl"} bridge beacon
 *   s3-gateway camera plane: intentionally dashed "未接入" (v1 has no camera)
 */

import SwiftUI

struct TopologyView: View {
    @Environment(AppState.self) private var app

    var body: some View {
        Page {
            ScrollView {
                VStack(spacing: 0) {
                    nodeChain
                    chipVersionsPanel
                }
                .padding(16)
            }
        }
    }

    // ---- node chain -------------------------------------------------------------

    private var nodeChain: some View {
        VStack(spacing: 0) {
            TopologyNode(
                icon: "iphone.gen3",
                title: "iPhone · S3 Remote",
                role: "遥控终端",
                status: (text: app.connState == .connected ? "WS UP" : "DOWN",
                         color: app.connState == .connected ? Theme.accent : Theme.crit),
                glow: app.connState == .connected) {
                    kv("App 版本", appVersion)
                    kv("RTT → C6", app.connState == .connected ? "\(max(app.rttLast, 0)) ms" : "--")
                    kv("信号", signalText)
            }

            TopologyLink(
                label: "Wi-Fi softAP → WS /ws · proto v2 (AA 55 02)",
                detail: linkDetail,
                up: app.connState == .connected)

            TopologyNode(
                icon: "wifi.router.fill",
                title: "ESP32-C6 · 车端网关",
                role: "softAP / WebSocket / 桥接",
                status: (text: app.connState == .connected ? "ONLINE" : "OFFLINE",
                         color: app.connState == .connected ? Theme.accent : Theme.dim),
                glow: false) {
                    kv("固件 (hello ver)", app.c6Ver.isEmpty ? "--" : app.c6Ver)
                    kv("配对窗口", app.pairWindow.map { $0.rawValue } ?? "--")
                    kv("TV 通道", app.tcUp ? "UP" : "DOWN")
            }

            TopologyLink(
                label: "SPI · SF 帧（板间 LINK）",
                detail: tcLinkDetail,
                up: app.tcUp)

            TopologyNode(
                icon: "cpu.fill",
                title: "TC275 · 车体控制",
                role: "三核 TriCore · SBL + App",
                status: (text: app.tcUp ? "LINK UP" : "LINK DOWN",
                         color: app.tcUp ? Theme.accent : Theme.warn),
                glow: app.tcUp) {
                    kv("App 固件", tcAppText)
                    kv("SBL 固件", app.tcSblVer.isEmpty ? "--" : app.tcSblVer)
                    kv("硬件版本", app.telemetry.map { "rev \($0.hwRev)" } ?? "--")
            }

            peripheralsRow

            gatewayStub
        }
    }

    // ---- 芯片版本清单 -------------------------------------------------------------

    private var chipVersionsPanel: some View {
        Panel { VStack(alignment: .leading, spacing: 10) {
            Label("芯片版本", systemImage: "info.circle.fill")
                .font(.subheadline.weight(.semibold))
                .foregroundStyle(Theme.dim)
                .padding(.bottom, 2)
            row("本 App（iPhone）", appVersion)
            row("ESP32-C6 固件", app.c6Ver.isEmpty ? "--" : "v\(app.c6Ver)")
            row("TC275 App 固件", tcAppText)
            row("TC275 SBL 固件", app.tcSblVer.isEmpty ? "--" : app.tcSblVer)
            row("TC275 硬件", app.telemetry.map { "rev \($0.hwRev)" } ?? "--")
            Text("C6 版本来自 hello 信标；TC275 App/SBL 来自 tcver 信标与遥测 fw_ver（0x00MMmmpp）")
                .font(.caption2)
                .foregroundStyle(Theme.dim)
        } }
        .padding(.top, 14)
    }

    // ---- pieces -----------------------------------------------------------------

    private var peripheralsRow: some View {
        HStack(spacing: 8) {
            peripheralChip("gearshape.2", "xcore 命令队列")
            peripheralChip("memorychip", "DFlash 标定")
            peripheralChip("arrow.left.and.right", "电机 / 编码器")
        }
        .padding(.leading, 28)
        .padding(.top, 10)
        .padding(.bottom, 4)
    }

    private func peripheralChip(_ icon: String, _ name: String) -> some View {
        HStack(spacing: 5) {
            Image(systemName: icon)
                .font(.caption2)
                .foregroundStyle(app.tcUp ? Theme.accent : Theme.dim)
            Text(name)
                .font(.caption2)
                .foregroundStyle(Theme.dim)
        }
        .padding(.horizontal, 9)
        .padding(.vertical, 6)
        .background(Theme.bgLift, in: Capsule())
        .overlay(Capsule().strokeBorder(app.tcUp ? Theme.accent.opacity(0.35) : Theme.panelStroke.opacity(0.6), lineWidth: 1))
    }

    /// 视觉平面（s3-gateway）v1 未接入：虚线占位，接通后此处换正式节点
    private var gatewayStub: some View {
        HStack(spacing: 10) {
            Image(systemName: "camera.on.rectangle")
                .font(.subheadline)
                .foregroundStyle(Theme.dim.opacity(0.7))
            VStack(alignment: .leading, spacing: 1) {
                Text("s3-gateway · 视觉网关")
                    .font(.footnote.weight(.semibold))
                    .foregroundStyle(Theme.dim)
                Text("Camera WS :81 — 相机面未接入（v1 控制面）")
                    .font(.caption2)
                    .foregroundStyle(Theme.dim.opacity(0.75))
            }
            Spacer()
            Text("IDLE")
                .font(Theme.mono(11, weight: .bold))
                .padding(.horizontal, 8)
                .padding(.vertical, 3)
                .background(Theme.dim.opacity(0.12), in: Capsule())
                .foregroundStyle(Theme.dim)
        }
        .padding(12)
        .background(Theme.panel.opacity(0.4), in: RoundedRectangle(cornerRadius: 16))
        .overlay(
            RoundedRectangle(cornerRadius: 16)
                .strokeBorder(Theme.panelStroke.opacity(0.55), style: StrokeStyle(lineWidth: 1.2, dash: [5, 4])))
        .padding(.top, 10)
        .padding(.leading, 28)
    }

    // ---- derived ----------------------------------------------------------------

    private var appVersion: String {
        let v = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String
        let b = Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String
        return "\(v ?? "?") (\(b ?? "?"))"
    }

    private var tcAppText: String {
        if !app.tcAppVer.isEmpty { return app.tcAppVer }
        if app.teleFresh, let t = app.telemetry { return t.fwVerText } // 遥测 fw_ver 兜底
        return "--"
    }

    private var signalText: String {
        guard app.connState == .connected else { return "--" }
        return "\(app.signalBars)/4 · \(app.signalLabel ?? "")"
    }

    private var linkDetail: String? {
        guard app.connState == .connected else { return nil }
        var parts = ["RTT \(max(app.rttLast, 0)) ms"]
        if let rssi = app.rssiDbm { parts.append("\(rssi) dBm") }
        return parts.joined(separator: " · ")
    }

    private var tcLinkDetail: String? {
        guard app.teleFresh, let t = app.telemetry else { return nil }
        return "RTT \(t.linkRttMs) ms · 误码 \(String(format: "%.1f %%", Double(t.linkErrRate) / 10))"
    }

    private func kv(_ label: String, _ value: String) -> some View {
        HStack {
            Text(label).font(.caption).foregroundStyle(Theme.dim)
            Spacer()
            Text(value).font(Theme.mono(12)).foregroundStyle(Theme.text)
        }
    }

    private func row(_ label: String, _ value: String) -> some View {
        HStack {
            Text(label).font(.subheadline).foregroundStyle(Theme.dim)
            Spacer()
            Text(value).font(Theme.mono(13)).foregroundStyle(Theme.text)
        }
    }
}

// ---- node card ------------------------------------------------------------------

private struct TopologyNode<Rows: View>: View {
    let icon: String
    let title: String
    let role: String
    let status: (text: String, color: Color)
    let glow: Bool
    @ViewBuilder let rows: Rows

    var body: some View {
        Panel {
            HStack(alignment: .top, spacing: 12) {
                Image(systemName: icon)
                    .font(.title2)
                    .foregroundStyle(status.color)
                    .frame(width: 44, height: 44)
                    .background(status.color.opacity(0.12), in: RoundedRectangle(cornerRadius: 12))
                    .glow(status.color, radius: glow ? 8 : 0, opacity: 0.35)
                VStack(alignment: .leading, spacing: 6) {
                    HStack {
                        Text(title)
                            .font(.subheadline.weight(.bold))
                            .foregroundStyle(Theme.text)
                        Spacer()
                        Text(status.text)
                            .font(Theme.mono(11, weight: .bold))
                            .padding(.horizontal, 8)
                            .padding(.vertical, 2)
                            .background(status.color.opacity(0.14), in: Capsule())
                            .foregroundStyle(status.color)
                    }
                    Text(role)
                        .font(.caption2)
                        .foregroundStyle(Theme.dim)
                    rows
                }
            }
        }
    }
}

// ---- link connector ---------------------------------------------------------------

private struct TopologyLink: View {
    let label: String
    var detail: String?
    let up: Bool

    var body: some View {
        HStack(alignment: .center, spacing: 10) {
            TimelineView(.animation(minimumInterval: 0.05)) { timeline in
                let phase = up ? 0.5 + 0.5 * sin(timeline.date.timeIntervalSinceReferenceDate * 2 * .pi / 1.8) : 0
                let color = up ? Theme.accent : Theme.crit.opacity(0.7)
                return VStack(spacing: 0) {
                    Rectangle().fill(color.opacity(0.55)).frame(width: 2, height: 12)
                    Circle()
                        .fill(color)
                        .frame(width: 8, height: 8)
                        .glow(color, radius: 5, opacity: 0.2 + 0.6 * phase)
                    Rectangle().fill(color.opacity(0.55)).frame(width: 2, height: 12)
                }
            }
            .frame(width: 24)
            VStack(alignment: .leading, spacing: 1) {
                Text(label)
                    .font(Theme.mono(11, weight: .semibold))
                    .foregroundStyle(Theme.dim)
                if let detail {
                    Text(detail)
                        .font(.caption2)
                        .foregroundStyle(Theme.text.opacity(0.8))
                }
            }
            Spacer()
        }
        .padding(.leading, 28)
        .padding(.vertical, 6)
    }
}
