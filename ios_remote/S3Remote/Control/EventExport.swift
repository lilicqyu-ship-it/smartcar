/*
 * EventExport.swift — shareable text rendering of the diagnostic event ring.
 * Pure formatting; DiagView hands the output to ShareLink / pasteboard.
 */

import Foundation

public enum EventExport {
    public static let maxLines = 200
    static let timeFormatter: DateFormatter = {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "HH:mm:ss.SSS"
        return f
    }()

    public static func text(_ events: [EventEntry], host: String,
                            exportedAt: Date = Date()) -> String {
        let stamp = ISO8601DateFormatter().string(from: exportedAt)
        var lines = [
            "S3Remote 事件日志",
            "网关: \(host)",
            "导出时间: \(stamp)",
        ]
        guard !events.isEmpty else {
            lines.append("(无事件)")
            return lines.joined(separator: "\n")
        }
        let slice = events.prefix(maxLines)
        lines.append("共 \(slice.count) 条（最新在前）")
        lines.append("--------")
        for e in slice {
            lines.append("\(timeFormatter.string(from: e.at)) \(e.level) \(e.text)")
        }
        return lines.joined(separator: "\n")
    }
}
