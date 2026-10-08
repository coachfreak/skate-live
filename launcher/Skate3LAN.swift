// Skate 3 LAN launcher: starts the official skate3recomp app with the LAN
// library injected (DYLD_INSERT_LIBRARIES) and, on the host's Mac, the Skate 3
// LAN server (bundled skate3_lan_server.py) that stands in for EA's servers.
// Styled like the game's menus (launcher/SkateStyle.swift).
import AppKit
import SwiftUI

let supportDir = FileManager.default.homeDirectoryForCurrentUser
  .appendingPathComponent("Library/Application Support/Skate3 LAN")
let gameLogURL = supportDir.appendingPathComponent("lan.log")
let serverLogURL = supportDir.appendingPathComponent("server.log")

@main
struct Skate3LANApp: App {
  @NSApplicationDelegateAdaptor(AppDelegate.self) var delegate

  var body: some Scene {
    Window("Skate.live", id: "main") {
      LANView(lan: delegate.lan)
        .frame(width: 520)
        .fixedSize()
        .preferredColorScheme(.dark)
    }
    .windowStyle(.hiddenTitleBar)
    .windowResizability(.contentSize)
  }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
  @MainActor let lan = LAN()
  func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
  func applicationWillTerminate(_ notification: Notification) {
    MainActor.assumeIsolated { lan.stopServer() }
  }
}

// MARK: - Model

@MainActor
final class LAN: ObservableObject {
  @Published var gameURL: URL? = LAN.findGame()
  @Published var hosting = UserDefaults.standard.object(forKey: "hosting") as? Bool ?? true {
    didSet { UserDefaults.standard.set(hosting, forKey: "hosting") }
  }
  @Published var hostIP = UserDefaults.standard.string(forKey: "hostIP") ?? "" {
    didSet { UserDefaults.standard.set(hostIP, forKey: "hostIP") }
  }
  // Xbox gamertags: up to 15 letters, digits and spaces.
  @Published var name = UserDefaults.standard.string(forKey: "name") ?? NSFullUserName().components(separatedBy: " ").first ?? "Skater" {
    didSet {
      let clean = String(name.filter { $0.isLetter || $0.isNumber || $0 == " " }.prefix(15))
      if clean != name { name = clean }
      UserDefaults.standard.set(name, forKey: "name")
    }
  }
  // Join: the host's server found on the LAN (its beacon), or an address typed in by hand.
  @Published var manualHost = UserDefaults.standard.bool(forKey: "manualHost") {
    didSet { UserDefaults.standard.set(manualHost, forKey: "manualHost") }
  }
  @Published var foundHost: (ip: String, name: String)?
  private var foundAt = Date.distantPast
  private var helloAt = Date.distantPast
  @Published var serverLines: [String] = []
  @Published var serverRunning = false
  @Published var gameRunning = false
  @Published var message: String?

  let myIP = LAN.lanAddress() ?? "127.0.0.1"
  // This player's own online XUID, made once: default recomp profiles all share one.
  let xuid: String = {
    if let saved = UserDefaults.standard.string(forKey: "xuid") { return saved }
    let made = String(format: "0009%012llX", UInt64.random(in: 1..<(1 << 48)))
    UserDefaults.standard.set(made, forKey: "xuid")
    return made
  }()
  private var server: Process?

  init() {
    if let path = UserDefaults.standard.string(forKey: "gamePath") { gameURL = URL(fileURLWithPath: path) }
    listenForHosts()
    Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { _ in
      Task { @MainActor in
        if self.foundHost != nil && Date().timeIntervalSince(self.foundAt) > 4 { self.foundHost = nil }
      }
    }
  }

  // MARK: discovery
  // The server broadcasts "SKATE3LAN 1 <ip> <host name>" to UDP 42128 every second. A joining
  // launcher answers "SKATE3LAN-HELLO <xuid> <name>" to UDP 42129, which puts this player in
  // everyone's in-game friend list (the server remembers them).

  private nonisolated func listenForHosts() {
    let fd = socket(AF_INET, SOCK_DGRAM, 0)
    guard fd >= 0 else { return }
    var one: Int32 = 1
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, socklen_t(MemoryLayout<Int32>.size))
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, socklen_t(MemoryLayout<Int32>.size))
    var addr = sockaddr_in()
    addr.sin_family = sa_family_t(AF_INET)
    addr.sin_port = in_port_t(42128).bigEndian
    addr.sin_addr.s_addr = INADDR_ANY
    let bound = withUnsafePointer(to: &addr) {
      $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) }
    }
    guard bound == 0 else {
      close(fd)
      return
    }
    Thread.detachNewThread {
      var buffer = [UInt8](repeating: 0, count: 256)
      while true {
        let n = recv(fd, &buffer, buffer.count, 0)
        guard n > 0 else { continue }
        let parts = String(decoding: buffer[0..<n], as: UTF8.self).split(separator: " ", maxSplits: 3).map(String.init)
        guard parts.count >= 3, parts[0] == "SKATE3LAN", inet_addr(parts[2]) != INADDR_NONE else { continue }
        let ip = parts[2], host = parts.count > 3 ? parts[3] : "Host"
        Task { @MainActor in self.hostSeen(ip: ip, name: host) }
      }
    }
  }

  private func hostSeen(ip: String, name host: String) {
    foundAt = Date()
    if foundHost?.ip != ip || foundHost?.name != host { foundHost = (ip, host) }
    guard !hosting, Date().timeIntervalSince(helloAt) > 5 else { return }
    helloAt = Date()
    let hello = "SKATE3LAN-HELLO \(xuid) \(name.isEmpty ? "Skater" : name)"
    let fd = socket(AF_INET, SOCK_DGRAM, 0)
    guard fd >= 0 else { return }
    var to = sockaddr_in()
    to.sin_family = sa_family_t(AF_INET)
    to.sin_port = in_port_t(42129).bigEndian
    to.sin_addr.s_addr = inet_addr(ip)
    _ = hello.withCString { text in
      withUnsafePointer(to: &to) {
        $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
          sendto(fd, text, strlen(text), 0, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
        }
      }
    }
    close(fd)
  }

  static func findGame() -> URL? {
    let home = FileManager.default.homeDirectoryForCurrentUser.path
    return ["/Applications/skate3recomp.app", "\(home)/Applications/skate3recomp.app",
            "\(home)/Downloads/Skate3Recomp-macOS/skate3recomp.app"]
      .map { URL(fileURLWithPath: $0) }
      .first { FileManager.default.fileExists(atPath: $0.path) }
  }

  // The first active IPv4 address on a real interface (en0 preferred).
  static func lanAddress() -> String? {
    var list: UnsafeMutablePointer<ifaddrs>?
    guard getifaddrs(&list) == 0 else { return nil }
    defer { freeifaddrs(list) }
    var found: String?
    for ptr in sequence(first: list, next: { $0?.pointee.ifa_next }) {
      guard let ifa = ptr?.pointee, let addr = ifa.ifa_addr, addr.pointee.sa_family == UInt8(AF_INET),
            ifa.ifa_flags & UInt32(IFF_UP) != 0, ifa.ifa_flags & UInt32(IFF_LOOPBACK) == 0 else { continue }
      var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
      getnameinfo(addr, socklen_t(addr.pointee.sa_len), &host, socklen_t(host.count), nil, 0, NI_NUMERICHOST)
      found = String(cString: host)
      if String(cString: ifa.ifa_name) == "en0" { break }
    }
    return found
  }

  var serverAddress: String {
    if hosting { return myIP }
    return manualHost ? hostIP.trimmingCharacters(in: .whitespaces) : foundHost?.ip ?? ""
  }

  var gameReady: Bool {
    guard let app = gameURL else { return false }
    return FileManager.default.isExecutableFile(atPath: app.appendingPathComponent("Contents/MacOS/skate3").path)
  }

  // MARK: server

  func startServer() {
    guard server == nil, let script = Bundle.main.path(forResource: "skate3_lan_server", ofType: "py") else { return }
    guard let python = ["/opt/homebrew/bin/python3", "/usr/local/bin/python3", "/usr/bin/python3"]
      .first(where: { FileManager.default.isExecutableFile(atPath: $0) })
    else {
      message = "Hosting needs Python 3. Install the Xcode Command Line Tools: xcode-select --install"
      return
    }
    // A server orphaned by a crashed or force-quit launcher still holds port 42127.
    let pkill = Process()
    pkill.executableURL = URL(fileURLWithPath: "/usr/bin/pkill")
    pkill.arguments = ["-f", "skate3_lan_server.py"]
    try? pkill.run()
    pkill.waitUntilExit()
    if pkill.terminationStatus == 0 { Thread.sleep(forTimeInterval: 0.3) }  // let it release the port
    try? FileManager.default.createDirectory(at: supportDir, withIntermediateDirectories: true)
    try? FileManager.default.removeItem(at: serverLogURL)
    let process = Process()
    process.executableURL = URL(fileURLWithPath: python)
    process.arguments = ["-u", script, "--ip", myIP, "--log", serverLogURL.path, "--name", name,
                         "--players", supportDir.appendingPathComponent("players.json").path]
    let pipe = Pipe()
    process.standardOutput = pipe
    process.standardError = pipe
    pipe.fileHandleForReading.readabilityHandler = { handle in
      let text = String(decoding: handle.availableData, as: UTF8.self)
      // Only the one-line events: requests are logged in full in server.log.
      let lines = text.split(separator: "\n").map(String.init).filter { !$0.hasPrefix(" ") }
      guard !lines.isEmpty else { return }
      Task { @MainActor in self.serverLines = Array((self.serverLines + lines).suffix(300)) }
    }
    process.terminationHandler = { _ in
      Task { @MainActor in
        self.server = nil
        self.serverRunning = false
      }
    }
    do {
      try process.run()
      server = process
      serverRunning = true
      serverLines = []
    } catch {
      message = "Couldn't start the server: \(error.localizedDescription)"
    }
  }

  func stopServer() {
    server?.terminate()
    server = nil
    serverRunning = false
  }

  // MARK: game

  func launch() {
    guard let app = gameURL, let bundled = Bundle.main.url(forResource: "libskate3lan", withExtension: "dylib") else {
      return
    }
    message = nil
    if serverAddress.isEmpty {
      message = manualHost ? "Enter the host's address first." : "No host found yet."
      return
    }
    if hosting { startServer() }
    do {
      // A private copy outside this app's bundle, so a quarantined download
      // (or a read-only translocated app) can't block dyld from loading it.
      try FileManager.default.createDirectory(at: supportDir, withIntermediateDirectories: true)
      let dylib = supportDir.appendingPathComponent("libskate3lan.dylib")
      try? FileManager.default.removeItem(at: dylib)
      try FileManager.default.copyItem(at: bundled, to: dylib)
      removexattr(dylib.path, "com.apple.quarantine", 0)

      let process = Process()
      process.executableURL = app.appendingPathComponent("Contents/MacOS/skate3")
      process.currentDirectoryURL = app.deletingLastPathComponent()
      var env = ProcessInfo.processInfo.environment
      env["DYLD_INSERT_LIBRARIES"] = dylib.path
      env["SKATE3_LAN_SERVER"] = serverAddress
      env["SKATE3_LAN_NAME"] = name.isEmpty ? "Skater" : name
      env["SKATE3_LAN_XUID"] = xuid
      env["SKATE3_LAN_ROLE"] = hosting ? "Host" : "Client"  // the game window's title
      env["SKATE3_LAN_ICON"] = Bundle.main.path(forResource: "AppIcon", ofType: "icns")  // Dock icon
      process.environment = env
      process.terminationHandler = { _ in Task { @MainActor in self.gameRunning = false } }
      try process.run()
      gameRunning = true
      DispatchQueue.main.asyncAfter(deadline: .now() + 2) {
        NSRunningApplication(processIdentifier: process.processIdentifier)?.activate()
      }
    } catch {
      message = "Couldn't start the game: \(error.localizedDescription)"
    }
  }

  func chooseGame() {
    let panel = NSOpenPanel()
    panel.message = "Choose skate3recomp.app"
    panel.allowedContentTypes = [.application]
    panel.directoryURL = URL(fileURLWithPath: "/Applications")
    if panel.runModal() == .OK, let url = panel.url {
      gameURL = url
      UserDefaults.standard.set(url.path, forKey: "gamePath")
    }
  }

  func copyAddress() {
    NSPasteboard.general.clearContents()
    NSPasteboard.general.setString(myIP, forType: .string)
    message = nil
  }
}

// MARK: - Views
// Styled after the game's own menus (launcher/SkateStyle.swift): the crossbar's worn
// icon tiles, the Game Settings card (paper frame, title, dark slate), lit items glow.

// A crossbar option tile: dark slate square with a worn light edge, glyph inside.
struct SkateTile<Glyph: View>: View {
  var size: CGFloat = 46
  var lit = false
  @ViewBuilder let glyph: Glyph

  var body: some View {
    glyph
      .frame(width: size, height: size)
      .background(Color(red: 0.16, green: 0.22, blue: 0.255))
      .overlay(Grain(color: .white, density: 2.2, seed: 21).opacity(0.35))
      .overlay(alignment: .topTrailing) {
        // the worn light corner of the game's tiles
        LinearGradient(colors: [Skate.cream.opacity(0.55), .clear], startPoint: .topTrailing, endPoint: .center)
          .frame(width: size * 0.7, height: size * 0.7)
          .mask(RoughRect(seed: 5).stroke(lineWidth: 2.5))
      }
      .clipShape(RoughRect(seed: 9))
      .overlay(RoughRect(seed: 9).stroke(lit ? Skate.cyan : Skate.cream.opacity(0.18), lineWidth: lit ? 1.5 : 1)
        .shadow(color: lit ? Skate.cyan : .clear, radius: 6))
  }
}

// The game's menu item: lit = glowing light blue, otherwise muted steel.
struct SkateItemText: View {
  let text: String
  var size: CGFloat = 21
  var lit = true
  var body: some View {
    Text(text)
      .font(Skate.text(size))
      .foregroundStyle(lit ? Skate.glow : Skate.steel)
      .shadow(color: lit ? Skate.cyan.opacity(0.85) : .clear, radius: 7)
      .lineLimit(1)
      .minimumScaleFactor(0.75)
  }
}

struct LANView: View {
  @ObservedObject var lan: LAN
  @FocusState private var nameFocused: Bool
  @FocusState private var hostFocused: Bool
  @State private var copied = false
  @State private var showServerLog = false

  var body: some View {
    VStack(alignment: .leading, spacing: 18) {
      modeBar
      panel
      if lan.hosting && (lan.serverRunning || !lan.serverLines.isEmpty) { activityPanel }
      footer
    }
    .padding(.horizontal, 26)
    .padding(.top, 18)
    .padding(.bottom, 20)
    .background(SkateBackdrop())
    .onAppear {
      DispatchQueue.main.async { endTyping() }  // the window would focus the gamertag field first
      // Return launches, left / right switch Host / Join, Esc quits (or ends typing).
      NSEvent.addLocalMonitorForEvents(matching: .keyDown) { event in
        guard NSApp.modalWindow == nil else { return event }
        let typing = nameFocused || hostFocused
        switch event.keyCode {
        case 36, 76:
          if typing { endTyping() } else if canLaunch { lan.launch() }
        case 53:
          if typing { endTyping() } else { NSApp.terminate(nil) }
        case 123 where !typing && !lan.gameRunning: lan.hosting = true
        case 124 where !typing && !lan.gameRunning: lan.hosting = false
        default: return event
        }
        return nil
      }
    }
  }

  private func endTyping() {
    nameFocused = false
    hostFocused = false
  }

  private var canLaunch: Bool { lan.gameReady && !lan.gameRunning && !lan.serverAddress.isEmpty }

  // MARK: crossbar

  private var modeBar: some View {
    HStack(spacing: 12) {
      modeButton(host: true, symbol: "antenna.radiowaves.left.and.right", title: "Host", subtitle: "Run the server")
      modeButton(host: false, symbol: "figure.skateboarding", title: "Join", subtitle: "Skate on a friend's server")
    }
    .disabled(lan.gameRunning)
  }

  private func modeButton(host: Bool, symbol: String, title: String, subtitle: String) -> some View {
    let on = lan.hosting == host
    return Button { withAnimation(.easeOut(duration: 0.15)) { lan.hosting = host } } label: {
      HStack(spacing: 12) {
        SkateTile(lit: on) {
          Image(systemName: symbol)
            .font(.system(size: 19, weight: .bold))
            .foregroundStyle(on ? Skate.glow : Skate.cream.opacity(0.6))
            .shadow(color: on ? Skate.cyan : .clear, radius: 5)
        }
        VStack(alignment: .leading, spacing: 1) {
          SkateItemText(text: title, size: 22, lit: on)
          Text(subtitle)
            .font(Skate.text(12.5))
            .foregroundStyle(on ? Skate.cyan : Skate.steel.opacity(0.7))
            .lineLimit(1)
            .minimumScaleFactor(0.8)
        }
        Spacer(minLength: 0)
      }
      .frame(maxWidth: .infinity)
      .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
  }

  // MARK: the card

  private var panel: some View {
    VStack(alignment: .leading, spacing: 0) {
      // Title like "Game Settings" at the top of the game's cards.
      Text(lan.hosting ? "Your Server" : "Find a Host")
        .font(Skate.title(24))
        .foregroundStyle(Skate.cream)
        .worn()
        .padding(.horizontal, 22)
        .padding(.top, 16)
        .padding(.bottom, 12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .overlay(alignment: .bottom) { SkateRule() }
      VStack(alignment: .leading, spacing: 16) {
        statusBlock
        SkateRule()
        SkateRow(label: "Gamertag") {
          SkateField(text: $lan.name, placeholder: "Skater", focus: $nameFocused)
        }
        SkateBigButton(title: lan.gameRunning ? "Skate 3 Is Running" : "Launch Skate 3", enabled: canLaunch,
                       action: lan.launch)
          .padding(.top, 2)
        if let message = lan.message {
          Text(message).font(Skate.text(14)).foregroundStyle(Skate.error)
        } else if !lan.gameReady {
          Text("skate3recomp.app not found: choose it below.").font(Skate.text(14)).foregroundStyle(Skate.warn)
        }
      }
      .padding(.horizontal, 22)
      .padding(.vertical, 18)
    }
    .skatePanel()
  }

  // Server state, lit like the selected item when there is a server to play on.
  @ViewBuilder private var statusBlock: some View {
    VStack(alignment: .leading, spacing: 10) {
      HStack(alignment: .center, spacing: 12) {
        VStack(alignment: .leading, spacing: 3) {
          SkateItemText(text: statusTitle, size: 25, lit: statusOn)
          Text(statusDetail)
            .font(Skate.text(14))
            .foregroundStyle(Skate.cream.opacity(0.6))
            .lineLimit(1)
            .textSelection(.enabled)
        }
        Spacer(minLength: 8)
        if lan.hosting {
          SkateSmallButton(title: copied ? "Copied" : "Copy IP") {
            lan.copyAddress()
            copied = true
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { copied = false }
          }
          SkateSmallButton(title: lan.serverRunning ? "Stop" : "Start") {
            lan.serverRunning ? lan.stopServer() : lan.startServer()
          }
        } else {
          SkateSmallButton(title: lan.manualHost ? "Auto" : "Enter IP") { lan.manualHost.toggle() }
        }
      }
      if !lan.hosting && lan.manualHost {
        SkateRow(label: "Address") {
          SkateField(text: $lan.hostIP, placeholder: "192.168.1.20", focus: $hostFocused)
        }
      }
    }
  }

  private var statusOn: Bool {
    lan.hosting ? lan.serverRunning : !lan.serverAddress.isEmpty
  }

  private var statusTitle: String {
    if lan.hosting { return lan.serverRunning ? "Server Online" : "Server Offline" }
    if lan.manualHost { return lan.serverAddress.isEmpty ? "Enter the Host's IP" : "Host Set" }
    return lan.foundHost.map { "\($0.name)'s Server" } ?? "Searching…"
  }

  private var statusDetail: String {
    if lan.hosting { return lan.serverRunning ? lan.myIP : "Starts with the game" }
    if lan.serverAddress.isEmpty {
      return lan.manualHost ? "Shown in the host's Skate.live window" : "The host's server has to be running"
    }
    return lan.serverAddress
  }

  // MARK: server activity

  private var activityPanel: some View {
    VStack(alignment: .leading, spacing: 10) {
      Button { withAnimation(.easeOut(duration: 0.15)) { showServerLog.toggle() } } label: {
        HStack(spacing: 8) {
          SkateStatusLight(on: lan.serverRunning)
          Text("Server Activity").font(Skate.text(16)).foregroundStyle(Skate.cream.opacity(0.85))
          Spacer()
          Text(showServerLog ? "Hide" : "Show").font(Skate.text(13)).foregroundStyle(Skate.steel)
        }
        .contentShape(Rectangle())
      }
      .buttonStyle(.plain)
      if showServerLog {
        ScrollViewReader { proxy in
          ScrollView {
            LazyVStack(alignment: .leading, spacing: 1) {
              ForEach(Array(lan.serverLines.enumerated()), id: \.offset) { i, line in
                Text(line)
                  .font(.system(size: 10.5, design: .monospaced))
                  .foregroundStyle(Skate.cream.opacity(0.7))
                  .textSelection(.enabled)
                  .id(i)
              }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
          }
          .frame(height: 120)
          .onChange(of: lan.serverLines.count) { _, count in proxy.scrollTo(count - 1, anchor: .bottom) }
        }
      }
    }
    .padding(.horizontal, 22)
    .padding(.vertical, 12)
    .skatePanel()
  }

  private var footer: some View {
    HStack(spacing: 18) {
      SkateLink(title: lan.gameReady ? "skate3recomp ✓" : "Choose skate3recomp…",
                color: lan.gameReady ? Skate.steel : Skate.warn, action: lan.chooseGame)
        .help((lan.gameURL?.path as NSString?)?.abbreviatingWithTildeInPath ?? "")
      Spacer()
      SkateLink(title: "Game Log", color: Skate.steel) { NSWorkspace.shared.open(gameLogURL) }
        .disabled(!FileManager.default.fileExists(atPath: gameLogURL.path))
      SkateLink(title: "Server Log", color: Skate.steel) { NSWorkspace.shared.open(serverLogURL) }
        .disabled(!FileManager.default.fileExists(atPath: serverLogURL.path))
    }
    .padding(.horizontal, 6)
  }
}
