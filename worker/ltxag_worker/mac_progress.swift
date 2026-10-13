// macOS progress window with a Cancel button (decision 64), built on first use by oslayer.py.
//
//   ltxag_progress <title> <width>
//
// Reads the same stdin lines as `zenity --progress --auto-close`: "# text" sets the text, a
// number sets the percentage; 100 or the end of input closes the window (exit 0). Cancel or the
// window's close button exits 1, which the caller sees as the process having gone.
import AppKit

final class FirstClickButton: NSButton {
    // The window belongs to a background app: without this the first click only activates it.
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
}

final class Handler: NSObject, NSWindowDelegate {
    @objc func cancel(_ sender: Any?) { exit(1) }
    func windowWillClose(_ notification: Notification) { exit(1) }
}

let args = CommandLine.arguments
let title = args.count > 1 ? args[1] : "Progress"
let width = CGFloat(args.count > 2 ? Double(args[2]) ?? 520 : 520)

let app = NSApplication.shared
app.setActivationPolicy(.accessory)  // no Dock icon or menu bar, and Flame keeps the focus
let handler = Handler()

let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: width, height: 120),
                      styleMask: [.titled, .closable], backing: .buffered, defer: false)
window.title = title
window.level = .floating  // stays above Flame's window
window.isReleasedWhenClosed = false
window.delegate = handler

let label = NSTextField(labelWithString: "Starting...")
label.frame = NSRect(x: 20, y: 80, width: width - 40, height: 20)
label.lineBreakMode = .byTruncatingTail

let bar = NSProgressIndicator(frame: NSRect(x: 20, y: 52, width: width - 40, height: 20))
bar.isIndeterminate = false
bar.minValue = 0
bar.maxValue = 100

let button = FirstClickButton(title: "Cancel", target: handler, action: #selector(Handler.cancel(_:)))
button.frame = NSRect(x: width - 120, y: 12, width: 100, height: 32)

for view in [label, bar, button] as [NSView] {
    window.contentView?.addSubview(view)
}
window.center()
window.orderFrontRegardless()

Thread {
    while let line = readLine() {
        let text = line.trimmingCharacters(in: .whitespaces)
        DispatchQueue.main.async {
            if text.hasPrefix("#") {
                label.stringValue = String(text.dropFirst()).trimmingCharacters(in: .whitespaces)
            } else if let pct = Double(text) {
                if pct >= 100 { exit(0) }
                bar.doubleValue = pct
            }
        }
    }
    DispatchQueue.main.async { exit(0) }  // the caller closed its end (or died)
}.start()

app.run()
