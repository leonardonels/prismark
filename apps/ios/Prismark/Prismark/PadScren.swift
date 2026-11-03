import SwiftUI

/// The iPad follows the desktop app: the tests, what the open test means with its ranking, and a report.
/// The reference machines are there from the start, before any run.
struct PadScreen: View {
    @Environment(Session.self) private var session
    @State private var group = "st"
    @State private var testID = "st_k2"
    @State private var reportID: String?

    private var test: Test { allTests.first { $0.id == testID }! }

    /// The highlighted result, else your latest run, else only what this device is.
    private var report: Doc {
        if let reportID {
            if let run = session.saved.first(where: { $0.runID == reportID }) { return run.doc }
            if let doc = references.first(where: { $0["run_id"] as? String == reportID }) { return doc }
        }
        return session.saved.first?.doc ?? session.info
    }

    var body: some View {
        GeometryReader { geo in
            let wide = geo.size.width > 900
            // An open iPhone Duo held upright: narrower than any iPad.
            let compact = geo.size.width < 640

            VStack(spacing: 6) {
                header(wide: wide, compact: compact)
                HStack(spacing: 6) {
                    testList(compact: compact).frame(width: wide ? 300 : compact ? 240 : 270)
                    testPage(showReport: !wide)
                    if wide {
                        PadReport(doc: report, testID: $testID, group: $group, inline: false).frame(width: 300)
                    }
                }
            }
            .padding(.top, 30)
            .background(alignment: .top) {
                // Behind the status bar, whose dark icons don't show on the ground.
                Color.panel.frame(height: 24)
            }
        }
        .background(Color.ground)
    }

    private func header(wide: Bool, compact: Bool) -> some View {
        HStack(spacing: 6) {
            if !compact {
                LogoButton {
                    HStack(spacing: 8) {
                        PrismMark().frame(width: 30, height: 30)
                        if wide { Text("PRISMARK").font(.saira(16, width: 125, weight: 800)).fixedSize() }
                    }
                    .foregroundStyle(Color.ink)
                    .padding(.horizontal, 12)
                    .frame(height: 64)
                    .background(Color.volt, ignoresSafeAreaEdges: [])
                }
            }

            ForEach(groups + [stressGroup]) { item in
                Button {
                    group = item.id
                    testID = allTests.first { $0.group == item.id }?.id ?? testID
                } label: {
                    VStack(alignment: .leading) {
                        Text(item.number).font(.mono(11))
                        Spacer()
                        Text(wide ? item.label : shortLabel(item)).font(.archivo(wide ? 14 : 13, weight: 700)).lineLimit(1).minimumScaleFactor(0.8)
                    }
                    .padding(.horizontal, 10)
                    .padding(.vertical, 9)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .frame(height: 64)
                    .foregroundStyle(Color.text)
                    .background(group == item.id ? Color.panel : Color.steel, ignoresSafeAreaEdges: [])
                    .overlay(alignment: .bottom) {
                        if group == item.id { Color.volt.frame(height: 5) }
                    }
                }
                .buttonStyle(.block)
            }

            QuickSwitch(height: 64)

            Button {
                session.start(only: tests)
            } label: {
                HStack(spacing: 8) {
                    Text(session.quick ? "RUN ALL QUICK" : "RUN ALL FULL")
                        .font(.saira(wide ? 14 : 12, width: 125, weight: 800))
                        .fixedSize()
                    if wide { Chevrons() }
                }
                .foregroundStyle(Color.ink)
                .padding(.horizontal, 12)
                .frame(height: 64)
                .background(Color.volt, ignoresSafeAreaEdges: [])
            }
            .buttonStyle(.block)
        }
    }

    private func shortLabel(_ group: TestGroup) -> String {
        switch group.id {
        case "mem": "Memory"
        case "resp": "Responsive"
        case "stress": "Stress"
        default: group.label
        }
    }

    private func testList(compact: Bool) -> some View {
        let about = (groups + [stressGroup]).first { $0.id == group }
        let members = group == stressGroup.id ? session.stressOffered : tests.filter { $0.group == group }

        return GeometryReader { geo in
            ScrollView {
                VStack(spacing: 6) {
                    VStack(alignment: .leading, spacing: 6) {
                        Mono(text: "Tests · \(about?.label ?? "")")
                        Text(about?.about ?? "")
                            .font(.archivo(13))
                            .foregroundStyle(Color.muted)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    .padding(14)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .background(Color.panel, ignoresSafeAreaEdges: [])

                    ForEach(members) { item in
                        TestCard(test: item, current: item.id == testID, compact: compact) {
                            testID = item.id
                        }
                    }

                    if group == stressGroup.id {
                        StressSettings().background(Color.panel, ignoresSafeAreaEdges: [])
                    }
                    if let about { GroupCard(group: about, members: members) }
                }
                .frame(minHeight: geo.size.height)
            }
        }
    }

    private func testPage(showReport: Bool) -> some View {
        let text = testTexts[test.id]
        let rows = ranking(for: test, run: session.saved.first)
        let top = rows.map(\.value.v).max() ?? 1
        let compared = test.read(report)?.v

        return ScrollView {
            VStack(alignment: .leading, spacing: 8) {
                Text(text?.title ?? test.name)
                    .font(.saira(30, width: 112))
                    .lineLimit(2)
                    .minimumScaleFactor(0.8)
                Text(text?.about ?? "").font(.archivo(15))
                PadSection(title: "How it is measured", text: text?.how ?? "")
                PadSection(title: "Reading the result", text: text?.reading ?? "")

                if test.bit == 0 {
                    Mono(text: "Ranking").padding(.top, 8)
                    HStack(spacing: 6) {
                        Legend(text: "Your runs", color: .yours, background: .raised)
                        Legend(text: "Reference", color: .reference, background: .raised)
                    }
                }
            }
            .fixedSize(horizontal: false, vertical: true)
            .padding(.horizontal, 20)
            .padding(.top, 20)
            .padding(.bottom, 8)
            .frame(maxWidth: .infinity, alignment: .leading)

            if let part = test.part {
                if let run = session.saved.first(where: { stressRow($0.doc, part) != nil }), let row = stressRow(run.doc, part) {
                    VStack(spacing: 6) {
                        StressBlock(test: test, row: row)
                        StressFacts(test: test, run: run, row: row)
                    }
                } else {
                    Text("No result yet. Run it from the list: it takes as long as you set there.")
                        .font(.archivo(14))
                        .foregroundStyle(Color.muted)
                        .padding(.horizontal, 20)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
            }

            LazyVStack(spacing: 0) {
                ForEach(Array(rows.enumerated()), id: \.element.id) { index, row in
                    Button {
                        reportID = row.runID
                    } label: {
                        RankRow(number: index + 1, row: row, top: top, yours: compared, highlighted: row.runID == reportID)
                    }
                    .buttonStyle(.block)
                }
            }

            if showReport {
                PadReport(doc: report, testID: $testID, group: $group, inline: true)
            }
        }
        .contentMargins(.bottom, 20, for: .scrollContent)
        .background(Color.panel, ignoresSafeAreaEdges: [])
    }
}

/// One test: tap to open it, or run it on its own.
struct TestCard: View {
    @Environment(Session.self) private var session
    let test: Test
    let current: Bool
    let compact: Bool
    let open: () -> Void

    var body: some View {
        let value = session.saved.lazy.compactMap { test.read($0.doc) }.first

        HStack(spacing: 0) {
            Button(action: open) {
                VStack(alignment: .leading, spacing: 2) {
                    Text(test.name).font(.archivo(15, weight: 700)).lineLimit(2).fixedSize(horizontal: false, vertical: true)
                    Text(test.sub).font(.archivo(12)).lineLimit(2)
                    Spacer(minLength: 4)
                    if let value {
                        HStack(alignment: .firstTextBaseline, spacing: 5) {
                            Text(format(value.v)).font(.saira(30, width: 50, weight: 600))
                            Text(test.unit).font(.mono(10))
                        }
                    } else {
                        Text("No result yet").font(.mono(11)).foregroundStyle(current ? Color.ink : Color.muted)
                    }
                }
                .padding(.leading, 14)
                .padding(.vertical, 10)
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .leading)
            }
            .buttonStyle(.block)

            Button {
                session.start(only: [test])
            } label: {
                Text(test.bit > 0 ? "RUN" : session.quick ? "RUN\nQUICK" : "RUN\nFULL")
                    .font(.saira(compact ? 10 : 12, width: 125, weight: 800))
                    .multilineTextAlignment(.center)
                    .foregroundStyle(current ? Color.volt : Color.text)
                    .frame(width: compact ? 58 : 72, height: 56)
                    .background(current ? Color.ink : Color.raised, ignoresSafeAreaEdges: [])
                    .padding(compact ? 6 : 10)
            }
            .buttonStyle(.block)
            .accessibilityLabel("Run \(test.name)")
        }
        .frame(height: 112)
        .foregroundStyle(current ? Color.ink : Color.text)
        .background(current ? Color.volt : Color.panel, ignoresSafeAreaEdges: [])
    }
}

struct PadSection: View {
    let title: String
    let text: String

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Mono(text: title)
            Text(text).font(.archivo(14))
        }
        .padding(.top, 8)
    }
}

struct Legend: View {
    let text: String
    let color: Color
    let background: Color

    var body: some View {
        HStack(spacing: 6) {
            color.frame(width: 8, height: 8)
            Mono(text: text)
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 6)
        .background(background, ignoresSafeAreaEdges: [])
    }
}

/// The report of one result: which machine, what it scored, and the file if it is yours.
struct PadReport: View {
    @Environment(Session.self) private var session
    let doc: Doc
    @Binding var testID: String
    @Binding var group: String
    /// Under the ranking in portrait it reads as one more section of the page, not as a pass of its own.
    let inline: Bool

    var body: some View {
        let machine = doc["machine"] as? Doc
        let cpu = machine?["cpu"] as? Doc
        let model = shortName(cpu?["model"] as? String ?? "Unknown")
        let id = doc["run_id"] as? String
        let run = session.saved.first { $0.runID == id }
        let cores = Machine(info: doc).cores
        let measured = allTests.filter { $0.read(doc) != nil }

        VStack(spacing: inline ? 0 : 6) {
            let tags = HStack(spacing: 4) {
                PadTag(text: id == nil ? "NO RUN YET" : run == nil ? "REFERENCE" : "YOUR RUN", strong: run != nil)
                if (doc["config"] as? Doc)?["quick"] as? Bool == true { PadTag(text: "QUICK", strong: false) }
            }

            if inline {
                VStack(alignment: .leading, spacing: 8) {
                    HStack {
                        Mono(text: "Report").foregroundStyle(Color.muted)
                        Spacer()
                        tags
                    }
                    Text(model)
                        .font(.saira(30, width: 112))
                        .lineLimit(1)
                        .minimumScaleFactor(0.7)
                }
                .padding(.horizontal, 20)
                .padding(.top, 20)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(Color.panel, ignoresSafeAreaEdges: [])
                .overlay(alignment: .top) { Color.line.frame(height: 1) }
            } else {
                VStack(alignment: .leading, spacing: 10) {
                    HStack {
                        Text("Processor:").font(.mono(12))
                        Spacer()
                        tags
                    }
                    Text(model)
                        .font(.saira(92, width: 50, weight: 600))
                        .foregroundStyle(Color.ink)
                        .lineLimit(1)
                        .minimumScaleFactor(0.35)
                }
                .padding(16)
                .frame(maxWidth: .infinity, alignment: .leading)
                .foregroundStyle(Color.onSignal)
                .background(Color.signal, ignoresSafeAreaEdges: [])
            }

            let section = VStack(alignment: .leading, spacing: 10) {
                Mono(text: "Results")
                if measured.isEmpty {
                    Text("No results yet. Run a test, or pick a machine in the ranking to see its report.")
                        .font(.archivo(14))
                        .foregroundStyle(Color.muted)
                        .fixedSize(horizontal: false, vertical: true)
                }
                ForEach(measured) { item in
                    Button {
                        testID = item.id
                        group = item.group
                    } label: {
                        HStack(alignment: .firstTextBaseline, spacing: 6) {
                            Text(item.name)
                                .font(.archivo(13, weight: item.id == testID ? 700 : 500))
                                .foregroundStyle(item.id == testID ? Color.reference : Color.text)
                                .lineLimit(1)
                                .minimumScaleFactor(0.8)
                            Spacer(minLength: 4)
                            Text(format(item.read(doc)!.v)).font(.archivo(13, weight: 700))
                            Text(item.unit).font(.mono(10)).foregroundStyle(Color.muted)
                        }
                        .frame(minHeight: 24)
                    }
                    .buttonStyle(.block)
                }

                Color.line.frame(height: 1)
                Mono(text: id == nil || run != nil ? "This \(UIDevice.current.model)" : "This machine")
                Grid(alignment: .leading, verticalSpacing: 4) {
                    line("Processor", cpu?["model"] as? String ?? "Unknown")
                    if !cores.hasPrefix("0") { line("Cores", cores) }
                    line("System", id == nil || run != nil ? "\(UIDevice.current.systemName) \(UIDevice.current.systemVersion)" : machine?["os"] as? String ?? "")
                }
                Spacer(minLength: 0)
            }
            .padding(inline ? 20 : 16)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(Color.panel, ignoresSafeAreaEdges: [])

            if inline {
                section
            } else {
                // A full run lists more than the column can hold.
                ScrollView { section }
                    .background(Color.panel, ignoresSafeAreaEdges: [])
            }

            if let run {
                VStack(alignment: .leading, spacing: 8) {
                    Text(run.started?.formatted(.dateTime.day().month().hour().minute()) ?? "").font(.mono(11))
                    Barcode(id: run.runID).frame(height: 36).clipped()
                    ShareFileButton(file: run.file) { busy in
                        Group {
                            if busy {
                                ProgressView().tint(Color.volt)
                            } else {
                                Text("Share result file").font(.archivo(14, weight: 700))
                            }
                        }
                        .foregroundStyle(Color.volt)
                        .frame(maxWidth: .infinity)
                        .frame(height: 44)
                        .background(Color.ink, ignoresSafeAreaEdges: [])
                    }
                }
                .foregroundStyle(Color.ink)
                .padding(14)
                .background(Color.volt, ignoresSafeAreaEdges: [])
            }
        }
    }

    private func line(_ label: String, _ value: String) -> some View {
        GridRow {
            Text(label).font(.mono(12)).foregroundStyle(Color.muted)
            Text(value).font(.archivo(13, weight: 600)).fixedSize(horizontal: false, vertical: true)
        }
    }
}

struct PadTag: View {
    let text: String
    let strong: Bool

    var body: some View {
        Text(text)
            .font(.mono(11))
            .padding(.horizontal, 7)
            .padding(.vertical, 3)
            .foregroundStyle(Color.ink)
            .background(strong ? Color.volt : Color.onSignal, ignoresSafeAreaEdges: [])
    }
}

/// A boarding card under the tests: it grows to fill whatever the tests leave, and says how far the group has got.
struct GroupCard: View {
    @Environment(Session.self) private var session
    let group: TestGroup
    let members: [Test]
    /// The run being read; without one, the latest.
    var run: Run? = nil

    var body: some View {
        let shown = run ?? session.saved.first
        let measured = shown.map { doc in members.filter { $0.ran(doc.doc) }.count } ?? 0

        GeometryReader { geo in
            if run != nil && geo.size.height < 230 {
                compact(height: geo.size.height, measured: measured, id: shown?.runID)
            } else if geo.size.height >= 150 {
                VStack(alignment: .leading, spacing: 8) {
                    HStack {
                        Mono(text: group.label)
                        Spacer()
                        Mono(text: group.id == stressGroup.id ? "Full load" : "\(group.number) of 04")
                    }
                    Spacer(minLength: 0)
                    Text("\(measured)/\(members.count)")
                        .font(.saira(min(geo.size.height * 0.34, 150), width: 50, weight: 600))
                        .foregroundStyle(Color.ink)
                        .lineLimit(1)
                        .minimumScaleFactor(0.5)
                    Text(shown == nil ? "No run yet" : run != nil ? "Measured in this run" : "Measured in your latest run")
                        .font(.archivo(13, weight: 600))
                    Rectangle()
                        .stroke(style: StrokeStyle(lineWidth: 2, dash: [6, 5]))
                        .frame(height: 1)
                        .padding(.vertical, 6)
                    Color.clear
                        .frame(height: min(geo.size.height * 0.16, 72))
                        .overlay(alignment: .leading) {
                            Barcode(id: String(repeating: shown?.runID ?? "5a3c91e7b20d48f6a1c7e93b", count: 3), bars: 64)
                                .foregroundStyle(Color.ink)
                        }
                        .clipped()
                }
                .padding(.horizontal, 14)
                .padding(.top, 14)
                .padding(.bottom, run == nil ? 24 : 34)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .foregroundStyle(Color.onSignal)
        .background(Color.signal, ignoresSafeAreaEdges: [])
    }

    /// Too short for a whole card: the count and the barcode side by side, as tall as there is room for.
    private func compact(height: CGFloat, measured: Int, id: String?) -> some View {
        let room = max(20, height - 66)

        return VStack(alignment: .leading, spacing: 6) {
            HStack {
                Mono(text: group.label)
                Spacer()
                Mono(text: group.id == stressGroup.id ? "Full load" : "\(group.number) of 04")
            }
            HStack(spacing: 14) {
                Text("\(measured)/\(members.count)")
                    .font(.saira(room * 1.2, width: 50, weight: 600))
                    .foregroundStyle(Color.ink)
                    .fixedSize()
                    .frame(height: room)
                Color.clear
                    .frame(height: room)
                    .overlay(alignment: .leading) {
                        Barcode(id: String(repeating: id ?? "", count: 3), bars: 64)
                            .foregroundStyle(Color.ink)
                    }
                    .clipped()
            }
        }
        .padding(.horizontal, 14)
        .padding(.top, 12)
        .padding(.bottom, 34)
    }
}
