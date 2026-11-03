import CoreText
import SwiftUI

extension Color {
    init(hex: UInt32) {
        self.init(
            red: Double((hex >> 16) & 0xFF) / 255,
            green: Double((hex >> 8) & 0xFF) / 255,
            blue: Double(hex & 0xFF) / 255
        )
    }

    // The same in light and dark.
    static let ink = Color(hex: 0x0C1430)
    static let signal = Color(hex: 0x2D5BFF)
    static let volt = Color(hex: 0xD6F53B)
    static let fault = Color(hex: 0xFF4F3A)
    static let onSignal = Color(hex: 0xF7F8F9)
    static let signalLine = Color(hex: 0x9DB3FF)

    // These follow the system's light or dark mode.
    init(light: UInt32, dark: UInt32) {
        self.init(uiColor: UIColor { traits in
            UIColor(Color(hex: traits.userInterfaceStyle == .dark ? dark : light))
        })
    }

    static let ground = Color(light: 0x0C1430, dark: 0x070C1F)
    static let panel = Color(light: 0xF7F8F9, dark: 0x141D3B)
    static let raised = Color(light: 0xE8ECEF, dark: 0x1F2B52)
    static let text = Color(light: 0x0C1430, dark: 0xE8ECEF)
    static let muted = Color(light: 0x3B4660, dark: 0xAEB8D6)
    static let line = Color(light: 0xC9D0DA, dark: 0x2A3760)
    static let steel = Color(light: 0xC9D0DA, dark: 0x0F1730)
    static let reference = Color(light: 0x2D5BFF, dark: 0x5C80FF)
    static let yours = Color(light: 0x0C1430, dark: 0xD6F53B)
}

enum Fonts {
    /// Makes the font files in the app available by name. Called once, at launch.
    static func register() {
        let files = Bundle.main.urls(forResourcesWithExtension: "ttf", subdirectory: nil) ?? []
        for file in files {
            CTFontManagerRegisterFontsForURL(file as CFURL, .process, nil)
        }
    }
}

extension Font {
    /// Saira, with its two axes: width (50 condensed to 125 wide) and weight.
    static func saira(_ size: CGFloat, width: CGFloat = 100, weight: CGFloat = 700) -> Font {
        variable("Saira", size, width: width, weight: weight)
    }

    /// Archivo, for running text and names.
    static func archivo(_ size: CGFloat, weight: CGFloat = 400) -> Font {
        variable("Archivo", size, width: 100, weight: weight)
    }

    /// IBM Plex Mono, for labels, units and ranges.
    static func mono(_ size: CGFloat, semibold: Bool = false) -> Font {
        .custom(semibold ? "IBMPlexMono-SemiBold" : "IBMPlexMono-Regular", fixedSize: size)
    }

    private static func variable(_ family: String, _ size: CGFloat, width: CGFloat, weight: CGFloat) -> Font {
        let axes: [UInt32: CGFloat] = [0x77647468: width, 0x77676874: weight] // 'wdth' and 'wght'
        let attributes: [CFString: Any] = [kCTFontFamilyNameAttribute: family, kCTFontVariationAttribute: axes]
        let descriptor = CTFontDescriptorCreateWithAttributes(attributes as CFDictionary)
        return Font(CTFontCreateWithFontDescriptor(descriptor, size, nil))
    }
}
