# HID BT Bridge

Flipper Zero application that acts as a bridge between USB and Bluetooth. It allows controlling the mouse and keyboard of a paired device (host) by sending commands from a computer via USB.

## Features
- **USB-BT Bridge**: Receives commands via Serial (USB) and retransmits them as HID (Bluetooth).
- **On-Screen Terminal**: Displays connection logs and received commands on the Flipper display.

## How to Use
1. Install the `hid_bt_bridge.fap` app on the Flipper Zero (`apps/Bluetooth/`).
2. Open the app on the Flipper and pair with the target device.
3. Use the `controller.py` script on the computer to send commands:
   ```bash
   python3 controller.py type "Hello World"
   python3 controller.py move 10 10
   ```

## Flipper Zero controls

This branch adds a native macOS application switcher and a permanent D-pad remap.

### Normal mode

The physical Flipper buttons are remapped as follows:

| Physical button | HID action |
|---|---|
| Left | Up Arrow |
| Up | Right Arrow |
| Right | Down Arrow |
| Down | Left Arrow |
| Back | Back / exit the application |
| OK | Start App Switcher |

The D-pad remap is active for the entire time the bridge application is running.

### macOS App Switcher

Press **OK** to enter the App Switcher. The application then presses and holds the macOS **Command** key through the HID interface.

While Command is held:

| Physical button | Action |
|---|---|
| Up | Tap Tab and keep Command held |
| Left | Tap `1` (Command+1), then release Command and return to normal mode |
| Back | Cancel immediately, release Command, and return to normal mode |

This allows the user to repeatedly press **Up** to cycle through applications using macOS Command+Tab. When the desired application is highlighted, **Left** sends Command+1 and releases Command.

Command is also released automatically during application cleanup, providing a safety path against leaving the modifier logically pressed.

### On-screen interface

The Flipper display shows the current operating mode:

- **BT HID BRIDGE** — normal D-pad remap and Bluetooth status.
- **APP SWITCHER** — Command state and the controls for Tab, Command+1, and cancellation.

The most recent actions are also recorded in the bridge history log.

### Build requirements

For an Unleashed firmware installation, build against the matching Unleashed SDK. This branch was tested with the Unleashed **API 88.9** SDK.

    ufbt update --index-url=https://up.unleashedflip.com/directory.json --channel=release
    ufbt

The resulting build should report:

    Target: 7, API: 88.9

