# HID BT Bridge

Flipper Zero application that acts as a bridge between USB and Bluetooth. It allows controlling the mouse and keyboard of a paired device (host) by sending commands from a computer via USB.

## Features
- **USB-BT Bridge**: Receives commands via Serial (USB) and retransmits them as HID (Bluetooth).
- **On-Screen Terminal**: Displays connection logs and received commands on the Flipper display.
- **Three-mode HID state machine**: Main menu, macOS application switcher, and window switcher.

## How to Use
1. Install the `hid_bt_bridge.fap` app on the Flipper Zero (`apps/Bluetooth/`).
2. Open the app on the Flipper and pair with the target device.
3. Use the `controller.py` script on the computer to send commands:
   ```bash
   python3 controller.py type "Hello World"
   python3 controller.py move 10 10
   ```

## Flipper Zero controls

This branch implements a three-mode HID state machine with explicit entry and exit transitions.

### Mode 1 — Main menu

Bluetooth connects and the bridge waits for HID input.

| Physical button | HID action |
|---|---|
| Left | Up Arrow |
| Up | Right Arrow |
| Right | Down Arrow |
| Down | Left Arrow |
| OK | Enter macOS App Switcher with Command+Tab |
| Back (hold) | Open exit confirmation |

A short Back press does not exit the application. A deliberate long Back press opens the exit confirmation screen. `OK` confirms exit and `Back` cancels the confirmation.

### Mode 2 — macOS App Switcher

Press **OK** in Mode 1.

The bridge performs this sequence:

1. Press and hold **Command**.
2. Tap **Tab** once.
3. Keep Command pressed while the app selector is active.

| Physical button | Action |
|---|---|
| Up | Tap Right Arrow; keep Command held |
| Down | Tap Left Arrow; keep Command held |
| OK | Tap Enter/Return; release Command; return to Mode 1 |
| Left | Tap Up Arrow; release Command; enter Mode 3 |
| Back | Cancel; release Command; return to Mode 1 |

### Mode 3 — Window Switcher

Mode 3 is entered from Mode 2 by pressing **Left**. Command has already been released.

The permanent D-pad mapping is active:

| Physical button | HID action |
|---|---|
| Left | Up Arrow |
| Up | Right Arrow |
| Right | Down Arrow |
| Down | Left Arrow |
| OK | Enter/Return; select the highlighted window; return to Mode 1 |
| Back | Start a new Command+Tab sequence and return to Mode 2 |

When Back is pressed in Mode 3, the bridge explicitly reconstructs the Mode 2 HID state: Command is pressed, Tab is tapped once, and Command remains held.

### Safety and state handling

The implementation uses an explicit state machine rather than loosely coupled mode flags:

- `HidModeMain`
- `HidModeAppSwitcher`
- `HidModeWindowSwitcher`
- `HidModeExitConfirm`

Command is released whenever a mode transition requires it and again during final application cleanup. All other keyboard actions are complete press/release taps, so they cannot remain logically held by the bridge.

### On-screen interface

The Flipper display shows the active operating mode and its controls:

- **BT HID BRIDGE** — normal D-pad remap and Bluetooth status.
- **APP SWITCHER** — Command state plus Right/Left navigation, window-mode transition, selection, and cancellation.
- **WINDOW SWITCHER** — D-pad mapping, window selection, and return to the App Switcher.
- **EXIT APPLICATION?** — explicit confirmation before closing the application.

The most recent actions are also recorded in the bridge history log.

### Build requirements

For an Unleashed firmware installation, build against the matching Unleashed SDK. This branch targets the Unleashed **API 88.9** SDK.

    ufbt update --index-url=https://up.unleashedflip.com/directory.json --channel=release
    ufbt

The resulting build should report:

    Target: 7, API: 88.9
