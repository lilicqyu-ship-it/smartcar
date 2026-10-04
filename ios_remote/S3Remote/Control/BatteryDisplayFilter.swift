/*
 * BatteryDisplayFilter.swift — port of the C6 control-page battery display
 * debounce (esp32c6_car assets_src/app.js, commit cee1189 "中值+平滑+迟滞锁存")
 * extended with charge recovery:
 *
 *   voltage : 5-point median + 500 ms time-constant EMA + ≤2 Hz display gate
 *             + 20 mV downward / 50 mV upward hysteresis — the shown value
 *             follows sustained falls AND sustained rises (charging), WS
 *             reconnects do not reset it, a fresh app launch re-initializes
 *             from the first valid sample
 *   percent : 5-point median + 1.5 s sustained-drop confirmation (a single
 *             noisy 1 % dip never latches) + 10 s sustained-rise confirmation
 *             at ≥+2 % (charging walks the display back up to 100 %); a >1 s
 *             sample gap clears pending candidates but keeps the shown value;
 *             0 % is a legal value when the voltage is valid
 *   reboot  : a meaningful uptime regression means the vehicle restarted
 *             (the typical power-off-charge-power-on cycle) — both planes
 *             reset and re-seed from the current frame, so the display snaps
 *             to the truth instead of staying latched below it
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
    public static let riseDeadbandMv = 50             // upward hysteresis (charging)
    public static let percentConfirmMs: Double = 1500 // sustained lower median
    public static let percentRiseHysteresis = 2       // % above shown to arm a rise
    public static let percentRiseConfirmMs: Double = 10_000 // charging is minutes-slow
    public static let percentGapResetMs: Double = 1000
    /// uptime regression beyond this = vehicle restart (not jitter).
    public static let rebootToleranceMs: UInt32 = 2_000

    // voltage plane
    private var vSamples: [Int] = []
    private var filteredMv: Double?
    private var shownMv: Int?
    private var sampleTs: Double?
    private var displayTs: Double = 0

    // percent plane
    private var pSamples: [Int] = []
    private var shownPct: Int?
    private var dropPct: Int?
    private var dropSince: Double = 0
    private var risePct: Int?
    private var riseSince: Double = 0
    private var pSampleTs: Double?

    private var lastUptimeMs: UInt32?

    public init() {}

    public var voltageShownMv: Int? { shownMv }
    public var percentShown: Int? { shownPct }

    /// One telemetry frame. Returns the values to publish; `nil` on a plane
    /// means "keep showing the previous value" (the DOM keeps its text in C6).
    /// Samples with `mv <= 0` are not-ready and never initialize either plane;
    /// `pct` outside 0...100 is ignored (0 % stays valid with valid voltage).
    /// A `uptimeMs` regression beyond the tolerance resets both planes.
    public mutating func apply(pct: Int, mv: Int, uptimeMs: UInt32, nowMs: Double) -> (mv: Int?, pct: Int?) {
        if let last = lastUptimeMs, uptimeMs > 0,
           uptimeMs + Self.rebootToleranceMs < last {
            reset()
        }
        lastUptimeMs = uptimeMs
        return (voltage(mv, nowMs: nowMs), percent(pct, mv: mv, nowMs: nowMs))
    }

    /// Fresh vehicle session: forget every latch, the next valid sample
    /// re-seeds the display (first-display paths are immediate).
    public mutating func reset() {
        vSamples.removeAll()
        filteredMv = nil
        shownMv = nil
        sampleTs = nil
        displayTs = 0
        pSamples.removeAll()
        shownPct = nil
        dropPct = nil
        risePct = nil
        pSampleTs = nil
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
        if shownMv == nil
            || roundedMv <= shownMv! - Self.deadbandMv      // discharge
            || roundedMv >= shownMv! + Self.riseDeadbandMv { // charging recovery
            shownMv = roundedMv
            return roundedMv
        }
        return nil
    }

    private mutating func percent(_ pct: Int, mv: Int, nowMs: Double) -> Int? {
        guard mv > 0, pct >= 0, pct <= 100 else { return nil }
        // stale stream (e.g. resumed after a gap): drop pending candidates,
        // keep the shown value
        if let ts = pSampleTs, nowMs - ts > Self.percentGapResetMs {
            pSamples.removeAll()
            dropPct = nil
            risePct = nil
        }
        pSampleTs = nowMs
        push(&pSamples, pct)
        let medianPct = median(pSamples)

        guard let shown = shownPct else {
            shownPct = medianPct
            return medianPct
        }
        if medianPct < shown {
            return confirmDrop(medianPct, nowMs: nowMs)
        }
        if medianPct >= shown + Self.percentRiseHysteresis {
            return confirmRise(medianPct, nowMs: nowMs)
        }
        // inside the hysteresis band: nothing pending survives
        dropPct = nil
        risePct = nil
        return nil
    }

    /// Sustained lower median latches down; the candidate takes the HIGHEST
    /// lower median so a short deep dip must not overshoot while fluctuating.
    private mutating func confirmDrop(_ medianPct: Int, nowMs: Double) -> Int? {
        risePct = nil
        if let candidate = dropPct {
            dropPct = max(candidate, medianPct)
        } else {
            dropPct = medianPct
            dropSince = nowMs
        }
        if nowMs - dropSince >= Self.percentConfirmMs {
            shownPct = dropPct
            dropPct = nil
            return shownPct
        }
        return nil
    }

    /// Sustained higher median (charging / resting recovery) latches up; the
    /// candidate takes the LOWEST rising median so a blip must not overshoot.
    private mutating func confirmRise(_ medianPct: Int, nowMs: Double) -> Int? {
        dropPct = nil
        if let candidate = risePct {
            risePct = min(candidate, medianPct)
        } else {
            risePct = medianPct
            riseSince = nowMs
        }
        if nowMs - riseSince >= Self.percentRiseConfirmMs {
            shownPct = risePct
            risePct = nil
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
