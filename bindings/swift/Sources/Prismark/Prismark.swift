// Swift interface to the Prismark core for the iOS/iPadOS app (phase 5).
//
// Measurement isolation (spec 7.1): the runner executes in-process. Call
// run() off the main thread and show a frozen screen (no animations, no
// timers) until it returns; the app must stay in the foreground.
//
// Copyright 2026 The Prismark Authors. Apache-2.0.
import CPrismark
import Foundation

public struct PrismarkEvent: Sendable {
    public enum Kind: Int, Sendable { case phase = 0, info = 1, warning = 2 }
    public let kind: Kind
    public let phase: String?
    public let kernel: String?
    public let message: String
    public let step: Int
    public let steps: Int
    public let temperatureC: Double?
}

public struct PrismarkModes: OptionSet, Sendable {
    public let rawValue: UInt32
    public init(rawValue: UInt32) { self.rawValue = rawValue }
    public static let coldBurst = PrismarkModes(rawValue: 1 << 0)
    public static let periodic = PrismarkModes(rawValue: 1 << 1)
    public static let stBurst = PrismarkModes(rawValue: 1 << 2)
    public static let stSustained = PrismarkModes(rawValue: 1 << 3)
    public static let mcThreaded = PrismarkModes(rawValue: 1 << 4)
    public static let mcInstances = PrismarkModes(rawValue: 1 << 5)
    public static let all: PrismarkModes = [.coldBurst, .periodic, .stBurst, .stSustained, .mcThreaded, .mcInstances]
}

public enum PrismarkError: Error {
    case failed(code: Int32, message: String)
    case notComparable(String)
}

public enum Prismark {
    public static var version: String { String(cString: pmk_version()) }

    private final class Box {
        let handler: (PrismarkEvent) -> Void
        init(_ h: @escaping (PrismarkEvent) -> Void) { handler = h }
    }

    /// Blocking. Returns the result document (JSON); a cancelled run still returns its partial result.
    public static func run(modes: PrismarkModes = .all, kernels: String? = nil, quick: Bool = false,
                           onEvent: @escaping (PrismarkEvent) -> Void = { _ in }) throws -> String {
        var cfg = pmk_config()
        pmk_config_init(&cfg)
        cfg.modes = modes.rawValue
        if quick {
            cfg.max_reps = 20
            cfg.cold_max_reps = 20
            cfg.periodic_seconds = 10
            cfg.window_ms = 250
            cfg.sustained_min_s = 3
            cfg.sustained_max_s = 6
        }
        let box = Unmanaged.passRetained(Box(onEvent))
        defer { box.release() }
        var json: UnsafeMutablePointer<CChar>? = nil
        let rc: Int32 = "gui".withCString { fe in
            "frozen".withCString { ui in
                cfg.frontend = fe
                cfg.ui_state = ui
                let call = { (k: UnsafePointer<CChar>?) -> Int32 in
                    cfg.kernels = k
                    return pmk_start(&cfg, { ev, user in
                        guard let ev = ev?.pointee, let user = user else { return }
                        let box = Unmanaged<Box>.fromOpaque(user).takeUnretainedValue()
                        box.handler(PrismarkEvent(
                            kind: PrismarkEvent.Kind(rawValue: Int(ev.kind.rawValue)) ?? .info,
                            phase: ev.phase.map { String(cString: $0) },
                            kernel: ev.kernel.map { String(cString: $0) },
                            message: ev.message.map { String(cString: $0) } ?? "",
                            step: Int(ev.step), steps: Int(ev.steps),
                            temperatureC: ev.temp_c.isFinite ? ev.temp_c : nil))
                    }, box.toOpaque(), &json, nil)
                }
                if let kernels { return kernels.withCString { call($0) } }
                return call(nil)
            }
        }
        defer { pmk_free(json) }
        guard let json else { throw PrismarkError.failed(code: rc, message: String(cString: pmk_strerror(rc))) }
        return String(cString: json)
    }

    public static func cancel() { pmk_cancel() }

    /// Compares two result documents (A against B) and computes the profiles.
    public static func compare(_ a: String, _ b: String, profiles: String? = nil) throws -> String {
        var json: UnsafeMutablePointer<CChar>? = nil
        var text: UnsafeMutablePointer<CChar>? = nil
        let rc = a.withCString { pa in
            b.withCString { pb in
                if let profiles { return profiles.withCString { pmk_compare(pa, pb, $0, &json, &text) } }
                return pmk_compare(pa, pb, nil, &json, &text)
            }
        }
        defer { pmk_free(json); pmk_free(text) }
        guard rc == 0, let json else {
            throw PrismarkError.notComparable(text.map { String(cString: $0) } ?? String(cString: pmk_strerror(rc)))
        }
        return String(cString: json)
    }
}
