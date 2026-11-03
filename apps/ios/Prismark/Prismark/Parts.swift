import SwiftUI

/// Buttons here are flat blocks: the whole block takes the tap, not only the words on it.
struct BlockButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .contentShape(Rectangle())
            .opacity(configuration.isPressed ? 0.7 : 1)
    }
}

extension ButtonStyle where Self == BlockButtonStyle {
    static var block: BlockButtonStyle { BlockButtonStyle() }
}

/// A small label in mono capitals, like TESTS · ONE CORE.
struct Mono: View {
    let text: String
    var body: some View {
        Text(text.uppercased())
            .font(.mono(11))
            .tracking(0.6)
    }
}

/// The square check: an outline when off, an ink box with a volt centre when on.
struct Check: View {
    let on: Bool
    var body: some View {
        ZStack {
            Rectangle()
                .strokeBorder(on ? Color.ink : Color.muted, lineWidth: 2)
                .background(on ? Color.ink : Color.clear)
            if on {
                Color.volt.frame(width: 10, height: 10)
            }
        }
        .frame(width: 24, height: 24)
    }
}

/// The three chevrons on the main action.
struct Chevrons: View {
    var body: some View {
        HStack(spacing: -3) {
            ForEach(0..<3, id: \.self) { _ in
                Image(systemName: "chevron.right")
                    .font(.system(size: 14, weight: .black))
            }
        }
    }
}

/// Bars drawn from a run's own ID, so every run has its own barcode.
struct Barcode: View {
    let id: String
    var vertical = false
    var bars = 24

    var body: some View {
        let digits = id.compactMap { $0.hexDigitValue }.prefix(bars)
        let lines = Array(digits.enumerated())
        let layout = vertical ? AnyLayout(VStackLayout(spacing: 0)) : AnyLayout(HStackLayout(spacing: 0))
        layout {
            ForEach(lines, id: \.offset) { index, digit in
                let size = CGFloat(1 + (digit + index) % 4)
                let gap = CGFloat(1 + (digit * 3 + index) % 3)
                Rectangle()
                    .frame(width: vertical ? nil : size, height: vertical ? size : nil)
                    .padding(vertical ? .bottom : .trailing, gap)
            }
        }
        .accessibilityHidden(true)
    }
}

/// Shares a result file. The system's sheet can take seconds to appear, so the button says it is working until then.
struct ShareFileButton<Label: View>: View {
    let file: URL
    @ViewBuilder let label: (_ busy: Bool) -> Label
    @State private var busy = false

    var body: some View {
        Button {
            guard !busy else { return }
            busy = true
            let scene = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.first
            guard var top = scene?.keyWindow?.rootViewController else {
                busy = false
                return
            }
            while let next = top.presentedViewController { top = next }
            let sheet = UIActivityViewController(activityItems: [file], applicationActivities: nil)
            sheet.popoverPresentationController?.sourceView = top.view
            sheet.popoverPresentationController?.sourceRect = CGRect(x: top.view.bounds.midX, y: top.view.bounds.midY, width: 1, height: 1)
            sheet.popoverPresentationController?.permittedArrowDirections = []
            top.present(sheet, animated: true) { busy = false }
        } label: {
            label(busy)
        }
        .buttonStyle(.block)
        .accessibilityLabel(busy ? "Preparing to share the result file" : "Share the result file")
    }
}

/// A folded iPhone Duo keeps its status pill down the top of the right side. Its height, or 0 on anything else.
func statusPillHeight() -> CGFloat {
    let window = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.first?.keyWindow
    guard let window, window.safeAreaInsets.right > 0, window.bounds.height > window.bounds.width else { return 0 }
    return 165
}

/// The processor's name, as big as the design draws it. Pro, Max and Ultra stand on their side to the right of the
/// number; longer words like Bionic go small underneath.
struct ChipName: View {
    let machine: Machine
    let size: CGFloat

    // Saira's height above and below the line, and of its capitals, as parts of the font size.
    private let ascent: CGFloat = 1.135
    private let descent: CGFloat = 0.439
    private let capital: CGFloat = 0.688

    var body: some View {
        let onSide = ["PRO", "MAX", "ULTRA"].contains(machine.suffix)

        VStack(alignment: .leading, spacing: max(6, size * 0.05)) {
            HStack(alignment: .bottom, spacing: size * 0.06) {
                Text(machine.chip)
                    .font(.saira(size, width: 50, weight: 600))
                    .foregroundStyle(Color.ink)
                    .lineLimit(1)
                    .minimumScaleFactor(0.25)
                    .fixedSize(horizontal: false, vertical: true)
                    // The box is the capitals alone, so nothing is scaled down to fit the font's whole line.
                    .padding(.top, -(ascent - capital) * size)
                    .padding(.bottom, -descent * size)
                if onSide {
                    Text(machine.suffix)
                        .font(.saira(size * 0.16, width: 125, weight: 800))
                        .fixedSize()
                        .frame(width: capital * size, height: size * 0.16, alignment: .trailing)
                        .rotationEffect(.degrees(90))
                        .frame(width: size * 0.16, height: capital * size)
                }
            }
            if !onSide && !machine.suffix.isEmpty {
                Text(machine.suffix)
                    .font(.saira(max(13, size * 0.125), width: 125, weight: 800))
                    .lineLimit(1)
            }
        }
    }
}

/// The pass's printed dots.
struct PassDots: View {
    var body: some View {
        Canvas { context, size in
            let dot = Color(hex: 0x5C80FF)
            for x in stride(from: CGFloat(12), to: size.width, by: 24) {
                for y in stride(from: CGFloat(12), to: size.height, by: 24) {
                    context.fill(Path(ellipseIn: CGRect(x: x - 1.2, y: y - 1.2, width: 2.4, height: 2.4)), with: .color(dot))
                }
            }
        }
        .background(Color.signal)
        .accessibilityHidden(true)
    }
}

/// The back button, a square 44 points wide. On a volt header it is ink, like the share button beside it.
struct BackButton: View {
    @Environment(Pages.self) private var pages
    var onVolt = false

    var body: some View {
        Button { pages.pop() } label: {
            Image(systemName: "chevron.left")
                .font(.system(size: 16, weight: .bold))
                .frame(width: 44, height: 44)
                .foregroundStyle(onVolt ? Color.volt : Color.text)
                .background(onVolt ? Color.ink : Color.raised, ignoresSafeAreaEdges: [])
        }
        .buttonStyle(.block)
        .accessibilityLabel("Back")
    }
}

/// A hidden touch: tapping the logo swaps dark and light, whatever the system is set to. Coming back to the system's
/// own look follows the system again.
struct LogoButton<Label: View>: View {
    @Environment(\.colorScheme) private var scheme
    @AppStorage("appearance") private var appearance = ""
    @ViewBuilder let label: Label

    var body: some View {
        Button {
            let scene = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.first
            let system = scene?.screen.traitCollection.userInterfaceStyle == .dark ? "dark" : "light"
            let swapped = scheme == .dark ? "light" : "dark"
            appearance = swapped == system ? "" : swapped
        } label: {
            label
        }
        .buttonStyle(.block)
        .accessibilityLabel("Prismark. Swap dark and light")
    }
}

/// The logo: a beam enters a prism and leaves as three bars. Drawn on a 64 by 64 grid.
struct PrismMark: View {
    var body: some View {
        Canvas { context, size in
            context.scaleBy(x: size.width / 64, y: size.height / 64)
            var beam = Path()
            beam.move(to: CGPoint(x: 0, y: 37))
            beam.addLine(to: CGPoint(x: 21.5, y: 34))
            context.stroke(beam, with: .color(.ink), lineWidth: 3)
            var prism = Path()
            prism.addLines([CGPoint(x: 12, y: 52), CGPoint(x: 32, y: 12), CGPoint(x: 52, y: 52)])
            context.fill(prism, with: .color(.ink))
            context.fill(Path(CGRect(x: 42, y: 25, width: 22, height: 5)), with: .color(.ink))
            context.fill(Path(CGRect(x: 46.5, y: 34, width: 17.5, height: 5)), with: .color(.signal))
            context.fill(Path(CGRect(x: 51, y: 43, width: 13, height: 5)), with: .color(.ink))
        }
        .accessibilityHidden(true)
    }
}
