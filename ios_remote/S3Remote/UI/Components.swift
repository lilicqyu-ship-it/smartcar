/*
 * Components.swift — cockpit instrument set shared across pages:
 * speed gauge, radial battery meter, RTT chip/sparkline, link pulse.
 */

import SwiftUI

// ---- speed gauge (Home hero) --------------------------------------------------

struct SpeedGaugeView: View {
    @Environment(AppState.self) private var app

    static let maxKmh = 2.2 // full scale: 600 mm/s ≈ 2.16 km/h

    var body: some View {
        ZStack {
            Canvas { ctx, size in
                drawDial(ctx: ctx, size: size)
            }
            VStack(spacing: 0) {
                Text(speedText)
                    .font(Theme.display(54, weight: .heavy))
                    .foregroundStyle(speedColor)
                    .contentTransition(.numericText())
                    .monospacedDigit()
                Text("km/h")
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(Theme.dim)
                stateChip
                    .padding(.top, 6)
            }
        }
    }

    private var speedKmh: Double {
        guard app.teleFresh, let t = app.telemetry else { return 0 }
        let mmS = (Int(t.vMeasL) + Int(t.vMeasR)) / 2
        return abs(Double(mmS) * 3.6 / 1000.0)
    }

    private var speedText: String {
        guard app.teleFresh, app.telemetry != nil else { return "--" }
        return String(format: "%.2f", speedKmh)
    }

    private var speedColor: Color {
        if !app.teleFresh { return Theme.dim }
        if app.stopLatched || app.emergActive { return Theme.stopRed }
        return Theme.text
    }

    private var stateChip: some View {
        let (text, color): (String, Color) = {
            if app.emergActive { return ("E-STOP", Theme.crit) }
            if app.stopLatched { return ("STOPPED", Theme.stopRed) }
            if app.connState != .connected { return ("OFFLINE", Theme.dim) }
            if !app.ctrlRole { return ("NO CTRL", Theme.warn) }
            if speedKmh < 0.02 { return ("READY", Theme.accent) }
            return (outV < 0 ? "REV" : "FWD", Theme.info)
        }()
        return Text(text)
            .font(Theme.mono(12, weight: .bold))
            .padding(.horizontal, 10)
            .padding(.vertical, 3)
            .background(color.opacity(0.14), in: Capsule())
            .overlay(Capsule().strokeBorder(color.opacity(0.55), lineWidth: 1))
            .foregroundStyle(color)
            .glow(color, radius: 4, opacity: 0.25)
    }

    private var outV: Int16 { app.outV }

    private func drawDial(ctx: GraphicsContext, size: CGSize) {
        let center = CGPoint(x: size.width / 2, y: size.height * 0.52)
        let radius = min(size.width, size.height) * 0.44
        let start = Angle.degrees(140)
        let sweep = Angle.degrees(260)
        let fraction = min(speedKmh / Self.maxKmh, 1)

        var track = Path()
        track.addArc(center: center, radius: radius,
                     startAngle: start, endAngle: start + sweep, clockwise: false)
        ctx.stroke(track, with: .color(Theme.panelStroke.opacity(0.55)), lineWidth: 13)

        if fraction > 0.005 {
            // GraphicsContext.Shading has no angular gradient: stroke the arc
            // in small segments, lerping accent → warn → crit along the sweep
            let segments = 44
            let style = StrokeStyle(lineWidth: 13, lineCap: .round)
            for i in 0..<segments {
                let f0 = fraction * Double(i) / Double(segments)
                let f1 = fraction * Double(i + 1) / Double(segments)
                var seg = Path()
                seg.addArc(center: center, radius: radius,
                           startAngle: start + sweep * f0,
                           endAngle: start + sweep * f1, clockwise: false)
                ctx.stroke(seg, with: .color(dialColor(f0)), style: style)
            }
            // needle tip dot at the arc head
            let head = start + sweep * fraction
            let tip = CGPoint(x: center.x + CGFloat(radius * CGFloat(cos(head.radians))),
                              y: center.y + CGFloat(radius * CGFloat(sin(head.radians))))
            ctx.fill(Path(ellipseIn: CGRect(x: tip.x - 5, y: tip.y - 5, width: 10, height: 10)),
                     with: .color(.white))
        }

        // ticks: 9 across the sweep, last fifth in crit (SPORT territory)
        for i in 0...8 {
            let angle = start + sweep * (Double(i) / 8)
            let c = CGFloat(cos(angle.radians))
            let s = CGFloat(sin(angle.radians))
            let inner = CGPoint(x: center.x + (radius - 22) * c,
                                y: center.y + (radius - 22) * s)
            let outer = CGPoint(x: center.x + (radius - 13) * c,
                                y: center.y + (radius - 13) * s)
            var tick = Path()
            tick.move(to: inner)
            tick.addLine(to: outer)
            ctx.stroke(tick, with: .color(i >= 7 ? Theme.crit.opacity(0.8) : Theme.dim.opacity(0.7)),
                       lineWidth: 2)
        }
    }

    /// Three-band dial coloring: accent (cruise) → warn (approaching limit)
    /// → crit (SPORT territory).
    private func dialColor(_ f: Double) -> Color {
        f < 0.55 ? Theme.accent : f < 0.8 ? Theme.warn : Theme.crit
    }
}

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
                Text(label)
                    .font(.caption2.weight(.semibold))
                    .foregroundStyle(Theme.dim)
            }
        }
        .frame(width: size, height: size)
    }
}

// ---- link pulse + chips ----------------------------------------------------------

/// Pulsing link dot: animates while connected, steady red when offline.
struct LinkDotView: View {
    @Environment(AppState.self) private var app

    var body: some View {
        TimelineView(.animation(minimumInterval: 0.05)) { timeline in
            let phase = app.connState == .connected
                ? 0.5 + 0.5 * sin(timeline.date.timeIntervalSinceReferenceDate * 2 * .pi / 1.6)
                : 0
            Circle()
                .fill(connColor)
                .frame(width: 11, height: 11)
                .glow(connColor, radius: 6, opacity: 0.25 + 0.55 * phase)
        }
    }

    private var connColor: Color {
        switch app.connState {
        case .connected: app.teleFresh ? Theme.accent : Theme.warn
        case .connecting: Theme.warn
        case .disconnected: Theme.crit
        }
    }
}

struct RTTChip: View {
    let ms: Int

    var body: some View {
        Text("\(ms) ms")
            .font(Theme.mono(12))
            .padding(.horizontal, 8)
            .padding(.vertical, 3)
            .background(color.opacity(0.14), in: Capsule())
            .foregroundStyle(color)
    }

    private var color: Color {
        ms <= 0 ? Theme.dim : ms <= 60 ? Theme.accent : ms <= 120 ? Theme.warn : Theme.crit
    }
}

// ---- signal strength bars -----------------------------------------------------

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
    }
}
