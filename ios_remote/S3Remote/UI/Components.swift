/*
 * Components.swift — cockpit instrument set shared across pages: radial
 * battery meter, signal bars, RTT sparkline, logo gauge, horn button.
 */

import SwiftUI

// ---- radial battery meter -------------------------------------------------------

struct BatteryRadialView: View {
    let pct: Int?
    let color: Color
    var size: CGFloat = 132
    var label: String = "SOC"

    var body: some View {
        ZStack {
            Circle()
                .stroke(Theme.panelStroke.opacity(0.6), lineWidth: 12)
            Circle()
                .trim(from: 0, to: max(0.001, Double(pct ?? 0) / 100))
                .stroke(color, style: StrokeStyle(lineWidth: 12, lineCap: .round))
                .rotationEffect(.degrees(-90))
                .glow(color, radius: 5, opacity: 0.35)
            VStack(spacing: 0) {
                Text(pct.map { "\($0)" } ?? "--")
                    .font(Theme.display(size * 0.24))
                    .foregroundStyle(Theme.text)
                    .monospacedDigit()
                    .contentTransition(.numericText())
                Text(label)
                    .font(.caption2.weight(.semibold))
                    .foregroundStyle(Theme.dim)
            }
        }
        .frame(width: size, height: size)
        .animation(.easeInOut(duration: 0.3), value: pct)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("电量")
        .accessibilityValue(pct.map { "\($0)%" } ?? "无数据")
    }
}

// ---- link pulse + chips ----------------------------------------------------------

// (LinkDotView and RTTChip removed in v1.3 — dead code; the live link chip
// lives in HomeView's StatusDeck and DiagView's stats panel.)

/// 4 ascending bars (0 = offline). Colors follow the S3 remote's quality
/// grading: ≥3 green, 2 amber, ≤1 red.
struct SignalBarsView: View {
    let bars: Int // 0...4
    var compact: Bool = true

    var body: some View {
        HStack(alignment: .bottom, spacing: compact ? 2 : 4) {
            ForEach(0..<4, id: \.self) { i in
                Capsule()
                    .fill(i < bars ? color : Theme.dim.opacity(0.28))
                    .frame(width: compact ? 3.5 : 7,
                           height: compact ? CGFloat(5 + 3 * i) : CGFloat(9 + 6 * i))
            }
        }
        .animation(.easeOut(duration: 0.2), value: bars)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("信号强度")
        .accessibilityValue("\(bars) 格")
    }

    private var color: Color {
        bars >= 3 ? Theme.accent : bars == 2 ? Theme.warn : bars == 1 ? Theme.crit : Theme.dim
    }
}

/// Live RTT sparkline (last 40 pings) for the diagnostics page.
struct RTTSparkline: View {
    let history: [Int]

    var body: some View {
        Canvas { ctx, size in
            guard history.count > 1 else { return }
            let maxMs = max(80, history.max() ?? 80)
            var path = Path()
            for (i, ms) in history.enumerated() {
                let x = size.width * CGFloat(i) / CGFloat(history.count - 1)
                let y = size.height * (1 - CGFloat(min(ms, maxMs)) / CGFloat(maxMs))
                if i == 0 { path.move(to: CGPoint(x: x, y: y)) }
                else { path.addLine(to: CGPoint(x: x, y: y)) }
            }
            ctx.stroke(path, with: .color(Theme.accent), style: StrokeStyle(lineWidth: 2, lineCap: .round))
            ctx.stroke(path, with: .color(Theme.accent.opacity(0.18)), style: StrokeStyle(lineWidth: 6))
        }
        .accessibilityLabel("往返时延走势图")
        .accessibilityValue(history.last.map { "当前 \($0) 毫秒" } ?? "暂无采样")
    }
}

// ---- logo gauge (iOS home-screen battery ring style) ----------------------------

/// Concentric arc rings around the app mark — outer ring = battery %, inner
/// ring = signal quality, in the style of the iOS home-screen battery widget.
/// Display-only: uses the debounced battery display value and the composite
/// signal bars; alarms/colors elsewhere keep using raw telemetry.
struct LogoGaugeView: View {
    @Environment(AppState.self) private var app

    private let ringWidth: CGFloat = 5
    private let innerPadding: CGFloat = 8.5

    private var batteryFraction: Double {
        min(max(Double(app.batteryDisplayPct ?? 0) / 100, 0), 1)
    }

    private var signalFraction: Double {
        min(max(Double(app.signalBars) / 4, 0), 1)
    }

    var body: some View {
        ZStack {
            ring(padding: 0, fraction: batteryFraction, color: batteryColor)
            ring(padding: innerPadding, fraction: signalFraction, color: signalColor)
            centerMark
        }
        .frame(width: 66, height: 66)
        .animation(.easeInOut(duration: 0.4), value: batteryFraction)
        .animation(.easeInOut(duration: 0.4), value: signalFraction)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("电量 \(app.batteryDisplayPct.map(String.init) ?? "--")%,信号 \(app.signalBars) 格")
    }

    /// Track + value arc, filling clockwise from 12 o'clock.
    private func ring(padding p: CGFloat, fraction: Double, color: Color) -> some View {
        ZStack {
            Circle()
                .stroke(Theme.dim.opacity(0.20), lineWidth: ringWidth)
                .padding(p)
            Circle()
                .trim(from: 0, to: fraction)
                .stroke(color, style: StrokeStyle(lineWidth: ringWidth, lineCap: .round))
                .padding(p)
                .rotationEffect(.degrees(-90))
        }
    }

    private var centerMark: some View {
        Image(systemName: "steeringwheel")
            .font(.system(size: 21, weight: .light))
            .foregroundStyle(Theme.accent)
            .frame(width: 38, height: 38)
            .background(Theme.panel, in: Circle())
            .overlay(Circle().strokeBorder(Theme.panelStroke, lineWidth: 1))
    }

    /// Same grading as the C6 page's raw battery pill: green >20 %, amber
    /// ≤20 %, red ≤10 %.
    private var batteryColor: Color {
        let pct = app.batteryDisplayPct ?? 100
        if pct <= 10 { return Theme.crit }
        if pct <= 20 { return Theme.warn }
        return Theme.live
    }

    /// Same grading as SignalBarsView: ≥3 accent, 2 amber, 1 red, 0 dim.
    private var signalColor: Color {
        switch app.signalBars {
        case 3...: return Theme.accent
        case 2: return Theme.warn
        case 1: return Theme.crit
        default: return Theme.dim
        }
    }
}

// ---- horn (synthesized locally, needs the sound toggle on) ----------------------

struct HornButton: View {
    @Environment(AppState.self) private var app

    var body: some View {
        Button {
            app.hornPressed()
        } label: {
            Image(systemName: "speaker.wave.3.fill")
                .font(.system(size: 15, weight: .bold))
                .frame(width: 40, height: 40)
                .background(.white.opacity(0.09), in: Circle())
                .foregroundStyle(.white)
        }.buttonStyle(.plain)
            .disabled(!app.ctrlRole)
            .opacity(app.ctrlRole ? 1 : 0.5)
            .accessibilityLabel("喇叭")
    }
}
