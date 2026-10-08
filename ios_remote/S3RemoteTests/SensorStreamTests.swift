/*
 * SensorStreamTests.swift — the diagnostic sensor side-channel that feeds the
 * 传感器 tab: {"t":"imu"} / {"t":"tofz"} JSON parsing against the exact
 * bridge_emit_imu / bridge_emit_tofz formats, the full-field fusion parse,
 * the TofZoneAssembler reassembly rules, and the AppState display buffers.
 */

import XCTest
@testable import S3Remote

final class SensorStreamTests: XCTestCase {
    private func parse(_ s: String) -> TextMessage? {
        TextMessage.parse(Data(s.utf8))
    }

    // ---- {"t":"imu"} -----------------------------------------------------------------

    func testImuHappyPath() {
        let json = #"{"t":"imu","seq":42,"ms":1234,"acc":[-512,250,995],"gyro":[1520,-330,80],"tp":2573}"#
        guard case .imu(let sample) = parse(json) else { return XCTFail("not imu") }
        XCTAssertEqual(sample.seq, 42)
        XCTAssertEqual(sample.stampMs, 1234)
        XCTAssertEqual(sample.accMg, [-512, 250, 995])
        XCTAssertEqual(sample.gyroMdps, [1520, -330, 80])
        XCTAssertEqual(sample.tempCentiC, 2573) // 25.73 °C
    }

    func testImuMalformed() {
        XCTAssertNil(parse(#"{"t":"imu","seq":42,"ms":1234,"acc":[-1,2],"gyro":[1,2,3],"tp":1}"#))
        XCTAssertNil(parse(#"{"t":"imu","seq":42,"ms":1234,"acc":[-1,2,3],"gyro":[1,2],"tp":1}"#))
        XCTAssertNil(parse(#"{"t":"imu","seq":42,"acc":[-1,2,3],"gyro":[1,2,3],"tp":1}"#))
        XCTAssertNil(parse(#"{"t":"imu","seq":-1,"ms":0,"acc":[0,0,0],"gyro":[0,0,0],"tp":0}"#))
        XCTAssertNil(parse(#"{"t":"imu","seq":1,"ms":0,"acc":[0,0,0],"gyro":[0,0,0]}"#))
    }

    func testRawImuPreviewMovesWithGravityAndGyro() {
        var tracker = RawImuAttitudeTracker()
        tracker.observe(ImuSample(seq: 1, stampMs: 100, accMg: [0, 0, 1000],
                                  gyroMdps: [0, 0, 0], tempCentiC: 2500))
        tracker.observe(ImuSample(seq: 2, stampMs: 150, accMg: [-500, 0, 866],
                                  gyroMdps: [0, 0, 90_000], tempCentiC: 2500))
        XCTAssertGreaterThan(tracker.attitude?.pitchDeg ?? 0, 8)
        XCTAssertEqual(tracker.attitude?.relativeYawDeg ?? 0, 4.5, accuracy: 0.001)
        tracker.observe(ImuSample(seq: 3, stampMs: 5000, accMg: [-500, 0, 866],
                                  gyroMdps: [0, 0, 90_000], tempCentiC: 2500))
        XCTAssertEqual(tracker.attitude?.relativeYawDeg ?? 0, 4.5, accuracy: 0.001)
        tracker.reset()
        XCTAssertNil(tracker.attitude)
    }

    func testImuAxisCalibrationDerivesRightHandedCarFrame() {
        let aligned = ImuAxisCalibration.derive(level: [0, 0, 1000],
                                                 noseUp: [-500, 0, 866])
        XCTAssertEqual(aligned?.axes, [1, 2, 3])
        let turned = ImuAxisCalibration.derive(level: [0, 0, 1000],
                                                noseUp: [0, 500, 866])
        XCTAssertEqual(turned?.axes, [-2, 1, 3])
        XCTAssertEqual(turned?.map([0, 500, 866]), [-500, 0, 866])
        XCTAssertNil(ImuAxisCalibration.derive(level: [0, 0, 1000],
                                                noseUp: [-50, 0, 999]))
        XCTAssertNil(ImuAxisCalibration(axes: [1, -2, 3]))
    }

    // ---- {"t":"tofz"} ----------------------------------------------------------------

    private func tofz(seq: Int = 7, f: Int = 0, m: Int = 1, v: Int = 58,
                      near: Int = 980, z: String = "61,61,61") -> String {
        #"{"t":"tofz","seq":\#(seq),"f":\#(f),"m":\#(m),"v":\#(v),"near":\#(near),"z":[\#(z)]}"#
    }

    func testTofzHappyPath() {
        let zones = (0..<25).map { String($0 * 8) }.joined(separator: ",")
        guard case .tofFragment(let seq, let frag, let mode, let valid, let near, let cells) =
            parse(tofz(z: zones)) else { return XCTFail("not tofz") }
        XCTAssertEqual(seq, 7)
        XCTAssertEqual(frag, 0)
        XCTAssertEqual(mode, 1)
        XCTAssertEqual(valid, 58)
        XCTAssertEqual(near, 980)
        XCTAssertEqual(cells.count, 25)
        XCTAssertEqual(cells[1], 8) // cell = mm/16 on the wire
    }

    func testTofz0xFFIsLegalCell() {
        let zones = (0..<24).map { _ in "255" }.joined(separator: ",")
        guard case .tofFragment(_, _, _, _, _, let cells) = parse(tofz(z: zones + ",255")) else {
            return XCTFail("not tofz")
        }
        XCTAssertTrue(cells.allSatisfy { $0 == 255 }) // 0xFF = untrusted, not an error
    }

    func testTofzMalformed() {
        XCTAssertNil(parse(tofz(f: 3))) // fragment index out of range
        XCTAssertNil(parse(tofz(m: 4))) // unknown scene mode
        XCTAssertNil(parse(tofz(v: 65)))
        XCTAssertNil(parse(tofz(z: "1,2,3,4"))) // wrong cell count
        XCTAssertNil(parse(tofz(z: "1,2,256"))) // cell out of u8
        XCTAssertNil(parse(tofz(seq: 65_536)))
        XCTAssertNil(parse(#"{"t":"tofz","seq":1,"f":0,"m":1,"v":1,"z":[1,2,3]}"#)) // missing near
    }

    // ---- fusion full-field parse -------------------------------------------------------

    func testFusionFullSnapshot() {
        // Exact string shape of bridge_emit_fusion (12 fields).
        let json = #"{"t":"fusion","reason":1,"flags":131,"distance":1200,"cap":600,"speed":300,"yawRate":-1250,"heading":9020,"roll":-30,"pitch":15,"age":40,"zones":58,"brake":0}"#
        guard case .fusion(let f) = parse(json) else { return XCTFail("not fusion") }
        XCTAssertEqual(f.reason, 1)
        XCTAssertEqual(f.flags, 131)
        XCTAssertEqual(f.distance, 1200)
        XCTAssertEqual(f.cap, 600)
        XCTAssertEqual(f.speedMmS, 300)
        XCTAssertEqual(f.yawRateCdegS, -1250)
        XCTAssertEqual(f.headingCdeg, 9020)
        XCTAssertEqual(f.rollCdeg, -30)
        XCTAssertEqual(f.pitchCdeg, 15)
        XCTAssertEqual(f.tofAgeMs, 40)
        XCTAssertEqual(f.validZones, 58)
        XCTAssertEqual(f.brake, 0)
    }

    func testFusionOutOfRangeAttitudeRejected() {
        // yawRate beyond i16 → the frame does not describe what it claims.
        let json = #"{"t":"fusion","reason":0,"flags":7,"distance":1,"cap":1,"speed":0,"yawRate":40000,"heading":0,"roll":0,"pitch":0,"age":0,"zones":1,"brake":0}"#
        XCTAssertNil(parse(json))
    }

    // ---- TofZoneAssembler ----------------------------------------------------------------

    private func fragment(_ seq: Int, _ frag: Int, cell: Int = 100) -> (seq: Int, frag: Int, mode: Int, valid: Int, nearestMm: Int, zones: [Int]) {
        (seq, frag, 1, 64, 1600, [Int](repeating: cell, count: 25))
    }

    func testAssemblerCompletesOnThirdFragment() {
        var asm = TofZoneAssembler()
        XCTAssertNil(asm.add(seq: 1, frag: 0, mode: 1, valid: 64, nearestMm: 1600,
                             zones: [Int](repeating: 100, count: 25)))
        XCTAssertNil(asm.add(seq: 1, frag: 1, mode: 1, valid: 64, nearestMm: 1600,
                             zones: [Int](repeating: 100, count: 25)))
        // Fragment 2 keeps its full 25-cell wire shape; cells past zone 63
        // are padding and must be discarded, not wrapped.
        let frame = asm.add(seq: 1, frag: 2, mode: 1, valid: 64, nearestMm: 1600,
                            zones: [Int](repeating: 100, count: 25))
        XCTAssertNotNil(frame)
        XCTAssertEqual(frame?.seq, 1)
        XCTAssertEqual(frame?.zones.count, 64)
        XCTAssertEqual(frame?.receivedFragments, 3)
        XCTAssertEqual(frame?.zones[0], 1600) // 100 cells × 16 mm
        XCTAssertEqual(frame?.zones[63], 1600)
    }

    func testAssemblerExposesOnlyReceivedCellsWhenAFragmentIsLost() {
        var asm = TofZoneAssembler()
        XCTAssertNil(asm.add(seq: 9, frag: 1, mode: 1, valid: 40, nearestMm: 720,
                             zones: [Int](repeating: 45, count: 25)))
        let partial = asm.partialFrame
        XCTAssertEqual(partial?.receivedFragments, 1)
        XCTAssertEqual(partial?.nearestMm, 720)
        XCTAssertNil(partial?.zones[0])
        XCTAssertEqual(partial?.zones[25], 720)
        XCTAssertNil(partial?.zones[50])
    }

    func testAssemblerAcceptsOutOfOrderFragments() {
        var asm = TofZoneAssembler()
        XCTAssertNil(asm.add(seq: 5, frag: 2, mode: 3, valid: 1, nearestMm: 400,
                             zones: [Int](repeating: 25, count: 25)))
        XCTAssertNil(asm.add(seq: 5, frag: 0, mode: 3, valid: 1, nearestMm: 400,
                             zones: [Int](repeating: 25, count: 25)))
        let frame = asm.add(seq: 5, frag: 1, mode: 3, valid: 1, nearestMm: 400,
                            zones: [Int](repeating: 25, count: 25))
        XCTAssertEqual(frame?.seq, 5)
        XCTAssertEqual(frame?.mode, 3)
    }

    func testAssemblerIgnoresDuplicateFragment() {
        var asm = TofZoneAssembler()
        XCTAssertNil(asm.add(seq: 1, frag: 0, mode: 1, valid: 1, nearestMm: 1,
                             zones: [Int](repeating: 1, count: 25)))
        XCTAssertNil(asm.add(seq: 1, frag: 0, mode: 1, valid: 1, nearestMm: 1,
                             zones: [Int](repeating: 2, count: 25))) // duplicate
        XCTAssertNil(asm.add(seq: 1, frag: 1, mode: 1, valid: 1, nearestMm: 1,
                             zones: [Int](repeating: 1, count: 25)))
        // The duplicate must not have overwritten cells 0-24 with 2×16=32.
        XCTAssertEqual(asm.add(seq: 1, frag: 2, mode: 1, valid: 1, nearestMm: 1,
                               zones: [Int](repeating: 1, count: 25))?.zones[0], 16)
    }

    func testAssemblerResetsOnSeqChange() {
        var asm = TofZoneAssembler()
        XCTAssertNil(asm.add(seq: 1, frag: 0, mode: 1, valid: 1, nearestMm: 1,
                             zones: [Int](repeating: 1, count: 25)))
        // A new seq arrives before the old frame completed: the stale
        // fragment is dropped, not glued onto the new frame.
        XCTAssertNil(asm.add(seq: 2, frag: 1, mode: 1, valid: 1, nearestMm: 1,
                             zones: [Int](repeating: 1, count: 25)))
        XCTAssertNil(asm.add(seq: 2, frag: 0, mode: 1, valid: 1, nearestMm: 1,
                             zones: [Int](repeating: 1, count: 25)))
        XCTAssertEqual(asm.add(seq: 2, frag: 2, mode: 1, valid: 1, nearestMm: 1,
                               zones: [Int](repeating: 1, count: 25))?.seq, 2)
    }

    func testAssemblerDequantisesAndMarksUntrusted() {
        var asm = TofZoneAssembler()
        var cells = [Int](repeating: 255, count: 25)
        cells[0] = 0
        cells[1] = 254 // max representable: 254×16 = 4064 mm (255 = invalid)
        XCTAssertNil(asm.add(seq: 1, frag: 0, mode: 1, valid: 2, nearestMm: 0,
                             zones: cells))
        XCTAssertNil(asm.add(seq: 1, frag: 1, mode: 1, valid: 2, nearestMm: 0,
                             zones: [Int](repeating: 255, count: 25)))
        let frame = asm.add(seq: 1, frag: 2, mode: 1, valid: 2, nearestMm: 0,
                            zones: [Int](repeating: 255, count: 25))
        XCTAssertEqual(frame?.zones[0], 0)
        XCTAssertEqual(frame?.zones[1], 4064)
        XCTAssertNil(frame?.zones[2]) // 0xFF → untrusted, never a distance
        XCTAssertNil(frame?.zones[63])
    }

    func testAssemblerRejectsBadFragment() {
        var asm = TofZoneAssembler()
        XCTAssertNil(asm.add(seq: 1, frag: 2, mode: 1, valid: 1, nearestMm: 1,
                             zones: [Int](repeating: 1, count: 24)))
        XCTAssertNil(asm.add(seq: 1, frag: 7, mode: 1, valid: 1, nearestMm: 1,
                             zones: [Int](repeating: 1, count: 25)))
    }

    // ---- AppState display buffers --------------------------------------------------------

    @MainActor
    func testAppStateImuBufferCapsAndPauses() {
        let app = AppState(settings: AppSettings(host: "127.0.0.1"))
        XCTAssertFalse(app.imuFresh)
        for i in 0..<250 {
            app.applyImu(ImuSample(seq: i, stampMs: i * 50, accMg: [0, 0, 1000],
                                   gyroMdps: [0, 0, 0], tempCentiC: 2500))
        }
        XCTAssertEqual(app.imuHistory.count, 200)
        XCTAssertEqual(app.imuHistory.last?.seq, 249)
        XCTAssertEqual(app.imuRateHz ?? 0, 20, accuracy: 0.01) // 50 ms stamps

        app.sensorPaused = true
        app.applyImu(ImuSample(seq: 999, stampMs: 99_999, accMg: [0, 0, 0],
                               gyroMdps: [0, 0, 0], tempCentiC: 0))
        XCTAssertEqual(app.imuHistory.last?.seq, 249) // frozen for reading
    }

    @MainActor
    func testAppStateTofBuffersAndClear() {
        let app = AppState(settings: AppSettings(host: "127.0.0.1"))
        XCTAssertFalse(app.tofMapFresh)
        app.handleOpen() // handleDown only acts from a live state
        var cells = [Int](repeating: 100, count: 25)
        cells[0] = 50 // 800 mm
        app.applyTofFragment(seq: 1, frag: 0, mode: 1, valid: 64, nearestMm: 800, zones: cells)
        XCTAssertNil(app.tofMap) // not complete yet
        XCTAssertEqual(app.tofPartialMap?.receivedFragments, 1)
        app.applyTofFragment(seq: 1, frag: 1, mode: 1, valid: 64, nearestMm: 800,
                             zones: [Int](repeating: 100, count: 25))
        app.applyTofFragment(seq: 1, frag: 2, mode: 1, valid: 64, nearestMm: 800,
                             zones: [Int](repeating: 100, count: 25))
        XCTAssertEqual(app.tofMap?.nearestMm, 800)
        XCTAssertNil(app.tofPartialMap)
        XCTAssertEqual(app.tofMap?.zones[0], 800)
        XCTAssertEqual(app.tofNearHistory, [800])

        // An all-invalid frame updates the map but not the trend (it is
        // "no reading", not "wall at 0 mm").
        app.applyTofFragment(seq: 2, frag: 0, mode: 0, valid: 0, nearestMm: 0,
                             zones: [Int](repeating: 255, count: 25))
        app.applyTofFragment(seq: 2, frag: 1, mode: 0, valid: 0, nearestMm: 0,
                             zones: [Int](repeating: 255, count: 25))
        app.applyTofFragment(seq: 2, frag: 2, mode: 0, valid: 0, nearestMm: 0,
                             zones: [Int](repeating: 255, count: 25))
        XCTAssertEqual(app.tofMap?.validZones, 0)
        XCTAssertEqual(app.tofNearHistory, [800])

        app.handleDown("test")
        XCTAssertNil(app.tofMap)
        XCTAssertNil(app.tofPartialMap)
        XCTAssertTrue(app.imuHistory.isEmpty)
        XCTAssertTrue(app.tofNearHistory.isEmpty)
    }

    // ---- IMU derived quantities --------------------------------------------------------

    func testHorizontalGRemovesGravityAtRest() {
        // Level ground, still: acc = (0, 0, 1000) mg, attitude level.
        let level = ImuKinematics.horizontalG(accMgX: 0, y: 0, z: 1000,
                                              rollDeg: 0, pitchDeg: 0)
        XCTAssertEqual(level.fwd, 0, accuracy: 1e-9)
        XCTAssertEqual(level.lat, 0, accuracy: 1e-9)

        // Nose up 30°, still: the sensor reports ax_rest = −g·sin(pitch),
        // ay_rest = 0 (fusion.c pitch = atan2(−ax, |ay,az|)). The ball must
        // stay centred — the tilt's gravity component is compensated away.
        let pitch = 30.0
        let axRest = -1000 * sin(pitch * .pi / 180)
        let noseUp = ImuKinematics.horizontalG(accMgX: axRest, y: 0, z: 1000,
                                               rollDeg: 0, pitchDeg: pitch)
        XCTAssertEqual(noseUp.fwd, 0, accuracy: 1e-9)
        XCTAssertEqual(noseUp.lat, 0, accuracy: 1e-9)

        // Rolled 20°, still: ay_rest = +g·sin(roll).
        let roll = 20.0
        let ayRest = 1000 * sin(roll * .pi / 180)
        let rolled = ImuKinematics.horizontalG(accMgX: 0, y: ayRest, z: 1000,
                                               rollDeg: roll, pitchDeg: 0)
        XCTAssertEqual(rolled.fwd, 0, accuracy: 1e-9)
        XCTAssertEqual(rolled.lat, 0, accuracy: 1e-9)
    }

    func testHorizontalGBrakingAndCornering() {
        // Level, braking at 0.3 g: accelerometer reports fwd = −300 mg
        // (specific force points backwards while decelerating).
        let braking = ImuKinematics.horizontalG(accMgX: -300, y: 0, z: 1000,
                                                rollDeg: 0, pitchDeg: 0)
        XCTAssertEqual(braking.fwd, -0.3, accuracy: 1e-9)
        XCTAssertEqual(braking.magnitude, 0.3, accuracy: 1e-9)

        // Cornering left at 0.4 g lateral.
        let corner = ImuKinematics.horizontalG(accMgX: 0, y: 400, z: 1000,
                                               rollDeg: 0, pitchDeg: 0)
        XCTAssertEqual(corner.lat, 0.4, accuracy: 1e-9)
        XCTAssertEqual(corner.magnitude, 0.4, accuracy: 1e-9)
    }

    func testAutoRangeMinimumSpanAndHeadroom() {
        // Quiet data (±60 mg) widens to the 500 mg minimum span, symmetric.
        let quiet = ImuKinematics.autoRange(values: [[-60, 20, 55]], minimumSpan: 500)
        XCTAssertEqual(quiet.lowerBound, -250, accuracy: 1e-9)
        XCTAssertEqual(quiet.upperBound, 250, accuracy: 1e-9)

        // Loud data (±2000 mg) gets 15 % headroom: 2000 × 1.15 = 2300.
        let loud = ImuKinematics.autoRange(values: [[-2000, 1000], [1500, -500]],
                                           minimumSpan: 500)
        XCTAssertEqual(loud.upperBound, 2300, accuracy: 1e-9)
        XCTAssertEqual(loud.lowerBound, -2300, accuracy: 1e-9)

        // No data: the minimum span, centred on zero.
        let empty = ImuKinematics.autoRange(values: [[]], minimumSpan: 20_000)
        XCTAssertEqual(empty.lowerBound, -10_000, accuracy: 1e-9)
    }

    func testPeakTrackerMonotone() {
        var tracker = PeakGTracker()
        XCTAssertEqual(tracker.peak, 0)
        tracker.observe(0.3)
        tracker.observe(0.2) // decays do not lower the peak
        XCTAssertEqual(tracker.peak, 0.3, accuracy: 1e-9)
        tracker.observe(0.55)
        XCTAssertEqual(tracker.peak, 0.55, accuracy: 1e-9)
        tracker.reset()
        XCTAssertEqual(tracker.peak, 0)
    }

    // ---- 3D projection -----------------------------------------------------------------

    func testCar3DRotationIdentityAndYaw() {
        let v: Car3DProjection.Vertex = (1, 0, 0)
        let identity = Car3DProjection.rotate(v, yawDeg: 0, pitchDeg: 0, rollDeg: 0)
        XCTAssertEqual(identity.x, 1, accuracy: 1e-12)
        XCTAssertEqual(identity.y, 0, accuracy: 1e-12)
        XCTAssertEqual(identity.z, 0, accuracy: 1e-12)

        // Yaw +90° (CCW seen from above): x-forward becomes y-left.
        let yawed = Car3DProjection.rotate(v, yawDeg: 90, pitchDeg: 0, rollDeg: 0)
        XCTAssertEqual(yawed.x, 0, accuracy: 1e-12)
        XCTAssertEqual(yawed.y, 1, accuracy: 1e-12)

        // Pitch +30°: the nose tip rises above the horizon plane.
        let pitched = Car3DProjection.rotate(v, yawDeg: 0, pitchDeg: 30, rollDeg: 0)
        XCTAssertGreaterThan(pitched.z, 0)
        XCTAssertEqual(pitched.z, sin(30 * .pi / 180), accuracy: 1e-12)

        // Roll +45° about x leaves the x axis itself invariant.
        let rolled = Car3DProjection.rotate(v, yawDeg: 0, pitchDeg: 0, rollDeg: 45)
        XCTAssertEqual(rolled.x, 1, accuracy: 1e-12)
    }

    func testCar3DProjectionAxesOnScreen() {
        // z up → screen y smaller (screen y grows downward).
        let raised = Car3DProjection.project((x: 0, y: 0, z: 0.5), size: 100)
        let lowered = Car3DProjection.project((x: 0, y: 0, z: -0.5), size: 100)
        XCTAssertLessThan(raised.y, lowered.y)
        // Forward (nose) lands up-right of centre, and higher than the tail
        // (the elevation tilt) — that's what makes pitch readable.
        let nose = Car3DProjection.project((x: 0.8, y: 0, z: 0), size: 100)
        let tail = Car3DProjection.project((x: -0.8, y: 0, z: 0), size: 100)
        XCTAssertGreaterThan(nose.x, 0)
        XCTAssertLessThan(nose.y, tail.y)
        // Left (body +y) lands screen-left of centre.
        let left = Car3DProjection.project((x: 0, y: 0.5, z: 0), size: 100)
        XCTAssertLessThan(left.x, 0)
        // Depth grows toward the nose (chase view: tail nearer).
        XCTAssertGreaterThan(nose.depth, tail.depth)
    }

    @MainActor
    func testAppStateAccelTrailAndPeak() {
        let app = AppState(settings: AppSettings(host: "127.0.0.1"))
        app.handleOpen() // handleDown only acts from a live state
        // Fusion level + braking samples → trail grows, peak tracks the max.
        // flags must carry IMU_OK|CALIBRATED (0x0a): without axis calibration
        // applyImu routes the g-ball through the raw-IMU attitude preview
        // instead of the car-frame fusion angles.
        app.applyFusion(FusionStatus(reason: 0, flags: 15, distance: 1000, cap: 600,
                                     speedMmS: 0, yawRateCdegS: 0, headingCdeg: 0,
                                     rollCdeg: 0, pitchCdeg: 0, tofAgeMs: 10,
                                     validZones: 60, brake: 0))
        app.applyImu(ImuSample(seq: 1, stampMs: 0, accMg: [-300, 0, 1000],
                               gyroMdps: [0, 0, 0], tempCentiC: 2500))
        app.applyImu(ImuSample(seq: 2, stampMs: 50, accMg: [-150, 100, 1000],
                               gyroMdps: [0, 0, 0], tempCentiC: 2500))
        XCTAssertEqual(app.accelTrail.count, 2)
        XCTAssertEqual(app.accelTrail[0].fwd, -0.3, accuracy: 1e-9)
        XCTAssertEqual(app.accelTrail[1].lat, 0.1, accuracy: 1e-9)
        XCTAssertEqual(app.peakHorizontalG, 0.3, accuracy: 1e-9)

        // Drop CALIBRATED → the raw-IMU preview takes over: braking reads
        // partly as tilt, never as the car-frame value.
        app.applyFusion(FusionStatus(reason: 0, flags: 7, distance: 1000, cap: 600,
                                     speedMmS: 0, yawRateCdegS: 0, headingCdeg: 0,
                                     rollCdeg: 0, pitchCdeg: 0, tofAgeMs: 10,
                                     validZones: 60, brake: 0))
        app.applyImu(ImuSample(seq: 3, stampMs: 100, accMg: [-300, 0, 1000],
                               gyroMdps: [0, 0, 0], tempCentiC: 2500))
        XCTAssertEqual(app.accelTrail.count, 3)
        XCTAssertNotNil(app.rawImuAttitude)
        XCTAssertNotEqual(app.accelTrail[2].fwd, -0.3, accuracy: 1e-6)

        // Pause freezes the trail.
        app.sensorPaused = true
        app.applyImu(ImuSample(seq: 4, stampMs: 150, accMg: [-990, 0, 1000],
                               gyroMdps: [0, 0, 0], tempCentiC: 2500))
        XCTAssertEqual(app.accelTrail.count, 3)
        XCTAssertEqual(app.peakHorizontalG, 0.3, accuracy: 1e-9)

        // Link down clears both.
        app.sensorPaused = false
        app.handleDown("test")
        XCTAssertTrue(app.accelTrail.isEmpty)
        XCTAssertEqual(app.peakHorizontalG, 0)
    }
}
