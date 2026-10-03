/*
 * DiagView.swift v2 — P4 diagnostics: link stat tiles, RTT sparkline,
 * pairing card (doc 04 §4 error→action mapping), event ring log with
 * severity dots.
 */

import SwiftUI

struct DiagView: View {
    @Environment(AppState.self) private var app
    @State private var pairing = false

    var body: some View {
        Page {
            ScrollView {
                VStack(spacing: 18) {
                    PageHeading(eyebrow: "CONNECT / INSIGHTS", title: "连接中心", subtitle: "配对车辆，查看链路与实时事件。")
                    statsPanel
                    pairPanel
                    eventLogPanel
                }
                .padding(20)
                .frame(maxWidth: 680)
                .frame(maxWidth: .infinity)
            }
        }
    }

    private var statsPanel: some View {
        Panel { VStack(spacing: 12) {
            HStack {
                Label("链路统计", systemImage: "antenna.rtl")
                    .font(.subheadline.weight(.semibold))
                    .foregroundStyle(Theme.dim)
                Spacer()
                Text(app.connState == .connected ? "已连接" : app.connState == .connecting ? "连接中" : "未连接")
                    .font(Theme.mono(13, weight: .bold))
                    .foregroundStyle(app.connState == .connected ? Theme.accent : Theme.dim)
            }
            HStack(spacing: 8) {
                statTile("TX", "\(app.txRate)", unit: "帧/s", color: Theme.info)
                statTile("RX", "\(app.rxRate)", unit: "帧/s", color: Theme.accent)
                statTile("丢包", String(format: "%.1f", app.lossCounter.lossPerMille),
                         unit: "‰", color: app.lossCounter.lossPerMille > 20 ? Theme.warn : Theme.text)
            }
            // signal strength: gateway RSSI when provided, RTT/loss estimate otherwise
            HStack(spacing: 10) {
                SignalBarsView(bars: app.signalBars, compact: false)
                VStack(alignment: .leading, spacing: 1) {
                    Text("信号强度")
                        .font(.subheadline)
                        .foregroundStyle(Theme.dim)
                    Text(app.signalLabel ?? "未连接")
                        .font(Theme.mono(12))
                        .foregroundStyle(Theme.text)
                }
                Spacer()
                if app.rssiDbm == nil && app.connState == .connected {
                    Text("网关未报 RSSI")
                        .font(.caption2)
                        .foregroundStyle(Theme.dim)
                }
            }
            RTTSparkline(history: app.rttHistory)
                .frame(height: 44)
            HStack {
                Text("RTT last / min / max")
                    .font(.subheadline)
                    .foregroundStyle(Theme.dim)
                Spacer()
                Text("\(app.rttLast) / \(app.rttMin) / \(app.rttMax) ms")
                    .font(Theme.mono(13))
            }
        } }
    }

    private func statTile(_ title: String, _ value: String, unit: String, color: Color) -> some View {
        VStack(spacing: 3) {
            Text(title)
                .font(.caption2.weight(.semibold))
                .foregroundStyle(Theme.dim)
            HStack(alignment: .firstTextBaseline, spacing: 3) {
                Text(value)
                    .font(Theme.display(20))
                    .foregroundStyle(color)
                    .monospacedDigit()
                Text(unit)
                    .font(.caption2)
                    .foregroundStyle(Theme.dim)
            }
        }
        .frame(maxWidth: .infinity, minHeight: 62)
        .background(Theme.bgLift, in: RoundedRectangle(cornerRadius: 14))
        .overlay(
            RoundedRectangle(cornerRadius: 14)
                .strokeBorder(Theme.panelStroke.opacity(0.6), lineWidth: 1))
    }

    private var pairPanel: some View {
        Panel { VStack(alignment: .leading, spacing: 10) {
            HStack {
                Label("配对", systemImage: "link.badge.plus")
                    .font(.subheadline.weight(.semibold))
                    .foregroundStyle(Theme.dim)
                Spacer()
                if let w = app.pairWindow {
                    Text("窗口: \(w.rawValue)")
                        .font(Theme.mono(12))
                        .padding(.horizontal, 7)
                        .padding(.vertical, 2)
                        .background(Theme.info.opacity(0.14), in: Capsule())
                        .foregroundStyle(Theme.info)
                }
            }
            Text("长按车侧配对键 3 s 开窗，然后点这里。配对成功后将自动连接。")
                .font(.caption)
                .foregroundStyle(Theme.dim)
            Button {
                pairing = true
                Haptics.light()
                Task {
                    let status = await app.link?.pair() ?? "引擎未启动"
                    app.pairStatus = status
                    pairing = false
                }
            } label: {
                HStack {
                    Image(systemName: pairing ? "hourglass" : "link")
                    Text(pairing ? "配对中…" : "配对车辆")
                        .font(.body.weight(.bold))
                }
                .frame(maxWidth: .infinity, minHeight: 44)
            }
            .buttonStyle(.borderedProminent)
                .foregroundStyle(.white)
            .disabled(pairing)
            if !app.pairStatus.isEmpty {
                Label(app.pairStatus, systemImage: "exclamationmark.bubble")
                    .font(.footnote)
                    .foregroundStyle(Theme.warn)
            }
        } }
    }

    private var eventLogPanel: some View {
        Panel { VStack(alignment: .leading, spacing: 9) {
            Label("事件日志", systemImage: "list.bullet.rectangle")
                .font(.subheadline.weight(.semibold))
                .foregroundStyle(Theme.dim)
            ForEach(app.events.prefix(50)) { e in
                HStack(alignment: .top, spacing: 8) {
                    Circle()
                        .fill(levelColor(e.level))
                        .frame(width: 7, height: 7)
                        .padding(.top, 4)
                    Text(e.at.formatted(.dateTime.hour().minute().second()))
                        .font(Theme.mono(10, weight: .regular))
                        .foregroundStyle(Theme.dim)
                    Text(e.text)
                        .font(.footnote)
                        .foregroundStyle(Theme.text.opacity(e.level == "INFO" ? 0.85 : 1))
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
            }
        } }
    }

    private func levelColor(_ level: String) -> Color {
        switch level {
        case "CRIT": Theme.crit
        case "WARN": Theme.warn
        default: Theme.accent
        }
    }
}
