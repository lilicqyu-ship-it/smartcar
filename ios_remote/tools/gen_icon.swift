/*
 * gen_icon.swift — renders the S3 Remote app icon (1024×1024) with pure
 * CoreGraphics: cockpit-dark background + faint grid, a glowing neon-green
 * joystick (side view) emitting signal arcs toward the top-right, and
 * green/red LED pills on the base. Matches the app's HUD design language.
 *
 * Usage: swift gen_icon.swift <output.png>
 */

import CoreGraphics
import Foundation
import ImageIO

let size = 1024.0
let out = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "icon-1024.png"

let space = CGColorSpace(name: CGColorSpace.sRGB)!
let ctx = CGContext(data: nil, width: Int(size), height: Int(size),
                    bitsPerComponent: 8, bytesPerRow: 0, space: space,
                    bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
// flip to top-down coordinates (design intent uses screen-like y)
ctx.translateBy(x: 0, y: size)
ctx.scaleBy(x: 1, y: -1)

func rgb(_ hex: UInt32, _ alpha: CGFloat = 1) -> CGColor {
    CGColor(srgbRed: CGFloat((hex >> 16) & 0xFF) / 255,
            green: CGFloat((hex >> 8) & 0xFF) / 255,
            blue: CGFloat(hex & 0xFF) / 255, alpha: alpha)
}

// ---- background: deep space + faint grid + green bloom ----------------------

let bg = CGGradient(colorsSpace: space,
                    colors: [rgb(0x0C141D), rgb(0x04080C)] as CFArray,
                    locations: [0, 1])!
ctx.drawRadialGradient(bg, startCenter: CGPoint(x: 430, y: 380), startRadius: 0,
                       endCenter: CGPoint(x: 430, y: 380), endRadius: 950,
                       options: [])

ctx.setStrokeColor(rgb(0xFFFFFF, 0.025))
ctx.setLineWidth(2)
var g: CGFloat = 64
while g < size {
    ctx.move(to: CGPoint(x: g, y: 0)); ctx.addLine(to: CGPoint(x: g, y: size))
    ctx.move(to: CGPoint(x: 0, y: g)); ctx.addLine(to: CGPoint(x: size, y: g))
    ctx.strokePath()
    g += 64
}

let knobCenter = CGPoint(x: 470, y: 398)
let bloom = CGGradient(colorsSpace: space,
                       colors: [rgb(0x2BD977, 0.16), rgb(0x2BD977, 0)] as CFArray,
                       locations: [0, 1])!
ctx.drawRadialGradient(bloom, startCenter: knobCenter, startRadius: 0,
                       endCenter: knobCenter, endRadius: 620, options: [])

// ---- signal arcs (remote waves toward top-right) -----------------------------

let arcSpecs: [(CGFloat, CGFloat, CGFloat)] = [(300, 30, 0.55), (390, 26, 0.34), (480, 22, 0.20)]
for (radius, width, alpha) in arcSpecs {
    let arc = CGMutablePath()
    arc.addArc(center: knobCenter, radius: radius,
               startAngle: -1.26, endAngle: -0.32, clockwise: false) // -72° … -18°
    ctx.setShadow(offset: .zero, blur: 14, color: rgb(0x35D977, 0.30))
    ctx.setStrokeColor(rgb(0x4BE98D, alpha))
    ctx.setLineWidth(width)
    ctx.setLineCap(.round)
    ctx.addPath(arc)
    ctx.strokePath()
    ctx.setShadow(offset: .zero, blur: 0, color: nil)
}

// ---- joystick: stalk + base ---------------------------------------------------

ctx.setStrokeColor(rgb(0x22303C))
ctx.setLineWidth(62)
ctx.setLineCap(.round)
ctx.move(to: CGPoint(x: 512, y: 760))
ctx.addLine(to: CGPoint(x: 476, y: 486))
ctx.strokePath()

let baseRect = CGRect(x: 242, y: 700, width: 540, height: 122)
let base = CGPath(roundedRect: baseRect, cornerWidth: 46, cornerHeight: 46, transform: nil)
ctx.addPath(base)
ctx.setFillColor(rgb(0x16202A))
ctx.fillPath()
ctx.addPath(base)
ctx.setStrokeColor(rgb(0x2B3742))
ctx.setLineWidth(5)
ctx.strokePath()

// LED pills: green (mode) left, red (STOP) right
let ledGreen = CGPath(roundedRect: CGRect(x: 292, y: 748, width: 130, height: 26),
                      cornerWidth: 13, cornerHeight: 13, transform: nil)
ctx.addPath(ledGreen)
ctx.setFillColor(rgb(0x2BA05F))
ctx.fillPath()

ctx.setShadow(offset: .zero, blur: 12, color: rgb(0xD12638, 0.5))
let ledRed = CGPath(roundedRect: CGRect(x: 592, y: 748, width: 150, height: 26),
                    cornerWidth: 13, cornerHeight: 13, transform: nil)
ctx.addPath(ledRed)
ctx.setFillColor(rgb(0xD12638))
ctx.fillPath()
ctx.setShadow(offset: .zero, blur: 0, color: nil)

// ---- knob: glow passes + radial gradient + rim + specular ---------------------

let knob = CGPath(ellipseIn: CGRect(x: knobCenter.x - 190, y: knobCenter.y - 190,
                                    width: 380, height: 380), transform: nil)
for (blur, alpha) in [(95.0, 0.32), (45.0, 0.45), (20.0, 0.55)] {
    ctx.setShadow(offset: .zero, blur: blur, color: rgb(0x35D977, alpha))
    ctx.addPath(knob)
    ctx.setFillColor(rgb(0x2BD977))
    ctx.fillPath()
}
ctx.setShadow(offset: .zero, blur: 0, color: nil)

ctx.addPath(knob)
ctx.clip()
let knobGrad = CGGradient(colorsSpace: space,
                          colors: [rgb(0x62E89B), rgb(0x35D977), rgb(0x0B7B45)] as CFArray,
                          locations: [0, 0.5, 1])!
ctx.drawRadialGradient(knobGrad,
                       startCenter: CGPoint(x: 424, y: 350), startRadius: 24,
                       endCenter: knobCenter, endRadius: 330, options: [])
ctx.resetClip()

ctx.addPath(knob)
ctx.setStrokeColor(rgb(0x0A5A36, 0.9))
ctx.setLineWidth(9)
ctx.strokePath()

// glossy highlight: opaque body, feathered edge (nothing shows through)
let specCenter = CGPoint(x: 396, y: 313)
let specGrad = CGGradient(colorsSpace: space,
                          colors: [rgb(0xFFFFFF, 0.94), rgb(0xEEFFF5, 0.82), rgb(0xFFFFFF, 0)] as CFArray,
                          locations: [0, 0.55, 1])!
ctx.drawRadialGradient(specGrad,
                       startCenter: specCenter, startRadius: 0,
                       endCenter: specCenter, endRadius: 100, options: [])

// ---- write PNG -----------------------------------------------------------------

let image = ctx.makeImage()!
let url = URL(fileURLWithPath: out) as CFURL
let dest = CGImageDestinationCreateWithURL(url, "public.png" as CFString, 1, nil)!
CGImageDestinationAddImage(dest, image, nil)
guard CGImageDestinationFinalize(dest) else {
    FileHandle.standardError.write("failed to write \(out)\n".data(using: .utf8)!)
    exit(1)
}
print("wrote \(out)")
