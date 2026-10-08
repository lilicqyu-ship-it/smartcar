/*
 * Car3DView.swift — 3D wireframe car for the IMU card. Hand-rolled 3D
 * (Model/Car3DProjection.swift is the testable math): vertices rotate with
 * the fusion attitude and project weak-perspective onto a Canvas, edges are
 * depth-shaded, the nose arrow and the body-frame axes triad mark
 * orientation. Zero dependencies, matching the app's no-third-party rule.
 *
 * Attitude source is the fusion stream (heading/roll/pitch). With no data
 * the car sits level at heading 0 with everything greyed out.
 */

import SwiftUI

struct Car3DView: View {
    var headingDeg: Double
    var rollDeg: Double
    var pitchDeg: Double
    var hasData: Bool

    var body: some View {
        VStack(spacing: 5) {
            Canvas { ctx, size in
                let (vertices, edges) = Car3DProjection.vertices()
                let scale = min(size.width, size.height) * 0.42
                let cx = size.width / 2, cy = size.height * 0.54

                func toCanvas(_ v: Car3DProjection.Vertex, yaw: Double, pitch: Double, roll: Double)
                    -> CGPoint {
                    let rotated = Car3DProjection.rotate(v, yawDeg: yaw, pitchDeg: pitch, rollDeg: roll)
                    let p = Car3DProjection.project(rotated, size: scale)
                    return CGPoint(x: cx + p.x, y: cy + p.y)
                }

                // ground shadow: the footprint projected flat on a stable plane
                // the eye can read roll/pitch against
                var shadow = Path()
                let shadowPts = vertices[0..<8].map {
                    toCanvas((x: $0.x, y: $0.y, z: -0.30), yaw: headingDeg, pitch: 0, roll: 0)
                }
                shadow.move(to: CGPoint(x: shadowPts[0].x, y: shadowPts[0].y + 12))
                for i in 1..<shadowPts.count {
                    shadow.addLine(to: CGPoint(x: shadowPts[i].x, y: shadowPts[i].y + 12))
                }
                shadow.closeSubpath()
                ctx.fill(shadow, with: .color(Theme.dim.opacity(hasData ? 0.14 : 0.07)))

                // depth-shaded wireframe
                let projected = vertices.map { v -> (point: CGPoint, depth: Double) in
                    let rotated = Car3DProjection.rotate(v, yawDeg: headingDeg,
                                                          pitchDeg: pitchDeg, rollDeg: rollDeg)
                    let p = Car3DProjection.project(rotated, size: scale)
                    return (CGPoint(x: cx + p.x, y: cy + p.y), p.depth)
                }
                for (a, b) in edges {
                    let pa = projected[a], pb = projected[b]
                    // depth = x + 1.4 ∈ [0.6 (tail, near) … 2.5 (nose arrow, far)]
                    let t = min(max((2.6 - (pa.depth + pb.depth) / 2) / 2.0, 0), 1) // 0 far … 1 near
                    let isWheel = (a >= 8 && a <= 11) || (b >= 8 && b <= 11)
                    var line = Path()
                    line.move(to: pa.point)
                    line.addLine(to: pb.point)
                    ctx.stroke(line, with: .color((isWheel ? Theme.dim : Theme.text)
                                .opacity((0.30 + 0.55 * t) * (hasData ? 1 : 0.4))),
                               style: StrokeStyle(lineWidth: isWheel ? 1.6 : 2.0, lineCap: .round))
                }

                // nose arrow (accent, so "which way is forward" is unmissable)
                var arrow = Path()
                arrow.move(to: projected[12].point)
                arrow.addLine(to: projected[13].point)
                ctx.stroke(arrow, with: .color(Theme.accent.opacity(hasData ? 0.95 : 0.4)),
                           style: StrokeStyle(lineWidth: 3.0, lineCap: .round))

                // body-frame IMU axes triad from the car's centre: x forward
                // (crit), y left (warn), z up (info) — the AxisLegend colors, so
                // the triad and the strip charts speak the same color language
                let triad: [(v: Car3DProjection.Vertex, label: String, color: Color)] = [
                    ((1.0, 0, 0), "x", Theme.crit), ((0, 0.85, 0), "y", Theme.warn), ((0, 0, 0.85), "z", Theme.info),
                ]
                let origin = toCanvas((0, 0, 0), yaw: headingDeg, pitch: pitchDeg, roll: rollDeg)
                for axis in triad {
                    let tip = toCanvas(axis.v, yaw: headingDeg, pitch: pitchDeg, roll: rollDeg)
                    var line = Path()
                    line.move(to: origin)
                    line.addLine(to: tip)
                    ctx.stroke(line, with: .color(axis.color.opacity(hasData ? 0.85 : 0.3)),
                               style: StrokeStyle(lineWidth: 1.8, lineCap: .round))
                    ctx.draw(Text(axis.label).font(Theme.mono(10)).foregroundStyle(axis.color.opacity(0.9)),
                             at: CGPoint(x: tip.x + (tip.x > origin.x ? 8 : -8), y: tip.y))
                }
            }
            .aspectRatio(1, contentMode: .fit)
            Text(hasData ? String(format: "航向 %.0f°", ((headingDeg + 360).truncatingRemainder(dividingBy: 360))) : "--")
                .font(Theme.mono(10))
                .foregroundStyle(Theme.dim)
            Text("车体 3D 姿态")
                .font(.caption2)
                .foregroundStyle(Theme.dim)
        }
        .background(Theme.bgLift, in: RoundedRectangle(cornerRadius: 14))
        .overlay(RoundedRectangle(cornerRadius: 14)
            .strokeBorder(Theme.panelStroke.opacity(0.6), lineWidth: 1))
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("3D 车体姿态")
        .accessibilityValue(hasData
            ? String(format: "航向 %.0f 度，横滚 %.1f 度，俯仰 %.1f 度", headingDeg, rollDeg, pitchDeg)
            : "等待数据")
    }
}
