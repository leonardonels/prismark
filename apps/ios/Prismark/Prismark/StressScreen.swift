import SwiftUI

/// How long a stress test runs, as on the desktop app. Remembered between runs.
enum StressLength: String, CaseIterable {
    case ten = "10m", thirty = "30m", open = "until-stopped"

    static let key = "stress_time"
    static let stopKey = "stress_stop_on_error"

    var seconds: Double {
        switch self {
        case .ten: 600
        case .thirty: 1800
        case .open: 0
        }
    }

    var label: String {
        switch self {
        case .ten: "10 min"
        case .thirty: "30 min"
        case .open: "Until stopped"
        }
    }
}

/// The row under the tests. Not picked with them: it runs on its own.
struct StressRow: View {
    var body: some View {
        PageLink(stressGroup) {
            HStack {
                VStack(alignment: .leading, spacing: 1) {
                    Text(stressGroup.label).font(.archivo(16, weight: 700))
                    Text("Runs on its own, 10 min or more").font(.mono(11))
                }
                Spacer()
                Image(systemName: "chevron.right").font(.system(size: 13, weight: .bold))
            }
            .padding(.leading, 56)
            .padding(.trailing, 14)
            .frame(height: 56)
            .foregroundStyle(Color.text)
            .background(Color.panel)
            .overlay(alignment: .leading) {
                Image(systemName: "flame.fill")
                    .font(.system(size: 18))
                    .foregroundStyle(Color.text)
                    .frame(width: 56)
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.block)
    }
}

/// The length, and whether to stop at the first wrong result.
struct StressSettings: View {
    @AppStorage(StressLength.key) private var length = StressLength.ten.rawValue
    @AppStorage(StressLength.stopKey) private var stopOnError = true

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Mono(text: "Length")
            HStack(spacing: 6) {
                ForEach(StressLength.allCases, id: \.self) { item in
                    let on = length == item.rawValue
                    Button {
                        length = item.rawValue
                    } label: {
                        Text(item.label)
                            .font(.archivo(13, weight: 700))
                            .lineLimit(1)
                            .minimumScaleFactor(0.7)
                            .foregroundStyle(on ? Color.ink : Color.text)
                            .frame(maxWidth: .infinity)
                            .frame(height: 44)
                            .background(on ? Color.volt : Color.raised, ignoresSafeAreaEdges: [])
                    }
                    .buttonStyle(.block)
                    .accessibilityAddTraits(on ? .isSelected : [])
                }
            }
            Button {
                stopOnError.toggle()
            } label: {
                HStack(spacing: 12) {
                    Check(on: stopOnError)
                    Text("Stop at the first wrong result").font(.archivo(14, weight: 600))
                    Spacer(minLength: 0)
                }
                .frame(height: 44)
                .contentShape(Rectangle())
            }
            .buttonStyle(.block)
            .accessibilityAddTraits(stopOnError ? .isSelected : [])
        }
        .padding(16)
    }
}

/// Pick a length, then run one part.
struct StressScreen: View {
    @Environment(Session.self) private var session

    var body: some View {
        VStack(spacing: 6) {
            VStack(alignment: .leading, spacing: 10) {
                BackButton()
                Text(stressGroup.label).font(.saira(36, width: 112)).lineLimit(1)
                Text(stressGroup.about)
                    .font(.archivo(14))
                    .foregroundStyle(Color.muted)
                    .fixedSize(horizontal: false, vertical: true)
            }
            .padding(.top, 62)
            .padding(.horizontal, 20)
            .padding(.bottom, 16)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(Color.panel)

            ScrollView {
                VStack(spacing: 6) {
                    StressSettings()
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .background(Color.panel)
                    ForEach(session.stressOffered) { test in
                        Button {
                            session.start(only: [test])
                        } label: {
                            HStack(spacing: 14) {
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(test.name).font(.archivo(16, weight: 700))
                                    Text(test.sub).font(.archivo(13)).multilineTextAlignment(.leading)
                                }
                                Spacer(minLength: 0)
                                Text("RUN")
                                    .font(.saira(13, width: 125, weight: 800))
                                    .foregroundStyle(Color.ink)
                                    .frame(width: 64, height: 44)
                                    .background(Color.volt, ignoresSafeAreaEdges: [])
                            }
                            .padding(.horizontal, 16)
                            .frame(height: 72)
                            .foregroundStyle(Color.text)
                            .background(Color.panel)
                        }
                        .buttonStyle(.block)
                        .accessibilityLabel("Run \(test.name)")
                    }
                    Text("It gets warm and uses a lot of battery, and the system slows it down if it gets too hot. The result is kept like any other.")
                        .font(.archivo(13))
                        .foregroundStyle(Color.muted)
                        .fixedSize(horizontal: false, vertical: true)
                        .padding(16)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .background(Color.raised)
                }
                .padding(.bottom, 34)
            }
        }
        .background(Color.ground)
    }
}

func stressVerdict(_ row: Doc) -> String {
    switch row["verdict"] as? String {
    case "failed": "Failed"
    case "too_hot": "Too hot"
    default: "Stable"
    }
}

/// A stress test under 10 minutes: its verdict, without a score.
struct StressTile: View {
    let test: Test
    let row: Doc

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("Under 10 min, no score").font(.mono(11)).foregroundStyle(Color.muted).lineLimit(1).minimumScaleFactor(0.7)
            Text(stressVerdict(row))
                .font(.saira(66, width: 50, weight: 600))
                .foregroundStyle(row["verdict"] as? String == "failed" ? Color.fault : Color.text)
                .lineLimit(1)
                .minimumScaleFactor(0.6)
                .frame(height: 72)
            Text(test.name)
                .font(.archivo(14, weight: 700))
                .lineLimit(2)
                .multilineTextAlignment(.leading)
                .frame(height: 36, alignment: .topLeading)
        }
        .padding(12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .frame(height: ScoreTile.height)
        .background(Color.panel)
    }
}

/// The score, or the verdict alone when the run was too short for one.
struct StressBlock: View {
    let test: Test
    let row: Doc

    var body: some View {
        let score = ci(row["score"] as? Doc)

        HStack(alignment: .bottom) {
            Text(score.map { format($0.v) } ?? stressVerdict(row))
                .font(.saira(score == nil ? 96 : 150, width: 50, weight: 600))
                .lineLimit(1)
                .minimumScaleFactor(0.5)
                .frame(height: 118, alignment: .bottom)
            Spacer(minLength: 8)
            VStack(alignment: .trailing, spacing: 6) {
                if score != nil {
                    Text(test.unit).font(.mono(12, semibold: true))
                    Text(stressVerdict(row).uppercased())
                        .font(.mono(12))
                        .foregroundStyle(Color.volt)
                        .padding(.horizontal, 7)
                        .padding(.vertical, 3)
                        .background(Color.ink, ignoresSafeAreaEdges: [])
                } else {
                    Text("Under 10 min:\nno score").font(.mono(12))
                }
            }
            .multilineTextAlignment(.trailing)
        }
        .foregroundStyle(Color.ink)
        .padding(.horizontal, 20)
        .padding(.vertical, 16)
        .background(row["verdict"] as? String == "failed" ? Color.fault : Color.volt, ignoresSafeAreaEdges: [])
    }
}

/// The speed over time, and what else the run recorded.
struct StressFacts: View {
    let test: Test
    let run: Run
    let row: Doc

    var body: some View {
        let samples = speeds

        VStack(alignment: .leading, spacing: 10) {
            if samples.count > 1 {
                Mono(text: "Speed over time, \(test.unit)")
                StressChart(values: samples).frame(height: 96)
            }
            Grid(alignment: .leading, verticalSpacing: 6) {
                ForEach(facts, id: \.0) { fact in
                    GridRow {
                        Text(fact.0).font(.mono(12)).foregroundStyle(Color.muted)
                        Text(fact.1).font(.archivo(14, weight: 600)).fixedSize(horizontal: false, vertical: true)
                    }
                }
            }
            if let cores = row["cores"] as? [Doc], !cores.isEmpty {
                Mono(text: "Each core").padding(.top, 8)
                Grid(alignment: .leading, verticalSpacing: 6) {
                    ForEach(Array(cores.enumerated()), id: \.offset) { _, core in
                        GridRow {
                            Text("CPU \((core["cpu"] as? NSNumber)?.intValue ?? 0)").font(.mono(12)).foregroundStyle(Color.muted)
                            Text(format(num(core["perf"] as? Doc, "est")) + " " + test.unit).font(.archivo(14, weight: 600))
                            Text(num(core, "wrong") > 0 ? "\(Int(num(core, "wrong"))) wrong" : "").font(.archivo(14, weight: 600)).foregroundStyle(Color.fault)
                        }
                    }
                }
            }
        }
        .padding(20)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color.panel, ignoresSafeAreaEdges: [])
    }

    /// One series only: core cycling keeps one for each core, listed instead.
    private var speeds: [Double] {
        let series = (run.doc["results"] as? [Doc] ?? []).filter {
            $0["mode"] as? String == "stress" && ($0["params"] as? Doc)?["part"] as? String == test.part
        }
        guard series.count == 1 else { return [] }
        return (series[0]["samples"] as? [Any] ?? []).compactMap { ($0 as? NSNumber)?.doubleValue }
    }

    private var facts: [(String, String)] {
        var out: [(String, String)] = []
        let seconds = num(row, "duration_s")
        if seconds.isFinite { out.append(("Ran for", String(format: "%d:%02d", Int(seconds) / 60, Int(seconds) % 60))) }
        let first = num(row["first_minute"] as? Doc, "est")
        if first.isFinite { out.append(("First minute", "\(format(first)) \(test.unit)")) }
        let last = num(row["last_minute"] as? Doc, "est")
        if last.isFinite { out.append(("Last minute", "\(format(last)) \(test.unit)")) }
        let hold = num(row["hold_up"] as? Doc, "est")
        if hold.isFinite { out.append(("Holds up", String(format: "%.0f %% of its start", hold * 100))) }
        let hot = num(row, "temp_max_c")
        if hot.isFinite { out.append(("Hottest", String(format: "%.0f °C", hot))) }
        let mhz = num(row, "mhz")
        if mhz.isFinite && mhz > 0 { out.append(("Clock", String(format: "%.0f MHz", mhz))) }
        let bytes = num(row, "bytes_tested")
        if bytes.isFinite { out.append(("Tested", String(format: "%.1f GB", bytes / 1_073_741_824))) }
        out.append(("Wrong", String(format: "%.0f results", num(row, "wrong_tasks") + max(0, num(row, "checksum_errors")))))
        switch row["stop_reason"] as? String {
        case "user": out.append(("Ended", "You stopped it"))
        case "error": out.append(("Ended", "At the first wrong result"))
        case "temperature": out.append(("Ended", "At the temperature limit"))
        default: break
        }
        return out
    }
}

/// Flat bars, at most one for each 60th of the run.
struct StressChart: View {
    let values: [Double]

    var body: some View {
        Canvas { context, size in
            let per = max(1, values.count / 60)
            let bars = stride(from: 0, to: values.count, by: per).map { start -> Double in
                let part = values[start..<min(start + per, values.count)]
                return part.reduce(0, +) / Double(part.count)
            }
            let top = bars.max() ?? 1
            let width = size.width / CGFloat(bars.count)
            for (index, bar) in bars.enumerated() where top > 0 {
                let height = size.height * CGFloat(bar / top)
                let box = CGRect(x: CGFloat(index) * width, y: size.height - height, width: max(1, width - 1), height: height)
                context.fill(Path(box), with: .color(.reference))
            }
        }
        .accessibilityHidden(true)
    }
}
