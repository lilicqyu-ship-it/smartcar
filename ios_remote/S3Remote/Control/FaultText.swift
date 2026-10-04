/*
 * FaultText.swift — human-readable robot task states and fault codes
 * (tc275_car/app/robot.c: fault 1 e-stop / 2 link timeout / 3 illegal cmd;
 * state 0x00 INIT / 0x01 IDLE / 0x02..0x09 motion / 0x0A FAULT). Pure text
 * mapping so VoiceOver and the vehicle page never show bare hex.
 */

import Foundation

public enum FaultText {
    public static func describe(_ code: UInt16) -> String {
        switch code {
        case 0: return "正常"
        case 0x0001: return "急停锁存"
        case 0x0002: return "通讯超时"
        case 0x0003: return "非法命令"
        default: return String(format: "未知故障 0x%04X", code)
        }
    }

    public static func robotState(_ state: UInt8) -> String {
        switch state {
        case 0x00: return "初始化"
        case 0x01: return "待命"
        case 0x02...0x09: return "运行中"
        case 0x0A: return "故障"
        default: return String(format: "未知 0x%02X", state)
        }
    }
}
