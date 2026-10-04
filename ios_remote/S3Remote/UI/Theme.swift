import SwiftUI

/// Warm paper, precise typography and a single orange interaction accent.
public enum Theme {
    public static let bg = Color(red: 0.96, green: 0.95, blue: 0.92)
    public static let bgLift = Color(red: 0.93, green: 0.92, blue: 0.89)
    public static let panel = Color(red: 0.995, green: 0.99, blue: 0.97)
    public static let panelStroke = Color(red: 0.85, green: 0.85, blue: 0.81)
    public static let accent = Color(red: 0.78, green: 0.25, blue: 0.08)
    public static let accentDeep = Color(red: 0.65, green: 0.19, blue: 0.05)
    public static let info = Color(red: 0.17, green: 0.36, blue: 0.53)
    public static let text = Color(red: 0.13, green: 0.17, blue: 0.17)
    public static let dim = Color(red: 0.40, green: 0.44, blue: 0.43)
    public static let warn = Color(red: 0.60, green: 0.36, blue: 0.02)
    public static let crit = Color(red: 0.76, green: 0.15, blue: 0.18)
    public static let stopRed = crit
    public static let stopRedDeep = Color(red: 0.59, green: 0.09, blue: 0.12)
    public static let ink = Color(red: 0.10, green: 0.15, blue: 0.16)
    public static let live = Color(red: 0.17, green: 0.48, blue: 0.36)
    public static let accentGradient = LinearGradient(colors: [accent, accentDeep], startPoint: .top, endPoint: .bottom)
    public static let stopGradient = LinearGradient(colors: [stopRed, stopRedDeep], startPoint: .leading, endPoint: .trailing)
    public static let panelGradient = LinearGradient(colors: [panel, panel], startPoint: .top, endPoint: .bottom)
    public static func display(_ size: CGFloat, weight: Font.Weight = .bold) -> Font {
        .system(size: size, weight: weight, design: .rounded)
    }
    public static func mono(_ size: CGFloat, weight: Font.Weight = .semibold) -> Font {
        .system(size: size, weight: weight, design: .monospaced)
    }
    public static func levelColor(_ level: AlertLevel) -> Color {
        switch level { case .warning: warn; case .critical: crit }
    }
}

public struct PanelModifier: ViewModifier {
    public func body(content: Content) -> some View {
        content.padding(20)
            .background(Theme.panel, in: RoundedRectangle(cornerRadius: 24))
            .overlay(RoundedRectangle(cornerRadius: 24).strokeBorder(Theme.panelStroke.opacity(0.5), lineWidth: 1))
    }
}
public func Panel<Content: View>(@ViewBuilder content: () -> Content) -> some View {
    content().modifier(PanelModifier())
}
public extension View {
    func panel() -> some View { modifier(PanelModifier()) }
    /// Two-layer shadow halo (tight core + wide falloff) — the "instrument
    /// glow" of the cockpit design language. Extracted helper keeps the
    /// parameters unit-testable (GlowModifierTests guards against another
    /// identity-function regression).
    func glow(_ color: Color, radius: CGFloat = 8, opacity: Double = 0.6) -> some View {
        modifier(GlowModifier(color: color, radius: radius, opacity: opacity))
    }
}

public struct GlowModifier: ViewModifier {
    public let color: Color
    public let radius: CGFloat
    public let opacity: Double

    public init(color: Color, radius: CGFloat, opacity: Double) {
        self.color = color
        self.radius = radius
        self.opacity = opacity
    }

    public static func shadows(color: Color, radius: CGFloat, opacity: Double) -> [(radius: CGFloat, spread: CGFloat)] {
        [(radius: radius, spread: 0.85), (radius: radius * 2.4, spread: 0.35)]
    }

    public func body(content: Content) -> some View {
        content
            .shadow(color: color.opacity(opacity * 0.85), radius: radius)
            .shadow(color: color.opacity(opacity * 0.35), radius: radius * 2.4)
    }
}
public struct Page<Content: View>: View {
    private let content: Content
    public init(@ViewBuilder content: () -> Content) { self.content = content() }
    public var body: some View {
        ZStack { Theme.bg.ignoresSafeArea(); content }
            .foregroundStyle(Theme.text)
    }
}

struct PageHeading: View {
    let eyebrow: String
    let title: String
    let subtitle: String
    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(eyebrow).font(Theme.mono(10)).tracking(3).foregroundStyle(Theme.accent)
            Text(title).font(.largeTitle.bold())
            Text(subtitle).font(.subheadline).foregroundStyle(Theme.dim)
        }.frame(maxWidth: .infinity, alignment: .leading).padding(.vertical, 8)
    }
}
