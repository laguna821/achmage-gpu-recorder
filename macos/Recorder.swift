// SPDX-License-Identifier: GPL-3.0-only
// Experimental macOS 13+ backend. Compile/runtime validation needs a Mac.
import AppKit
import Carbon
import ScreenCaptureKit
import AVFoundation
import VideoToolbox

let files = FileManager.default
let dataDir = files.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0].appendingPathComponent("AchmageRecorder")
func writeJSON(_ name: String, _ value: [String: Any]) {
    try? files.createDirectory(at: dataDir, withIntermediateDirectories: true)
    if let data = try? JSONSerialization.data(withJSONObject: value, options: [.prettyPrinted, .sortedKeys]) {
        try? data.write(to: dataDir.appendingPathComponent(name), options: .atomic)
    }
}
func freeBytes(_ url: URL) -> Int64 {
    (try? url.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]).volumeAvailableCapacityForImportantUsage) ?? 0
}
final class CaptureWorker: NSObject, SCStreamOutput, SCStreamDelegate {
    let queue = DispatchQueue(label: "achmage.capture", qos: .userInitiated)
    var stream: SCStream?
    var writer: AVAssetWriter?
    var input: AVAssetWriterInput?
    var started = false
    var stopped = false
    var frames = 0
    var output: URL!
    var partial: URL!
    var errorText = ""
    var timer: DispatchSourceTimer?
    var firstTime = CMTime.invalid
    var firstWall: TimeInterval = 0
    var lastSample: CMSampleBuffer?
    func start(displayID: CGDirectDisplayID, parentPID: Int32, path: String) async throws {
        output = URL(fileURLWithPath: path); partial = URL(fileURLWithPath: path + ".partial.mp4")
        guard freeBytes(output.deletingLastPathComponent()) >= 2 * 1024 * 1024 * 1024 else {
            throw NSError(domain: "AchmageRecorder", code: 1, userInfo: [NSLocalizedDescriptionKey: "Less than 2GiB free"])
        }
        let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: true)
        guard let display = content.displays.first(where: { $0.displayID == displayID }) else {
            throw NSError(domain: "AchmageRecorder", code: 2, userInfo: [NSLocalizedDescriptionKey: "Display unavailable"])
        }
        let scale = min(1.0, min(1920.0 / Double(display.width), 1080.0 / Double(display.height)))
        let width = max(2, Int(Double(display.width) * scale) & ~1), height = max(2, Int(Double(display.height) * scale) & ~1)
        let config = SCStreamConfiguration()
        config.width = width; config.height = height; config.minimumFrameInterval = CMTime(value: 1, timescale: 30)
        config.queueDepth = 3; config.showsCursor = true; config.capturesAudio = false
        config.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
        let excluded = content.windows.filter { $0.owningApplication?.processID == parentPID }
        let filter = SCContentFilter(display: display, excludingWindows: excluded)
        let w = try AVAssetWriter(outputURL: partial, fileType: .mp4)
        let settings: [String: Any] = [
            AVVideoCodecKey: AVVideoCodecType.h264,
            AVVideoWidthKey: width, AVVideoHeightKey: height,
            AVVideoCompressionPropertiesKey: [AVVideoAverageBitRateKey: 8_000_000, AVVideoExpectedSourceFrameRateKey: 30],
            AVVideoEncoderSpecificationKey: [kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder as String: true]
        ]
        let i = AVAssetWriterInput(mediaType: .video, outputSettings: settings)
        i.expectsMediaDataInRealTime = true
        guard w.canAdd(i) else { throw NSError(domain: "AchmageRecorder", code: 3) }
        w.add(i); writer = w; input = i
        guard w.startWriting() else { throw w.error ?? NSError(domain: "AchmageRecorder", code: 4) }
        let s = SCStream(filter: filter, configuration: config, delegate: self)
        try s.addStreamOutput(self, type: .screen, sampleHandlerQueue: queue)
        stream = s; try await s.startCapture()
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now() + 1, repeating: 1)
        t.setEventHandler { [weak self] in
            guard let self = self else { return }
            if freeBytes(self.output.deletingLastPathComponent()) < 2 * 1024 * 1024 * 1024 {
                Task { await self.stop() }
            }
        }
        timer = t; t.resume()
    }
    func stream(_ stream: SCStream, didOutputSampleBuffer sample: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .screen, sample.isValid, !stopped, let w = writer, let i = input else { return }
        guard let attachments = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: false) as? [[SCStreamFrameInfo: Any]],
              let raw = attachments.first?[.status] as? Int, raw == SCFrameStatus.complete.rawValue else { return }
        if !started {
            firstTime = sample.presentationTimeStamp
            firstWall = ProcessInfo.processInfo.systemUptime
            w.startSession(atSourceTime: firstTime); started = true
        }
        if i.isReadyForMoreMediaData {
            if i.append(sample) { frames += 1; lastSample = sample }
            else { errorText = w.error?.localizedDescription ?? "Frame append failed"; Task { await self.stop() } }
        } // backpressure drops frames rather than growing an application queue
    }
    func stream(_ stream: SCStream, didStopWithError error: Error) {
        errorText = error.localizedDescription; Task { await stop() }
    }
    func stop() async {
        guard !stopped else { return }; stopped = true; timer?.cancel(); timer = nil
        try? await stream?.stopCapture(); stream = nil
        await withCheckedContinuation { (c: CheckedContinuation<Void, Never>) in queue.async {
            // Keep a single final frame so a static desktop still has the full
            // recording duration. This retains at most one capture sample.
            if self.started, let last = self.lastSample, let i = self.input, i.isReadyForMoreMediaData {
                let end = self.firstTime + CMTime(seconds: ProcessInfo.processInfo.systemUptime - self.firstWall, preferredTimescale: 600)
                var timing = CMSampleTimingInfo(duration: CMTime(value: 1, timescale: 30), presentationTimeStamp: end, decodeTimeStamp: .invalid)
                var copy: CMSampleBuffer?
                if CMSampleBufferCreateCopyWithNewTiming(allocator: kCFAllocatorDefault, sampleBuffer: last, sampleTimingEntryCount: 1, sampleTimingArray: &timing, sampleBufferOut: &copy) == noErr, let copy = copy {
                    if i.append(copy) { self.frames += 1 }
                }
            }
            self.lastSample = nil
            self.input?.markAsFinished(); c.resume()
        } }
        if let w = writer, started { await w.finishWriting() }
        let ok = writer?.status == .completed && frames > 0 && errorText.isEmpty
        if ok { do { try files.moveItem(at: partial, to: output) } catch { errorText = error.localizedDescription } }
        let success = ok && errorText.isEmpty
        writeJSON("last-recording.json", ["success": success, "frames": frames,
            "hardwareRequired": true, "file": success ? output.path : partial.path, "error": errorText])
        input = nil; writer = nil
        exit(success ? 0 : 1)
    }
}
var activeBroker: Broker?
final class Broker: NSObject, NSApplicationDelegate {
    var menuItem: NSStatusItem!
    var hotKey: EventHotKeyRef?
    var task: Process?
    var pipe: Pipe?
    var file = ""
    var exitWhenDone = false
    var panel: NSPanel?
    func applicationDidFinishLaunching(_ note: Notification) {
        activeBroker = self
        menuItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        menuItem.button?.title = "○"
        let menu = NSMenu()
        menu.addItem(withTitle: "녹화 시작/종료 · ⌘⌥R", action: #selector(toggle), keyEquivalent: "")
        menu.addItem(withTitle: "종료", action: #selector(quit), keyEquivalent: "")
        for item in menu.items { item.target = self }; menuItem.menu = menu
        var event = EventTypeSpec(eventClass: OSType(kEventClassKeyboard), eventKind: UInt32(kEventHotKeyPressed))
        InstallEventHandler(GetApplicationEventTarget(), { _, _, _ in activeBroker?.toggle(); return noErr }, 1, &event, nil, nil)
        let id = EventHotKeyID(signature: 0x41434D47, id: 1)
        let code = RegisterEventHotKey(UInt32(kVK_ANSI_R), UInt32(cmdKey | optionKey), id, GetApplicationEventTarget(), 0, &hotKey)
        if code != noErr { show("단축키 등록 실패", "다른 앱의 ⌘⌥R 설정을 확인해 주세요.") }
        writeJSON("status.json", ["status": code == noErr ? "idle" : "error", "brokerPid": getpid()])
        for command in ["toggle", "stop", "exit"] {
            DistributedNotificationCenter.default().addObserver(self, selector: #selector(commandNotification(_:)),
                name: NSNotification.Name("com.achmage.recorder." + command), object: nil)
        }
        NSWorkspace.shared.notificationCenter.addObserver(self, selector: #selector(stop), name: NSWorkspace.willSleepNotification, object: nil)
        DistributedNotificationCenter.default().addObserver(self, selector: #selector(stop), name: NSNotification.Name("com.apple.screenIsLocked"), object: nil)
    }
    @objc func commandNotification(_ notification: Notification) {
        switch notification.name.rawValue {
        case "com.achmage.recorder.toggle": toggle()
        case "com.achmage.recorder.stop": stop()
        case "com.achmage.recorder.exit": quit()
        default: break
        }
    }
    func show(_ title: String, _ text: String) {
        let alert = NSAlert(); alert.messageText = title; alert.informativeText = text
        alert.addButton(withTitle: "확인"); alert.runModal()
    }
    @objc func toggle() {
        if task != nil { stop(); return }
        guard CGPreflightScreenCaptureAccess() else {
            CGRequestScreenCaptureAccess()
            show("화면 녹화 권한 필요", "시스템 설정에서 Achmage Recorder 화면 녹화 권한을 허용한 뒤 다시 실행해 주세요."); return
        }
        let point = NSEvent.mouseLocation
        guard let screen = NSScreen.screens.first(where: { $0.frame.contains(point) }),
              let display = screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber else { return }
        let dir = files.urls(for: .downloadsDirectory, in: .userDomainMask)[0]
        guard freeBytes(dir) >= 2 * 1024 * 1024 * 1024 else { show("녹화 시작 실패", "저장 공간이 2GiB 미만입니다."); return }
        let format = DateFormatter(); format.dateFormat = "yyyyMMdd_HHmmss_SSS"
        file = dir.appendingPathComponent("Video_" + format.string(from: Date()) + "_" + UUID().uuidString.prefix(6) + ".mp4").path
        let p = Process(); p.executableURL = Bundle.main.executableURL
        p.arguments = ["--worker", String(display.uint32Value), String(getpid()), file]
        let stdin = Pipe(); p.standardInput = stdin
        p.terminationHandler = { [weak self] p in DispatchQueue.main.async {
            guard let self = self else { return }
            self.task = nil; self.pipe = nil; self.menuItem.button?.title = "○"
            self.panel?.close(); self.panel = nil
            writeJSON("status.json", ["status": p.terminationStatus == 0 ? "idle" : "error"])
            if p.terminationStatus != 0 { self.show("녹화 실패", "오류 기록: " + dataDir.appendingPathComponent("last-recording.json").path) }
            else { self.show("녹화 저장 완료", self.file) }
            if self.exitWhenDone { NSApp.terminate(nil) }
        }}
        do {
            try p.run(); task = p; pipe = stdin; menuItem.button?.title = "●"
            writeJSON("status.json", ["status": "recording", "workerPid": p.processIdentifier, "file": file])
        } catch { show("녹화 시작 실패", error.localizedDescription) }
    }
    @objc func stop() { try? pipe?.fileHandleForWriting.close() }
    @objc func quit() {
        if task != nil { exitWhenDone = true; stop(); return }
        NSApp.terminate(nil)
    }
}
@main struct RecorderApp {
    static func main() {
        if CommandLine.arguments.count == 2 {
            let command = CommandLine.arguments[1]
            if command == "status" {
                if let data = try? Data(contentsOf: dataDir.appendingPathComponent("status.json")) { FileHandle.standardOutput.write(data) }
                else { print("{\"status\":\"offline\"}") }
                return
            }
            if ["toggle", "stop", "exit"].contains(command) {
                DistributedNotificationCenter.default().postNotificationName(NSNotification.Name("com.achmage.recorder." + command), object: nil, userInfo: nil, deliverImmediately: true)
                return
            }
        }
        if CommandLine.arguments.count == 5 && CommandLine.arguments[1] == "--worker" {
            let worker = CaptureWorker()
            Task {
                do {
                    try await worker.start(displayID: UInt32(CommandLine.arguments[2])!, parentPID: Int32(CommandLine.arguments[3])!, path: CommandLine.arguments[4])
                    DispatchQueue.global().async { _ = FileHandle.standardInput.readDataToEndOfFile(); Task { await worker.stop() } }
                } catch {
                    writeJSON("last-recording.json", ["success": false, "error": error.localizedDescription]); exit(1)
                }
            }
            RunLoop.main.run()
        } else {
            let app = NSApplication.shared; let delegate = Broker()
            app.setActivationPolicy(.accessory); app.delegate = delegate
            app.run()
        }
    }
}
