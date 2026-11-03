import SwiftUI

/// Your saved runs, each one a stub with its own barcode. Newest first.
struct RunsScreen: View {
    @Environment(Session.self) private var session
    @Environment(\.horizontalSizeClass) private var sizeClass

    var body: some View {
        PageStack(destination: { page in
            if let open = page.base as? OpenTest {
                AnyView(TestScreen(test: open.test, run: open.run))
            } else {
                AnyView(ResultsScreen(run: page.base as! Run))
            }
        }) {
            VStack(spacing: 6) {
                HStack(alignment: .firstTextBaseline) {
                    Text("Your runs").font(.saira(36, width: 112))
                    Spacer()
                    Mono(text: "\(session.saved.count) saved").foregroundStyle(Color.muted)
                }
                .padding(.top, 62)
                .padding(.horizontal, 20)
                .padding(.bottom, 14)
                .background(Color.panel)

                if session.saved.isEmpty {
                    Text("No runs yet. Your results will be kept here.")
                        .font(.archivo(16))
                        .foregroundStyle(Color.muted)
                        .padding(20)
                        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
                        .background(Color.panel)
                } else {
                    ScrollView {
                        LazyVGrid(columns: [GridItem(.adaptive(minimum: sizeClass == .regular ? 340 : 280), spacing: 6)], spacing: 6) {
                            ForEach(session.saved) { run in
                                PageLink(run) {
                                    RunStub(run: run, latest: run == session.saved.first)
                                }
                                .buttonStyle(.block)
                            }
                        }
                        .padding(.bottom, 34)
                    }
                }
            }
            .background(Color.ground)
        }
    }
}

struct RunStub: View {
    let run: Run
    let latest: Bool

    var body: some View {
        HStack(spacing: 0) {
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    Text(run.started?.formatted(.dateTime.year().hour().minute()) ?? "")
                    Spacer()
                    Text("\(run.measured) of \(tests.count)")
                }
                .font(.mono(11))
                Text(run.started?.formatted(.dateTime.day().month(.abbreviated)).uppercased() ?? "—")
                    .font(.saira(62, width: 50, weight: 600))
                    .lineLimit(1)
                HStack(spacing: 4) {
                    Tag(text: run.quick ? "QUICK" : "FULL", latest: latest)
                    if !run.complete { Tag(text: "PARTIAL", latest: latest) }
                    if run.busy { Tag(text: "BUSY", latest: latest) }
                    if run.skipped { Tag(text: "SKIPPED", latest: latest) }
                }
            }
            .padding(14)
            Barcode(id: run.runID, vertical: true)
                .frame(width: 44, height: 110, alignment: .top)
                .clipped()
                .padding(.horizontal, 12)
                .frame(maxHeight: .infinity)
                .overlay(alignment: .leading) {
                    Rectangle()
                        .stroke(style: StrokeStyle(lineWidth: 2, dash: [6, 5]))
                        .frame(width: 1)
                }
        }
        .foregroundStyle(latest ? Color.ink : Color.text)
        .background(latest ? Color.volt : Color.panel)
    }
}

struct Tag: View {
    let text: String
    let latest: Bool
    var body: some View {
        Text(text)
            .font(.mono(11))
            .padding(.horizontal, 7)
            .padding(.vertical, 3)
            .foregroundStyle(latest ? Color.volt : Color.text)
            .background(latest ? Color.ink : Color.raised)
    }
}
