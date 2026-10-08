/* 3D 姿态仪：车身、车顶和车轮共用旋转；固定比例与地面参照显示倾斜。 */
import SwiftUI

struct Car3DView: View {
    var headingDeg: Double
    var rollDeg: Double
    var pitchDeg: Double
    var hasData: Bool
    var sourceLabel: String = "车体融合"

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 18).fill(Theme.ink)
            Canvas { ctx, size in render(ctx, size: size) }
            VStack {
                HStack {
                    Label("相对旋转", systemImage: "rotate.3d")
                        .font(.caption2.weight(.semibold))
                        .foregroundStyle(.white.opacity(0.7))
                    Spacer()
                    Text(hasData ? String(format: "%+.0f°", headingDeg) : "--°")
                        .font(Theme.mono(17, weight: .bold))
                        .foregroundStyle(.white)
                        .monospacedDigit()
                }
                Spacer()
                HStack {
                    Text(hasData ? sourceLabel : "等待 IMU 数据")
                    Spacer()
                    Text("3D ATTITUDE").tracking(1.2)
                }
                .font(Theme.mono(10))
                .foregroundStyle(.white.opacity(0.66))
            }
            .padding(16)
        }
        .clipShape(RoundedRectangle(cornerRadius: 18))
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("IMU 三维姿态模型")
        .accessibilityValue(hasData
            ? String(format: "%@，相对旋转 %.0f 度，横滚 %.1f 度，俯仰 %.1f 度",
                     sourceLabel, headingDeg, rollDeg, pitchDeg)
            : "等待数据")
    }

    private func render(_ ctx: GraphicsContext, size: CGSize) {
        let centre = CGPoint(x: size.width / 2, y: size.height * 0.55)
        // 地面固定，车身绕自身原点转。逐帧自动缩放会掩盖角度变化。
        for fraction in [0.55, 0.82, 1.10] {
            let r = size.width * fraction / 2
            ctx.stroke(Path(ellipseIn: CGRect(x: centre.x - r, y: centre.y - r * 0.32,
                                             width: 2 * r, height: r * 0.64)),
                       with: .color(.white.opacity(0.11)), lineWidth: 1)
        }
        var grid = Path()
        grid.move(to: CGPoint(x: 16, y: centre.y))
        grid.addLine(to: CGPoint(x: size.width - 16, y: centre.y))
        grid.move(to: CGPoint(x: centre.x, y: 37))
        grid.addLine(to: CGPoint(x: centre.x, y: size.height - 28))
        ctx.stroke(grid, with: .color(.white.opacity(0.09)),
                   style: StrokeStyle(lineWidth: 1, dash: [3, 5]))
        ctx.fill(Path(ellipseIn: CGRect(x: size.width * 0.28, y: size.height * 0.75,
                                      width: size.width * 0.44, height: 17)),
                 with: .color(.black.opacity(0.34)))

        let chassis = Car3DProjection.vertices().0
        let roof: [Car3DProjection.Vertex] = [
            (0.18, -0.36, 0.59), (0.18, 0.36, 0.59),
            (-0.47, 0.36, 0.59), (-0.47, -0.36, 0.59),
        ]
        let fixedScale = min(size.width * 0.67, size.height * 1.02)
        let points = (chassis + roof).map { v -> CGPoint in
            let turned = Car3DProjection.rotate(v, yawDeg: headingDeg,
                                                pitchDeg: pitchDeg, rollDeg: rollDeg)
            let p = Car3DProjection.project(turned, size: 1)
            return CGPoint(x: centre.x + p.x * fixedScale,
                           y: centre.y + (p.y - 0.5) * fixedScale)
        }
        for i in 8..<12 {
            let p = points[i]
            let tire = Path(ellipseIn: CGRect(x: p.x - 12, y: p.y - 8, width: 24, height: 16))
            ctx.fill(tire, with: .color(.black.opacity(0.96)))
            ctx.stroke(tire, with: .color(.white.opacity(0.34)), lineWidth: 1)
            ctx.fill(Path(ellipseIn: CGRect(x: p.x - 3, y: p.y - 3, width: 6, height: 6)),
                     with: .color(.white.opacity(0.44)))
        }
        let bodyColor = Color(red: 0.64, green: 0.70, blue: 0.67)
        let sideColor = Color(red: 0.31, green: 0.39, blue: 0.39)
        ctx.fill(polygon([points[4], points[5], points[1], points[0]]),
                 with: .color(Theme.accentDeep))
        ctx.fill(polygon([points[5], points[6], points[2], points[1]]),
                 with: .color(sideColor.opacity(0.85)))
        ctx.fill(polygon([points[7], points[4], points[0], points[3]]),
                 with: .color(sideColor))
        let deck = polygon([points[0], points[1], points[2], points[3]])
        ctx.fill(deck, with: .color(bodyColor))
        ctx.stroke(deck, with: .color(.white.opacity(0.73)), lineWidth: 1.5)
        ctx.fill(polygon([points[0], points[1], points[15], points[14]]),
                 with: .color(Theme.info.opacity(0.82)))
        ctx.fill(polygon([points[1], points[2], points[16], points[15]]),
                 with: .color(Theme.info.opacity(0.57)))
        ctx.fill(polygon([points[3], points[0], points[14], points[17]]),
                 with: .color(Theme.info.opacity(0.67)))
        let roofPath = polygon([points[14], points[15], points[16], points[17]])
        ctx.fill(roofPath, with: .color(Color(red: 0.75, green: 0.79, blue: 0.76)))
        ctx.stroke(roofPath, with: .color(.white.opacity(0.7)), lineWidth: 1.3)
        let front = midpoint(points[0], points[1])
        let tail = midpoint(points[2], points[3])
        let tip = CGPoint(x: front.x + (front.x - tail.x) * 0.24,
                          y: front.y + (front.y - tail.y) * 0.24)
        var arrow = Path()
        arrow.move(to: midpoint(front, tail))
        arrow.addLine(to: tip)
        ctx.stroke(arrow, with: .color(Theme.accent),
                   style: StrokeStyle(lineWidth: 4, lineCap: .round))
        ctx.fill(Path(ellipseIn: CGRect(x: tip.x - 4, y: tip.y - 4, width: 8, height: 8)),
                 with: .color(Theme.accent))
    }

    private func polygon(_ points: [CGPoint]) -> Path {
        Path { path in
            guard let first = points.first else { return }
            path.move(to: first)
            for p in points.dropFirst() { path.addLine(to: p) }
            path.closeSubpath()
        }
    }

    private func midpoint(_ a: CGPoint, _ b: CGPoint) -> CGPoint {
        CGPoint(x: (a.x + b.x) / 2, y: (a.y + b.y) / 2)
    }
}
