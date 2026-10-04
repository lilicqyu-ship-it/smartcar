/*
 * OnboardingState.swift — first-launch guidance flag. UserDefaults-backed
 * with an injectable store so tests stay hermetic; "重看引导" in Settings
 * resets it.
 */

import Foundation

public enum OnboardingState {
    static let key = "s3remote.onboarding.v1"

    public static func shouldShow(_ defaults: UserDefaults = .standard) -> Bool {
        !defaults.bool(forKey: key)
    }

    public static func markSeen(_ defaults: UserDefaults = .standard) {
        defaults.set(true, forKey: key)
    }

    public static func reset(_ defaults: UserDefaults = .standard) {
        defaults.removeObject(forKey: key)
    }
}
