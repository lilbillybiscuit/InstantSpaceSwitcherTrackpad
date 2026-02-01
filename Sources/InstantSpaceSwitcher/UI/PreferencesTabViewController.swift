import AppKit

final class PreferencesTabViewController: NSTabViewController {
    override func viewDidLoad() {
        super.viewDidLoad()
        
        tabStyle = .toolbar
        
        let shortcutsTab = NSTabViewItem(viewController: KeyboardShortcutsViewController())
        shortcutsTab.label = "Keyboard"
        shortcutsTab.image = NSImage(systemSymbolName: "keyboard", accessibilityDescription: "Keyboard")
        
        addTabViewItem(shortcutsTab)
    }
}
