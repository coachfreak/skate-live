import AppKit
import SwiftUI

// Renders the README header: neon "skate.live" on a speckled slate card in a paper frame,
// like the app icon. Usage: swift tools/make_wordmark.swift docs/wordmark.png
let glow = Color(red: 0.64, green: 0.87, blue: 1.0)
let cyan = Color(red: 0.20, green: 0.60, blue: 1.0)
let slate = Color(red: 0.11, green: 0.14, blue: 0.17)
let paper = Color(red: 0.83, green: 0.78, blue: 0.65)

struct Speckles: View {
    let count: Int
    let color: Color
    var body: some View {
        Canvas { context, size in
            var seed: UInt64 = 0x5EED
            func next() -> Double {
                seed = seed &* 6364136223846793005 &+ 1442695040888963407
                return Double(seed >> 33) / Double(1 << 31)
            }
            for _ in 0..<count {
                let r = 0.8 + next() * 1.8
                let rect = CGRect(x: next() * size.width, y: next() * size.height, width: r, height: r)
                context.fill(Path(ellipseIn: rect), with: .color(color.opacity(0.25 + next() * 0.5)))
            }
        }
    }
}

struct Wordmark: View {
    var body: some View {
        VStack(spacing: 18) {
            ZStack {
                ForEach([28.0, 12.0, 4.0], id: \.self) { radius in
                    Text("skate.live").foregroundStyle(cyan).blur(radius: radius)
                }
                Text("skate.live").foregroundStyle(glow)
            }
            .font(.custom("Arial Rounded MT Bold", size: 132))
            Text("SKATE 3 RECOMP · ONLINE ON MAC")
                .font(.custom("Futura-Medium", size: 22))
                .tracking(6)
                .foregroundStyle(slate)
                .padding(.horizontal, 18)
                .padding(.vertical, 7)
                .background(paper, in: RoundedRectangle(cornerRadius: 6, style: .continuous))
        }
        .padding(.horizontal, 90)
        .padding(.vertical, 56)
        .background(Speckles(count: 900, color: .white))
        .background(slate, in: RoundedRectangle(cornerRadius: 26, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 26, style: .continuous).strokeBorder(glow.opacity(0.35), lineWidth: 2))
        .padding(18)
        .background(Speckles(count: 1400, color: Color(red: 0.42, green: 0.36, blue: 0.27)))
        .background(paper, in: RoundedRectangle(cornerRadius: 38, style: .continuous))
        .padding(20)
    }
}

MainActor.assumeIsolated {
    let renderer = ImageRenderer(content: Wordmark())
    renderer.scale = 2
    guard let image = renderer.nsImage, let tiff = image.tiffRepresentation,
          let png = NSBitmapImageRep(data: tiff)?.representation(using: .png, properties: [:]) else { exit(1) }
    try! png.write(to: URL(fileURLWithPath: CommandLine.arguments[1]))
}
