import SwiftUI

struct RunScreen: View {
    @Environment(Session.self) private var session

    var body: some View {
        PageStack(destination: { AnyView(GroupScreen(group: $0.base as! TestGroup)) }) {
            VStack(spacing: 6) {
                MachinePass()
                ForEach(groups) { group in
                    GroupRow(group: group)
                }
                RunBar()
            }
            .background(Color.ground)
        }
    }
}

/// The blue pass: which processor this is.
struct MachinePass: View {
    @Environment(Session.self) private var session

    var body: some View {
        let machine = Machine(info: session.info)

        VStack(alignment: .leading) {
            HStack {
                HStack(spacing: 8) {
                    PrismMark().frame(width: 24, height: 24)
                    Text("PRISMARK").font(.saira(14, width: 125, weight: 800))
                }
                .foregroundStyle(Color.ink)
                .padding(.horizontal, 10)
                .frame(height: 40)
                .background(Color.volt, ignoresSafeAreaEdges: [])
                Spacer()
                Text("REV. \(Prismark.version)").font(.mono(11))
            }
            Spacer(minLength: 8)
            Text("Processor:").font(.mono(12))
            ChipName(machine: machine, size: 176)
                .padding(.top, 6)
                .padding(.bottom, 10)
            Rectangle().fill(Color.signalLine).frame(height: 1)
            Text(machine.summary)
                .font(.archivo(13, weight: 600))
                .padding(.top, 6)
        }
        .padding(.top, 62)
        .padding([.horizontal], 20)
        .padding(.bottom, 14)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .leading)
        .foregroundStyle(Color.onSignal)
        .background { PassDots() }
    }
}

/// What the core says about this device.
struct Machine {
    let chip: String
    let suffix: String
    let fullName: String
    let fast: Int
    let efficient: Int
    let cores: String
    let memory: String
    let summary: String

    init(info: Doc) {
        let machine = info["machine"] as? Doc
        let cpu = machine?["cpu"] as? Doc
        let model = (cpu?["model"] as? String ?? "Unknown").replacingOccurrences(of: "Apple ", with: "")
        let words = model.split(separator: " ")
        chip = String(words.first ?? "?")
        suffix = words.dropFirst().joined(separator: " ").uppercased()
        let types = (cpu?["cores"] as? [Doc] ?? []).compactMap { $0["type"] as? String }
        fullName = model
        fast = types.filter { $0 == "P" }.count
        efficient = types.count - fast
        cores = efficient > 0 ? "\(fast) fast + \(efficient) efficient" : "\(fast) cores"
        memory = "\(Int((Double(ProcessInfo.processInfo.physicalMemory) / 1_073_741_824).rounded())) GB"
        summary = "\(cores) · \(memory) · \(UIDevice.current.systemName) \(UIDevice.current.systemVersion)"
    }
}

/// One test group: the check picks them all, the rest opens the group.
struct GroupRow: View {
    @Environment(Session.self) private var session
    let group: TestGroup

    var body: some View {
        let members = tests.filter { $0.group == group.id }
        let count = members.filter { session.picked.contains($0.id) }.count
        let on = count > 0

        HStack(spacing: 0) {
            Button {
                for test in members {
                    if on { session.picked.remove(test.id) } else { session.picked.insert(test.id) }
                }
            } label: {
                Check(on: on).frame(width: 56, height: 56).contentShape(Rectangle())
            }
            .accessibilityLabel((on ? "Leave out " : "Include ") + group.label)

            PageLink(group) {
                HStack {
                    VStack(alignment: .leading, spacing: 1) {
                        Text(group.label).font(.archivo(16, weight: 700))
                        Text("\(count) of \(members.count) tests").font(.mono(11))
                    }
                    Spacer()
                    Image(systemName: "chevron.right").font(.system(size: 13, weight: .bold))
                }
                .padding(.trailing, 14)
                .frame(height: 56)
                .contentShape(Rectangle())
            }
        }
        .buttonStyle(.block)
        .foregroundStyle(on ? Color.ink : Color.text)
        .background(on ? Color.volt : Color.panel)
    }
}

/// Quick switch and the main button.
struct RunBar: View {
    @Environment(Session.self) private var session

    var body: some View {
        let count = session.picked.count

        // Both reach the screen's edges, like the rows above; the bottom inset keeps their contents off the home bar.
        HStack(spacing: 6) {
            QuickSwitch(height: 94, bottom: 34)

            Button {
                session.start()
            } label: {
                HStack {
                    Text(count == 0 ? "PICK A TEST" : "RUN \(count) TESTS")
                        .font(.saira(16, width: 125, weight: 800))
                        .lineLimit(1)
                        .minimumScaleFactor(0.7)
                    Spacer(minLength: 4)
                    Chevrons()
                }
                .foregroundStyle(count == 0 ? Color.muted : Color.ink)
                .padding(.horizontal, 14)
                .padding(.bottom, 34)
                .frame(maxWidth: .infinity)
                .frame(height: 94)
                .background(count == 0 ? Color.raised : Color.volt, ignoresSafeAreaEdges: [])
            }
            .buttonStyle(.block)
            .disabled(count == 0)
        }
    }
}

/// Quick runs give a first look and are not ranked.
struct QuickSwitch: View {
    @Environment(Session.self) private var session
    let height: CGFloat
    var bottom: CGFloat = 0

    var body: some View {
        Button {
            session.quick.toggle()
        } label: {
            VStack(alignment: .leading) {
                Mono(text: "Quick")
                Spacer()
                HStack(spacing: 6) {
                    if session.quick { Text("ON").font(.mono(10, semibold: true)) }
                    Rectangle().frame(width: 12, height: 12)
                    if !session.quick { Text("OFF").font(.mono(10, semibold: true)) }
                }
                .foregroundStyle(session.quick ? Color.ink : Color.text)
                .padding(.horizontal, 5)
                .padding(.vertical, 3)
                .background(session.quick ? Color.volt : Color.line, ignoresSafeAreaEdges: [])
            }
            .padding(12)
            .padding(.bottom, bottom)
            .frame(width: 96, height: height, alignment: .leading)
            .foregroundStyle(Color.text)
            .background(Color.panel, ignoresSafeAreaEdges: [])
        }
        .buttonStyle(.block)
        .accessibilityValue(session.quick ? "On" : "Off")
    }
}
