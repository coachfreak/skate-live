// The look of Skate 3's own menus for the Skate.live launcher:
// cream rounded titles, a paper card around a dark slate
// panel with a torn edge, Futura rows in steel blue, the selected one glowing cyan.
import AppKit
import SwiftUI

enum Skate {
  static let backdrop = Color(red: 0.05, green: 0.045, blue: 0.04)
  static let haze = Color(red: 0.17, green: 0.155, blue: 0.135)
  static let cream = Color(red: 0.85, green: 0.82, blue: 0.74)
  static let paper = Color(red: 0.83, green: 0.78, blue: 0.65)
  static let fleck = Color(red: 0.42, green: 0.36, blue: 0.27)
  static let slate = Color(red: 0.11, green: 0.14, blue: 0.17)
  static let steel = Color(red: 0.30, green: 0.43, blue: 0.57)
  static let glow = Color(red: 0.64, green: 0.87, blue: 1.0)
  static let cyan = Color(red: 0.20, green: 0.60, blue: 1.0)
  static let off = Color(white: 0.42)
  static let warn = Color(red: 0.93, green: 0.62, blue: 0.22)
  static let error = Color(red: 0.90, green: 0.33, blue: 0.27)

  static func title(_ size: CGFloat) -> Font { .custom("Arial Rounded MT Bold", size: size) }
  static func text(_ size: CGFloat) -> Font { .custom("Futura-Medium", size: size) }
}

// MARK: - Menu rows

struct MenuItem {
  let label: String
  var value: Text?
  var enabled = true
  let help: Text
  let action: () -> Void
}

struct MenuRow: View {
  let item: MenuItem
  let selected: Bool

  var body: some View {
    let lit = selected && item.enabled
    HStack(alignment: .firstTextBaseline) {
      Text(item.label)
        .font(Skate.text(25))
        .foregroundStyle(!item.enabled ? (selected ? Color(white: 0.62) : Skate.off) : lit ? Skate.glow : Skate.steel)
        .shadow(color: lit ? Skate.cyan : .clear, radius: 6)
        .background {
          if lit {
            LinearGradient(colors: [Skate.cyan.opacity(0.35), Skate.cyan.opacity(0.08)], startPoint: .top,
                           endPoint: .bottom)
              .padding(-6)
              .blur(radius: 9)
          }
        }
      Spacer(minLength: 12)
      item.value?
        .font(Skate.text(17))
        .foregroundStyle(Skate.cream.opacity(0.85))
    }
    .padding(.horizontal, 24)
    .padding(.vertical, 5)
    .contentShape(Rectangle())
  }
}

// MARK: - Screen parts

// The pause menu's category header: title, fading underline, the icon.
struct SkateHeader: View {
  let icon: NSImage?
  let title: String
  let subtitle: String

  var body: some View {
    HStack(spacing: 16) {
      if let icon {
        Image(nsImage: icon)
          .resizable()
          .interpolation(.high)
          .frame(width: 62, height: 62)
          .shadow(color: Skate.cyan.opacity(0.45), radius: 10)
      }
      VStack(alignment: .leading, spacing: 3) {
        Text(title)
          .font(Skate.title(32))
          .lineLimit(1)
          .minimumScaleFactor(0.7)
          .foregroundStyle(Skate.cream)
          .worn()
        Text(subtitle)
          .font(Skate.text(14))
          .foregroundStyle(Skate.steel)
      }
    }
    .padding(.bottom, 12)
    .frame(maxWidth: .infinity, alignment: .leading)
    .overlay(alignment: .bottom) {
      LinearGradient(colors: [Skate.cream.opacity(0.8), Skate.cream.opacity(0)], startPoint: .leading,
                     endPoint: .trailing)
        .frame(height: 2)
    }
  }
}

// The settings screen: paper card, dark panel, rows, help for the selected row.
struct SkateMenuPanel: View {
  let items: [MenuItem]
  @Binding var selected: Int
  var highlighted = false
  let activate: (Int) -> Void

  var body: some View {
    VStack(alignment: .leading, spacing: 0) {
      VStack(alignment: .leading, spacing: 0) {
        ForEach(items.indices, id: \.self) { i in
          Button { activate(i) } label: { MenuRow(item: items[i], selected: i == selected) }
            .buttonStyle(.plain)
            .onHover { if $0 { selected = i } }
        }
      }
      .padding(.top, 16)
      .padding(.bottom, 12)

      items[min(selected, items.count - 1)].help
        .font(Skate.text(14))
        .foregroundStyle(Skate.cream.opacity(0.75))
        .lineSpacing(2)
        .fixedSize(horizontal: false, vertical: true)
        .frame(maxWidth: .infinity, minHeight: 62, alignment: .topLeading)
        .padding(.horizontal, 24)
        .padding(.top, 12)
        .padding(.bottom, 16)
        .overlay(alignment: .top) { Skate.cream.opacity(0.12).frame(height: 1).padding(.horizontal, 24) }
    }
    .skatePanel(highlighted: highlighted)
  }
}

// The blurred, dimmed game behind the menu.
struct SkateBackdrop: View {
  var body: some View {
    ZStack {
      Skate.backdrop
      RadialGradient(colors: [Skate.haze, .clear], center: UnitPoint(x: 0.75, y: 0.2), startRadius: 0,
                     endRadius: 460)
      RadialGradient(colors: [Skate.cyan.opacity(0.10), .clear], center: UnitPoint(x: 0.9, y: 1), startRadius: 0,
                     endRadius: 320)
      Grain(color: .white, density: 0.8, seed: 9).opacity(0.35)
    }
    .ignoresSafeArea()
  }
}

extension View {
  // A dark slate panel with a torn edge, set in a speckled paper card.
  func skatePanel(highlighted: Bool = false) -> some View {
    background(Skate.slate)
      .overlay(Grain(color: .white, density: 1.2, seed: 11).opacity(0.25))
      .overlay(RoughRect().stroke(.black.opacity(0.55), lineWidth: 4).blur(radius: 2))
      .clipShape(RoughRect())
      .overlay(RoughRect().stroke(Skate.cyan, lineWidth: 2).shadow(color: Skate.cyan, radius: 8)
        .opacity(highlighted ? 1 : 0))
      .padding(14)
      .background(Skate.paper)
      .overlay(Grain(color: Skate.fleck, density: 7, seed: 2))
      .clipShape(RoundedRectangle(cornerRadius: 5))
      .shadow(color: .black.opacity(0.7), radius: 24, y: 10)
  }
}

// MARK: - Textures

struct SeededRandom: RandomNumberGenerator {
  var state: UInt64

  mutating func next() -> UInt64 {  // SplitMix64
    state &+= 0x9E37_79B9_7F4A_7C15
    var z = state
    z = (z ^ (z >> 30)) &* 0xBF58_476D_1CE4_E5B9
    z = (z ^ (z >> 27)) &* 0x94D0_49BB_1331_11EB
    return z ^ (z >> 31)
  }
}

// Printed-paper specks, the same on every redraw.
struct Grain: View, Equatable {
  let color: Color
  let density: Double  // specks per 100 pt²
  var seed: UInt64 = 1

  var body: some View {
    Canvas { ctx, size in
      var rng = SeededRandom(state: seed)
      for _ in 0..<Int(size.width * size.height / 100 * density) {
        let d = CGFloat.random(in: 0.4...1.4, using: &rng)
        let x = CGFloat.random(in: 0...size.width, using: &rng), y = CGFloat.random(in: 0...size.height, using: &rng)
        ctx.fill(Path(ellipseIn: CGRect(x: x, y: y, width: d, height: d)),
                 with: .color(color.opacity(.random(in: 0.15...0.6, using: &rng))))
      }
    }
    .allowsHitTesting(false)
  }
}

// A rectangle with a torn, brushed edge, like the menu's dark panels.
struct RoughRect: Shape {
  var seed: UInt64 = 7

  func path(in rect: CGRect) -> Path {
    var rng = SeededRandom(state: seed)
    let r = rect.insetBy(dx: 2, dy: 2)
    let corners = [CGPoint(x: r.minX, y: r.minY), CGPoint(x: r.maxX, y: r.minY), CGPoint(x: r.maxX, y: r.maxY),
                   CGPoint(x: r.minX, y: r.maxY)]
    var path = Path()
    for c in 0..<4 {
      let a = corners[c], b = corners[(c + 1) % 4]
      let steps = max(Int(hypot(b.x - a.x, b.y - a.y) / 6), 1)
      for s in 0..<steps {
        let t = CGFloat(s) / CGFloat(steps), j = CGFloat.random(in: -1.1...1.1, using: &rng)
        let p = CGPoint(x: a.x + (b.x - a.x) * t + (a.y == b.y ? 0 : j), y: a.y + (b.y - a.y) * t + (a.y == b.y ? j : 0))
        if path.isEmpty { path.move(to: p) } else { path.addLine(to: p) }
      }
    }
    path.closeSubpath()
    return path
  }
}

extension View {
  // Worn print: dark specks through the shape, like the menu's titles.
  func worn(seed: UInt64 = 3) -> some View {
    overlay(Grain(color: .black, density: 14, seed: seed).mask(self))
  }
}

// MARK: - Controls (Skate 3 LAN)

// The online map's tab bar: dark tabs, the selected one lit.
struct SkateTabs: View {
  let tabs: [String]
  @Binding var selection: Int

  var body: some View {
    HStack(spacing: 6) {
      ForEach(tabs.indices, id: \.self) { i in
        let on = i == selection
        Button { selection = i } label: {
          Text(tabs[i])
            .font(Skate.text(19))
            .foregroundStyle(on ? Skate.glow : Skate.steel)
            .shadow(color: on ? Skate.cyan.opacity(0.8) : .clear, radius: 6)
            .frame(maxWidth: .infinity)
            .padding(.vertical, 7)
            .background(on ? Skate.slate : Skate.slate.opacity(0.45))
            .overlay(alignment: .top) { Skate.cream.opacity(on ? 0.55 : 0.12).frame(height: 1) }
            .clipShape(UnevenRoundedRectangle(topLeadingRadius: 5, topTrailingRadius: 5))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
      }
    }
    .padding(.horizontal, 14)
  }
}

// "Team Options" style: small steel heading.
struct SkateSectionTitle: View {
  let text: String
  init(_ text: String) { self.text = text }
  var body: some View {
    Text(text).font(Skate.text(14)).foregroundStyle(Skate.steel).textCase(.uppercase).tracking(1.2)
  }
}

struct SkateRule: View {
  var body: some View { Skate.cream.opacity(0.12).frame(height: 1) }
}

// A label on the left, a control on the right.
struct SkateRow<Content: View>: View {
  let label: String
  @ViewBuilder let content: Content
  var body: some View {
    HStack(alignment: .firstTextBaseline) {
      Text(label).font(Skate.text(21)).foregroundStyle(Skate.cream.opacity(0.9))
      Spacer(minLength: 16)
      content
    }
  }
}

struct SkateField: View {
  @Binding var text: String
  let placeholder: String
  var focus: FocusState<Bool>.Binding

  var body: some View {
    TextField("", text: $text, prompt: Text(placeholder).foregroundColor(Skate.off))
      .focused(focus)
      .textFieldStyle(.plain)
      .multilineTextAlignment(.trailing)
      .font(Skate.text(19))
      .foregroundStyle(Skate.cream)
      .frame(width: 220)
      .padding(.vertical, 3)
      .overlay(alignment: .bottom) {
        (focus.wrappedValue ? Skate.cyan : Skate.cream.opacity(0.25)).frame(height: focus.wrappedValue ? 2 : 1)
          .shadow(color: focus.wrappedValue ? Skate.cyan : .clear, radius: 4)
      }
  }
}

// Off / On, like a settings value: ‹ On ›
struct SkateSwitch: View {
  @Binding var isOn: Bool
  @Environment(\.isEnabled) private var enabled
  var body: some View {
    Button { isOn.toggle() } label: {
      HStack(spacing: 10) {
        Text("‹").foregroundStyle(Skate.steel)
        Text(isOn ? "On" : "Off")
          .foregroundStyle(isOn ? Skate.glow : Skate.cream.opacity(0.8))
          .shadow(color: isOn ? Skate.cyan.opacity(0.7) : .clear, radius: 5)
          .frame(minWidth: 34)
        Text("›").foregroundStyle(Skate.steel)
      }
      .font(Skate.text(19))
      .opacity(enabled ? 1 : 0.5)
      .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
  }
}

struct SkateStatusLight: View {
  let on: Bool
  var color: Color = Skate.glow
  var size: CGFloat = 9
  var body: some View {
    Circle()
      .fill(on ? color : Skate.off)
      .frame(width: size, height: size)
      .shadow(color: on ? color.opacity(0.9) : .clear, radius: size * 0.6)
  }
}

struct SkateSmallButton: View {
  let title: String
  let action: () -> Void
  @State private var hover = false
  var body: some View {
    Button(action: action) {
      Text(title)
        .font(Skate.text(14))
        .foregroundStyle(hover ? Skate.glow : Skate.cream.opacity(0.85))
        .padding(.horizontal, 10)
        .padding(.vertical, 3)
        .overlay(RoundedRectangle(cornerRadius: 4).stroke(hover ? Skate.cyan : Skate.cream.opacity(0.3), lineWidth: 1))
        .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .onHover { hover = $0 }
  }
}

// The screen's main action.
struct SkateBigButton: View {
  let title: String
  let enabled: Bool
  let action: () -> Void
  @State private var hover = false

  var body: some View {
    Button(action: action) {
      Text(title)
        .font(Skate.text(25))
        .foregroundStyle(enabled ? (hover ? Color.white : Skate.glow) : Skate.off)
        .shadow(color: enabled ? Skate.cyan : .clear, radius: hover ? 10 : 6)
      .frame(maxWidth: .infinity)
      .padding(.vertical, 12)
      .background {
        if enabled {
          LinearGradient(colors: [Skate.cyan.opacity(hover ? 0.35 : 0.22), Skate.cyan.opacity(0.05)],
                         startPoint: .top, endPoint: .bottom)
        }
      }
      .overlay(RoughRect(seed: 4).stroke(enabled ? Skate.cyan.opacity(0.7) : Skate.off.opacity(0.4), lineWidth: 1.5))
      .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .disabled(!enabled)
    .onHover { hover = $0 }
  }
}

struct SkateLink: View {
  let title: String
  let color: Color
  let action: () -> Void
  @Environment(\.isEnabled) private var enabled
  var body: some View {
    Button(action: action) {
      Text(title).font(Skate.text(13)).foregroundStyle(enabled ? color : Skate.off.opacity(0.6))
    }
    .buttonStyle(.plain)
  }
}

extension Text {
  func skateHint() -> some View {
    font(Skate.text(13)).foregroundStyle(Skate.cream.opacity(0.55)).fixedSize(horizontal: false, vertical: true)
  }
}
