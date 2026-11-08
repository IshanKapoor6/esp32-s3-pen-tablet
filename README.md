# ESP32-S3 Pen Tablet

Turn a Waveshare ESP32-S3-Touch-LCD-4.3B into a USB HID mousepad for a PC. The project reuses the board setup and LVGL display port from the Desktop `spotify_esp32_display` project and reads the GT911 capacitive touchscreen.

## What the screen shows

The display shows a dark diagnostic screen. It says **WAITING FOR TOUCH** until the touch controller reports a point, then displays **TOUCH DETECTED** with X/Y coordinates and a dot at that position. This is useful for checking touch independent of PC mouse movement.

## Mouse behavior

- Touch and move to move the PC pointer (relative mousepad motion).
- Tap without moving to click.
- Swipes only move the pointer; dragging is disabled for now.
- Touch release is debounced to prevent brief gaps in touch reports from generating repeated clicks.
- Sensitivity is set to 3x the raw touch movement.
- With multiple contacts, the smallest reported GT911 contact area is used as a stylus proxy; much larger remaining contacts are ignored until all contacts lift.
- A finger or capacitive stylus works. Passive plastic pens do not. No pressure or tilt data is available.
- Hold four contacts for about 0.65 seconds to switch between mouse and controller modes. The switching gesture is ignored as input until the contacts lift.

## Controller behavior

- In controller mode, touch the left side of the screen to move the USB HID gamepad's left stick.
- Touch the colored Y, X, B, and A areas on the right to hold those buttons. The lower edge maps to Select and Start.
- Multiple contacts can operate the stick and buttons at the same time. Releasing a contact releases its gamepad input.
- Hold four contacts again for about 0.65 seconds to return to mouse mode.

The GT911 does not identify an active pen separately from a finger. Multi-touch rejection is therefore an area-size heuristic: a broad-tipped capacitive stylus may not separate cleanly from fingers, and a single finger touch is accepted as normal input.

## Hardware

- Waveshare ESP32-S3-Touch-LCD-4.3B (800x480 RGB LCD, GT911 capacitive touch)
- USB-C data cable connected to the ESP32-S3 native USB/OTG port

## Arduino IDE setup

Install the libraries used by the Spotify display project:

- `ESP32_Display_Panel`
- `esp-lib-utils`
- `ESP32_IO_Expander`
- `lvgl` (v8.4.x)

The ESP32 Arduino core provides `USBHIDMouse.h` and `USBHIDGamepad.h`. Select **ESP32S3 Dev Module**:

| Setting | Value |
|---|---|
| PSRAM | OPI |
| Flash Mode | QIO 80MHz |
| Flash Size | 16MB |
| USB Mode | USB-OTG (TinyUSB) |
| USB CDC On Boot | Enabled |

If the firmware HID mouse is running and Arduino IDE shows the cached port as disconnected, enter ROM download mode by unplugging USB, holding BOOT, reconnecting USB while holding BOOT, then releasing BOOT. Select the newly connected port and upload.
