import Foundation

/// Converts screen drag coordinates into vehicle axes; screen up is forward.
enum JoystickInput {
    static func axes(x: Double, y: Double, radius: Double, deadzone: Double) -> (v: Double, w: Double) {
        let magnitude = hypot(x, y)
        guard radius > 0, magnitude > 0 else { return (0, 0) }
        let zone = min(max(deadzone, 0), 0.99)
        let amount = max(0, (min(magnitude / radius, 1) - zone) / (1 - zone))
        return (-y / magnitude * amount, x / magnitude * amount)
    }
}
