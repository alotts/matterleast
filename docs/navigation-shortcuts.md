# Navigation tab and exit shortcuts

These window-level shortcuts operate on the [central navigation tabs](navigation-tabs.md)
and the application exit path. They do not create a second navigation model; they
route through the existing tab controller.

| Shortcut | Action |
| --- | --- |
| `Ctrl+W` | Close the active central navigation tab |
| `Ctrl+PgUp` | Activate the previous navigation tab (wrap-around) |
| `Ctrl+PgDn` | Activate the next navigation tab (wrap-around) |
| `Ctrl+Shift+Q` | Quit the application |

## Tab shortcuts

`NavigationUiController` owns the tab shortcuts next to the browser Back/Forward
shortcuts. They are created in `setupTabShortcuts()` once the tab bar exists.

- `Ctrl+W` is equivalent to the tab's X button / middle-click: it calls
  `closeTab(activeTabIndex)` and never closes a docked right-hand thread. It is a
  no-op when there is at most one tab (the model's empty-fallback would only
  recreate a tab) and when the current central surface is a collection page
  (Saved, Drafts, Search) rather than a tab, so a background tab is never closed
  while an unrelated page is visible.
- `Ctrl+PgUp` / `Ctrl+PgDn` cycle with wrap-around through
  `activateTab(index)`, which performs the outgoing bookmark save and incoming
  restore. They are no-ops when there is at most one tab. Unlike `Ctrl+W`,
  cycling is allowed while a collection page is visible so it is a way back into
  the tab set.
- Cycling uses `NavigationTabsModel::cycledIndex()`, a pure wrap-around helper.
  An out-of-range `activeTabIndex` (for example `-1` before any tab is active)
  seeds the move from just before the first entry when going forward and from the
  first entry when going backward, rather than restarting from `0`.

## Exit shortcut

`Ctrl+Shift+Q` is a File > Quit menu item with `Qt::ApplicationShortcut` and
`QAction::QuitRole`. It calls `qApp->quit()`, the same path as the tray Quit
action, so `QApplication::aboutToQuit` saves the main-window and navigation
session state and the tray "hide on close" behavior is not triggered.
