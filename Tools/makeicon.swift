// Renders the app icon masters. Run via Tools/makeicons.sh.
//
//   makeicon <out-ios.png> <out-macos.png>
//
// A tapering loop with an arrowhead — the looper motif — in blue on a dark blue
// ground. iOS gets a full-bleed square (the system applies its own mask); macOS
// gets a squircle inset inside a transparent square, as Apple's grid expects.

import Foundation
import CoreGraphics
import ImageIO
import UniformTypeIdentifiers

let S: CGFloat = 1024

func ctx() -> CGContext {
    CGContext(data: nil, width: Int(S), height: Int(S), bitsPerComponent: 8, bytesPerRow: 0,
              space: CGColorSpace(name: CGColorSpace.sRGB)!,
              bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
}

func rgb(_ r: CGFloat, _ g: CGFloat, _ b: CGFloat, _ a: CGFloat = 1) -> CGColor {
    CGColor(srgbRed: r / 255, green: g / 255, blue: b / 255, alpha: a)
}

/// Squircle-ish rounded rect.
func roundedPath(_ rect: CGRect, radius: CGFloat) -> CGPath {
    CGPath(roundedRect: rect, cornerWidth: radius, cornerHeight: radius, transform: nil)
}

/// Tapered ribbon following a circular arc, plus an arrowhead at the end.
func drawLoop(_ c: CGContext, center: CGPoint, radius R: CGFloat,
              from a0: CGFloat, sweep: CGFloat, wStart: CGFloat, wEnd: CGFloat) {
    let steps = 260
    var outer: [CGPoint] = []
    var inner: [CGPoint] = []
    for i in 0...steps {
        let t = CGFloat(i) / CGFloat(steps)
        let a = a0 + sweep * t
        let w = wStart + (wEnd - wStart) * t
        let co = cos(a), si = sin(a)
        outer.append(CGPoint(x: center.x + co * (R + w / 2), y: center.y + si * (R + w / 2)))
        inner.append(CGPoint(x: center.x + co * (R - w / 2), y: center.y + si * (R - w / 2)))
    }
    let p = CGMutablePath()
    p.move(to: outer[0])
    for pt in outer.dropFirst() { p.addLine(to: pt) }
    for pt in inner.reversed() { p.addLine(to: pt) }
    p.closeSubpath()

    c.saveGState()
    c.addPath(p)
    c.clip()
    let grad = CGGradient(colorsSpace: CGColorSpace(name: CGColorSpace.sRGB)!,
                          colors: [rgb(46, 130, 255), rgb(86, 225, 255)] as CFArray,
                          locations: [0, 1])!
    c.drawLinearGradient(grad,
                         start: CGPoint(x: center.x - R, y: center.y - R),
                         end: CGPoint(x: center.x + R, y: center.y + R),
                         options: [])
    c.restoreGState()

    // Arrowhead, aimed along the tangent at the end of the sweep.
    let aEnd = a0 + sweep
    let tip = aEnd + 0.115
    let head = CGMutablePath()
    let tipPt = CGPoint(x: center.x + cos(tip) * R, y: center.y + sin(tip) * R)
    let backOuter = CGPoint(x: center.x + cos(aEnd) * (R + wEnd * 0.92),
                            y: center.y + sin(aEnd) * (R + wEnd * 0.92))
    let backInner = CGPoint(x: center.x + cos(aEnd) * (R - wEnd * 0.92),
                            y: center.y + sin(aEnd) * (R - wEnd * 0.92))
    head.move(to: tipPt)
    head.addLine(to: backOuter)
    head.addLine(to: backInner)
    head.closeSubpath()
    c.addPath(head)
    c.setFillColor(rgb(86, 225, 255))
    c.fillPath()
}

func render(fullBleed: Bool) -> CGImage {
    let c = ctx()
    let rect: CGRect
    let corner: CGFloat
    if fullBleed {
        rect = CGRect(x: 0, y: 0, width: S, height: S)
        corner = 0
    } else {
        let inset = S * 0.085
        rect = CGRect(x: inset, y: inset, width: S - inset * 2, height: S - inset * 2)
        corner = rect.width * 0.225
    }

    // Background
    c.saveGState()
    c.addPath(roundedPath(rect, radius: corner))
    c.clip()
    let bg = CGGradient(colorsSpace: CGColorSpace(name: CGColorSpace.sRGB)!,
                        colors: [rgb(9, 22, 45), rgb(16, 52, 96)] as CFArray,
                        locations: [0, 1])!
    c.drawLinearGradient(bg, start: CGPoint(x: rect.minX, y: rect.maxY),
                         end: CGPoint(x: rect.maxX, y: rect.minY), options: [])

    let center = CGPoint(x: rect.midX, y: rect.midY)
    let R = rect.width * 0.295

    // Faint inner companion ring — hints at layered loops.
    c.setStrokeColor(rgb(86, 225, 255, 0.16))
    c.setLineWidth(rect.width * 0.022)
    c.addArc(center: center, radius: R * 0.58, startAngle: 0, endAngle: .pi * 2, clockwise: false)
    c.strokePath()

    // Main loop with glow.
    c.setShadow(offset: .zero, blur: rect.width * 0.055, color: rgb(46, 150, 255, 0.75))
    drawLoop(c, center: center, radius: R,
             from: .pi * 0.62, sweep: .pi * 1.62,
             wStart: rect.width * 0.028, wEnd: rect.width * 0.092)
    c.restoreGState()

    return c.makeImage()!
}

func write(_ image: CGImage, to path: String) {
    let url = URL(fileURLWithPath: path)
    let dest = CGImageDestinationCreateWithURL(url as CFURL, UTType.png.identifier as CFString, 1, nil)!
    CGImageDestinationAddImage(dest, image, nil)
    CGImageDestinationFinalize(dest)
}

let args = CommandLine.arguments
guard args.count == 3 else {
    FileHandle.standardError.write("usage: makeicon <ios.png> <macos.png>\n".data(using: .utf8)!)
    exit(1)
}
write(render(fullBleed: true), to: args[1])
write(render(fullBleed: false), to: args[2])
print("wrote \(args[1]) and \(args[2])")
