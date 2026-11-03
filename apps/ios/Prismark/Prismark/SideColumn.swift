import SwiftUI

struct SideColumn: View {
    @Binding var selection: Section
    @Environment(\.horizontalSizeClass) private var sizeClass

    private var pillHeight: CGFloat { statusPillHeight() }

    var body: some View {
        VStack(spacing: 6) {
            ColumnTab(number: "01", name: sizeClass == .regular ? "TESTS" : "RUN", section: .run, selection: $selection, top: max(62, pillHeight), bottom: 14)
                .frame(minHeight: pillHeight > 0 ? pillHeight + 125 : nil)
            ColumnTab(number: "02", name: "RESULTS", section: .results, selection: $selection, top: 14, bottom: 14)
            ColumnTab(number: "03", name: "DEVICE", section: .device, selection: $selection, top: 14, bottom: 40)
        }
        .frame(width: 66)
    }
}

struct ColumnTab: View {
    let number: String
    let name: String
    let section: Section
    @Binding var selection: Section
    let top: CGFloat
    let bottom: CGFloat

    var body: some View {
        let isCurrent = selection == section

        Button {
            selection = section
        } label: {
            VStack {
                Text(number).font(.mono(11))
                Spacer()
                Text(name)
                    .font(.saira(19, width: 125, weight: 800))
                    .fixedSize()
                    .frame(width: 24, height: 96)
                    .rotationEffect(.degrees(-90))
            }
            .padding(.top, top)
            .padding(.bottom, bottom)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background(isCurrent ? Color.panel : Color.steel)
            .overlay(alignment: .leading) {
                if isCurrent { Color.volt.frame(width: 6) }
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.block)
        .accessibilityLabel(name.capitalized)
        .accessibilityAddTraits(isCurrent ? .isSelected : [])
    }
}
