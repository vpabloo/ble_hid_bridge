## Context

- **Objective**: Flipper Zero App that acts as a "BLE HID Bridge". It receives commands from a computer via USB (Serial/CDC) and retransmits them as keyboard/mouse commands via Bluetooth to a paired device (host).
- **Status**: Fully functional C application implemented in `applications_user/hid_ble_bridge/`. Python control script (`controller.py`) ready for use.
- **History and Fixes**:
  - The app was rewritten from scratch using the `ble_profile` library (firmware standard) and referencing the official "Bluetooth Remote" application.
  - **Crash Fix (Null Pointer)**: The previous version suffered from crashes due to incorrect manual management of GATT descriptors and lifecycle. The new version uses the `ble_profile` and `ble_profile_hid` API, which abstracts complexity and ensures safe initialization/finalization.
  - **Responsive UI**: Implemented interface that reflects the Bluetooth connection state.

## Project Structure

- **App Firmware**: `unleashed-firmware/applications_user/hid_ble_bridge/`
  - `ble_hid_bridge.c`: Main logic, UI, Threads (CDC and Main), and BLE callbacks.
  - `application.fam`: Application manifest (configured as external app, Bluetooth category).
- **Controller**: `controller.py` (Python script to send commands from PC to Flipper).

## Solution Architecture

1.  **BLE Profile (HID)**:
    - Flipper uses the `ble_profile_hid` template to behave as a standard HID device (Keyboard + Mouse).
    - Automatically manages advertising, pairing, and reconnection.

2.  **USB CDC Bridge (Serial)**:
    - A dedicated thread (`cdc_thread_task`) is started to read data from the Virtual USB CDC port without blocking the main thread or BLE stack.
    - This prevents data loss and UI freezes during fast transfers.

3.  **Operation Flow**:
    - **Start**: The app starts USB and Bluetooth. The screen displays "Connecting to BT...".
    - **Connection**: User connects the Bluetooth Host to Flipper.
    - **Activation**: Upon detecting the connection (`BtStatusConnected`), the UI changes to "BT HID Bridge Active". The app is ready to relay.
    - **Transmission**: The `controller.py` script sends commands (e.g., `M MOVE 10 10`) via serial. The CDC thread receives, parser processes, and the `ble_profile_hid_*` function sends the HID command via Bluetooth.

## Usage Guide

### 1. Installation on Flipper
Compile and install the application:
```bash
./fbt fap_hid_ble_bridge
# Copy build/f7-firmware-D/.extapps/hid_ble_bridge.fap to Flipper (apps/Bluetooth folder)
```

### 2. Execution
1. On Flipper, open `Apps > Bluetooth > BT Remote Bridge`.
2. The screen will show "Connecting to BT...".
3. On the target computer/phone, pair with the Flipper Bluetooth device.
4. When connected, the screen will change to "BT HID Bridge Active".

### 3. Controller (PC via USB)
Connect Flipper via USB to the controller computer and run the script.

**Environment Setup (Mandatory)**:
This script requires the use of a virtual environment (venv) to avoid dependency conflicts and ensure correct execution. The error `module 'serial' has no attribute 'Serial'` is common when dependencies are mixed in the global environment.

```bash
# 1. Create and activate venv
python3 -m venv .venv
source .venv/bin/activate  # Linux/macOS
# .venv\Scripts\activate   # Windows

# 2. Install dependencies
# Attention: The 'serial' package conflicts with 'pyserial'. If an attribute error occurs, run:
# pip uninstall serial && pip install --force-reinstall pyserial
pip install -r requirements.txt
```

**Usage Examples**:

```bash
# Move mouse (X=100, Y=50)
python3 controller.py move 100 50

# Click left button
python3 controller.py btn 1

# Type text
python3 controller.py type "Hello World"
```

The app on Flipper will show the last received command on the screen.
