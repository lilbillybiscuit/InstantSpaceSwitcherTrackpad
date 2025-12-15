import SwiftUI
import ISS
import Carbon

@main
struct InstantSpaceSwitcherApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate
    
    var body: some Scene {
        Settings {
            EmptyView()
        }
    }
}

class AppDelegate: NSObject, NSApplicationDelegate {
    var statusItem: NSStatusItem!
    
    func applicationDidFinishLaunching(_ notification: Notification) {
        // Initialize the event tap for trusted gesture posting
        if !iss_init() {
            print("Failed to initialize ISS event tap")
        }
        
        setupStatusItem()
        registerHotkeys()
    }
    
    func applicationWillTerminate(_ notification: Notification) {
        iss_destroy()
    }
    
    func setupStatusItem() {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        if let button = statusItem.button {
            button.image = NSImage(systemSymbolName: "arrow.left.and.right.square", accessibilityDescription: "InstantSpaceSwitcher")
        }
        
        let menu = NSMenu()
        menu.addItem(NSMenuItem(title: "Quit InstantSpaceSwitcher", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q"))
        statusItem.menu = menu
    }
    
    func registerHotkeys() {
        // Register Command + Option + Control + Arrow Left/Right
        
        // Modifiers: cmd (256) + opt (2048) + ctrl (4096)
        let modifiers = cmdKey | optionKey | controlKey
        
        // Left
        HotKeyManager.shared.register(keyCode: UInt32(kVK_LeftArrow), modifiers: UInt32(modifiers)) {
            iss_switch(ISSDirectionLeft)
        }
        
        // Right
        HotKeyManager.shared.register(keyCode: UInt32(kVK_RightArrow), modifiers: UInt32(modifiers)) {
            iss_switch(ISSDirectionRight)
        }
    }
}

class HotKeyManager {
    static let shared = HotKeyManager()
    private var hotKeys: [UInt32: () -> Void] = [:]
    private var currentId: UInt32 = 1
    
    private init() {
        installEventHandler()
    }
    
    func register(keyCode: UInt32, modifiers: UInt32, handler: @escaping () -> Void) {
        let id = currentId
        currentId += 1
        
        var hotKeyRef: EventHotKeyRef?
        let hotKeyID = EventHotKeyID(signature: 0x1111, id: id)
        
        RegisterEventHotKey(keyCode, modifiers, hotKeyID, GetApplicationEventTarget(), 0, &hotKeyRef)
        
        hotKeys[id] = handler
    }
    
    private func installEventHandler() {
        var eventSpec = EventTypeSpec(eventClass: OSType(kEventClassKeyboard), eventKind: UInt32(kEventHotKeyPressed))
        
        InstallEventHandler(GetApplicationEventTarget(), { (_, event, _) -> OSStatus in
            var hotKeyID = EventHotKeyID()
            GetEventParameter(event, EventParamName(kEventParamDirectObject), EventParamType(typeEventHotKeyID), nil, MemoryLayout<EventHotKeyID>.size, nil, &hotKeyID)
            
            if let handler = HotKeyManager.shared.hotKeys[hotKeyID.id] {
                handler()
            }
            
            return noErr
        }, 1, &eventSpec, nil, nil)
    }
}
