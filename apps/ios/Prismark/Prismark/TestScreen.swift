import SwiftUI

/// One test: your score, then every machine.
struct TestScreen: View {
    let test: Test
    let run: Run

    var body: some View {
        VStack(spacing: 6) {
            VStack(alignment: .leading, spacing: 8) {
                BackButton()
                Text(test.name)
                    .font(.saira(28, width: 112))
                    .lineLimit(2)
                    .minimumScaleFactor(0.8)
                Text(test.sub + ". " + (test.bit > 0 ? "A score needs 10 minutes." : test.higherBetter ? "Higher is better." : "Lower is better."))
                    .font(.archivo(14))
                    .foregroundStyle(Color.muted)
                    .fixedSize(horizontal: false, vertical: true)
            }
            .padding(.top, 62)
            .padding(.horizontal, 20)
            .padding(.bottom, 14)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(Color.panel)

            if let part = test.part, let row = stressRow(run.doc, part) {
                StressBlock(test: test, row: row)
                ScrollView {
                    StressFacts(test: test, run: run, row: row).padding(.bottom, 34)
                }
                .background(Color.panel)
            } else {
                TestDetail(test: test, run: run)
            }
        }
        .background(Color.ground)
    }
}

/// The score block and the ranking. The phone shows one run at a time.
struct TestDetail: View {
    let test: Test
    let run: Run

    var body: some View {
        let rows = ranking(for: test, run: run)
        let rank = (rows.firstIndex { $0.yours } ?? 0) + 1
        let value = test.read(run.doc)
        let top = rows.map(\.value.v).max() ?? 1

        VStack(spacing: 6) {
            HStack(alignment: .bottom) {
                Text(value.map { format($0.v) } ?? "—")
                    .font(.saira(150, width: 50, weight: 600))
                    .lineLimit(1)
                    .minimumScaleFactor(0.5)
                    .frame(height: 118, alignment: .bottom)
                Spacer(minLength: 8)
                VStack(alignment: .trailing, spacing: 6) {
                    Text(test.unit).font(.mono(12, semibold: true))
                    Text(rangeText(value)).font(.mono(12))
                    if value != nil, !run.quick, run.complete {
                        Text("RANK \(String(format: "%02d", rank)) / \(rows.count)")
                            .font(.mono(12))
                            .foregroundStyle(Color.volt)
                            .padding(.horizontal, 7)
                            .padding(.vertical, 3)
                            .background(Color.ink)
                    }
                }
                .multilineTextAlignment(.trailing)
            }
            .foregroundStyle(Color.ink)
            .padding(.horizontal, 20)
            .padding(.vertical, 16)
            .background(Color.volt)

            ScrollView {
                // Not lazy, so the rows slide in with the screen.
                VStack(spacing: 0) {
                    ForEach(Array(rows.enumerated()), id: \.element.id) { index, row in
                        RankRow(number: index + 1, row: row, top: top, yours: value?.v)
                    }
                }
                .padding(.bottom, 34)
            }
            .background(Color.panel)
        }
    }

    private func rangeText(_ value: Value?) -> String {
        guard let value else { return "Not measured\non this device" }
        guard value.hasRange else { return "No range:\nmay be noise" }
        return "95 % range\n\(format(value.lo))–\(format(value.hi))"
    }
}

struct RankRow: View {
    let number: Int
    let row: Rank
    let top: Double
    let yours: Double?
    var highlighted = false

    var body: some View {
        HStack(spacing: 8) {
            Text(String(format: "%02d", number)).font(.mono(12)).frame(width: 22, alignment: .leading)
            VStack(alignment: .leading, spacing: 5) {
                HStack(alignment: .firstTextBaseline, spacing: 6) {
                    Text(row.name).font(.archivo(14, weight: row.yours ? 700 : 500)).lineLimit(1)
                    Text(row.os).font(.mono(10)).foregroundStyle(Color.muted)
                }
                GeometryReader { geo in
                    Rectangle()
                        .fill(row.yours ? Color.yours : Color.reference)
                        .frame(width: geo.size.width * row.value.v / top)
                }
                .frame(height: 7)
            }
            Text(format(row.value.v))
                .font(.saira(20, width: 75, weight: 600))
                .frame(width: 44, alignment: .trailing)
            Text(delta)
                .font(.mono(11))
                .frame(width: 50, alignment: .trailing)
        }
        .padding(.horizontal, 16)
        .frame(height: 54)
        .background(row.yours || highlighted ? Color.raised : Color.clear)
        .overlay(alignment: .top) { Color.line.frame(height: 1) }
    }

    private var delta: String {
        guard let yours, yours != 0 else { return "—" }
        let change = (row.value.v / yours - 1) * 100
        guard abs(change) >= 0.05 else { return "—" }
        return (change > 0 ? "+" : "−") + String(format: "%.1f%%", abs(change))
    }
}
