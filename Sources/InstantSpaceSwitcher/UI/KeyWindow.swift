import AppKit

final class KeyWindow: NSWindow {
    override var canBecomeKey: Bool { true }
    override var canBecomeMain: Bool { true }
    
    override func performKeyEquivalent(with event: NSEvent) -> Bool {
        // Handle Cmd+W to close window
        if event.modifierFlags.contains(.command) && event.charactersIgnoringModifiers == "w" {
            performClose(nil)
            return true
        }
        return super.performKeyEquivalent(with: event)
    }
}
