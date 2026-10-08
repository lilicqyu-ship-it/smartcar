/*
 * TofZoneAssembler.swift — reassembles the {"t":"tofz"} JSON fragments into
 * the car's 64-zone ToF distance map. Wire truth source: tc275_car
 * app/sensor_stream.h — a frame splits into 3 fragments of 25/25/14 zones,
 * cells are distanceMm/16 with 0xFF = no trusted target, and every fragment
 * header repeats {seq, mode, valid, nearest} so arrival order does not
 * matter. Pure value logic: the reassembly rules are unit-testable without
 * a link, and AppState just stores whatever a completed call returns.
 */

/// One 64-zone snapshot; unknown cells stay nil until their fragment arrives.
public struct TofZoneFrame: Equatable, Sendable {
    public static let zoneCount = 64
    public static let cellsPerFragment = 25
    public static let fragmentCount = 3

    public let seq: Int
    /// FUSION_MODE_* scene classification sampled at the source (0 blind,
    /// 1 tracked, 2 degraded, 3 open).
    public let mode: Int
    /// Whole-frame trusted-zone count (status 5/9), as computed on the car.
    public let validZones: Int
    /// Nearest trusted distance of this frame, mm (0 when none).
    public let nearestMm: Int
    /// 1/2 means a partial diagnostic view; 3 means all 64 zones arrived.
    public let receivedFragments: Int
    /// Row-major 8×8, mm; nil = untrusted zone (driver status not 5/9).
    public let zones: [Int?]
}

public struct TofZoneAssembler: Sendable {
    private var seq = -1
    private var mode = 0
    private var validZones = 0
    private var nearestMm = 0
    private var seen = [Bool](repeating: false, count: TofZoneFrame.fragmentCount)
    private var cells = [Int?](repeating: nil, count: TofZoneFrame.zoneCount)

    public init() {}

    /// Available cells from the current frame. Missing fragments remain nil,
    /// so a lossy diagnostic stream can still show honest partial data.
    public var partialFrame: TofZoneFrame? {
        let received = seen.filter { $0 }.count
        guard seq >= 0, received > 0 else { return nil }
        return TofZoneFrame(seq: seq, mode: mode, validZones: validZones,
                            nearestMm: nearestMm, receivedFragments: received,
                            zones: cells)
    }

    /// Feed one fragment; returns the completed frame when this fragment
    /// finishes its set. Malformed pieces, duplicates and fragments of a
    /// superseded frame are swallowed (diagnostic stream: drop, never fake).
    public mutating func add(seq newSeq: Int, frag: Int, mode newMode: Int,
                             valid: Int, nearestMm newNearest: Int,
                             zones: [Int]) -> TofZoneFrame? {
        guard zones.count == TofZoneFrame.cellsPerFragment,
              (0...(TofZoneFrame.fragmentCount - 1)).contains(frag) else { return nil }
        if newSeq != seq {
            reset()
            seq = newSeq
        }
        guard !seen[frag] else { return nil }
        seen[frag] = true
        mode = newMode
        validZones = valid
        nearestMm = newNearest
        for (i, cell) in zones.enumerated() {
            let index = frag * TofZoneFrame.cellsPerFragment + i
            guard index < TofZoneFrame.zoneCount else { break } // fragment 2 padding
            // mm/16 on the wire; 0xFF marks an untrusted zone, 0xFF*16=4080mm
            // would otherwise read as a very close wall.
            cells[index] = (cell == 0xFF) ? nil : cell * 16
        }
        guard seen.allSatisfy({ $0 }) else { return nil }
        let frame = TofZoneFrame(seq: seq, mode: mode, validZones: validZones,
                                 nearestMm: nearestMm,
                                 receivedFragments: TofZoneFrame.fragmentCount,
                                 zones: cells)
        reset() // a repeated seq (u16 wrap) must not glue two frames together
        return frame
    }

    public mutating func reset() {
        seq = -1
        mode = 0
        validZones = 0
        nearestMm = 0
        seen = [Bool](repeating: false, count: TofZoneFrame.fragmentCount)
        cells = [Int?](repeating: nil, count: TofZoneFrame.zoneCount)
    }
}
