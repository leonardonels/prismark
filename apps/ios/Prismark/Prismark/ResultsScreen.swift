import SwiftUI

struct OpenTest: Hashable {
    let test: Test
    let run: Run
}

/// One run: each test on its own, a group at a time. There is no total.
struct ResultsScreen: View {
    let run: Run
    @State private var group = "st"

    var body: some View {
        let shown = tests.filter { $0.group == group && $0.read(run.doc) != nil }

        VStack(spacing: 6) {
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    BackButton()
                    Spacer()
                    Mono(text: "\(run.quick ? "Quick" : "Full") · \(run.measured) tests")
                    ShareFileButton(file: run.file) { busy in
                        Group {
                            if busy {
                                ProgressView().tint(Color.volt)
                            } else {
                                Image(systemName: "square.and.arrow.up")
                            }
                        }
                        .frame(width: 44, height: 44)
                        .background(Color.ink, ignoresSafeAreaEdges: [])
                        .foregroundStyle(Color.volt)
                    }
                }
                Text(run.started?.formatted(.dateTime.day().month().year().hour().minute()) ?? "")
                    .font(.saira(24, width: 112))
                    .lineLimit(1)
                    .minimumScaleFactor(0.7)
            }
            .foregroundStyle(Color.ink)
            .padding(.top, 62)
            .padding(.horizontal, 16)
            .padding(.bottom, 14)
            .background(Color.volt)

            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 6) {
                    ForEach(groups) { item in
                        Button {
                            group = item.id
                        } label: {
                            Text(item.label)
                                .font(.archivo(14, weight: 700))
                                .padding(.horizontal, 14)
                                .frame(height: 48)
                                .background(group == item.id ? Color.panel : Color.steel)
                                .overlay(alignment: .bottom) {
                                    if group == item.id { Color.volt.frame(height: 5) }
                                }
                        }
                        .buttonStyle(.block)
                    }
                }
            }
            .frame(height: 48)

            if shown.isEmpty {
                Text(run.skipped ? "Not measured in this run. A touch during the tests that measure waking up discarded them." : "Not measured in this run: cancelled, or not available here.")
                    .font(.archivo(15))
                    .foregroundStyle(Color.muted)
                    .padding(20)
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
                    .background(Color.panel)
            } else {
                LazyVGrid(columns: [GridItem(.flexible(), spacing: 6), GridItem(.flexible())], spacing: 6) {
                    ForEach(shown) { test in
                        PageLink(OpenTest(test: test, run: run)) {
                            ScoreTile(test: test, value: test.read(run.doc)!)
                        }
                        .buttonStyle(.block)
                    }
                }
                Spacer(minLength: 0)
            }
        }
        .background(Color.ground)
        .onAppear {
            if let first = tests.first(where: { $0.read(run.doc) != nil }), !tests.contains(where: { $0.group == group && $0.read(run.doc) != nil }) {
                group = first.group
            }
        }
    }
}

struct ScoreTile: View {
    let test: Test
    let value: Value

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Text(test.unit)
                Spacer()
                Image(systemName: test.higherBetter ? "arrow.up" : "arrow.down")
                    .accessibilityLabel(test.higherBetter ? "Higher is better" : "Lower is better")
            }
            .font(.mono(11))
            .foregroundStyle(Color.muted)
            Text(format(value.v))
                .font(.saira(66, width: 50, weight: 600))
                .lineLimit(1)
                .minimumScaleFactor(0.6)
            Text(test.name)
                .font(.archivo(14, weight: 700))
                .lineLimit(2)
                .multilineTextAlignment(.leading)
                .frame(height: 36, alignment: .topLeading)
        }
        .padding(12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color.panel)
    }
}
