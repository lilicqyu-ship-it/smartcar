/*
 * SoundEngine.swift — synthesized car audio with zero asset files. A live
 * engine hum (AVAudioSourceNode: saw + sub-sine through a one-pole low-pass,
 * pitch/brightness follow the actually-sent drive output) plus one-shot horn
 * and stunt-blip buffers. Ambient session: respects the silent switch, mixes
 * with other audio. Every audio call fails soft (simulator / no session).
 *
 * The render block runs on the real-time audio thread: it only touches the
 * lock-guarded HumParams and values captured before node creation — never
 * MainActor state.
 */

import AVFoundation
import Foundation

/// State shared between the MainActor (writes targets) and the audio render
/// thread (advances phase/gain/low-pass). Lock-guarded, non-isolated.
final class HumParams: @unchecked Sendable {
    private let lock = NSLock()
    private var freq = 60.0
    private var targetGain = 0.0
    private var cutoff = 400.0
    private var phase = 0.0
    private var subPhase = 0.0
    private var gain = 0.0
    private var lp = 0.0

    func setTarget(freq: Double, gain: Double, cutoff: Double) {
        lock.lock()
        self.freq = freq
        self.targetGain = gain
        self.cutoff = cutoff
        lock.unlock()
    }

    /// Renders `frames` mono samples straight into both channel pointers.
    func render(frames: Int, sampleRate: Double,
                left: UnsafeMutablePointer<Float>, right: UnsafeMutablePointer<Float>) {
        lock.lock()
        defer { lock.unlock() }
        let alpha = 1 - exp(-2 * Double.pi * cutoff / sampleRate)
        let gainStep = targetGain > gain ? 0.004 : 0.0025
        for i in 0..<frames {
            phase += freq / sampleRate
            if phase >= 1 { phase -= 1 }
            subPhase += freq / sampleRate / 2
            if subPhase >= 1 { subPhase -= 1 }
            let saw = phase * 2 - 1
            let sub = sin(subPhase * 2 * .pi)
            lp += ((0.55 * saw + 0.45 * sub) - lp) * alpha
            gain += (targetGain - gain) * gainStep
            if gain < 0.00005, targetGain == 0 { gain = 0 }
            let s = Float(lp * gain)
            left[i] = s
            right[i] = s
        }
    }
}

@MainActor
final class SoundEngine {
    /// Output level when the engine hum is live.
    static let humGain = 0.09

    private var engine: AVAudioEngine?
    private var player: AVAudioPlayerNode?
    private var params: HumParams?
    private var sampleRate: Double = 48_000
    private(set) var enabled = false

    // ---- enable / disable -------------------------------------------------------

    func setEnabled(_ on: Bool) {
        guard on != enabled else { return }
        enabled = on
        if on {
            configureAndStart()
        } else {
            params?.setTarget(freq: 60, gain: 0, cutoff: 400)
        }
    }

    private func configureAndStart() {
        if let engine, engine.isRunning { return }
        if engine != nil { try? engine?.start(); return }
        do {
            let session = AVAudioSession.sharedInstance()
            try session.setCategory(.ambient, options: [.mixWithOthers])
            try session.setActive(true)
            if session.sampleRate > 8_000 { sampleRate = session.sampleRate }
            let sr = sampleRate
            guard let format = AVAudioFormat(standardFormatWithSampleRate: sr, channels: 2) else {
                throw NSError(domain: "SoundEngine", code: 1)
            }

            let eng = AVAudioEngine()
            let hum = HumParams()
            let src = AVAudioSourceNode(format: format) { _, _, frameCount, bufferList in
                let abl = UnsafeMutableAudioBufferListPointer(bufferList)
                guard abl.count >= 2,
                      let left = abl[0].mData?.assumingMemoryBound(to: Float.self),
                      let right = abl[1].mData?.assumingMemoryBound(to: Float.self)
                else { return noErr }
                hum.render(frames: Int(frameCount), sampleRate: sr, left: left, right: right)
                return noErr
            }
            let hornPlayer = AVAudioPlayerNode()

            engine = eng
            params = hum
            player = hornPlayer
            eng.attach(src)
            eng.attach(hornPlayer)
            eng.connect(src, to: eng.mainMixerNode, format: format)
            eng.connect(hornPlayer, to: eng.mainMixerNode, format: format)
            try eng.start()
            hornPlayer.play()
        } catch {
            engine = nil
            params = nil
            player = nil
        }
    }

    // ---- live engine hum (30 Hz from the control tick) ----------------------------

    func update(active: Bool, speedNorm: Double) {
        guard enabled, params != nil else { return }
        let n = min(max(speedNorm, 0), 1)
        params?.setTarget(freq: 55 + 110 * n,
                          gain: active ? Self.humGain : 0,
                          cutoff: 350 + 2500 * n)
    }

    // ---- one-shots -----------------------------------------------------------------

    func horn() {
        guard enabled, let player, engine?.isRunning == true else { return }
        // classic dual-tone horn
        let buffer = oneShot(frames: Int(sampleRate * 0.45)) { t, sr in
            let s = tanh(sin(t * 2 * .pi * 420 / sr) * 2.4) + tanh(sin(t * 2 * .pi * 520 / sr) * 2.4)
            return s * 0.13 * Self.envelope(t, duration: 0.45, attack: 0.008, release: 0.14)
        }
        if let buffer { player.scheduleBuffer(buffer) }
    }

    func blip() {
        guard enabled, let player, engine?.isRunning == true else { return }
        // short rising sweep = stunt launch cue
        let buffer = oneShot(frames: Int(sampleRate * 0.16)) { t, sr in
            let f = 500 + 900 * (t / 0.16)
            return sin(t * 2 * .pi * f / sr) * 0.16 * Self.envelope(t, duration: 0.16, attack: 0.004, release: 0.09)
        }
        if let buffer { player.scheduleBuffer(buffer) }
    }

    // ---- helpers ---------------------------------------------------------------------

    private func oneShot(frames: Int, gen: (Double, Double) -> Double) -> AVAudioPCMBuffer? {
        guard frames > 0,
              let format = AVAudioFormat(standardFormatWithSampleRate: sampleRate, channels: 2),
              let buffer = try? AVAudioPCMBuffer(pcmFormat: format, frameCapacity: AVAudioFrameCount(frames))
        else { return nil }
        buffer.frameLength = AVAudioFrameCount(frames)
        if let ch = buffer.floatChannelData {
            for i in 0..<frames {
                let s = Float(gen(Double(i) / sampleRate, sampleRate))
                ch[0][i] = s
                ch[1][i] = s
            }
        }
        return buffer
    }

    static func envelope(_ t: Double, duration: Double, attack: Double, release: Double) -> Double {
        if t < attack { return t / attack }
        let remaining = duration - t
        if remaining < release { return max(0, remaining / release) }
        return 1
    }
}
