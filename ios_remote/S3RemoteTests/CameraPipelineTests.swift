/*
 * CameraPipelineTests — end-to-end over the real decode path: a rendered
 * image → JPEG → multipart stream (as camera_stream.c writes it) →
 * MjpegParser at random chunk sizes → UIImage back. Guards the whole
 * byte-stream→frame pipeline, not just the parser math.
 */

import XCTest
@testable import S3Remote

final class CameraPipelineTests: XCTestCase {
    /// A real (small) JPEG produced by the platform encoder.
    private func makeJPEG(width: Int = 64, height: Int = 48) -> Data {
        let renderer = UIGraphicsImageRenderer(size: CGSize(width: width, height: height))
        let image = renderer.image { ctx in
            UIColor(Theme.accent).setFill()
            ctx.fill(CGRect(x: 0, y: 0, width: width, height: height))
            UIColor.white.setFill()
            ctx.fill(CGRect(x: 8, y: 8, width: 16, height: 16))
        }
        return image.jpegData(compressionQuality: 0.6)!
    }

    private func multipart(_ frames: [Data]) -> Data {
        var stream = Data()
        for f in frames {
            stream += Data("\r\n--\(MjpegParser.boundary)\r\n".utf8)
            stream += Data("Content-Type: image/jpeg\r\nContent-Length: \(f.count)\r\n\r\n".utf8)
            stream += f
        }
        return stream
    }

    func testStreamToImageRoundTrip() {
        let jpegs = [makeJPEG(), makeJPEG(width: 32, height: 32), makeJPEG(width: 80, height: 60)]
        let stream = multipart(jpegs)
        var parser = MjpegParser()

        // random-ish chunk sizes (deterministic LCG for repeatability)
        var seed: UInt64 = 0x9E3779B97F4A7C15
        var decoded: [Data] = []
        var offset = 0
        while offset < stream.count {
            seed = seed &* 6364136223846793005 &+ 1442695040888963407
            let chunk = Int(seed >> 33 % 500) + 1
            let end = min(offset + chunk, stream.count)
            decoded += parser.feed(stream.subdata(in: offset..<end))
            offset = end
        }
        XCTAssertEqual(decoded, jpegs)
        XCTAssertEqual(parser.framesParsed, 3)
        for (frame, original) in zip(decoded, jpegs) {
            let image = UIImage(data: frame)
            XCTAssertNotNil(image)
            // JPEG is lossy but the container must be identical to the source
            XCTAssertEqual(frame, original)
            XCTAssertGreaterThan(image!.size.width, 0)
        }
    }

    /// A frame split so that its body bytes AND the next part's header arrive
    /// in the same chunk — the parser must cut exactly at Content-Length.
    func testBodyAndNextHeaderInOneChunk() {
        let jpeg = makeJPEG(width: 24, height: 24)
        let stream = multipart([jpeg, jpeg])
        var parser = MjpegParser()
        // feed everything at once: headers, body, next headers, body — all merged
        XCTAssertEqual(parser.feed(stream), [jpeg, jpeg])
    }

    func testInterleavedSmallFeedsNeverEmitPartialFrames() {
        let jpeg = makeJPEG(width: 20, height: 20)
        let stream = multipart([jpeg])
        var parser = MjpegParser()
        var decoded: [Data] = []
        // 2-byte nibbles; every chunk that ends before the body's last byte
        // must emit nothing (the final chunk may complete the frame, whether
        // the stream length is even or odd)
        for i in stride(from: 0, to: stream.count, by: 2) {
            decoded += parser.feed(stream.subdata(in: i..<min(i + 2, stream.count)))
            if i + 2 < stream.count {
                XCTAssertTrue(decoded.isEmpty,
                              "no complete frame until the body's last byte arrives")
            }
        }
        XCTAssertEqual(decoded, [jpeg])
    }
}
