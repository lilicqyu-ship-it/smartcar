/*
 * MjpegParserTests — incremental multipart/x-mixed-replace parsing against
 * the s3-gateway wire format (camera_stream.c): leading CRLF before every
 * boundary, Content-Length per part, NO trailing CRLF after the body.
 * Feeds are chunked at awkward sizes to exercise every split boundary.
 */

import XCTest
@testable import S3Remote

final class MjpegParserTests: XCTestCase {
    private let jpegA = Data([0xFF, 0xD8, 0x01, 0x02, 0x03, 0xFF, 0xD9])
    private let jpegB = Data([0xFF, 0xD8, 0x10, 0x20, 0x30, 0x40, 0x50, 0xFF, 0xD9])

    /// Exactly one part as the gateway writes it (body has no trailing CRLF).
    private func part(_ jpeg: Data, extraHeader: String = "") -> Data {
        var d = Data("\r\n--\(MjpegParser.boundary)\r\n".utf8)
        d += Data("Content-Type: image/jpeg\r\n".utf8)
        if !extraHeader.isEmpty { d += Data(extraHeader.utf8) }
        d += Data("Content-Length: \(jpeg.count)\r\n\r\n".utf8)
        d += jpeg
        return d
    }

    private func feed(_ parser: inout MjpegParser, _ data: Data, chunk: Int) -> [Data] {
        var frames: [Data] = []
        var offset = 0
        while offset < data.count {
            let end = min(offset + chunk, data.count)
            frames += parser.feed(data.subdata(in: offset..<end))
            offset = end
        }
        return frames
    }

    func testSingleFrameByteByByte() {
        var p = MjpegParser()
        let frames = feed(&p, part(jpegA), chunk: 1)
        XCTAssertEqual(frames, [jpegA])
        XCTAssertEqual(p.framesParsed, 1)
    }

    func testLeadingPreambleIsDropped() {
        var p = MjpegParser()
        let stream = Data("HTTP warmup noise \r\n".utf8) + part(jpegA)
        XCTAssertEqual(feed(&p, stream, chunk: 5), [jpegA])
    }

    func testConsecutiveFramesAtOddChunkSizes() {
        var p = MjpegParser()
        let stream = part(jpegA) + part(jpegB)
        for chunk in [3, 7, 11, 13, 64, 4096] {
            var q = MjpegParser()
            XCTAssertEqual(feed(&q, stream, chunk: chunk), [jpegA, jpegB], "chunk \(chunk)")
        }
        _ = p // (kept single parser var for clarity above)
    }

    func testExtraHeadersAreTolerated() {
        var p = MjpegParser()
        let stream = part(jpegA, extraHeader: "X-Timestamp: 123\r\nCache-Control: no-store\r\n")
        XCTAssertEqual(feed(&p, stream, chunk: 9), [jpegA])
    }

    func testPartWithoutContentLengthIsSkipped() {
        var p = MjpegParser()
        var stream = Data("\r\n--\(MjpegParser.boundary)\r\nContent-Type: image/jpeg\r\n\r\n".utf8)
        stream += Data([0x00, 0x01, 0x02]) // unbounded junk body
        stream += part(jpegB)
        // the junk body is dropped once the next marker surfaces
        let frames = feed(&p, stream, chunk: 6)
        XCTAssertEqual(frames, [jpegB])
    }

    func testGarbageBetweenPartsIsDropped() {
        var p = MjpegParser()
        let stream = part(jpegA) + Data("<<<corruption>>>".utf8) + part(jpegB)
        XCTAssertEqual(feed(&p, stream, chunk: 4), [jpegA, jpegB])
    }

    func testOversizedHeaderPartIsAbandoned() {
        var p = MjpegParser()
        var stream = Data("\r\n--\(MjpegParser.boundary)\r\n".utf8)
        stream += Data(String(repeating: "X-Junk: v\r\n", count: 200).utf8) // > 1024 B, no terminator guard hit
        stream += part(jpegB)
        let frames = feed(&p, stream, chunk: 128)
        XCTAssertEqual(frames, [jpegB], "runaway header part must not wedge the parser")
    }

    func testResetClearsState() {
        var p = MjpegParser()
        _ = feed(&p, part(jpegA), chunk: 3)
        p.reset()
        XCTAssertEqual(p.phase, .marker)
        XCTAssertEqual(p.feed(part(jpegB)), [jpegB])
    }

    func testContentLengthParsing() {
        XCTAssertNil(MjpegParser.contentLength(in: "Content-Type: image/jpeg"))
        XCTAssertEqual(MjpegParser.contentLength(in: "content-length: 42"), 42)
        XCTAssertEqual(MjpegParser.contentLength(in: "A: 1\r\nContent-Length :  7 "), 7)
    }
}
