/*
 * IMU 显示标定：以车身水平和抬起车头两次重力向量推断传感器轴到车体系。
 * 仅用于 iOS 诊断显示，不写 TC275 的融合配置或驾驶保护。
 */
import Foundation

public struct ImuAxisCalibration: Codable, Equatable, Sendable {
    /// 车体 X 前 / Y 左 / Z 上，对应传感器的有符号轴编号 ±1...±3。
    public let axes: [Int]

    public init?(axes: [Int]) {
        guard axes.count == 3,
              Set(axes.map(abs)) == Set([1, 2, 3]),
              Self.determinant(axes) == 1 else { return nil }
        self.axes = axes
    }

    public static func derive(level: [Double], noseUp: [Double]) -> ImuAxisCalibration? {
        guard level.count == 3, noseUp.count == 3 else { return nil }
        let norm = level.map { $0 * $0 }.reduce(0, +).squareRoot()
        guard (850...1150).contains(norm) else { return nil }
        let zIndex = (0..<3).max { abs(level[$0]) < abs(level[$1]) }!
        guard abs(level[zIndex]) > 800,
              (0..<3).allSatisfy({ $0 == zIndex || abs(level[$0]) < 250 }) else { return nil }
        let horizontal = (0..<3).filter { $0 != zIndex }
        let xIndex = horizontal.max { abs(noseUp[$0] - level[$0]) < abs(noseUp[$1] - level[$1]) }!
        let delta = noseUp[xIndex] - level[xIndex]
        guard abs(delta) > 250,
              abs(noseUp[zIndex]) > 500 else { return nil }
        let yIndex = horizontal.first { $0 != xIndex }!
        let x = (delta < 0 ? 1 : -1) * (xIndex + 1)
        let z = (level[zIndex] > 0 ? 1 : -1) * (zIndex + 1)
        let unsigned = [xIndex + 1, yIndex + 1, zIndex + 1]
        let parity = Self.determinant(unsigned)
        let ySign = parity * (x > 0 ? 1 : -1) * (z > 0 ? 1 : -1)
        return ImuAxisCalibration(axes: [x, ySign * (yIndex + 1), z])
    }

    public func map(_ values: [Int]) -> [Int] {
        guard values.count == 3 else { return values }
        return axes.map { axis in (axis > 0 ? 1 : -1) * values[abs(axis) - 1] }
    }

    public func map(_ sample: ImuSample) -> ImuSample {
        ImuSample(seq: sample.seq, stampMs: sample.stampMs,
                  accMg: map(sample.accMg), gyroMdps: map(sample.gyroMdps),
                  tempCentiC: sample.tempCentiC)
    }

    private static func determinant(_ a: [Int]) -> Int {
        let indices = a.map { abs($0) }
        var inversions = 0
        for i in 0..<3 {
            for j in (i + 1)..<3 where indices[i] > indices[j] { inversions += 1 }
        }
        let sign = a.reduce(1) { $0 * ($1 > 0 ? 1 : -1) }
        return (inversions % 2 == 0 ? 1 : -1) * sign
    }
}
