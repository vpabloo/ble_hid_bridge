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
