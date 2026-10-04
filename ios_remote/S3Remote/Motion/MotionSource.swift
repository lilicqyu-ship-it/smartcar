/*
 * MotionSource.swift — CoreMotion wrapper for tilt steering. CMMotionManager
 * needs no Info.plist usage string; gravity samples arrive in device-frame
 * g units and are hopped onto the MainActor for AppState.
 */

import CoreMotion
import Foundation

@MainActor
final class MotionSource {
    private let manager = CMMotionManager()
    private var onUpdate: ((Double, Double, Double) -> Void)?
    private(set) var active = false

    init() {
        manager.deviceMotionUpdateInterval = 1.0 / 30.0
    }

    func start(onUpdate: @escaping (Double, Double, Double) -> Void) {
        guard manager.isDeviceMotionAvailable else { return }
        self.onUpdate = onUpdate
        active = true
        manager.startDeviceMotionUpdates(to: .main) { [weak self] motion, _ in
            guard let motion else { return }
            let g = motion.gravity
            Task { @MainActor [weak self] in
                guard let self, self.active else { return }
                self.onUpdate?(g.x, g.y, g.z)
            }
        }
    }

    func stop() {
        manager.stopDeviceMotionUpdates()
        active = false
        onUpdate = nil
    }
}
