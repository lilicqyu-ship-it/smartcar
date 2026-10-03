import SwiftUI

struct JoystickView: View {
    var deadzone: Double
    var enabled: Bool
    var onTouch: () -> Void
    var onChange: (_ v: Double, _ w: Double) -> Void

    @Environment(AppState.self) private var app
    @State private var knob: CGSize = .zero
    @State private var dragging = false

    private let radius: CGFloat = 78

    var body: some View {
        ZStack {
            telemetryRing
            baseDisc
            innerGuides
            knobView
        }
        .frame(width: radius * 2 + 32, height: radius * 2 + 32)
        .opacity(enabled ? 1 : 0.75)
        .accessibilityLabel("驾驶摇杆，上下控制前后，左右控制转向")
        .onChange(of: enabled) { _, value in if !value { reset() } }
        .onDisappear { reset() }
        .contentShape(Circle())
        .gesture(enabled ? drag : nil)
    }

    private var telemetryRing: some View {
        Circle().strokeBorder(.white.opacity(0.07), lineWidth: 1).padding(2)
    }

    private var baseDisc: some View {
        Circle().fill(.white.opacity(0.035))
            .overlay(Circle().strokeBorder(.white.opacity(0.10), lineWidth: 1))
            .padding(18)
    }

    private var innerGuides: some View {
        ZStack {
            Circle().strokeBorder(.white.opacity(0.08), style: StrokeStyle(lineWidth: 1, dash: [3, 6]))
                .frame(width: radius * 1.35, height: radius * 1.35)
            Rectangle().fill(.white.opacity(0.07)).frame(width: 1, height: radius * 1.6)
            Rectangle().fill(.white.opacity(0.07)).frame(width: radius * 1.6, height: 1)
            Circle().strokeBorder(.white.opacity(0.2), lineWidth: 1)
                .frame(width: radius * 2 * deadzone, height: radius * 2 * deadzone)
            Image(systemName: "chevron.up").offset(y: -radius + 5)
            Image(systemName: "chevron.down").offset(y: radius - 5)
            Image(systemName: "chevron.left").offset(x: -radius + 5)
            Image(systemName: "chevron.right").offset(x: radius - 5)
        }.font(.system(size: 10, weight: .bold)).foregroundStyle(.white.opacity(0.4))
    }

    private var knobView: some View {
        Circle()
            .fill(enabled ? Color(red: 0.96, green: 0.40, blue: 0.19) : Color(red: 0.44, green: 0.50, blue: 0.49))
            .frame(width: 68, height: 68)
            .overlay(Circle().strokeBorder(.white.opacity(0.25), lineWidth: 1))
            .overlay(Image(systemName: "plus").font(.system(size: 20, weight: .light)).foregroundStyle(.white))
            .shadow(color: .black.opacity(0.25), radius: 12, y: 6)
            .scaleEffect(dragging ? 1.05 : 1)
            .offset(knob)
    }

    private func reset() {
        knob = .zero
        dragging = false
        onChange(0, 0)
    }

    private var drag: some Gesture {
        DragGesture(minimumDistance: 0)
            .onChanged { g in
                if !dragging {
                    dragging = true
                    Haptics.light()
                    onTouch() // re-take control: clears the STOP latch
                }
                let dx = g.translation.width
                let dy = g.translation.height
                let mag = sqrt(dx * dx + dy * dy)
                guard mag > 0 else { return }
                let clamped = min(mag, radius)
                let ux = dx / mag
                let uy = dy / mag
                knob = CGSize(width: ux * clamped, height: uy * clamped)
                let axes = JoystickInput.axes(x: Double(dx), y: Double(dy),
                                              radius: Double(radius), deadzone: deadzone)
                onChange(axes.v, axes.w)
            }
            .onEnded { _ in
                dragging = false
                withAnimation(.spring(response: 0.3, dampingFraction: 0.6)) {
                    knob = .zero
                }
                onChange(0, 0)
            }
    }
}
