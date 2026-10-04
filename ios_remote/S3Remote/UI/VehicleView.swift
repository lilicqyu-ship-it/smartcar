import SwiftUI

struct VehicleView: View {
    @Environment(AppState.self) private var app

    var body: some View {
        Page {
            ScrollView {
                VStack(spacing: 18) {
                    PageHeading(eyebrow: "VEHICLE / OVERVIEW", title: "车辆概览", subtitle: "了解车辆状态，让每一次出发更安心。")
                    batteryCard
                    speedCard
                    odoCard
                    statusCard
                }
                .padding(20)
                .frame(maxWidth: 680)
                .frame(maxWidth: .infinity)
            }
        }
    }

    private var liveTelemetry: Telemetry? { app.teleFresh ? app.telemetry : nil }

    private var batteryCard: some View {
        Panel {
            VStack(alignment: .leading, spacing: 18) {
                HStack(spacing: 22) {
                    BatteryRadialView(pct: app.teleFresh ? app.batteryDisplayPct : nil,
                                      color: batteryColor, size: 100, label: "剩余电量 %")
                    VStack(alignment: .leading, spacing: 8) {
                        Text("电池状态").font(.headline)
                        Text((app.teleFresh ? app.batteryDisplayMv : nil).map {
                            String(format: "%.2f V", Double($0) / 1000)
                        } ?? "等待遥测").font(Theme.display(24)).monospacedDigit()
                        Text("电量过低时请及时充电").font(.caption).foregroundStyle(Theme.dim)
                    }
                    Spacer(minLength: 0)
                }
                Divider()
                Label("低电提醒 20% · 临界提醒 10%", systemImage: "bolt.shield")
                    .font(.caption).foregroundStyle(Theme.dim)
            }
        }
    }

    private var batteryColor: Color {
        guard let pct = liveTelemetry?.batteryPct else { return Theme.dim } // no data yet
        return pct > 20 ? Theme.accent : (pct > 10 ? Theme.warn : Theme.crit)
    }

    private var speedCard: some View {
        Panel { VStack(spacing: 12) {
            HStack(spacing: 6) {
                Image(systemName: "speedometer")
                    .font(.caption)
                    .foregroundStyle(Theme.accent)
                Text("轮速 mm/s（目标 / 实测）")
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(Theme.dim)
                Spacer()
            }
            sideRow("左", target: liveTelemetry?.vTargetL, meas: liveTelemetry?.vMeasL)
            sideRow("右", target: liveTelemetry?.vTargetR, meas: liveTelemetry?.vMeasR)
        } }
    }

    private func sideRow(_ side: String, target: Int16?, meas: Int16?) -> some View {
        HStack {
            Text(side)
                .font(Theme.mono(13, weight: .bold))
                .foregroundStyle(Theme.info)
                .frame(width: 30, height: 30)
                .background(Theme.info.opacity(0.12), in: Circle())
            Text(target.map { String($0) } ?? "--")
                .font(Theme.mono(16))
                .frame(maxWidth: .infinity)
            Text(meas.map { String($0) } ?? "--")
                .font(Theme.mono(16, weight: .bold))
                .foregroundStyle(Theme.accent)
                .frame(maxWidth: .infinity)
        }
    }

    private var odoCard: some View {
        Panel {
            HStack(spacing: 0) {
                Spacer()
                VStack(spacing: 2) {
                    Text(liveTelemetry.map { String(format: "%.1f", Double($0.odoSessionMm) / 1000) } ?? "--")
                        .font(Theme.display(24))
                        .monospacedDigit()
                    Text("本次 m")
                        .font(.caption2)
                        .foregroundStyle(Theme.dim)
                }
                .frame(maxWidth: .infinity)
                Divider().frame(height: 40)
                VStack(spacing: 2) {
                    Text(liveTelemetry.map { String(format: "%.2f", Double($0.odoTotalMm) / 1_000_000) } ?? "--")
                        .font(Theme.display(24))
                        .monospacedDigit()
                    Text("总里程 km")
                        .font(.caption2)
                        .foregroundStyle(Theme.dim)
                }
                .frame(maxWidth: .infinity)
                Spacer()
            }
        }
    }

    private var statusCard: some View {
        Panel { VStack(spacing: 10) {
            kv("任务状态", liveTelemetry.map { FaultText.robotState($0.state) } ?? "--")
            kv("车辆状态", faultDisplay)
            kv("运行时间", liveTelemetry.map { formatUptime($0.uptimeMs) } ?? "--")
            kv("链路 RTT (车端)", liveTelemetry.map { "\($0.linkRttMs) ms" } ?? "--")
            kv("链路误码", liveTelemetry.map { String(format: "%.1f %%", Double($0.linkErrRate) / 10) } ?? "--")
            Divider()
            kv("C6 固件", app.c6Ver.isEmpty ? "--" : app.c6Ver)
            kv("TC275 固件", liveTelemetry.map { $0.fwVerText } ?? "--")
            kv("硬件版本", liveTelemetry.map { "rev \($0.hwRev)" } ?? "--")
        } }
    }

    /// Chinese fault text, hex kept for unknown codes (FaultText mapping).
    private var faultDisplay: String {
        guard let code = liveTelemetry?.faultCode else { return "--" }
        return code == 0 ? "正常" : FaultText.describe(code)
    }

    private func formatUptime(_ ms: UInt32) -> String {
        let s = Int(ms / 1000)
        return String(format: "%02d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60)
    }

    private func kv(_ label: String, _ value: String) -> some View {
        HStack {
            Text(label).font(.subheadline).foregroundStyle(Theme.dim)
            Spacer()
            Text(value).font(Theme.mono(14)).foregroundStyle(Theme.text)
        }
    }
}
