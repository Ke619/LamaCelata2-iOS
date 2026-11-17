// The launcher: a settings screen shown before the game starts, the same for
// every game and for the Mac and iOS. Its choices become the port's
// environment switches (SCIMITAR_WINDOWED, SCIMITAR_ASPECT, ...) of this
// process, which the port reads as the game starts, and are kept in the app's
// user defaults. The C entry points are in launcher.h.

import SwiftUI

#if os(macOS)
import AppKit
#else
import UIKit
import UniformTypeIdentifiers
#endif

// What the launcher knows about the game (game::Info, games/common/game.h).
struct LauncherGame {
    let id: String
    let title: String
    let appEnv: String  // the environment variable naming the game's .app (or folder)
    let appName: String  // the .app's (or folder's) usual name
    let defaultApp: String  // where the build found it (Mac) or the Documents copy (iOS)
    // The file the port runs, relative to the game's location; the first one
    // there counts (game::Info::game_file, '|' between alternatives).
    let gameFiles: [String]
    // The settings the port supports (game::Info::launcher_settings); nil: all.
    // "windowed": Mode, as SCIMITAR_WINDOWED=1 for a window (the Mac ports);
    // "full-screen": Mode, as SCIMITAR_FULL_SCREEN=1 for full screen (the
    // Windows ports, which start in a window); "aspect", "stats", "intro",
    // "languages", "prompts", "options"; "graphics": the F4 menu's switches;
    // "aniso": anisotropic filtering alone; "metal": the Metal renderer and
    // its shader precompile.
    let settings: Set<String>?

    func supports(_ setting: String) -> Bool { settings?.contains(setting) ?? true }
    // A Mac port, which runs the Mac release's scimitar_sf (not a Windows .exe).
    var isMacGame: Bool { gameFiles == ["Contents/MacOS/scimitar_sf"] }
}

// The languages of the games' Mac releases (both have these voices,
// Contents/Resources/sounddata), by scimitar::Language's short names.
let kLanguages: [(code: String, name: String)] = [
    ("", "Default"), ("eng", "English"), ("fre", "French"), ("ger", "German"),
    ("ita", "Italian"), ("spa", "Spanish"), ("rus", "Russian"),
]

// Their data also has text (no voices) in some of these.
let kTextLanguages = kLanguages + [
    ("dan", "Danish"), ("dut", "Dutch"), ("nor", "Norwegian"), ("pol", "Polish"), ("swe", "Swedish"),
]

// scimitar::Language's numbers, for localization.lang's bit masks.
let kLanguageNumbers: [String: Int] = [
    "eng": 1, "fre": 2, "spa": 3, "pol": 4, "ger": 5, "ita": 8, "rus": 12, "dut": 13, "dan": 14, "nor": 15, "swe": 16,
]

let kPrompts: [(code: String, name: String)] = [
    ("", "Follow the controller"), ("keyboard", "Keyboard and mouse"), ("xbox", "Xbox"),
    ("playstation", "PlayStation"),
]

// The Graphics section: the switches of the F4 graphics menu's settings
// (games/common/graphics_menu.cpp), with the same values. A choice other than
// "Game setting" is kept in effect over the game's own graphics options.
struct GraphicsSetting {
    let env: String
    let name: String
    let values: [(value: String, name: String)]
}

// Those the game supports: all, or anisotropic filtering alone (the Windows
// ports' Direct3D layer, platform/win32/d3d9_metal.mm), or none.
func graphicsSettings(_ game: LauncherGame) -> [GraphicsSetting] {
    let all = allGraphicsSettings(game.id)
    if game.supports("graphics") { return all }
    return game.supports("aniso") ? all.filter { $0.env == "SCIMITAR_ANISO" } : []
}

func allGraphicsSettings(_ gameID: String) -> [GraphicsSetting] {
    let acb = gameID == "acb"
    func list(_ v: [String], low: String, high: String) -> [(value: String, name: String)] {
        v.map { ($0, $0.replacingOccurrences(of: "/", with: " / ")
            + ($0 == low ? " (game: lowest)" : $0 == high ? " (game: highest)" : "")) }
    }
    return [
        GraphicsSetting(env: "SCIMITAR_ANISO", name: "Anisotropic filtering",
                        values: [("2", "2x"), ("4", "4x"), ("8", "8x"), ("16", "16x")]),
        GraphicsSetting(env: "SCIMITAR_DRAW_DISTANCE", name: "Draw distance",
                        values: acb ? list(["0.5", "0.8", "1.0", "1.5", "2.0", "3.0", "4.0", "6.0"], low: "0.5", high: "2.0")
                                    : list(["0.5", "1.0", "1.5", "2.0", "3.0", "4.0", "6.0"], low: "", high: "1.0")
                                        .map { ($0.value, $0.value == "1.0" ? "1.0 (game)" : $0.name) }),
        // Brotherhood multiplies the characters' detail distances, AC2 their distance.
        GraphicsSetting(env: "SCIMITAR_CHARACTER_DETAIL", name: "Character detail",
                        values: acb ? list(["1.0", "1.3", "1.6", "2.0", "3.0", "4.0"], low: "1.0", high: "1.6")
                                    : list(["1.6", "1.3", "1.0", "0.75", "0.5", "0.33"], low: "1.6", high: "1.0")),
        GraphicsSetting(env: "SCIMITAR_ENV_DISTANCES", name: "Environment distances",
                        values: list(["35/55", "40/60", "45/65"] + (acb ? ["60/80", "70/90"] : ["50/70", "70/90"])
                                         + ["100/130", "150/200", "250/300"],
                                     low: "35/55", high: acb ? "70/90" : "50/70")),
        GraphicsSetting(env: "SCIMITAR_SHADOW_DISTANCES", name: "Shadow distances",
                        values: acb ? list(["9/20", "12/25", "15/30", "20/40", "30/60", "40/80", "60/120"], low: "9/20", high: "20/40")
                                    : list(["9/15", "9/20", "12/25", "15/30", "20/40", "30/60", "45/90"], low: "9/15", high: "15/30")),
        GraphicsSetting(env: "SCIMITAR_SHADOW_MAP", name: "Shadow map size",
                        values: list(["512", "1024", "2048", "4096"], low: "512", high: acb ? "2048" : "1024")),
    ]
}

final class LauncherSettings: ObservableObject {
    let game: LauncherGame
    @Published var appPath: String { didSet { if appPath != oldValue { readLanguages() } } }
    @Published var fullScreen: Bool
    @Published var fillScreen: Bool
    @Published var textLanguage: String
    @Published var voiceLanguage: String
    @Published var prompts: String
    @Published var stats: Bool
    @Published var skipIntro: Bool
    @Published var noColorFilter: Bool  // AC2: the color filter off (SCIMITAR_NO_COLOR_FILTER)
    @Published var frameRate: Int  // iOS: the most frames a second (24, 30, 45, 60), 0: the screen's own rate
    @Published var options: String
    @Published var showAtStart: Bool
    @Published var graphics: [String: String]  // switch -> value ("" or absent: the game's setting)

    private let defaults = UserDefaults.standard
    private func key(_ name: String) -> String { "\(game.id).\(name)" }

    init(game: LauncherGame) {
        self.game = game
        let d = UserDefaults.standard
        func bool(_ name: String, _ fallback: Bool) -> Bool { d.object(forKey: "\(game.id).\(name)") as? Bool ?? fallback }
        func string(_ name: String) -> String { d.string(forKey: "\(game.id).\(name)") ?? "" }
        fullScreen = bool("fullScreen", true)
        fillScreen = bool("fillScreen", true)
        textLanguage = string("textLanguage")
        voiceLanguage = string("voiceLanguage")
        prompts = string("prompts")
        // On the iPhone the toggle is the one-line FPS, heat and memory display, on by default.
        #if os(macOS)
        stats = bool("stats", false)
        #else
        stats = bool("stats", true)
        #endif
        skipIntro = bool("skipIntro", false)
        noColorFilter = bool("noColorFilter", false)
        frameRate = d.integer(forKey: "\(game.id).frameRate")
        options = string("options")
        showAtStart = bool("showAtStart", true)
        graphics = Dictionary(uniqueKeysWithValues: graphicsSettings(game).map { ($0.env, string("graphics.\($0.env)")) })
        appPath = Self.findGame(game, saved: string("appPath"))
        readLanguages()
    }

    func save() {
        defaults.set(appPath, forKey: key("appPath"))
        defaults.set(fullScreen, forKey: key("fullScreen"))
        defaults.set(fillScreen, forKey: key("fillScreen"))
        defaults.set(textLanguage, forKey: key("textLanguage"))
        defaults.set(voiceLanguage, forKey: key("voiceLanguage"))
        defaults.set(prompts, forKey: key("prompts"))
        defaults.set(stats, forKey: key("stats"))
        defaults.set(skipIntro, forKey: key("skipIntro"))
        defaults.set(noColorFilter, forKey: key("noColorFilter"))
        defaults.set(frameRate, forKey: key("frameRate"))
        defaults.set(options, forKey: key("options"))
        defaults.set(showAtStart, forKey: key("showAtStart"))
        for (env, value) in graphics { defaults.set(value, forKey: key("graphics.\(env)")) }
    }

    // The choices as the port's switches, for the game this process starts
    // (those it supports: it ignores the others).
    func apply() {
        func set(_ name: String, _ value: String?) {
            if let value { setenv(name, value, 1) } else { unsetenv(name) }
        }
        #if os(macOS)
        set(game.appEnv, appPath)
        if game.supports("windowed") { set("SCIMITAR_WINDOWED", fullScreen ? nil : "1") }
        if game.supports("full-screen") { set("SCIMITAR_FULL_SCREEN", fullScreen ? "1" : nil) }
        // Metal; OpenGL only when asked for in the environment (tests).
        if game.supports("metal") && ProcessInfo.processInfo.environment["SCIMITAR_RENDERER"] == nil {
            set("SCIMITAR_RENDERER", "metal")
        }
        #endif
        #if !os(macOS)
        set("SCIMITAR_FPS", frameRate > 0 ? String(frameRate) : nil)
        #endif
        // Unset for "Game setting", unless set in the environment (tests).
        for (env, value) in graphics where !value.isEmpty || ProcessInfo.processInfo.environment[env] == nil {
            set(env, value.isEmpty ? nil : value)
        }
        if game.supports("aspect") { set("SCIMITAR_ASPECT", fillScreen ? nil : "16:9") }
        // (A choice the game at appPath lacks, made for another copy, is dropped.)
        func language(_ code: String, _ list: [(code: String, name: String)]) -> String? {
            code.isEmpty || !list.contains { $0.code == code } ? nil : code
        }
        if game.supports("languages") {
            set("SCIMITAR_TEXT_LANGUAGE", language(textLanguage, textLanguages))
            set("SCIMITAR_VOICE_LANGUAGE", language(voiceLanguage, voiceLanguages))
        }
        if game.supports("prompts") { set("SCIMITAR_BUTTON_PROMPTS", prompts.isEmpty ? nil : prompts) }
        if game.supports("stats") { set("SCIMITAR_STATS", stats ? "1" : nil) }
        if game.supports("intro") { set("SCIMITAR_SKIP_INTRO", skipIntro ? "1" : nil) }
        if game.id == "ac2" { set("SCIMITAR_NO_COLOR_FILTER", noColorFilter ? "1" : nil) }
        if game.supports("options") {
            let trimmed = options.trimmingCharacters(in: .whitespacesAndNewlines)
            set("SCIMITAR_GAME_OPTIONS", trimmed.isEmpty ? nil : trimmed)
        }
    }

    // A location holding the game's file (LauncherGame.gameFiles).
    static func isGame(_ path: String, _ game: LauncherGame) -> Bool {
        !path.isEmpty && game.gameFiles.contains { FileManager.default.isReadableFile(atPath: path + "/" + $0) }
    }
    var gameFound: Bool { Self.isGame(appPath, game) }

    // The pickers' languages for the game at appPath, read when it changes
    // (not at each redraw: the disk may be slow).
    @Published private(set) var textLanguages = kTextLanguages
    @Published private(set) var voiceLanguages = kLanguages
    private func readLanguages() {
        guard game.supports("languages") else { return }
        textLanguages = Self.readTextLanguages(appPath)
        voiceLanguages = Self.readVoiceLanguages(appPath)
    }

    // The text languages the game has, when it lists them (Brotherhood's
    // Resources/localization.lang: "LANG", a byte, then the text languages'
    // big-endian bit mask; see Russian below). Otherwise all (a language AC2 lacks falls back to
    // its default).
    private static func readTextLanguages(_ appPath: String) -> [(code: String, name: String)] {
        guard let d = FileManager.default.contents(atPath: appPath + "/Contents/Resources/localization.lang"),
              d.count >= 9, d.prefix(4) == Data("LANG".utf8) else { return kTextLanguages }
        let b = [UInt8](d)
        var mask = UInt32(b[5]) << 24 | UInt32(b[6]) << 16 | UInt32(b[7]) << 8 | UInt32(b[8])
        // Russian, which the file leaves out where DataMAC.forge has its text
        // (the package names are in the forge's index, in its first megabyte;
        // games/acb/language.cpp does the same).
        if let f = FileHandle(forReadingAtPath: appPath + "/Contents/Resources/DataMAC.forge") {
            let index = (try? f.read(upToCount: 1 << 20)) ?? Data()
            try? f.close()
            if index.range(of: Data("LocalizationPackage_Russian_Subtitles".utf8)) != nil,
               index.range(of: Data("LocalizationPackage_Russian\0".utf8)) != nil {
                mask |= 1 << 12
            }
        }
        return kTextLanguages.filter { l in
            guard let n = kLanguageNumbers[l.code] else { return true }
            return mask & (1 << UInt32(n)) != 0
        }
    }

    // The voices the game has (sounddata/Mac/sounds_<code>.pck), when known.
    private static func readVoiceLanguages(_ appPath: String) -> [(code: String, name: String)] {
        let fm = FileManager.default
        for dir in ["sounddata", "SoundData"] {
            let base = appPath + "/Contents/Resources/\(dir)/Mac"
            if let files = try? fm.contentsOfDirectory(atPath: base), !files.isEmpty {
                return kLanguages.filter { $0.code.isEmpty || files.contains("sounds_\($0.code).pck") }
            }
        }
        return kLanguages
    }

    // The game's location for a chosen path: the path, or, for the game's
    // file itself (a Windows port's .exe, as in $AC1_EXE), the location
    // holding it.
    static func location(of path: String, _ game: LauncherGame) -> String {
        for file in game.gameFiles where path.hasSuffix("/" + file) {
            return String(path.dropLast(file.count + 1))
        }
        return path
    }

    // Where the game may be: the last choice, the environment, where the build
    // found it, the Applications folders, Steam's library.
    static func findGame(_ game: LauncherGame, saved: String) -> String {
        var candidates = [saved]
        if let env = ProcessInfo.processInfo.environment[game.appEnv] { candidates.append(location(of: env, game)) }
        candidates.append(game.defaultApp)
        #if os(macOS)
        let home = FileManager.default.homeDirectoryForCurrentUser.path
        candidates += ["/Applications/\(game.appName)", "\(home)/Applications/\(game.appName)"]
        let steam = "\(home)/Library/Application Support/Steam/steamapps/common"
        candidates.append("\(steam)/\(game.appName)")
        for dir in (try? FileManager.default.contentsOfDirectory(atPath: steam)) ?? [] {
            candidates.append("\(steam)/\(dir)/\(game.appName)")
        }
        #endif
        let path = candidates.first { isGame($0, game) } ?? (saved.isEmpty ? game.defaultApp : saved)
        return (path as NSString).standardizingPath
    }
}

// The "Compiling shaders" step after Play (Metal only): the port's
// scimitar_shader_precompile (platform/src/metal/shader_cache.mm), found with
// dlsym so a build without it just plays. It compiles what the shader cache
// does not hold yet: everything on a first start, little or nothing later.
final class ShaderProgress: ObservableObject {
    static let shared = ShaderProgress()
    @Published var active = false
    @Published var done = 0
    @Published var total = 0

    private typealias Progress = @convention(c) (Int, Int, UnsafeMutableRawPointer?) -> Void
    private typealias Precompile = @convention(c) (UnsafePointer<CChar>?, Progress?, UnsafeMutableRawPointer?) -> Int

    // Runs on another thread; `then` runs on the main thread afterwards.
    func run(game: LauncherGame, appPath: String, then: @escaping () -> Void) {
        guard game.supports("metal") else { return then() }
        let renderer = getenv("SCIMITAR_RENDERER").map { String(cString: $0) }
        guard renderer == "metal", getenv("SCIMITAR_PRECOMPILE").map({ String(cString: $0) }) != "0",
              let sym = dlsym(UnsafeMutableRawPointer(bitPattern: -2), "scimitar_shader_precompile")  // RTLD_DEFAULT
        else { return then() }
        let precompile = unsafeBitCast(sym, to: Precompile.self)
        active = true
        DispatchQueue.global(qos: .userInitiated).async {
            _ = appPath.withCString { app in
                precompile(app, { done, total, _ in
                    onMain { ShaderProgress.shared.done = done; ShaderProgress.shared.total = total }
                }, nil)
            }
            onMain { ShaderProgress.shared.active = false; then() }
        }
    }
}

// Runs `f` on the main thread, also while the launcher's modal loop runs.
private func onMain(_ f: @escaping () -> Void) {
    #if os(macOS)
    RunLoop.main.perform(inModes: [.common, .modalPanel], block: f)
    CFRunLoopWakeUp(CFRunLoopGetMain())
    #else
    DispatchQueue.main.async(execute: f)
    #endif
}

struct ShaderProgressView: View {
    @ObservedObject var progress: ShaderProgress
    var body: some View {
        VStack(spacing: 10) {
            Text("Compiling shaders").font(.headline)
            ProgressView(value: Double(progress.done), total: Double(max(progress.total, 1)))
            Text(progress.total == 0 ? "Looking for the game's shaders…"
                 : "\(progress.done.formatted()) of \(progress.total.formatted()) shaders (\(progress.done * 100 / max(progress.total, 1))%)")
                .monospacedDigit()
            Text("Done once, so the game does not stutter the first time it shows something. Later starts only compile what is new.")
                .font(.caption).foregroundStyle(.secondary).multilineTextAlignment(.center)
        }
        .padding(24)
        .frame(maxWidth: 420)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 12))
    }
}

// Saves for Assassin's Creed II: a save from the Windows game (N.save, from Ubisoft Game Launcher's
// savegames folder) or from this port (p4-1.sav) goes into the port's saves folder, as the game's
// local Orbit layer reads it (platform/src/orbit.cpp). Windows file: u32 header length (548), the
// slot name as UTF-16 at byte 40, then the game's data from byte 4 + header length. Port file:
// "ACSV", u32 version 1, product id, save id, name length, name as UTF-32, data length, data.
// The data is passed through unchanged. The save being replaced is copied to saves-backup first
// (outside saves/: the port reads every file in that folder).
enum SaveImport {
    struct Failure: Error { let message: String }
    static let productID: UInt32 = 4  // AC2's, as in the port's own p4-1.sav
    static let saveID: UInt32 = 1
    static let slotName = "ACII 0"
    static let magic: [UInt8] = Array("ACSV".utf8)

    static var root: URL {
        URL(fileURLWithPath: NSHomeDirectory()).appendingPathComponent("Library/Application Support/scimitar-recomp")
    }

    private static func u32(_ b: [UInt8], _ o: Int) -> UInt32 {
        UInt32(b[o]) | UInt32(b[o + 1]) << 8 | UInt32(b[o + 2]) << 16 | UInt32(b[o + 3]) << 24
    }
    private static func put(_ v: UInt32, _ out: inout [UInt8]) {
        out += [UInt8(v & 255), UInt8((v >> 8) & 255), UInt8((v >> 16) & 255), UInt8(v >> 24)]
    }

    // The file the port reads, from either kind of save.
    static func portFile(from b: [UInt8]) throws -> (file: [UInt8], id: UInt32, name: String, size: Int, converted: Bool) {
        guard b.count >= 64, b.count <= 16 << 20 else { throw Failure(message: "That file is not an Assassin's Creed II save.") }
        if Array(b[0..<4]) == magic {
            let nl = Int(u32(b, 16))
            guard u32(b, 4) == 1, nl <= 4096, b.count >= 24 + 4 * nl else { throw Failure(message: "That save is damaged.") }
            let n = Int(u32(b, 20 + 4 * nl))
            guard 24 + 4 * nl + n == b.count, u32(b, 8) == productID else {
                throw Failure(message: "That is not an Assassin's Creed II save from this port.")
            }
            var name = ""
            for k in 0..<nl { if let c = Unicode.Scalar(u32(b, 20 + 4 * k)) { name.unicodeScalars.append(c) } }
            return (b, u32(b, 12), name, n, false)
        }
        let headerLength = Int(u32(b, 0))
        let start = 4 + headerLength
        guard headerLength >= 44, headerLength <= 4096, start + 36 <= b.count else {
            throw Failure(message: "That file is not a Windows Assassin's Creed II save.")
        }
        var units: [UInt16] = []
        var i = 40
        while i + 1 < start {
            let c = UInt16(b[i]) | UInt16(b[i + 1]) << 8
            if c == 0 { break }
            units.append(c)
            i += 2
        }
        let name = String(decoding: units, as: UTF16.self)
        // Only the main slot: another slot's data would replace the game's progress with something else.
        guard name == slotName else {
            throw Failure(message: "That save is the slot \"\(name)\", not \"\(slotName)\" (the game's progress). Pick another file.")
        }
        let data = Array(b[start...])
        guard Int(u32(data, 28)) == data.count - 32 else {
            throw Failure(message: "That does not look like an Assassin's Creed II save (its data has the wrong size).")
        }
        var out = magic
        put(1, &out); put(productID, &out); put(saveID, &out); put(UInt32(name.unicodeScalars.count), &out)
        for c in name.unicodeScalars { put(c.value, &out) }
        put(UInt32(data.count), &out)
        out += data
        return (out, saveID, name, data.count, true)
    }

    // Puts the save in place; the message for the launcher.
    static func importSave(_ bytes: [UInt8]) throws -> String {
        let r = try portFile(from: bytes)
        let fm = FileManager.default
        let dir = root.appendingPathComponent("saves")
        let target = dir.appendingPathComponent("p\(productID)-\(r.id).sav")
        try fm.createDirectory(at: dir, withIntermediateDirectories: true)
        var backedUp = false
        if fm.fileExists(atPath: target.path) {
            let backups = root.appendingPathComponent("saves-backup")
            try fm.createDirectory(at: backups, withIntermediateDirectories: true)
            let stamp = DateFormatter()
            stamp.locale = Locale(identifier: "en_US_POSIX")
            stamp.dateFormat = "yyyyMMdd-HHmmss"
            try fm.copyItem(at: target, to: backups.appendingPathComponent("p\(productID)-\(r.id)-\(stamp.string(from: Date())).sav"))
            backedUp = true
        }
        try Data(r.file).write(to: target, options: .atomic)
        return "Imported \(r.name) (\(r.size / 1024) KB)" + (r.converted ? " from a Windows save" : "")
            + (backedUp ? ". The previous save was backed up." : ".")
    }
}

struct LauncherView: View {
    @ObservedObject var settings: LauncherSettings
    @ObservedObject var compile = ShaderProgress.shared
    let onPlay: () -> Void
    let onQuit: (() -> Void)?
    #if os(macOS)
    // The tab shown first (SCIMITAR_LAUNCHER_TAB: general, graphics, language, advanced; for screenshots).
    @State private var tab = ProcessInfo.processInfo.environment["SCIMITAR_LAUNCHER_TAB"] ?? "general"
    #else
    @State private var importing = false
    @State private var importMessage: String?
    // The on-screen pad's settings: the same user defaults touchpad_ios.mm reads.
    @AppStorage("touchpad.mode") private var padMode = "auto"
    @AppStorage("touchpad.scale") private var padScale = 0.855
    @AppStorage("touchpad.opacity") private var padOpacity = 0.68
    @AppStorage("touchpad.haptics") private var padHaptics = true
    @AppStorage("touchpad.toggleRT") private var padToggleRT = true
    @AppStorage("touchpad.trail") private var padTrail = true
    // The reset button: 0 as usual, 1 asking to be tapped again (for three seconds), 2 "done" (for two).
    @State private var padResetStage = 0
    @State private var padResetToken = 0
    // The new launcher: the home screen or the settings, and the settings' tab.
    @State private var settingsOpen = false
    @State private var tab = "Install"
    #endif

    var body: some View {
        #if os(macOS)
        VStack(spacing: 0) {
            header.padding([.horizontal, .top], 20).padding(.bottom, 4)
            form
            Divider()
            HStack {
                Toggle("Show this window at start", isOn: $settings.showAtStart)
                    .help("When off, the game starts at once; hold Option while opening the app to see this window.")
                Spacer()
                Button("Quit") { onQuit?() }.keyboardShortcut(.cancelAction)
                Button("Play") { onPlay() }
                    .keyboardShortcut(.defaultAction)
                    .buttonStyle(.borderedProminent)
                    .disabled(!settings.gameFound)
            }
            .padding(16)
        }
        .frame(width: 560)
        .disabled(compile.active)
        .overlay { if compile.active { ShaderProgressView(progress: compile) } }
        #else
        phoneBody
        #endif
    }

    #if os(macOS)
    private var header: some View {
        HStack(spacing: 12) {
            // The app's own icon (the game's, copied at build time): the
            // game's .app gets the unsupported sign, being a 32-bit app.
            Image(nsImage: NSApp.applicationIconImage)
                .resizable().frame(width: 48, height: 48)
            VStack(alignment: .leading, spacing: 2) {
                Text(settings.game.title).font(.title2.bold())
                Text("Recompiled for this Mac").foregroundStyle(.secondary)
            }
            Spacer()
        }
    }
    #endif

    #if !os(macOS)
    static let accent = Color(red: 0.80, green: 0.17, blue: 0.17)
    static let card = Color.white.opacity(0.07)
    static let backdrop = LinearGradient(
        colors: [Color(red: 0.11, green: 0.11, blue: 0.14), Color(red: 0.03, green: 0.03, blue: 0.05)],
        startPoint: .top, endPoint: .bottom)
    // The app's own icon, from the files the build put in the bundle.
    static var launcherIcon: UIImage? {
        ["AppIcon60x60", "AppIcon76x76"].lazy.compactMap { UIImage(named: $0) }.first
    }

    private var hero: some View {
        VStack(spacing: 10) {
            Group {
                if let icon = Self.launcherIcon {
                    Image(uiImage: icon).resizable().scaledToFill()
                } else {
                    Image(systemName: "shield.lefthalf.filled").resizable().scaledToFit().padding(22)
                }
            }
            .frame(width: 96, height: 96)
            .clipShape(RoundedRectangle(cornerRadius: 22, style: .continuous))
            .shadow(color: .black.opacity(0.5), radius: 14, y: 8)
            Text(settings.game.title).font(.title.bold())
            Text("Recompiled for \(UIDevice.current.userInterfaceIdiom == .pad ? "iPad" : "iPhone")")
                .font(.subheadline).foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 14)
    }

    // PLAY, a row like the others under the game's name: it scrolls with the rest.
    private var playRow: some View {
        VStack(spacing: 6) {
            Button(action: onPlay) {
                Text("PLAY").font(.headline.weight(.heavy)).tracking(3)
                    .frame(maxWidth: .infinity).padding(.vertical, 6)
            }
            .buttonStyle(.borderedProminent)
            .buttonBorderShape(.capsule)
            .controlSize(.large)
            .disabled(!settings.gameFound || compile.active)
            if !settings.gameFound {
                Text("Game not found: see Game below.").font(.caption).foregroundStyle(.secondary)
            }
        }
        .padding(.horizontal, 24).padding(.bottom, 10)
    }

    private var controlsSection: some View {
        Section {
            Picker("On-screen controls", selection: $padMode) {
                Text("Automatic").tag("auto")
                Text("Always").tag("on")
                Text("Off").tag("off")
            }
            if padMode != "off" {
                VStack(alignment: .leading) {
                    Text("Size")
                    Slider(value: $padScale, in: 0.6...1.6)
                }
                VStack(alignment: .leading) {
                    Text("Opacity")
                    Slider(value: $padOpacity, in: 0.15...1.0)
                }
                Toggle("Haptic feedback", isOn: $padHaptics)
                Toggle("RT toggles on tap (sprint)", isOn: $padToggleRT)
                Toggle("Camera trail (line and dot)", isOn: $padTrail)
                // A bordered button on purpose: the rows here have their own background (listRowBackground),
                // which takes away the list's own highlight, so a plain row button showed nothing when tapped.
                Button(action: padResetTapped) {
                    Text(padResetStage == 1 ? "Tap again to reset" : padResetStage == 2 ? "Layout reset ✓" : "Reset button layout")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
                .tint(padResetStage == 2 ? .green : .red)
            }
        } header: {
            Text("On-screen controls")
        } footer: {
            Text("Automatic hides them while a controller is connected. The empty left and right sides of the screen are the sticks. In the game, tap the three dots at the top for Edit, Mouse, extra buttons and Reset layout.")
                .font(.caption).foregroundStyle(.secondary)
        }
    }

    // The first tap asks, the second resets: the layout, the fixed stick, the size and the opacity. The camera
    // trail, haptics and RT settings are preferences, not layout, and stay.
    private func padResetTapped() {
        padResetToken += 1
        let token = padResetToken
        if padResetStage == 1 {
            UserDefaults.standard.removeObject(forKey: "touchpad.layout")
            UserDefaults.standard.set(false, forKey: "touchpad.fixedStick")
            padScale = 0.855
            padOpacity = 0.68
            padResetStage = 2
            UINotificationFeedbackGenerator().notificationOccurred(.success)
            DispatchQueue.main.asyncAfter(deadline: .now() + 2) {
                if padResetToken == token { padResetStage = 0 }
            }
        } else {
            padResetStage = 1
            UIImpactFeedbackGenerator(style: .light).impactOccurred()
            DispatchQueue.main.asyncAfter(deadline: .now() + 3) {
                if padResetToken == token && padResetStage == 1 { padResetStage = 0 }
            }
        }
    }

    private var savesSection: some View {
        Section {
            Button { importing = true } label: { Label("Import a save…", systemImage: "square.and.arrow.down") }
            if let message = importMessage { Text(message).font(.footnote).foregroundStyle(.secondary) }
        } header: {
            Text("Saves")
        } footer: {
            Text("Choose a Windows save (the numbered .save file named ACII 0, from Ubisoft Game Launcher's savegames folder) or a save from this port (p4-1.sav). The save it replaces is backed up first.")
                .font(.caption).foregroundStyle(.secondary)
        }
    }

    private func importSave(_ result: Result<[URL], Error>) {
        do {
            guard let url = try result.get().first else { return }
            let scoped = url.startAccessingSecurityScopedResource()
            defer { if scoped { url.stopAccessingSecurityScopedResource() } }
            importMessage = try SaveImport.importSave([UInt8](try Data(contentsOf: url)))
        } catch let failure as SaveImport.Failure {
            importMessage = failure.message
        } catch let error as CocoaError where error.code == .userCancelled {
            return
        } catch {
            importMessage = "Could not import that file: \(error.localizedDescription)"
        }
    }

    // ---- The iPhone launcher: the screens of Launcher_preview.html. Sizes are in points (the preview's pixels);
    // the looks and the small controls are in LauncherPhoneUI.swift. ----

    private var phoneTabs: [String] {
        var tabs = ["Install"]
        if settings.game.id == "ac2" { tabs.append("Save manager") }
        tabs += ["Display", "Touch controls"]
        if hasGraphics { tabs.append("Graphics") }
        if hasLanguage { tabs.append("Language") }
        if hasAdvanced { tabs.append("Advanced") }
        return tabs
    }

    // The real size of the notch side, read from the window (not from SwiftUI's measurement, which came back
    // empty here and let the text slide under the Dynamic Island).
    private var windowInsets: UIEdgeInsets {
        let scenes = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
        let window = scenes.flatMap { $0.windows }.first { $0.isKeyWindow } ?? scenes.first?.windows.first
        return window?.safeAreaInsets ?? .zero
    }

    private var phoneBody: some View {
        let insets = windowInsets
        return ZStack {
            // The backgrounds fill the whole screen; the content stays inside the system's safe area (clear of
            // the notch and the home indicator, whichever way the phone is turned), with the preview's margins:
            // 5 more at the notch side, and at least 24 at the sides of a phone without a notch.
            PhoneBackdrop().ignoresSafeArea()
            PhoneBackground().ignoresSafeArea()
            Group {
                if settingsOpen { settingsPage } else { homePage }
            }
            .padding(EdgeInsets(top: 14, leading: max(5, 24 - insets.left),
                                bottom: max(1, 14 - insets.bottom), trailing: max(0, 24 - insets.right)))
            .disabled(compile.active)
            if compile.active { shaderOverlay }
        }
        .preferredColorScheme(.dark)
        .tint(PhoneStyle.accent)
        .dynamicTypeSize(.large)
        .fileImporter(isPresented: $importing, allowedContentTypes: [.data], allowsMultipleSelection: false) { result in
            importSave(result)
        }
    }

    // The home screen: the logo, the title, PLAY (INSTALL when the game is not there) and Settings.
    private var homePage: some View {
        VStack(spacing: 10) {
            PhoneImage(image: PhoneArt.logo)
                .frame(width: 72, height: 72)
                .shadow(color: Color.black.opacity(0.75), radius: 7, x: 0, y: 6)
            if settings.game.id == "ac2" {
                PhoneImage(image: PhoneArt.wordmark)
                    .frame(width: 270)
                    .shadow(color: Color.black.opacity(0.6), radius: 5, x: 0, y: 4)
            } else {
                Text(settings.game.title)
                    .font(PhoneStyle.font(28, .bold))
                    .foregroundColor(.white)
            }
            Text("RECOMPILED FOR iOS")
                .font(PhoneStyle.font(13))
                .tracking(1.3)
                .foregroundColor(PhoneStyle.subtitle)
            Button(action: playTapped) {
                Text(settings.gameFound ? "PLAY" : "INSTALL")
            }
            .buttonStyle(PhonePlayStyle(size: 22, horizontal: 76, vertical: 18))
            Button { settingsOpen = true } label: {
                Text("Settings")
            }
            .buttonStyle(PhoneGhostStyle())
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    // PLAY starts the game; INSTALL (no game found) opens the settings on the Install tab.
    private func playTapped() {
        if settings.gameFound {
            onPlay()
        } else {
            tab = "Install"
            settingsOpen = true
        }
    }

    private var settingsPage: some View {
        VStack(spacing: 0) {
            ZStack {
                HStack {
                    Button { settingsOpen = false } label: {
                        Text("BACK")
                    }
                    .buttonStyle(PhonePlayStyle(size: 12, horizontal: 20, vertical: 7))
                    Spacer(minLength: 0)
                }
                Text("Settings")
                    .font(PhoneStyle.font(16, .bold))
                    .foregroundColor(.white)
            }
            .frame(height: 40)
            HStack(alignment: .top, spacing: 14) {
                VStack(spacing: 0) {
                    ForEach(phoneTabs, id: \.self) { name in
                        PhoneTab(title: name, selected: name == tab) { tab = name }
                    }
                    Spacer(minLength: 0)
                    githubLink
                }
                .frame(width: 162)
                ScrollView {
                    VStack(spacing: 0) {
                        PhoneCard(rows: phoneRows(tab))
                        if !phoneFooter(tab).isEmpty { PhoneFoot(text: phoneFooter(tab)) }
                    }
                }
                .id(tab)
            }
            .padding(.top, 10)
            .frame(maxHeight: .infinity)
        }
    }

    // The GitHub mark at the bottom left of the settings: it opens the profile.
    private var githubLink: some View {
        Link(destination: PhoneStyle.profile) {
            HStack(spacing: 8) {
                PhoneImage(image: PhoneArt.github).frame(width: 28, height: 28)
                Text("Follow on GitHub")
                    .font(PhoneStyle.font(12, .semibold))
                    .foregroundColor(PhoneStyle.subtitle)
                    .lineSpacing(1.5)
                    .multilineTextAlignment(.leading)
                    .fixedSize(horizontal: false, vertical: true)
                Spacer(minLength: 0)
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 6)
            .opacity(0.9)
        }
        .accessibilityLabel("Follow on GitHub")
    }

    private func phoneToggle(_ title: String, _ isOn: Binding<Bool>) -> AnyView {
        AnyView(Toggle(title, isOn: isOn)
            .toggleStyle(PhoneToggleStyle())
            .padding(.horizontal, 14)
            .padding(.vertical, 8)
            .frame(minHeight: 38))
    }

    // The rows of a tab, the same choices as the old form.
    private func phoneRows(_ tab: String) -> [AnyView] {
        var rows: [AnyView] = []
        switch tab {
        case "Install":
            rows.append(AnyView(PhoneRow("Location") {
                Text((settings.appPath as NSString).abbreviatingWithTildeInPath)
                    .font(PhoneStyle.font(13))
                    .foregroundColor(PhoneStyle.muted)
                    .lineLimit(1)
                    .truncationMode(.middle)
            }))
            rows.append(AnyView(PhoneFreeRow {
                if settings.gameFound {
                    Text("✓ Found")
                        .font(PhoneStyle.font(13))
                        .foregroundColor(PhoneStyle.found)
                } else {
                    Text(notFoundText)
                        .font(PhoneStyle.font(12))
                        .foregroundColor(PhoneStyle.warning)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }))
        case "Save manager":
            rows.append(AnyView(PhoneFreeRow {
                Button { importing = true } label: {
                    Text("⤓ Import a save…")
                        .font(PhoneStyle.font(13, .semibold))
                        .foregroundColor(PhoneStyle.link)
                }
                .buttonStyle(.plain)
            }))
            if let message = importMessage {
                rows.append(AnyView(PhoneFreeRow {
                    Text(message)
                        .font(PhoneStyle.font(12))
                        .foregroundColor(PhoneStyle.muted)
                        .fixedSize(horizontal: false, vertical: true)
                }))
            }
        case "Display":
            if settings.game.supports("aspect") {
                rows.append(AnyView(PhoneRow("Screen shape") {
                    PhoneSegments(options: [("Fill the screen", true), ("16:9 (original)", false)],
                                  selection: $settings.fillScreen)
                }))
            }
            rows.append(AnyView(PhoneRow("Frame rate") {
                PhoneSegments(options: [("Automatic", 0), ("24", 24), ("30", 30), ("45", 45), ("60", 60)],
                              selection: $settings.frameRate)
            }))
            if settings.game.supports("stats") { rows.append(phoneToggle("Show FPS, heat and memory", $settings.stats)) }
            if settings.game.supports("intro") { rows.append(phoneToggle("Skip intro videos", $settings.skipIntro)) }
            if settings.game.id == "ac2" { rows.append(phoneToggle("Remove color filter", $settings.noColorFilter)) }
        case "Touch controls":
            rows.append(AnyView(PhoneRow("On-screen controls") {
                PhoneSegments(options: [("Automatic", "auto"), ("Always", "on"), ("Off", "off")], selection: $padMode)
            }))
            if padMode != "off" {
                rows.append(AnyView(PhoneRow("Size") {
                    Slider(value: $padScale, in: 0.6...1.6).tint(PhoneStyle.accent).frame(width: 190)
                }))
                rows.append(AnyView(PhoneRow("Opacity") {
                    Slider(value: $padOpacity, in: 0.15...1.0).tint(PhoneStyle.accent).frame(width: 190)
                }))
                rows.append(phoneToggle("Haptic feedback", $padHaptics))
                rows.append(phoneToggle("RT toggles on tap (sprint)", $padToggleRT))
                rows.append(phoneToggle("Camera trail (line and dot)", $padTrail))
                rows.append(AnyView(PhoneFreeRow {
                    Spacer(minLength: 0)
                    Button(action: padResetTapped) {
                        Text(padResetStage == 1 ? "Tap again to reset" : padResetStage == 2 ? "Layout reset ✓" : "Reset button layout")
                    }
                    .buttonStyle(PhonePillStyle(color: padResetStage == 2 ? PhoneStyle.resetGreen : PhoneStyle.resetRed,
                                                border: padResetStage == 2 ? PhoneStyle.resetGreen : PhoneStyle.resetRed))
                }))
            }
        case "Graphics":
            for g in graphicsSettings(settings.game) {
                var options: [(label: String, value: String)] = [(label: "Game setting", value: "")]
                for v in g.values { options.append((label: v.name, value: v.value)) }
                rows.append(AnyView(PhoneRow(g.name) {
                    PhoneMenu(options: options, selection: Binding(get: { settings.graphics[g.env] ?? "" },
                                                                   set: { settings.graphics[g.env] = $0 }))
                }))
            }
        case "Language":
            if settings.game.supports("languages") {
                let texts = settings.textLanguages.map { (label: $0.name, value: $0.code) }
                let voices = settings.voiceLanguages.map { (label: $0.name, value: $0.code) }
                rows.append(AnyView(PhoneRow("Text and subtitles") {
                    PhoneMenu(options: texts, selection: $settings.textLanguage)
                }))
                rows.append(AnyView(PhoneRow("Voices") {
                    PhoneMenu(options: voices, selection: $settings.voiceLanguage)
                }))
            }
            if settings.game.supports("prompts") {
                let prompts = kPrompts.map { (label: $0.name, value: $0.code) }
                rows.append(AnyView(PhoneRow("Button prompts") {
                    PhoneMenu(options: prompts, selection: $settings.prompts)
                }))
            }
        case "Advanced":
            rows.append(AnyView(PhoneRow("Game options") {
                PhoneTextField(prompt: "/name:value ...", text: $settings.options)
            }))
        default:
            break
        }
        return rows
    }

    // The small print under a tab (the old form's footers).
    private func phoneFooter(_ tab: String) -> String {
        switch tab {
        case "Save manager":
            return "Choose a Windows save (the numbered .save file named ACII 0, from Ubisoft Game Launcher's savegames folder) or a save from this port (p4-1.sav). The save it replaces is backed up first."
        case "Display":
            return displayFooter + colorFilterNote
        case "Touch controls":
            return "Automatic hides them while a controller is connected. The empty left and right sides of the screen are the sticks. In the game, tap the three dots at the top for Edit, Mouse, extra buttons and Reset layout."
        case "Graphics":
            return graphicsFooter
        case "Language":
            return "Voices lists those installed with the game. Button prompts change the HUD's and the memories' controller art."
        case "Advanced":
            return "Options for the engine's own command line parser, separated by spaces."
        default:
            return ""
        }
    }

    // "Compiling shaders", over everything.
    private var shaderOverlay: some View {
        ZStack {
            Color.black.opacity(0.72).ignoresSafeArea()
            VStack(spacing: 9) {
                Text("Compiling shaders")
                    .font(PhoneStyle.font(13, .bold))
                    .foregroundColor(.white)
                PhoneProgressBar(fraction: Double(compile.done) / Double(max(compile.total, 1)))
                Text(shaderStatus)
                    .font(PhoneStyle.font(13))
                    .monospacedDigit()
                    .foregroundColor(.white)
                Text("Done once, so the game does not stutter the first time it shows something. Later starts only compile what is new.")
                    .font(PhoneStyle.font(11))
                    .foregroundColor(PhoneStyle.muted)
            }
            .multilineTextAlignment(.center)
            .padding(.horizontal, 24)
            .padding(.vertical, 20)
            .frame(width: 360)
            .background(RoundedRectangle(cornerRadius: 12).fill(Color(red: 44 / 255, green: 44 / 255, blue: 52 / 255).opacity(0.96)))
        }
    }

    private var shaderStatus: String {
        if compile.total == 0 { return "Looking for the game's shaders…" }
        let percent = compile.done * 100 / max(compile.total, 1)
        return "\(compile.done.formatted()) of \(compile.total.formatted()) shaders (\(percent)%)"
    }
    #endif

    // The sections the game has settings for (General always).
    private var hasGraphics: Bool { !graphicsSettings(settings.game).isEmpty }
    private var hasLanguage: Bool { settings.game.supports("languages") || settings.game.supports("prompts") }
    private var hasAdvanced: Bool { settings.game.supports("options") }

    #if os(macOS)
    private var tabs: [(String, String)] {
        [("general", "General")] + (hasGraphics ? [("graphics", "Graphics")] : [])
            + (hasLanguage ? [("language", "Language and controls")] : [])
            + (hasAdvanced ? [("advanced", "Advanced")] : [])
    }
    #endif

    // The Mac shows the sections in tabs, iOS in one scrolling form.
    @ViewBuilder private var form: some View {
        #if os(macOS)
        // The tab bar, in plain SwiftUI: TabView puts its tabs in the window's
        // toolbar, where they collapse behind an overflow button, and a
        // segmented Picker (an NSSegmentedControl) came up stretched past the
        // window until something laid it out again.
        VStack(spacing: 0) {
            HStack(spacing: 2) {
                ForEach(tabs, id: \.0) { id, name in
                    Button { tab = id } label: {
                        Text(name)
                            .padding(.horizontal, 12).padding(.vertical, 4)
                            .foregroundStyle(tab == id ? Color.white : Color.primary)
                            .background(tab == id ? Color.accentColor : Color.clear,
                                        in: RoundedRectangle(cornerRadius: 6))
                            .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                }
            }
            .padding(2)
            .background(.quaternary, in: RoundedRectangle(cornerRadius: 8))
            .padding(.top, 8)
            Form {
                switch tab {
                case "graphics" where hasGraphics: graphicsSection
                case "language" where hasLanguage: languageSection
                case "advanced" where hasAdvanced: advancedSection
                default: gameSection; displaySection
                }
            }
            .formStyle(.grouped)
        }
        .frame(height: 450)
        #else
        Form {
            Section {
                hero
                playRow
            }
            .listRowBackground(Color.clear)
            .listRowInsets(EdgeInsets())
            .listRowSeparator(.hidden)
            gameSection.listRowBackground(Self.card)
            if settings.game.id == "ac2" { savesSection.listRowBackground(Self.card) }
            displaySection.listRowBackground(Self.card)
            controlsSection.listRowBackground(Self.card)
            if hasGraphics { graphicsSection.listRowBackground(Self.card) }
            if hasLanguage { languageSection.listRowBackground(Self.card) }
            if hasAdvanced { advancedSection.listRowBackground(Self.card) }
        }
        .formStyle(.grouped)
        .scrollContentBackground(.hidden)
        .background(Self.backdrop.ignoresSafeArea())
        .fileImporter(isPresented: $importing, allowedContentTypes: [.data], allowsMultipleSelection: false) { result in
            importSave(result)
        }
        #endif
    }

    private var gameSection: some View {
        Section {
            gameLocation
        } header: {
            Text("Game")
        }
    }

    private var displaySection: some View {
        Section {
            #if os(macOS)
            if settings.game.supports("windowed") || settings.game.supports("full-screen") {
                Picker("Mode", selection: $settings.fullScreen) {
                    Text("Full screen").tag(true)
                    Text("Window").tag(false)
                }
                .pickerStyle(.segmented)
            }
            #endif
            if settings.game.supports("aspect") {
                Picker("Screen shape", selection: $settings.fillScreen) {
                    Text("Fill the screen").tag(true)
                    Text("16:9 (original)").tag(false)
                }
                .pickerStyle(.segmented)
            }
            #if !os(macOS)
            Picker("Frame rate", selection: $settings.frameRate) {
                Text("Automatic").tag(0)
                Text("24").tag(24)
                Text("30").tag(30)
                Text("45").tag(45)
                Text("60").tag(60)
            }
            #endif
            if settings.game.supports("stats") {
                #if os(macOS)
                Toggle("Show frame rate and memory", isOn: $settings.stats)
                #else
                Toggle("Show FPS, heat and memory", isOn: $settings.stats)
                #endif
            }
            if settings.game.supports("intro") { Toggle("Skip intro videos", isOn: $settings.skipIntro) }
            if settings.game.id == "ac2" { Toggle("Remove color filter", isOn: $settings.noColorFilter) }
        } header: {
            Text("Display")
        } footer: {
            Text(displayFooter + colorFilterNote).font(.caption).foregroundStyle(.secondary)
        }
    }

    private var languageSection: some View {
        Section {
            if settings.game.supports("languages") {
                Picker("Text and subtitles", selection: $settings.textLanguage) {
                    ForEach(settings.textLanguages, id: \.code) { Text($0.name).tag($0.code) }
                }
                Picker("Voices", selection: $settings.voiceLanguage) {
                    ForEach(settings.voiceLanguages, id: \.code) { Text($0.name).tag($0.code) }
                }
            }
            if settings.game.supports("prompts") {
                Picker("Button prompts", selection: $settings.prompts) {
                    ForEach(kPrompts, id: \.code) { Text($0.name).tag($0.code) }
                }
            }
        } header: {
            Text("Language and controls")
        } footer: {
            Text("Voices lists those installed with the game. Button prompts change the HUD's and the memories' controller art.")
                .font(.caption).foregroundStyle(.secondary)
        }
    }

    private var advancedSection: some View {
        Section {
            TextField("Game options", text: $settings.options, prompt: Text("/name:value ..."))
                .autocorrectionDisabled()
                #if os(iOS)
                .textInputAutocapitalization(.never)
                #endif
        } header: {
            Text("Advanced")
        } footer: {
            Text("Options for the engine's own command line parser, separated by spaces.")
                .font(.caption).foregroundStyle(.secondary)
        }
    }

    private var graphicsSection: some View {
        Section {
            ForEach(graphicsSettings(settings.game), id: \.env) { g in
                Picker(g.name, selection: Binding(get: { settings.graphics[g.env] ?? "" },
                                                  set: { settings.graphics[g.env] = $0 })) {
                    Text("Game setting").tag("")
                    ForEach(g.values, id: \.value) { Text($0.name).tag($0.value) }
                }
            }
        } header: {
            Text("Graphics")
        } footer: {
            Text(graphicsFooter).font(.caption).foregroundStyle(.secondary)
        }
    }

    private var graphicsFooter: String {
        if !settings.game.supports("graphics") {
            return "At least this much filtering for the game's textures, past its own options. Higher values cost frame rate."
        }
        let what = "Choices other than Game setting replace the game's graphics options while playing, past their highest values."
        #if os(macOS)
        return what + " F4 shows them while playing, to change them live."
        #else
        return what + " Higher values cost frame rate and battery."
        #endif
    }

    // Assassin's Creed II's Remove color filter (games/ac2/color_filter.cpp).
    private var colorFilterNote: String {
        settings.game.id == "ac2"
            ? " Remove color filter switches off the game's own color filter in every world, as the PC mod AC2's True Colors does; the game's files are not changed."
            : ""
    }

    private var displayFooter: String {
        #if os(macOS)
        settings.game.isMacGame
            ? "Resolution, including the display's native one, is chosen in the game's options. F6 shows or hides the frame rate while playing."
            : "Resolution is chosen in the game's options; the picture keeps its shape. Command-Return switches between full screen and a window while playing, F6 shows or hides the frame rate."
        #else
        "Resolution is chosen in the game's options, and every choice fills the screen. Automatic follows the screen (up to 120). A lower limit saves battery and heat; 24, 30 and 60 fit evenly on a 120 Hz screen, 45 may look less even."
        #endif
    }

    @ViewBuilder private var gameLocation: some View {
        #if os(macOS)
        HStack {
            TextField("Game", text: $settings.appPath, prompt: Text(settings.game.appName))
                .labelsHidden()
                .textFieldStyle(.roundedBorder)
            Button("Choose…") { choose() }
        }
        #else
        LabeledContent("Location", value: (settings.appPath as NSString).abbreviatingWithTildeInPath)
        #endif
        if settings.gameFound {
            Label("Found", systemImage: "checkmark.circle.fill").foregroundStyle(.green)
        } else {
            Label(notFoundText, systemImage: "exclamationmark.triangle.fill").foregroundStyle(.orange)
        }
    }

    // The Windows port's executable (the last part of its game file).
    private var exeName: String { (settings.game.gameFiles.last ?? "" as String).components(separatedBy: "/").last ?? "" }

    private var notFoundText: String {
        #if os(macOS)
        if !settings.game.isMacGame {
            let macRelease = settings.game.gameFiles.contains { $0.hasPrefix("Contents/") }
            return "No \(exeName) here: choose the folder of the installed Windows game"
                + (macRelease ? ", or \(settings.game.appName) (the Mac release)." : ".")
        }
        return "No \(settings.game.appName) here: choose the installed game (the Mac release from Steam)."
        #else
        "Copy \(settings.game.appName) from the Mac release into this app's folder (Finder or the Files app, On My iPhone or iPad), then reopen the app."
        #endif
    }

    #if os(macOS)
    private func choose() {
        let panel = NSOpenPanel()
        panel.title = "Choose \(settings.game.title)"
        panel.treatsFilePackagesAsDirectories = false
        if settings.game.isMacGame {
            panel.message = "Choose the game's application (\(settings.game.appName))."
            panel.allowedContentTypes = [.applicationBundle]
            panel.canChooseDirectories = false
        } else {
            // A Windows port: the game's folder, its .exe, or a Mac release's .app holding it.
            panel.message = "Choose the game's folder (with \(exeName))."
            panel.canChooseDirectories = true
            panel.canChooseFiles = true
        }
        if settings.gameFound {
            panel.directoryURL = URL(fileURLWithPath: settings.appPath).deletingLastPathComponent()
        }
        if panel.runModal() == .OK, let url = panel.url {
            var path = LauncherSettings.location(of: url.path, settings.game)
            // Another file in the game's folder (the .exe of another copy's layout): its folder.
            if !settings.game.isMacGame && !LauncherSettings.isGame(path, settings.game) && !url.hasDirectoryPath {
                path = url.deletingLastPathComponent().path
            }
            settings.appPath = path
        }
    }
    #endif
}

private func game(_ id: UnsafePointer<CChar>, _ title: UnsafePointer<CChar>, _ appEnv: UnsafePointer<CChar>,
                  _ appName: UnsafePointer<CChar>, _ defaultApp: UnsafePointer<CChar>,
                  gameFile: String = "Contents/MacOS/scimitar_sf", settings: String = "") -> LauncherGame {
    LauncherGame(id: String(cString: id), title: String(cString: title), appEnv: String(cString: appEnv),
                 appName: String(cString: appName), defaultApp: String(cString: defaultApp),
                 gameFiles: gameFile.split(separator: "|").map(String.init),
                 settings: settings.isEmpty ? nil : Set(settings.split(separator: " ").map(String.init)))
}

#if os(macOS)

private final class LauncherWindowDelegate: NSObject, NSWindowDelegate {
    func windowWillClose(_ notification: Notification) { NSApp.stopModal(withCode: .cancel) }
}

// A menu bar while the launcher shows: Quit, and Edit for the text fields'
// copy and paste. The game makes its own (MacApplicationInit).
private func launcherMenu(_ title: String) -> NSMenu {
    let bar = NSMenu()
    let app = NSMenu()
    app.addItem(withTitle: "Quit \(title)", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
    let appItem = NSMenuItem()
    appItem.submenu = app
    bar.addItem(appItem)
    let edit = NSMenu(title: "Edit")
    edit.addItem(withTitle: "Undo", action: Selector(("undo:")), keyEquivalent: "z")
    edit.addItem(withTitle: "Redo", action: Selector(("redo:")), keyEquivalent: "Z")
    edit.addItem(.separator())
    edit.addItem(withTitle: "Cut", action: #selector(NSText.cut(_:)), keyEquivalent: "x")
    edit.addItem(withTitle: "Copy", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
    edit.addItem(withTitle: "Paste", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
    edit.addItem(withTitle: "Select All", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
    let editItem = NSMenuItem()
    editItem.submenu = edit
    bar.addItem(editItem)
    return bar
}

// int scimitar_launcher_run(..., game_file, settings): shows the launcher (unless its "show at
// start" is off and Option is not held) and applies its choices. 1: start the
// game (the window did not show), 2: start it (the window showed), 0: quit.
@_cdecl("scimitar_launcher_run")
public func scimitarLauncherRun(_ id: UnsafePointer<CChar>, _ title: UnsafePointer<CChar>,
                                _ appEnv: UnsafePointer<CChar>, _ appName: UnsafePointer<CChar>,
                                _ defaultApp: UnsafePointer<CChar>, _ gameFile: UnsafePointer<CChar>,
                                _ supported: UnsafePointer<CChar>) -> Int32 {
    let settings = LauncherSettings(game: game(id, title, appEnv, appName, defaultApp,
                                               gameFile: String(cString: gameFile), settings: String(cString: supported)))
    if !settings.showAtStart && settings.gameFound && !NSEvent.modifierFlags.contains(.option)
        && ProcessInfo.processInfo.environment["SCIMITAR_LAUNCHER_TAB"] == nil {
        settings.apply()
        return 1
    }
    let app = NSApplication.shared
    app.setActivationPolicy(.regular)
    app.mainMenu = launcherMenu(settings.game.title)
    var play = false
    let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 560, height: 600),
                          styleMask: [.titled, .closable], backing: .buffered, defer: false)
    let delegate = LauncherWindowDelegate()
    window.delegate = delegate
    window.title = settings.game.title
    window.isReleasedWhenClosed = false
    // Play: the choices applied, the shaders compiled (with progress in the
    // window), then the game.
    let start = {
        settings.apply()
        ShaderProgress.shared.run(game: settings.game, appPath: settings.appPath) {
            play = true
            // The game runs in a fresh image of this process (main.cpp): pass
            // it the activation, or its full screen may come up behind the
            // app in front, its title bar and the menu bar left showing.
            if let id = Bundle.main.bundleIdentifier { app.yieldActivation(toApplicationWithBundleIdentifier: id) }
            app.stopModal()
        }
    }
    let view = LauncherView(settings: settings, onPlay: start, onQuit: { app.stopModal() })
    let hosting = NSHostingView(rootView: view)
    window.contentView = hosting
    window.setContentSize(hosting.fittingSize)
    window.center()
    app.activate(ignoringOtherApps: true)
    window.makeKeyAndOrderFront(nil)
    // SCIMITAR_LAUNCHER_PLAY_AFTER=seconds: Play by itself (tests).
    if let after = ProcessInfo.processInfo.environment["SCIMITAR_LAUNCHER_PLAY_AFTER"].flatMap(Double.init) {
        let timer = Timer(timeInterval: after, repeats: false) { _ in start() }
        RunLoop.main.add(timer, forMode: .modalPanel)
    }
    app.runModal(for: window)
    window.delegate = nil
    window.orderOut(nil)
    settings.save()
    if play { settings.apply() }
    return play ? 2 : 0
}

#else

// void* scimitar_launcher_make(..., on_play): the launcher's view controller
// (retained: the caller takes it with __bridge_transfer). on_play runs on the
// main thread once the choices are applied.
// The whole app is landscape-only (Info.plist, the game's view controller in shell_ios.mm, and this launcher).
final class LauncherHostingController: UIHostingController<LauncherView> {
    override var supportedInterfaceOrientations: UIInterfaceOrientationMask { .landscape }
}

@_cdecl("scimitar_launcher_make")
public func scimitarLauncherMake(_ id: UnsafePointer<CChar>, _ title: UnsafePointer<CChar>,
                                 _ appEnv: UnsafePointer<CChar>, _ appName: UnsafePointer<CChar>,
                                 _ defaultApp: UnsafePointer<CChar>,
                                 _ onPlay: @convention(c) () -> Void) -> UnsafeMutableRawPointer {
    let settings = LauncherSettings(game: game(id, title, appEnv, appName, defaultApp))
    let view = LauncherView(settings: settings, onPlay: {
        settings.save()
        settings.apply()
        ShaderProgress.shared.run(game: settings.game, appPath: settings.appPath) { onPlay() }
    }, onQuit: nil)
    let controller = LauncherHostingController(rootView: view)
    return Unmanaged.passRetained(controller).toOpaque()
}

#endif
