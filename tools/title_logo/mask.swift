// Renders a white mask of the text into a W x H PNG, fitted into the box.
import AppKit
let args = CommandLine.arguments
let text = args[1], out = args[2], fontName = args[3]
// canvas W x H, text fitted into the box x0..x1, y0..y1 (pixels, y down)
let W = Int(args[4])!, H = Int(args[5])!
let box = (x0: CGFloat(Double(args[6])!), y0: CGFloat(Double(args[7])!), x1: CGFloat(Double(args[8])!), y1: CGFloat(Double(args[9])!))
let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: W, pixelsHigh: H, bitsPerSample: 8, samplesPerPixel: 4,
                           hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: W * 4, bitsPerPixel: 32)!
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
let ctx = NSGraphicsContext.current!.cgContext
var font: CTFont
if fontName == "rounded" {
  let base = NSFont.systemFont(ofSize: 100, weight: .black)
  font = NSFont(descriptor: base.fontDescriptor.withDesign(.rounded)!, size: 100)! as CTFont
} else { font = CTFontCreateWithName(fontName as CFString, 100, nil) }
let attr = NSAttributedString(string: text, attributes: [.font: font, .kern: -2])
let line = CTLineCreateWithAttributedString(attr)
let bounds = CTLineGetBoundsWithOptions(line, .useGlyphPathBounds)
// fit: ascender top at y=H-2 .. baseline such that descender-less text fills height
let bw = box.x1 - box.x0, bh = box.y1 - box.y0
let scale = min(bw / bounds.width, bh / bounds.height)
// CoreGraphics y is up: the box's bottom edge is H - y1
// left-aligned (the text follows the logo's icon), centered vertically
ctx.translateBy(x: box.x0 - bounds.minX * scale,
                y: CGFloat(H) - box.y1 + (bh - bounds.height * scale) / 2 - bounds.minY * scale)
ctx.scaleBy(x: scale, y: scale)
ctx.setFillColor(NSColor.white.cgColor)
ctx.textPosition = .zero
CTLineDraw(line, ctx)
NSGraphicsContext.restoreGraphicsState()
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: out))
print("scale", scale)
let raw = Data(bytes: rep.bitmapData!, count: rep.bytesPerRow * H)
try! raw.write(to: URL(fileURLWithPath: out + ".raw"))
print("bpr", rep.bytesPerRow)
