// Renders the launcher icon (1024 px PNG) in the pause menu's style: the
// glyph on a dark slate panel set in a speckled paper card.
// Usage: swift make_icon.swift glyph.png out.png
import AppKit

let size: CGFloat = 1024
let glyph = NSImage(contentsOfFile: CommandLine.arguments[1])!
let image = NSImage(size: NSSize(width: size, height: size))
image.lockFocus()
let ctx = NSGraphicsContext.current!.cgContext
srand48(7)

func specks(_ rect: CGRect, count: Int, color: NSColor) {
  for _ in 0..<count {
    let d = 2 + drand48() * 4
    ctx.setFillColor(color.withAlphaComponent(0.15 + drand48() * 0.45).cgColor)
    ctx.fillEllipse(in: CGRect(x: rect.minX + drand48() * rect.width, y: rect.minY + drand48() * rect.height,
                               width: d, height: d))
  }
}

// Paper card, with the macOS icon margin.
let card = CGRect(x: 100, y: 100, width: 824, height: 824)
ctx.saveGState()
ctx.setShadow(offset: CGSize(width: 0, height: -12), blur: 30, color: NSColor(white: 0, alpha: 0.5).cgColor)
ctx.addPath(CGPath(roundedRect: card, cornerWidth: 185, cornerHeight: 185, transform: nil))
ctx.setFillColor(NSColor(srgbRed: 0.83, green: 0.78, blue: 0.65, alpha: 1).cgColor)
ctx.fillPath()
ctx.restoreGState()
ctx.saveGState()
ctx.addPath(CGPath(roundedRect: card, cornerWidth: 185, cornerHeight: 185, transform: nil))
ctx.clip()
specks(card, count: 14000, color: NSColor(srgbRed: 0.42, green: 0.36, blue: 0.27, alpha: 1))
ctx.restoreGState()

// Dark slate panel with a torn edge.
let panel = card.insetBy(dx: 58, dy: 58)
let edge = CGMutablePath()
let corners = [CGPoint(x: panel.minX, y: panel.minY), CGPoint(x: panel.maxX, y: panel.minY),
               CGPoint(x: panel.maxX, y: panel.maxY), CGPoint(x: panel.minX, y: panel.maxY)]
for c in 0..<4 {
  let a = corners[c], b = corners[(c + 1) % 4]
  for s in 0..<60 {
    let t = CGFloat(s) / 60, j = CGFloat(drand48() * 10 - 5)
    let p = CGPoint(x: a.x + (b.x - a.x) * t + (a.y == b.y ? 0 : j), y: a.y + (b.y - a.y) * t + (a.y == b.y ? j : 0))
    if edge.isEmpty { edge.move(to: p) } else { edge.addLine(to: p) }
  }
}
edge.closeSubpath()
ctx.saveGState()
ctx.addPath(CGPath(roundedRect: panel, cornerWidth: 130, cornerHeight: 130, transform: nil))
ctx.clip()
ctx.addPath(edge)
ctx.clip()
let colors = [NSColor(srgbRed: 0.15, green: 0.19, blue: 0.23, alpha: 1).cgColor,
              NSColor(srgbRed: 0.08, green: 0.10, blue: 0.13, alpha: 1).cgColor] as CFArray
let gradient = CGGradient(colorsSpace: CGColorSpaceCreateDeviceRGB(), colors: colors, locations: [0, 1])!
ctx.drawLinearGradient(gradient, start: CGPoint(x: 0, y: panel.maxY), end: CGPoint(x: 0, y: panel.minY), options: [])
specks(panel, count: 900, color: .white)
ctx.restoreGState()

// The icon, glowing like the selected crossbar category.
ctx.setShadow(offset: .zero, blur: 50, color: NSColor(srgbRed: 0.2, green: 0.6, blue: 1, alpha: 0.7).cgColor)
glyph.draw(in: panel.insetBy(dx: 70, dy: 70))

image.unlockFocus()
let rep = NSBitmapImageRep(data: image.tiffRepresentation!)!
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: CommandLine.arguments[2]))
