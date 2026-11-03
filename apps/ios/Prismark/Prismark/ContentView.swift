import SwiftUI

enum Section {
    case run, results, device
}

struct ContentView: View {
    @Environment(Session.self) private var session
    @State private var section = Section.run

    var body: some View {
        GeometryReader { geo in
            let large = min(geo.size.width, geo.size.height) >= largeSide

            Group {
                if session.running {
                    MeasuringScreen()
                } else {
                    HStack(spacing: 6) {
                        switch section {
                        case .run:
                            if large { PadScreen() } else { RunScreen() }
                        case .results: RunsScreen()
                        case .device: DeviceScreen()
                        }
                        SideColumn(selection: $section)
                            .ignoresSafeArea()
                    }
                }
            }
            .background(Color.ground)
            .ignoresSafeArea()
            .foregroundStyle(Color.text)
            // Everything below reads the layout from this: on an iPhone Duo it changes as the phone unfolds.
            .environment(\.horizontalSizeClass, large ? .regular : .compact)
            .onChange(of: large) { _, _ in
                let scenes = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
                scenes.first?.keyWindow?.rootViewController?.setNeedsUpdateOfSupportedInterfaceOrientations()
            }
            .onChange(of: session.running) { _, running in
                UIApplication.shared.isIdleTimerDisabled = running
                if !running && session.current != nil && !large { section = .results }
            }
        }
        .ignoresSafeArea()
    }
}

#Preview {
    ContentView().environment(Session())
}
 
