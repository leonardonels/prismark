// What the app knows: the picked tests, the run in progress, the saved runs.
import Foundation
import Observation

struct Run: Identifiable, Hashable {
    let file: URL
    let doc: Doc
    var id: String { file.lastPathComponent }

    static func == (a: Run, b: Run) -> Bool { a.id == b.id }
    func hash(into hasher: inout Hasher) { hasher.combine(id) }

    var runID: String { doc["run_id"] as? String ?? "0000" }
    var started: Date? { ISO8601DateFormatter().date(from: doc["started_utc"] as? String ?? "") }
    var quick: Bool { (doc["config"] as? Doc)?["quick"] as? Bool == true }
    var complete: Bool { doc["complete"] as? Bool == true }
    var busy: Bool {
        guard let load = ((doc["preflight"] as? Doc)?["background_load"] as? NSNumber)?.doubleValue else { return false }
        return load > 0.05
    }
    var measured: Int { tests.filter { $0.read(doc) != nil }.count }
    var skipped: Bool { !(doc["skipped_modes"] as? [Any] ?? []).isEmpty }
}

@Observable final class Session {
    var picked = Set(tests.map(\.id))
    var quick = false
    var running = false
    var phase = ""
    var part = 0
    var parts = 0
    /// Minutes the run may still take, set at each step of the quiet tests. Nil outside them.
    var quietMinutes: Int?
    var error: String?
    var current: Run?
    var saved: [Run] = []
    let info: Doc

    init() {
        info = Prismark.info()
        loadSaved()
    }

    private var folder: URL {
        let url = URL.documentsDirectory.appending(path: "runs")
        try? FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }

    func loadSaved() {
        let files = (try? FileManager.default.contentsOfDirectory(at: folder, includingPropertiesForKeys: nil)) ?? []
        saved = files
            .filter { $0.pathExtension == "json" }
            .compactMap(read)
            .sorted { ($0.started ?? .distantPast) > ($1.started ?? .distantPast) }
    }

    private func read(_ file: URL) -> Run? {
        guard let data = try? Data(contentsOf: file),
              let doc = try? JSONSerialization.jsonObject(with: data) as? Doc else { return nil }
        return Run(file: file, doc: doc)
    }

    /// Runs the picked tests on a worker thread. The screen changes once per step, and in the quiet phases only between tests.
    func start(only: [Test]? = nil) {
        let chosen = only ?? tests.filter { picked.contains($0.id) }
        guard !running, !chosen.isEmpty else { return }
        let modes = chosen.reduce(into: PrismarkModes()) { $0.insert($1.mode) }
        let kernels = Array(Set(chosen.map(\.kernel))).sorted().joined(separator: ",")
        let quick = quick
        let folder = folder
        running = true
        error = nil
        phase = "Starting"
        part = 0
        parts = 0
        quietMinutes = nil

        let worker = Thread { [self] in
            var last = ""
            do {
                let json = try Prismark.run(modes: modes, kernels: kernels, quick: quick) { event in
                    if event.kind == .phase, let name = event.phase, !(name == last && quietPhases.contains(name)) {
                        last = name
                        DispatchQueue.main.async { self.phase = phaseNames[name] ?? name }
                    } else if event.kind == .info, event.phase == "progress" {
                        let shown = event.step
                        let total = event.steps
                        let quiet = event.message.hasSuffix("quiet") || quietPhases.contains(last)
                        let minutes = quiet ? event.remainingSeconds.map { max(1, Int(($0 / 60).rounded(.up))) } : nil
                        DispatchQueue.main.async {
                            self.part = shown
                            self.parts = total
                            self.quietMinutes = minutes
                        }
                    }
                }
                let file = folder.appending(path: "prismark-\(Int(Date().timeIntervalSince1970)).json")
                try json.write(to: file, atomically: true, encoding: .utf8)
                DispatchQueue.main.async {
                    self.loadSaved()
                    self.current = self.saved.first
                    self.running = false
                }
            } catch {
                let message = "\(error)"
                DispatchQueue.main.async {
                    self.error = message
                    self.running = false
                }
            }
        }
        worker.qualityOfService = .userInteractive
        worker.start()
    }
}

/// The other machines, from the same reduced file the website uses.
let references: [Doc] = {
    guard let url = Bundle.main.url(forResource: "references", withExtension: "json"),
          let data = try? Data(contentsOf: url) else { return [] }
    return (try? JSONSerialization.jsonObject(with: data) as? [Doc]) ?? []
}()

struct Rank: Identifiable {
    let id = UUID()
    let name: String
    let os: String
    let value: Value
    let yours: Bool
    let runID: String
}

/// One test, every machine, best first.
func ranking(for test: Test, run: Run?) -> [Rank] {
    var rows = references.compactMap { doc -> Rank? in
        guard let value = test.read(doc), let machine = doc["machine"] as? Doc else { return nil }
        let cpu = (machine["cpu"] as? Doc)?["model"] as? String ?? "?"
        return Rank(name: shortName(cpu), os: machine["os"] as? String ?? "", value: value, yours: false, runID: doc["run_id"] as? String ?? "")
    }
    if let run, let value = test.read(run.doc) {
        rows.append(Rank(name: "This device", os: "", value: value, yours: true, runID: run.runID))
    }
    return rows.sorted { test.higherBetter ? $0.value.v > $1.value.v : $0.value.v < $1.value.v }
}

func shortName(_ model: String) -> String {
    var name = model
    for junk in ["(R)", "(TM)", "AMD ", "Intel ", " Processor", "Genuine ", "Ryzen Threadripper PRO", "Apple "] {
        name = name.replacingOccurrences(of: junk, with: junk.hasPrefix("Ryzen T") ? "Threadripper" : "")
    }
    if let cut = name.range(of: " CPU") ?? name.range(of: " w/") ?? name.range(of: " 6-Core") ?? name.range(of: " 64-Cores") {
        name = String(name[..<cut.lowerBound])
    }
    return name
}
