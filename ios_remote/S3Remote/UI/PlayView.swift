/*
 * PlayView.swift — the playground tab: one-tap stunts, live trail canvas,
 * lap-stopwatch challenge and the local record wall. Everything rides the
 * normal drive gate: stunts abort on STOP / e-stop / takeover / link loss.
 */

import SwiftUI
import UIKit // UIImage / ImageRenderer for the trail snapshot share

struct PlayView: View {
    @Environment(AppState.self) private var app

    var body: some View {
        Page {
            ScrollView {
                VStack(spacing: 18) {
                    PageHeading(eyebrow: "PLAYGROUND", title: "玩出花样",
                                subtitle: "特技、轨迹与纪录，全部走同一套安全门控。")
                    if !app.ctrlRole {
                        ControlHintCard()
                    }
                    StuntGrid()
                    TrailCard()
                    LapCard()
                    RecordsCard()
                }
                .padding(.horizontal, 20).padding(.top, 8).padding(.bottom, 16)
                .frame(maxWidth: 680)
                .frame(maxWidth: .infinity)
            }
        }
    }
}

/// Non-blocking hint when driving is not possible yet.
struct ControlHintCard: View {
    @Environment(AppState.self) private var app

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: app.connState == .connected ? "key.fill" : "wifi")
                .font(.system(size: 18, weight: .semibold))
            Text(app.connState == .connected
                 ? "已连接但未取得控制权 — 前往「连接」页完成配对"
                 : "先连接车辆，再开始玩耍")
                .font(.subheadline)
            Spacer()
        }.foregroundStyle(Theme.text).padding(14)
            .background(Theme.warn.opacity(0.12), in: RoundedRectangle(cornerRadius: 16))
    }
}

// ---- stunts ------------------------------------------------------------------------

struct StuntGrid: View {
    @Environment(AppState.self) private var app
    private let columns = [GridItem(.flexible(), spacing: 10), GridItem(.flexible(), spacing: 10)]

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text("特技动作").font(.subheadline.bold())
                Spacer()
                if app.sequencer.active {
                    Label("执行中 · 再点中止", systemImage: "waveform")
                        .font(.caption).foregroundStyle(Theme.accent)
                }
            }
            LazyVGrid(columns: columns, spacing: 10) {
                ForEach(Stunt.library) { stunt in
                    StuntCard(stunt: stunt)
                }
            }
        }
    }
}

struct StuntCard: View {
    let stunt: Stunt
    @Environment(AppState.self) private var app

    private var running: Bool { app.sequencer.stunt == stunt }
    private var enabled: Bool { app.ctrlRole && !app.emergActive && app.connState == .connected }

    var body: some View {
        Button {
            Haptics.light()
            app.startStunt(stunt)
        } label: {
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    Image(systemName: stunt.icon)
                        .font(.system(size: 19, weight: .semibold))
                        .foregroundStyle(running ? Color.white : Theme.accent)
                        .frame(width: 34, height: 34)
                        .background(running ? Color.white.opacity(0.18) : Theme.accent.opacity(0.10),
                                    in: RoundedRectangle(cornerRadius: 10))
                    Spacer()
                    if running {
                        Text("\(Int(app.sequencer.progress * 100))%")
                            .font(Theme.mono(11)).monospacedDigit()
                            .foregroundStyle(.white.opacity(0.85))
                    }
                }
                Text(stunt.name).font(.subheadline.bold())
                    .foregroundStyle(running ? .white : Theme.text)
                Text(stunt.subtitle).font(.caption2)
                    .foregroundStyle(running ? .white.opacity(0.75) : Theme.dim)
                if running {
                    ProgressView(value: app.sequencer.progress)
                        .tint(.white)
                }
            }
            .padding(12)
            .frame(maxWidth: .infinity, minHeight: 108, alignment: .leading)
            .background(running ? Theme.accent : Theme.panel,
                        in: RoundedRectangle(cornerRadius: 18))
            .overlay(RoundedRectangle(cornerRadius: 18)
                .strokeBorder(running ? Theme.accentDeep : Theme.panelStroke, lineWidth: 1))
            .animation(.easeInOut(duration: 0.2), value: running)
        }.buttonStyle(.plain)
            .disabled(!enabled)
            .opacity(enabled ? 1 : 0.5)
            .accessibilityHint(running ? "再次点击中止" : "开始执行\(stunt.name)")
    }
}

// ---- live trail ----------------------------------------------------------------------

struct TrailCard: View {
    @Environment(AppState.self) private var app
    @State private var shareImage: UIImage?
    @State private var showShare = false

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text("实时轨迹").font(.subheadline.bold())
                Spacer()
                Text(String(format: "本迹 %.1f m", app.odometry.distanceMm / 1000))
                    .font(Theme.mono(11)).foregroundStyle(Theme.dim)
                Button {
                    shareImage = renderSnapshot()
                    showShare = shareImage != nil
                } label: {
                    Image(systemName: "square.and.arrow.up")
                        .font(.system(size: 13, weight: .semibold))
                        .frame(width: 30, height: 30)
                        .background(Theme.bgLift, in: RoundedRectangle(cornerRadius: 8))
                }.buttonStyle(.plain)
                    .disabled(app.odometry.points.count < 2)
                    .accessibilityLabel("分享轨迹图")
                Button {
                    Haptics.light()
                    app.clearTrail()
                } label: {
                    Label("清除", systemImage: "eraser")
                        .font(.caption).foregroundStyle(Theme.accent)
                }.buttonStyle(.plain)
            }
            TrailCanvas(points: app.odometry.points, heading: app.odometry.heading)
                .frame(height: 250)
                .background(Theme.ink, in: RoundedRectangle(cornerRadius: 20))
            Text("由左右轮速推算 · 断流超 2 秒自动重画 · 设置里可调轮距")
                .font(.caption2).foregroundStyle(Theme.dim)
        }.padding(14)
            .background(Theme.panel, in: RoundedRectangle(cornerRadius: 22))
            .overlay(RoundedRectangle(cornerRadius: 22).strokeBorder(Theme.panelStroke, lineWidth: 1))
            .sheet(isPresented: $showShare) {
                if let shareImage {
                    ActivityShareSheet(items: [shareImage])
                }
            }
    }

    /// Off-screen render of the current trail for the share sheet.
    private func renderSnapshot() -> UIImage? {
        let rendered = TrailCanvas(points: app.odometry.points, heading: app.odometry.heading)
            .frame(width: 640, height: 420)
            .background(Theme.ink)
        let renderer = ImageRenderer(content: rendered)
        renderer.scale = 2
        return renderer.uiImage
    }
}

/// UIActivityViewController bridge for the trail snapshot share.
struct ActivityShareSheet: UIViewControllerRepresentable {
    let items: [Any]

    func makeUIViewController(context: Context) -> UIActivityViewController {
        UIActivityViewController(activityItems: items, applicationActivities: nil)
    }

    func updateUIViewController(_ vc: UIActivityViewController, context: Context) {}
}

/// Trail renderer. Points/heading are passed in (read in body) so @Observable
/// tracking fires on every telemetry frame — Canvas closures alone don't.
struct TrailCanvas: View {
    let points: [OdometryPoint]
    let heading: Double

    /// px per mm ceiling: spinning in place barely moves, don't blow it up.
    private static let maxScale: Double = 0.5

    var body: some View {
        Canvas { ctx, size in
            guard points.count > 1 else {
                drawHint(ctx: ctx, size: size)
                return
            }
            drawTrail(ctx: ctx, size: size)
        }
        .drawingGroup()
        .accessibilityLabel("车辆轨迹图")
    }

    private func drawHint(ctx: GraphicsContext, size: CGSize) {
        ctx.draw(Text("开车走出第一条轨迹")
            .font(.caption).foregroundStyle(.white.opacity(0.35)),
                 at: CGPoint(x: size.width / 2, y: size.height / 2))
    }

    private func drawTrail(ctx: GraphicsContext, size: CGSize) {
        let pad: CGFloat = 18
        var minX = 0.0, maxX = 0.0, minY = 0.0, maxY = 0.0
        for p in points {
            minX = min(minX, p.x); maxX = max(maxX, p.x)
            minY = min(minY, p.y); maxY = max(maxY, p.y)
        }
        let w = max(maxX - minX, 40), h = max(maxY - minY, 40)
        let scale = min(Double(size.width - pad * 2) / w,
                        Double(size.height - pad * 2) / h,
                        Self.maxScale)
        let midX = (minX + maxX) / 2, midY = (minY + maxY) / 2
        func map(_ p: OdometryPoint) -> CGPoint {
            CGPoint(x: size.width / 2 + CGFloat((p.x - midX) * scale),
                    y: size.height / 2 - CGFloat((p.y - midY) * scale)) // y flip: +y is left
        }

        let whole = Path { path in
            path.move(to: map(points[0]))
            for p in points.dropFirst() { path.addLine(to: map(p)) }
        }
        ctx.stroke(whole, with: .color(.white.opacity(0.40)),
                   style: StrokeStyle(lineWidth: 2, lineCap: .round, lineJoin: .round))

        // recent tail in accent — where the car is now
        let tailStart = max(0, points.count - max(points.count / 8, 12))
        let tail = Path { path in
            path.move(to: map(points[tailStart]))
            for p in points[(tailStart + 1)...] { path.addLine(to: map(p)) }
        }
        ctx.stroke(tail, with: .color(Theme.accent),
                   style: StrokeStyle(lineWidth: 2.5, lineCap: .round, lineJoin: .round))

        // origin ring + current position with heading tick
        let origin = map(points[0])
        ctx.stroke(Path(ellipseIn: CGRect(x: origin.x - 5, y: origin.y - 5, width: 10, height: 10)),
                   with: .color(.white.opacity(0.5)), lineWidth: 1.5)
        let here = map(points[points.count - 1])
        ctx.fill(Path(ellipseIn: CGRect(x: here.x - 4.5, y: here.y - 4.5, width: 9, height: 9)),
                 with: .color(.white))
        let dx = CGFloat(cos(heading)), dy = CGFloat(-sin(heading))
        var tick = Path()
        tick.move(to: here)
        tick.addLine(to: CGPoint(x: here.x + dx * 13, y: here.y + dy * 13))
        ctx.stroke(tick, with: .color(Theme.accent), style: StrokeStyle(lineWidth: 2.5, lineCap: .round))
    }
}

// ---- lap stopwatch -----------------------------------------------------------------

struct LapCard: View {
    @Environment(AppState.self) private var app

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text("圈速挑战").font(.subheadline.bold())
                Spacer()
                if app.recordsStore.records.bestLapS > 0 {
                    Label(String(format: "最佳 %.2f s", app.recordsStore.records.bestLapS),
                          systemImage: "crown.fill")
                        .font(.caption).foregroundStyle(Theme.warn)
                }
            }
            TimelineView(.periodic(from: .now, by: 0.05)) { _ in
                Text(Self.format(app.lapTimer.currentS(nowMs: app.nowMs)))
                    .font(Theme.display(42, weight: .heavy))
                    .monospacedDigit()
                    .contentTransition(.numericText())
                    .animation(.easeInOut(duration: 0.15), value: Self.format(app.lapTimer.currentS(nowMs: app.nowMs)))
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .foregroundStyle(app.lapTimer.running ? Theme.text : Theme.dim)
            }
            HStack(spacing: 10) {
                if app.lapTimer.running {
                    lapButton("打圈", icon: "flag.checkered.fill", prominent: true) { app.lapSplit() }
                    lapButton("结束", icon: "stop.fill", prominent: false) { app.lapStop() }
                } else {
                    lapButton(app.lapTimer.finishedS == nil ? "开始计时" : "再来一轮",
                              icon: "timer", prominent: true) { app.lapStart() }
                    lapButton("重置", icon: "arrow.uturn.backward", prominent: false) { app.lapReset() }
                        .disabled(app.lapTimer.finishedS == nil && app.lapTimer.laps.isEmpty)
                }
            }
            if !app.lapTimer.laps.isEmpty {
                lapList
            }
        }.padding(14)
            .background(Theme.panel, in: RoundedRectangle(cornerRadius: 22))
            .overlay(RoundedRectangle(cornerRadius: 22).strokeBorder(Theme.panelStroke, lineWidth: 1))
    }

    private var lapList: some View {
        let best = app.recordsStore.records.bestLapS
        return VStack(spacing: 6) {
            ForEach(Array(app.lapTimer.laps.enumerated().reversed().prefix(6)), id: \.offset) { item in
                let (index, split) = item
                HStack {
                    Text("LAP \(index + 1)").font(Theme.mono(11)).foregroundStyle(Theme.dim)
                    Spacer()
                    if abs(split - best) < 0.0001 {
                        Image(systemName: "crown.fill").font(.caption2).foregroundStyle(Theme.warn)
                    }
                    Text(Self.format(split)).font(Theme.mono(13)).monospacedDigit()
                }
            }
        }
    }

    private func lapButton(_ title: String, icon: String, prominent: Bool,
                           action: @escaping () -> Void) -> some View {
        Button {
            Haptics.light()
            action()
        } label: {
            Label(title, systemImage: icon)
                .font(.subheadline.bold())
                .frame(maxWidth: .infinity, minHeight: 44)
                .foregroundStyle(prominent ? .white : Theme.text)
                .background(prominent ? Theme.accent : Theme.bgLift,
                            in: RoundedRectangle(cornerRadius: 14))
        }.buttonStyle(.plain)
    }

    static func format(_ seconds: Double) -> String {
        let s = max(0, seconds)
        return String(format: "%02d:%05.2f", Int(s) / 60, s.truncatingRemainder(dividingBy: 60))
    }
}

// ---- record wall ----------------------------------------------------------------------

struct RecordsCard: View {
    @Environment(AppState.self) private var app
    @State private var confirmClear = false

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text("纪录墙").font(.subheadline.bold())
                Spacer()
                Button("清空纪录") { confirmClear = true }
                    .font(.caption).foregroundStyle(Theme.accent)
                    .disabled(app.recordsStore.records == Records())
            }
            HStack(spacing: 10) {
                recordTile("极速", value: String(format: "%.2f", app.recordsStore.records.topSpeedKmh),
                           unit: "km/h", icon: "speedometer")
                recordTile("单程最远", value: String(format: "%.1f", app.recordsStore.records.longestSessionM),
                           unit: "m", icon: "road.lanes")
                recordTile("最快圈速",
                           value: app.recordsStore.records.bestLapS > 0
                               ? String(format: "%.2f", app.recordsStore.records.bestLapS) : "—",
                           unit: "s", icon: "crown.fill")
            }
        }.padding(14)
            .background(Theme.panel, in: RoundedRectangle(cornerRadius: 22))
            .overlay(RoundedRectangle(cornerRadius: 22).strokeBorder(Theme.panelStroke, lineWidth: 1))
            .confirmationDialog("清空全部纪录？", isPresented: $confirmClear, titleVisibility: .visible) {
                Button("清空", role: .destructive) { app.clearRecords() }
                Button("取消", role: .cancel) {}
            } message: {
                Text("极速、单程最远与最快圈速将归零，不可恢复。")
            }
    }

    private func recordTile(_ title: String, value: String, unit: String, icon: String) -> some View {
        VStack(alignment: .leading, spacing: 5) {
            Image(systemName: icon).font(.system(size: 14, weight: .semibold))
                .foregroundStyle(Theme.accent)
            HStack(alignment: .firstTextBaseline, spacing: 4) {
                Text(value).font(Theme.display(21)).monospacedDigit()
                    .contentTransition(.numericText())
                Text(unit).font(.caption2).foregroundStyle(Theme.dim)
            }
            .animation(.easeInOut(duration: 0.3), value: value)
            Text(title).font(.caption2).foregroundStyle(Theme.dim)
        }.frame(maxWidth: .infinity, minHeight: 86, alignment: .leading)
            .padding(11)
            .background(Theme.bgLift, in: RoundedRectangle(cornerRadius: 15))
    }
}
