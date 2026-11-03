import SwiftUI

/// A phone stays upright. Something as wide as a tablet, like an open iPhone Duo or an iPad, turns freely.
final class AppDelegate: NSObject, UIApplicationDelegate {
    func application(_ application: UIApplication, supportedInterfaceOrientationsFor window: UIWindow?) -> UIInterfaceOrientationMask {
        guard let size = window?.bounds.size else { return .portrait }
        return min(size.width, size.height) >= largeSide ? .all : .portrait
    }
}

/// The shorter side, in points, from which the app uses its tablet layout.
let largeSide: CGFloat = 600

@main struct MyApp: App {
    @UIApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @State private var session = Session()

    init() {
        Fonts.register()
    }

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environment(session)
        }
    }
}
