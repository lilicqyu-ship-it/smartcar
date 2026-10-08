/*
 * Car3DProjection.swift — the math behind the sensor tab's 3D car view.
 * Pure value logic, unit-testable: yaw/pitch/roll rotation of the wireframe
 * vertices and a weak perspective projection onto the 2D canvas.
 *
 * Frame convention matches the car: x forward (nose), y left, z up; yaw
 * (heading) about z, pitch about y, roll about x — the same order the
 * fusion publishes them in. The projection looks at the car from behind
 * and above, so "nose right" on screen means the car turned right.
 */

import Foundation

public enum Car3DProjection {
    /// One wireframe vertex in body units (car ~ 1.6 long, 1.0 wide, 0.5 tall).
    public typealias Vertex = (x: Double, y: Double, z: Double)

    /// Chassis box + four wheel hubs + nose arrow tips, in body units.
    /// Callers index fixed slices: 0..<8 chassis, 8..<12 wheels, 12..<14 nose.
    public static func vertices() -> ([Vertex], [(Int, Int)]) {
        let l = 0.80, w = 0.50, h = 0.28       // half extents
        let wl = 0.52, ww = 0.62, wz = -0.12   // wheels: forward offset, half width, height
        let v: [Vertex] = [
            // chassis corners, CCW from top-front-left
            (l, -w, h), (l, w, h), (-l, w, h), (-l, -w, h),
            (l, -w, -0.10), (l, w, -0.10), (-l, w, -0.10), (-l, -w, -0.10),
            // wheel hubs (left-front, right-front, left-rear, right-rear)
            (wl, ww, wz), (wl, -ww, wz), (-wl, ww, wz), (-wl, -ww, wz),
            // nose arrow (ahead of the front face)
            (l + 0.30, 0, 0.06), (l + 0.14, 0, 0.06),
        ]
        let edges: [(Int, Int)] = [
            (0, 1), (1, 2), (2, 3), (3, 0),         // top face
            (4, 5), (5, 6), (6, 7), (7, 4),         // bottom face
            (0, 4), (1, 5), (2, 6), (3, 7),         // pillars
            (8, 9), (10, 11),                        // axles
            (12, 13),                                 // nose arrow shaft
        ]
        return (v, edges)
    }

    /// Rotate a body vertex by yaw (heading, +z, CCW from above), then
    /// pitch (+y, nose-up positive), then roll (+x, left-side-up positive)
    /// — the fusion's own attitude conventions (heading/roll/pitch are
    /// independent estimates; this composite is for display only).
    /// Standard right-handed rotations:
    ///   Rz(ψ): x' = x·cψ − y·sψ, y' = x·sψ + y·cψ
    ///   Ry(θ) for nose-up +θ: x' = x·cθ − z·sθ, z' = x·sθ + z·cθ
    ///   Rx(φ): y' = y·cφ − z·sφ, z' = y·sφ + z·cφ
    public static func rotate(_ v: Vertex, yawDeg: Double, pitchDeg: Double, rollDeg: Double) -> Vertex {
        let yaw = yawDeg * .pi / 180, pitch = pitchDeg * .pi / 180, roll = rollDeg * .pi / 180
        // yaw about z
        var x = v.x * cos(yaw) - v.y * sin(yaw)
        var y = v.x * sin(yaw) + v.y * cos(yaw)
        var z = v.z
        // pitch about y, nose-up positive: the nose tip (x>0) rises (z>0)
        let xp = x * cos(pitch) - z * sin(pitch)
        let zp = x * sin(pitch) + z * cos(pitch)
        x = xp; z = zp
        // roll about x
        let yp = y * cos(roll) - z * sin(roll)
        let zpp = y * sin(roll) + z * cos(roll)
        y = yp; z = zpp
        return (x, y, z)
    }

    /// Oblique ¾-view projection (chase-cam flavour, no look-at matrix):
    /// body x (forward) runs up-right on screen and foreshortens through
    /// the depth scale, body y (left) runs screen-left, body z (up) is
    /// screen-up. `depth` (for edge shading) grows toward the nose.
    public static func project(_ v: Vertex, size: Double) -> (x: Double, y: Double, depth: Double) {
        let focal = 2.6
        let cameraOffset = 1.4
        let depth = v.x + cameraOffset        // tail nearer, nose farther
        let scale = focal / (focal + depth)
        let ax = 0.50, ay = 0.90, az = 0.62, elev = 0.30
        let screenX = (v.x * ax - v.y * ay) * size * scale
        let screenY = size * 0.5 - (v.z * az + v.x * elev) * size * scale
        return (screenX, screenY, depth)
    }
}
