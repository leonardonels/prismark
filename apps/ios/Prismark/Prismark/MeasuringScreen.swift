import SwiftUI

/// Quiet while measuring: no animation and no timer. The text changes only between steps.
struct MeasuringScreen: View {
    @Environment(Session.self) private var session
    @Environment(\.horizontalSizeClass) private var sizeClass
    @State private var asking = false

    var body: some View {
        let number = String(format: "%02d", session.part)

        ZStack(alignment: .bottom) {
            VStack(alignment: .leading, spacing: 16) {
                HStack {
                    Text("RUN / MEASURING")
                    Spacer()
                    Text(session.stressing ? "STRESS TEST" : session.quick ? "QUICK RUN" : "FULL RUN")
                        .padding(.trailing, statusPillHeight() > 0 ? 62 : 0)
                }
                .font(.mono(12, semibold: true))

                HStack(alignment: .top, spacing: 0) {
                    Text(number)
                        .font(.saira(236, width: 50, weight: 600))
                        .fixedSize()
                        .frame(width: 190, height: 130)
                        .rotationEffect(.degrees(90))
                        .padding(.top, 18)
                    VStack(alignment: .leading, spacing: 10) {
                        Spacer()
                        Text("NOW").font(.mono(11))
                        Text(session.phase).font(.saira(25, width: 100)).lineLimit(4).minimumScaleFactor(0.7)
                        if session.parts > 0 { Text("Step \(session.part) of \(session.parts)").font(.mono(12)) }
                    }
                    .padding(.vertical, 18)
                    .padding(.trailing, 14)
                    Spacer(minLength: 0)
                }
                .foregroundStyle(Color.ink)
                .background(Color.volt, ignoresSafeAreaEdges: [])

                Text(session.stressing ? "It will get warm. That is the test." : "Leave it on a table and do not touch it.")
                    .font(.archivo(19, weight: 600))
                Text(note)
                    .font(.archivo(14))
                    .fixedSize(horizontal: false, vertical: true)

                Button {
                    // Stopping is how a stress test without a set length ends, so there is nothing to confirm.
                    if session.stressOpen {
                        session.phase = "Stopping"
                        Prismark.cancel()
                    } else {
                        asking = true
                    }
                } label: {
                    Text(session.stressOpen ? "Stop stress test" : "Cancel run")
                        .font(.archivo(16, weight: 600))
                        .frame(maxWidth: .infinity)
                        .frame(height: 60)
                        .overlay(Rectangle().strokeBorder(Color.onSignal, lineWidth: 2))
                }
                .buttonStyle(.block)
            }
            .padding(.top, 62)
            .padding(.horizontal, 20)
            .padding(.bottom, 34)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .foregroundStyle(Color.onSignal)
            .background(Color.signal)

            if asking {
                Color.ink.opacity(0.78)
                CancelStub(asking: $asking)
            }
        }
        // Every touch reaches the core first, which skips the quiet tests.
        .simultaneousGesture(DragGesture(minimumDistance: 0).onChanged { _ in Prismark.notifyInput() })
    }
}

private extension MeasuringScreen {
    var note: String {
        if session.stressOpen {
            return "It runs until you stop it, and everything so far is kept. A score needs 10 minutes. It uses a lot of battery; touching the screen does no harm."
        }
        if session.stressing {
            return "\(session.stressMinutes) minutes at full load. It uses a lot of battery, and the system slows it down if it gets too hot. Touching the screen does no harm."
        }
        guard let minutes = session.quietMinutes else {
            return "Any touch during the tests that measure waking up discards them, even on Cancel run. The screen stays on."
        }
        return "Up to about \(minutes) min left. The step number moves only between tests, so the screen cannot disturb them. Any touch, even on Cancel run, discards them."
    }
}

/// A dialog is a stub torn off the pass: a code, one sentence, two buttons.
struct CancelStub: View {
    @Environment(Session.self) private var session
    @Binding var asking: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack {
                Text("RUN / CANCEL")
                Spacer()
                Text("Step \(session.part) of \(session.parts)")
            }
            .font(.mono(12, semibold: true))
            .foregroundStyle(Color.onSignal)
            .padding(.horizontal, 20)
            .padding(.vertical, 14)
            .background(Color.signal)

            VStack(alignment: .leading, spacing: 10) {
                Text("Cancel the run?").font(.saira(30, width: 112))
                Text("The tests finished so far are kept as a partial result. It is not ranked.")
                    .font(.archivo(15))
                    .foregroundStyle(Color.muted)
                    .fixedSize(horizontal: false, vertical: true)
            }
            .padding(20)

            Rectangle()
                .stroke(style: StrokeStyle(lineWidth: 2, dash: [6, 5]))
                .foregroundStyle(Color.line)
                .frame(height: 1)

            HStack(spacing: 6) {
                Button {
                    asking = false
                    Prismark.cancel()
                } label: {
                    Text("Cancel run")
                        .font(.archivo(15, weight: 600))
                        .frame(maxWidth: .infinity)
                        .frame(height: 56)
                        .overlay(Rectangle().strokeBorder(Color.text, lineWidth: 2))
                }
                Button {
                    asking = false
                } label: {
                    Text("Keep measuring")
                        .font(.archivo(15, weight: 700))
                        .foregroundStyle(Color.ink)
                        .frame(maxWidth: .infinity)
                        .frame(height: 56)
                        .background(Color.volt, ignoresSafeAreaEdges: [])
                }
            }
            .buttonStyle(.block)
            .padding(.horizontal, 20)
            .padding(.top, 14)
            .padding(.bottom, 34)
        }
        .foregroundStyle(Color.text)
        .background(Color.panel)
    }
}
