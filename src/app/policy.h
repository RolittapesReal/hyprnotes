#pragma once
// Pure lifecycle rules (spec 4.3 / desktop design section 4) so they can be unit tested.
namespace hn::app {

enum class OrganizerClose { HideToTray, CloseNormally };

// Organizer close hides only when the tray is enabled AND usable; otherwise it closes via the save path.
constexpr OrganizerClose organizerCloseAction(bool trayEnabled, bool trayAvailable) {
    return trayEnabled && trayAvailable ? OrganizerClose::HideToTray : OrganizerClose::CloseNormally;
}
// Closing the last application window exits only when no usable tray keeps the instance reachable.
// Workspace switches are not closes; hidden-but-open stickies still count as windows.
constexpr bool exitAfterWindowClosed(bool trayUsable, int remainingWindows) {
    return !trayUsable && remainingWindows == 0;
}

} // namespace hn::app
