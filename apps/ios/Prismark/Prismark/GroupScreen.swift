import SwiftUI

/// Pick the tests of one group.
struct GroupScreen: View {
    @Environment(Session.self) private var session
    let group: TestGroup

    var body: some View {
        let members = tests.filter { $0.group == group.id }

        VStack(spacing: 6) {
            VStack(alignment: .leading, spacing: 10) {
                HStack {
                    BackButton()
                    Spacer()
                    Mono(text: "Tests · \(group.number) of 04").foregroundStyle(Color.muted)
                }
                Text(group.label)
                    .font(.saira(group.label.count > 10 ? 28 : 36, width: 112))
                    .lineLimit(1)
                    .minimumScaleFactor(0.7)
                Text(group.about)
                    .font(.archivo(14))
                    .foregroundStyle(Color.muted)
                    .fixedSize(horizontal: false, vertical: true)
            }
            .padding(.top, 62)
            .padding(.horizontal, 20)
            .padding(.bottom, 16)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(Color.panel)

            ForEach(members) { test in
                let on = session.picked.contains(test.id)
                Button {
                    if on { session.picked.remove(test.id) } else { session.picked.insert(test.id) }
                } label: {
                    HStack(spacing: 14) {
                        Check(on: on)
                        VStack(alignment: .leading, spacing: 2) {
                            Text(test.name).font(.archivo(16, weight: 700))
                            Text(test.sub).font(.archivo(13))
                        }
                        Spacer(minLength: 0)
                    }
                    .padding(.horizontal, 16)
                    .frame(maxWidth: .infinity, minHeight: 60, maxHeight: 90)
                    .background(on ? Color.volt : Color.panel)
                    .foregroundStyle(on ? Color.ink : Color.text)
                }
                .buttonStyle(.block)
                .accessibilityAddTraits(on ? .isSelected : [])
            }
            Spacer(minLength: 0)
        }
        .background(Color.ground)
    }
}
