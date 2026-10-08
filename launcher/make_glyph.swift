// Draws the Skate 3 LAN glyph (512 px PNG) in the style of Skate 3's
// pause-menu icons: icy speckled shapes with a cyan outline. A skateboard under
// signal waves. The app icon is made from it by launcher/make_icon.swift.
// Usage: swift make_glyph.swift out.png
import AppKit

let size: CGFloat = 512
let image = NSImage(size: NSSize(width: size, height: size))
image.lockFocus()
let ctx = NSGraphicsContext.current!.cgContext
srand48(5)

let ice = NSColor(srgbRed: 0.88, green: 0.94, blue: 0.97, alpha: 1)
let fleck = NSColor(srgbRed: 0.42, green: 0.62, blue: 0.72, alpha: 1)
let outline = NSColor(srgbRed: 0.40, green: 0.82, blue: 0.93, alpha: 1)

// Fill a shape with speckled ice and trace it in cyan.
func icy(_ path: CGPath) {
  ctx.saveGState()
  ctx.addPath(path)
  ctx.clip()
  ctx.setFillColor(ice.cgColor)
  ctx.fill(path.boundingBox)
  let box = path.boundingBox
  for _ in 0..<Int(box.width * box.height / 40) {
    let d = 1.5 + drand48() * 3
    ctx.setFillColor(fleck.withAlphaComponent(0.2 + drand48() * 0.5).cgColor)
    ctx.fillEllipse(in: CGRect(x: box.minX + drand48() * box.width, y: box.minY + drand48() * box.height,
                               width: d, height: d))
  }
  ctx.restoreGState()
  ctx.saveGState()
  ctx.setShadow(offset: .zero, blur: 6, color: outline.withAlphaComponent(0.6).cgColor)
  ctx.addPath(path)
  ctx.setStrokeColor(outline.cgColor)
  ctx.setLineWidth(11)
  ctx.setLineJoin(.round)
  ctx.strokePath()
  ctx.restoreGState()
}

// Signal waves: three arc bands and a dot (y up).
let center = CGPoint(x: 256, y: 250)
for (i, radius) in [CGFloat(70), 128, 186].enumerated() {
  let arc = CGMutablePath()
  arc.addArc(center: center, radius: radius, startAngle: .pi * 0.25, endAngle: .pi * 0.75, clockwise: false)
  icy(arc.copy(strokingWithWidth: CGFloat(34 - i * 2), lineCap: .round, lineJoin: .round, miterLimit: 4))
}
icy(CGPath(ellipseIn: CGRect(x: center.x - 24, y: center.y - 4, width: 48, height: 48), transform: nil))

// Skateboard: a deck with kicked tails, two wheels.
let deck = CGMutablePath()
deck.addLines(between: [CGPoint(x: 30, y: 150), CGPoint(x: 62, y: 172), CGPoint(x: 96, y: 142),
                        CGPoint(x: 410, y: 158), CGPoint(x: 450, y: 212), CGPoint(x: 482, y: 196),
                        CGPoint(x: 440, y: 128), CGPoint(x: 96, y: 110)])
deck.closeSubpath()
for x in [CGFloat(150), 370] {
  let truck = CGMutablePath()
  truck.addLines(between: [CGPoint(x: x - 34, y: 116), CGPoint(x: x + 34, y: 118), CGPoint(x: x + 16, y: 84),
                           CGPoint(x: x - 18, y: 84)])
  truck.closeSubpath()
  icy(truck)
  icy(CGPath(ellipseIn: CGRect(x: x - 40, y: 18, width: 80, height: 80), transform: nil))
}
icy(deck)

image.unlockFocus()
let rep = NSBitmapImageRep(data: image.tiffRepresentation!)!
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: CommandLine.arguments[1]))
