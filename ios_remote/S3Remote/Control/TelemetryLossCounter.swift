/*
 * TelemetryLossCounter.swift — port of scr_link.c telemetry loss estimate
 * (spec 28): E2E seq gaps; Δ ∈ (1,1000) counts Δ-1 lost frames (mod-2^32
 * wrap is ignored); loss‰ = lost / (lost + received).
 */

import Foundation

public struct TelemetryLossCounter: Equatable, Sendable {
    public private(set) var lost: UInt64 = 0
    public private(set) var received: UInt64 = 0

    private var prevSeq: UInt32 = 0
    private var prevValid = false

    public init() {}

    public mutating func onTelemetry(seq: UInt32) {
        if prevValid {
            let d = seq &- prevSeq
            if d > 1 && d < 1000 {
                lost &+= UInt64(d - 1)
            }
        }
        prevSeq = seq
        prevValid = true
        received += 1
    }

    public var lossPerMille: Double {
        let total = lost + received
        guard total > 0 else { return 0 }
        return Double(lost) / Double(total) * 1000
    }
}
