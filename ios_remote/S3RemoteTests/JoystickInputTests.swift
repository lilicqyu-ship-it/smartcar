import XCTest
@testable import S3Remote

final class JoystickInputTests: XCTestCase {
    func testCardinalDirectionsMapToVehicleAxes() {
        for (x, y, v, w) in [(0.0, -90.0, 1.0, 0.0), (0, 90, -1, 0), (90, 0, 0, 1), (-90, 0, 0, -1)] {
            let result = JoystickInput.axes(x: x, y: y, radius: 90, deadzone: 0.08)
            XCTAssertEqual(result.v, v, accuracy: 0.0001)
            XCTAssertEqual(result.w, w, accuracy: 0.0001)
        }
    }
    func testCenterAndDeadzoneProduceZero() {
        for x in [0.0, 2, 7] {
            let result = JoystickInput.axes(x: x, y: 0, radius: 90, deadzone: 0.08)
            XCTAssertEqual(result.v, 0)
            XCTAssertEqual(result.w, 0)
        }
    }
    func testDiagonalClampsWithoutChangingDirection() {
        let result = JoystickInput.axes(x: 300, y: -300, radius: 90, deadzone: 0.08)
        XCTAssertEqual(result.v, result.w, accuracy: 0.0001)
        XCTAssertEqual(hypot(result.v, result.w), 1, accuracy: 0.0001)
    }
}
