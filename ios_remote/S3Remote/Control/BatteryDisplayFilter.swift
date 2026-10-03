/*
 * BatteryDisplayFilter.swift — port of the C6 control-page battery display
 * debounce (esp32c6_car assets_src/app.js, commit cee1189 "中值+平滑+迟滞锁存"):
 *
 *   voltage : 5-point median + 500 ms time-constant EMA + ≤2 Hz display gate
 *             + 20 mV downward hysteresis — within one session the shown value
 *             only holds or falls; WS reconnects do not reset it, a fresh app
 *             launch re-initializes from the first valid sample
 *   percent : 5-point median + 1.5 s sustained-drop confirmation (a single
 *             noisy 1 % dip never latches); likewise holds-or-falls; a >1 s
 *             sample gap clears the pending candidate but keeps the shown
 *             value; 0 % is a legal value when the voltage is valid
 *
 * Display layer ONLY: alarms (SafetyMonitor) and colors keep using the real
 * telemetry values, exactly like the C6 page.
 */

import Foundation

public struct BatteryDisplayFilter: Sendable {
    public static let windowSize = 5
    public static let displayIntervalMs: Double = 500 // ≤2 updates/s
    public static let tauMs: Double = 500             // EMA time constant
    public static let deadbandMv = 20                 // downward hysteresis
    public static let percentConfirmMs: Double = 1500 // sustained lower median
    public static let percentGapResetMs: Double = 1000

    // voltage plane
    private var vSamples: [Int] = []
    private var filteredMv: Double?
    private var shownMv: Int?
    private var sampleTs: Double?
    private var displayTs: Double = 0

    // percent plane
    private var pSamples: [Int] = []
    private var shownPct: Int?
    private var candidatePct: Int?
    private var candidateSince: Double = 0
    private var pSampleTs: Double?

    public init() {}

    public var voltageShownMv: Int? { shownMv }
    public var percentShown: Int? { shownPct }

    /// One telemetry frame. Returns the values to publish; `nil` on a plane
    /// means "keep showing the previous value" (the DOM keeps its text in C6).
    /// Samples with `mv <= 0` are not-ready and never initialize either plane;
    /// `pct` outside 0...100 is ignored (0 % stays valid with valid voltage).
    public mutating func apply(pct: Int, mv: Int, nowMs: Double) -> (mv: Int?, pct: Int?) {
        (voltage(mv, nowMs: nowMs), percent(pct, mv: mv, nowMs: nowMs))
    }

    private mutating func voltage(_ mv: Int, nowMs: Double) -> Int? {
        guard mv > 0 else { return nil } // absent measurement must not latch 0 V
        push(&vSamples, mv)
        let medianMv = median(vSamples)

        if let filtered = filteredMv {
            let dt = max(0, min(1000, nowMs - (sampleTs ?? nowMs)))
            filteredMv = filtered + (Double(medianMv) - filtered) * (1 - exp(-dt / Self.tauMs))
        } else {
            filteredMv = Double(medianMv)
        }
        sampleTs = nowMs

        // ≤2 Hz display gate; the very first display is immediate
        if shownMv != nil && nowMs - displayTs < Self.displayIntervalMs { return nil }
        displayTs = nowMs

        let roundedMv = Int(((filteredMv ?? 0) / 10).rounded()) * 10 // Math.round(x/10)*10
        if shownMv == nil || roundedMv <= shownMv! - Self.deadbandMv {
            shownMv = roundedMv
            return roundedMv
        }
        return nil
    }

    private mutating func percent(_ pct: Int, mv: Int, nowMs: Double) -> Int? {
        guard mv > 0, pct >= 0, pct <= 100 else { return nil }
        // stale stream (e.g. resumed after a gap): drop the pending candidate,
        // keep the shown value
        if let ts = pSampleTs, nowMs - ts > Self.percentGapResetMs {
            pSamples.removeAll()
            candidatePct = nil
        }
        pSampleTs = nowMs
        push(&pSamples, pct)
        let medianPct = median(pSamples)

        guard let shown = shownPct else {
            shownPct = medianPct
            return medianPct
        }
        if medianPct >= shown {
            candidatePct = nil
            return nil
        }
        if candidatePct == nil {
            candidatePct = medianPct
            candidateSince = nowMs
        } else {
            // highest lower estimate in the window: a short deep dip must not
            // set a falsely low permanent display while readings fluctuate
            candidatePct = max(candidatePct!, medianPct)
        }
        if nowMs - candidateSince >= Self.percentConfirmMs {
            shownPct = candidatePct
            candidatePct = nil
            return shownPct
        }
        return nil
    }

    private func push(_ array: inout [Int], _ value: Int) {
        array.append(value)
        if array.count > Self.windowSize {
            array.removeFirst(array.count - Self.windowSize)
        }
    }

    /// sorted[floor(n/2)] — same index choice as the C6 page (upper middle
    /// for even counts, middle for odd).
    private func median(_ array: [Int]) -> Int {
        precondition(!array.isEmpty)
        return array.sorted()[array.count / 2]
    }
}
