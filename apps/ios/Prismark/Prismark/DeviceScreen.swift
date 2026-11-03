import Combine
import SwiftUI

/// What this device is, how it is right now, and what the system will not tell an app.
struct DeviceScreen: View {
    @Environment(Session.self) private var session
    @Environment(\.horizontalSizeClass) private var sizeClass
    @State private var status = DeviceStatus.now()

    private let changes = NotificationCenter.default.publisher(for: ProcessInfo.thermalStateDidChangeNotification)
        .merge(with: NotificationCenter.default.publisher(for: .NSProcessInfoPowerStateDidChange),
               NotificationCenter.default.publisher(for: UIDevice.batteryLevelDidChangeNotification),
               NotificationCenter.default.publisher(for: UIDevice.batteryStateDidChangeNotification))
        .receive(on: RunLoop.main)

    var body: some View {
        let machine = Machine(info: session.info)

        Group {
            if sizeClass == .regular {
                padBody(machine)
            } else {
                phoneBody(machine)
            }
        }
        .background(Color.ground)
        .ignoresSafeArea(.container, edges: .vertical)
        .onAppear {
            UIDevice.current.isBatteryMonitoringEnabled = true
            status = .now()
        }
        .onDisappear { UIDevice.current.isBatteryMonitoringEnabled = false }
        .onReceive(changes) { _ in status = .now() }
    }

    private func phoneBody(_ machine: Machine) -> some View {
        GeometryReader { geo in
            VStack(spacing: 6) {
                DeviceHeader(machine: machine, chipSize: 84)
                    .padding(.top, 62)
                    .padding(.horizontal, 20)
                    .padding(.bottom, 10)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .background { PassDots() }
                StatusTiles(status: status, valueSize: 30, height: 114)
                CoreMap(machine: machine, cache: cache, fastSize: 50, efficientSize: 24, bandHeight: 46, efficientLabel: "EFF")
                VStack(alignment: .leading, spacing: 5) {
                    Mono(text: "Not readable").foregroundStyle(Color.muted)
                    Text("Temperature in degrees, clock speed, core placement and the cycle counter are closed to apps.")
                        .font(.archivo(12))
                        .foregroundStyle(Color.muted)
                        .fixedSize(horizontal: false, vertical: true)
                }
                .padding(.horizontal, 16)
                .padding(.vertical, 9)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(Color.raised)
                CompareLink(bottom: 34)
            }
            .frame(height: geo.size.height)
        }
    }

    private func padBody(_ machine: Machine) -> some View {
        GeometryReader { geo in
            Group {
                if geo.size.width > geo.size.height {
                    HStack(spacing: 6) {
                        CoreMap(machine: machine, cache: cache, fastSize: 110, efficientSize: 76, bandHeight: 72, efficientLabel: "EFFICIENT")
                            .frame(width: min(620, geo.size.width * 0.55))
                        VStack(spacing: 6) {
                            padHeader(machine, chipSize: 130)
                            StatusTiles(status: status, valueSize: 50, height: 150)
                            UnknownList()
                            CompareLink(bottom: 0)
                        }
                    }
                } else {
                    VStack(spacing: 6) {
                        padHeader(machine, chipSize: 92)
                        StatusTiles(status: status, valueSize: 50, height: 130)
                        CoreMap(machine: machine, cache: cache, fastSize: 80, efficientSize: 56, bandHeight: 64, efficientLabel: "EFFICIENT")
                        UnknownList().frame(height: 240)
                        CompareLink(bottom: 0)
                    }
                }
            }
            .padding(.top, 30)
            .padding(.bottom, 14)
            .background(alignment: .top) {
                // Behind the status bar, whose dark icons don't show on the ground.
                Color.panel.frame(height: 24)
            }
        }
    }

    private func padHeader(_ machine: Machine, chipSize: CGFloat) -> some View {
        DeviceHeader(machine: machine, chipSize: chipSize)
            .padding(.horizontal, 20)
            .padding(.vertical, 16)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background { PassDots() }
    }

    private var cache: String? {
        let cpu = (session.info["machine"] as? Doc)?["cpu"] as? Doc
        guard let bytes = (cpu?["llc_bytes"] as? NSNumber)?.doubleValue, bytes > 0 else { return nil }
        return "\(Int((bytes / 1_048_576).rounded())) MB"
    }
}

/// How the device is right now. The screen reads this only when the system says it changed.
struct DeviceStatus {
    let heat: String
    let hot: Bool
    let lowPower: Bool
    let battery: String
    let batteryNote: String

    static func now() -> DeviceStatus {
        let thermal = ProcessInfo.processInfo.thermalState
        let heat = switch thermal {
        case .nominal: "Nominal"
        case .fair: "Fair"
        case .serious: "Serious"
        case .critical: "Critical"
        @unknown default: "Unknown"
        }
        let level = UIDevice.current.batteryLevel
        // iOS gives apps the level in steps of 5 %, so it is shown as approximate.
        let note = switch UIDevice.current.batteryState {
        case .charging: "Charging, to the nearest 5 %"
        case .full: "Full, plugged in"
        case .unplugged: "On battery, to the nearest 5 %"
        default: "Not reported"
        }
        return DeviceStatus(
            heat: heat,
            hot: thermal == .serious || thermal == .critical,
            lowPower: ProcessInfo.processInfo.isLowPowerModeEnabled,
            battery: level < 0 ? "—" : "~\(Int((level * 100).rounded())) %",
            batteryNote: note
        )
    }
}

struct DeviceHeader: View {
    let machine: Machine
    let chipSize: CGFloat

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            Mono(text: "This \(UIDevice.current.model)")
            ChipName(machine: machine, size: chipSize)
                .padding(.vertical, 8)
            Text("\(UIDevice.current.systemName) \(UIDevice.current.systemVersion) · \(machine.cores)")
                .font(.archivo(13, weight: 600))
                .padding(.top, 4)
        }
        .foregroundStyle(Color.onSignal)
    }
}

struct StatusTiles: View {
    let status: DeviceStatus
    let valueSize: CGFloat
    let height: CGFloat

    var body: some View {
        HStack(spacing: 6) {
            tile("Heat", status.heat, "As \(UIDevice.current.systemName) reports it", alert: status.hot)
            tile("Low Power", status.lowPower ? "On" : "Off", "Slows the CPU when on", alert: status.lowPower)
            tile("Battery", status.battery, status.batteryNote, alert: false)
        }
        .frame(height: height)
    }

    private func tile(_ name: String, _ value: String, _ note: String, alert: Bool) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Mono(text: name).foregroundStyle(Color.muted)
            Spacer(minLength: 0)
            Text(value)
                .font(.saira(valueSize, width: 50, weight: 600))
                .foregroundStyle(alert ? Color.fault : Color.text)
                .lineLimit(1)
                .minimumScaleFactor(0.6)
            Text(note)
                .font(.archivo(12))
                .foregroundStyle(Color.muted)
                .lineLimit(2)
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 9)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .leading)
        .background(Color.panel, ignoresSafeAreaEdges: [])
    }
}

/// The processor drawn as it is: one tile per core, then what the cores share.
struct CoreMap: View {
    let machine: Machine
    let cache: String?
    let fastSize: CGFloat
    let efficientSize: CGFloat
    let bandHeight: CGFloat
    let efficientLabel: String

    var body: some View {
        VStack(spacing: 6) {
            cores(machine.fast, "FAST", fastSize, Color.volt, Color.ink)
            if machine.efficient > 0 {
                cores(machine.efficient, efficientLabel, efficientSize, Color.steel, Color.text)
                    .frame(minHeight: 30, maxHeight: efficientSize * 1.6 + 40)
            }
            if let cache { band("Shared cache", cache, Color.raised) }
            band("Memory", machine.memory, Color.panel)
        }
        .accessibilityElement(children: .combine)
    }

    private func cores(_ count: Int, _ label: String, _ size: CGFloat, _ fill: Color, _ text: Color) -> some View {
        HStack(spacing: 6) {
            ForEach(0..<count, id: \.self) { index in
                VStack(alignment: .leading) {
                    Text(label).font(.mono(10))
                    Spacer(minLength: 0)
                    // The capitals alone, so the tiles can stay short on a folded iPhone Duo.
                    Text("\(index + 1)")
                        .font(.saira(size, width: 50, weight: 600))
                        .fixedSize()
                        .padding(.top, -0.447 * size)
                        .padding(.bottom, -0.439 * size)
                }
                .padding(.horizontal, 10)
                .padding(.vertical, 8)
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .leading)
                .foregroundStyle(text)
                .background(fill, ignoresSafeAreaEdges: [])
                .clipped()
            }
        }
    }

    private func band(_ name: String, _ value: String, _ fill: Color) -> some View {
        HStack {
            Mono(text: name)
            Spacer()
            Text(value).font(.saira(28, width: 50, weight: 600))
        }
        .padding(.horizontal, 12)
        .frame(minHeight: 30, maxHeight: bandHeight)
        .background(fill, ignoresSafeAreaEdges: [])
    }
}

struct UnknownList: View {
    private let system = UIDevice.current.systemName

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Mono(text: "Not readable on this \(UIDevice.current.model)").foregroundStyle(Color.muted)
            row("Temperature", "\(system) gives apps a coarse state, not degrees.")
            row("Clock speed", "The CPU clock is not readable by apps.")
            row("Core placement", "Threads cannot be pinned; \(system) chooses.")
            row("Cycle counter", "Hardware counters are closed to apps.")
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 20)
        .padding(.vertical, 14)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .leading)
        .background(Color.raised, ignoresSafeAreaEdges: [])
    }

    private func row(_ name: String, _ why: String) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Color.line.frame(height: 1)
            Text(name).font(.archivo(14, weight: 700)).padding(.top, 4)
            Text(why).font(.archivo(13)).foregroundStyle(Color.muted)
        }
    }
}

struct CompareLink: View {
    let bottom: CGFloat

    var body: some View {
        Link(destination: URL(string: "https://prismark.leonardonels.com/compare.html")!) {
            HStack {
                Text("Compare processors on the web").font(.archivo(15, weight: 600))
                Spacer()
                Image(systemName: "arrow.up.right")
            }
            .foregroundStyle(Color.onSignal)
            .padding(.horizontal, 20)
            .padding(.bottom, bottom)
            .frame(height: 56 + bottom)
            .background(Color.signal, ignoresSafeAreaEdges: bottom > 0 ? .all : [])
        }
        .buttonStyle(.block)
    }
}
