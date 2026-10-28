# ESP32 Bluetooth Relay Remote Control

[简体中文](README.md) | English

This project is the **ESP32 receiver for `remote-control-app`**. The mobile app stores names and device codes for multiple devices and sends Bluetooth On/Off advertisements to the **currently selected device**. The ESP32 uses the **device code** to identify commands intended for it and control the relays. The device code saved in the app must match the value configured in the receiver firmware.

## Using the Receiver with remote-control-app

The instructions below use the app's English UI labels. You can switch between Chinese and English using the language menu in the top-right corner of the home screen. The **Firmware Source Code** link at the bottom of the home screen opens this repository.

### 1. Get the Device Code

**For this firmware, the device code is the `kNetIdUuid` configured in the ESP32 firmware. It is also the iBeacon UUID carried in the Bluetooth advertisement. The currently selected device in the app and the receiver must use the same value.**

Enter the complete UUID as the device code, for example:

```text
12345678-1234-5678-9abc-def012345678
```

The value above is only an example. To find the actual device code:

- **For an already deployed device:** ask the person who configured it for the device code.
- **When configuring a device yourself:** check `kNetIdUuid` in `include/local_config.h`. The entire string inside the quotation marks is the device code. Initial configuration and flashing instructions are provided below.
- **To check the configuration of a flashed device:** connect to its serial port and find `Configured netId UUID: ...` in the startup log. The value after the colon is the device code used by that firmware.

The device code is configured during deployment and flashed to the ESP32 as part of the firmware. The receiver only processes advertisements with a matching UUID. The app's **Device Name** can be any name that helps you identify the device, such as "Home Garage"; it is not used to match advertisements.

When saving a device, the app only checks that the name and device code are nonempty after trimming leading and trailing whitespace. It preserves the letter case of the device code and **does not validate the UUID format when saving**. The code is converted to a UUID only when sending a command. Saving successfully therefore does not guarantee that the receiver can be controlled: you must still enter a valid, complete UUID that matches the receiver.

### 2. Add and Select a Device in the App

1. Wire the ESP32 receiver, flash the firmware, and power it on.
2. On first use, the home screen shows **No Devices**. Tap **Add Device** to open **Devices**, then tap **Add Device** (the plus button) in the top-right corner. If devices already exist, open the same list through **Switch Device** or **Select Device** on the home screen.
3. Enter a **Device Name** and **Device Code**. Use a recognizable name and the complete UUID actually flashed to the receiver, then tap **Save**.
4. A newly saved device becomes the current device automatically. After the editor closes, you remain on the device list. Tap the device entry to return to the home screen and verify the name and code shown under **Current Device**.
5. Turn on Bluetooth on your phone, allow the app to use Bluetooth, and wait for **Bluetooth Ready** on the home screen. The **On** and **Off** buttons are unavailable when no device is selected or Bluetooth is not ready.
6. Keep the app in the foreground and tap **On** or **Off** while within Bluetooth range of the receiver. When the device code matches, the receiver triggers the corresponding relay with a pulse lasting approximately **500 milliseconds**.

The current protocol uses Bluetooth advertisements and requires neither Wi-Fi nor Bluetooth pairing. The app does not scan for nearby receivers; devices must be added manually.

### 3. Commands and Advertising Behavior

- Each tap creates an iBeacon advertisement using the currently selected device's code. **On** corresponds to `major=1`, and **Off** to `major=2`. The `minor` field is a 16-bit command sequence number that increments with each transmission.
- Each advertisement runs for **1 second** and then stops automatically. If you tap again during that second, the app stops the previous advertisement, immediately sends the new command, and restarts the 1-second timer from the latest tap.
- Opening the device list, switching devices, or editing or deleting the current device stops any ongoing advertisement. Advertising also stops when the app becomes inactive, such as when it moves to the background, or when Bluetooth becomes unavailable. After returning to the foreground or restoring Bluetooth, tap again to send a command.
- Adding, saving, selecting, or deleting a device never sends an On/Off command automatically. These operations only update device information and selection stored locally on the phone; they do not change the device code in the receiver firmware.

The receiver does not acknowledge command execution. **Bluetooth Ready** or **Sending for 1s. Tap again to resend.** only describes the phone's state. Verify execution through the device's physical response or its serial log.

For consecutive operations, wait for the previous hardware action to finish before tapping again. The app immediately switches to the latest command, but the receiver ignores new commands while an action is pending or running and does not queue them. Stopping an advertisement does not cancel an action the receiver has already accepted. See [Duplicate Advertisements and Consecutive Commands](#duplicate-advertisements-and-consecutive-commands) for receiver deduplication and retry behavior.

### 4. Manage Multiple Devices

- **Switch devices:** open **Devices** from the home screen and tap the target entry to select it and return to the home screen. Subsequent taps on **On** or **Off** use that device's code.
- **Edit a device:** tap the ellipsis menu on the right of its entry and choose **Edit** to change its name or code. Editing an existing device preserves the current selection. If you change the current device's code, the next transmission uses the new value.
- **Delete a device:** choose **Delete** from the same menu and confirm. Deleting the current device clears the selection; another device is not selected automatically. Select a remaining device manually or add a new one before sending another command.
- **Local storage:** the device list and current selection are saved on the phone and restored when the app is reopened.
- **Upgrade from the single-device version:** the previously saved device code is migrated once into a device record and selected automatically. Its initial name is the old device code; use **Edit** to give it a recognizable name.

### 5. Device Code FAQ

| Question | Answer |
| --- | --- |
| What should I enter as the device code? | Enter the complete UUID from `kNetIdUuid` in the receiver firmware, including hyphens. Hexadecimal letters may be uppercase or lowercase. |
| I can save a device, but tapping a button shows "Unable to create a Bluetooth broadcast for this device code". | Saving only checks required fields; UUID conversion happens when sending. Edit the device and replace its code with the complete UUID actually used by the receiver. |
| The device does not respond after I enter its code. | First check **Current Device** and its code on the home screen against the UUID actually flashed to the target receiver. Editing the local configuration file without reflashing does not change the receiver's code. Then check power, Bluetooth permissions, whether the app is in the foreground, and distance. |
| The On and Off buttons are unavailable after I delete a device. | Deleting the current device clears the selection. Select another device from the list or add a new one, and make sure Bluetooth is ready. |
| How do I change the receiver's device code? | Change `kNetIdUuid` in `include/local_config.h`, rebuild and flash the firmware, and update the app to use the new code. Changing only the code in the app does not change the receiver's configuration. |
| What happens if multiple receivers use the same device code? | All receivers within advertising range whose device codes match may respond. Assign a different UUID to each receiver if you need to control them separately. |

## Hardware Deployment and Development Reference

The following sections cover initial hardware deployment, reflashing, and troubleshooting.

The firmware uses **ESP32 + Arduino + PlatformIO**. The ESP32 continuously performs a passive scan for iBeacon advertisements, matches the configured UUID (`netId`), and triggers the open or close relay according to the advertisement's `major` value. These are the commands sent by the app's **On** and **Off** buttons. Each trigger outputs a low pulse of approximately 500 milliseconds, then returns the pin high; it does not keep the relay energized.

### 1. Prepare the Hardware and Development Environment

- An ESP32 development board. The current PlatformIO configuration uses `esp32dev`.
- A USB data cable for power, flashing, and serial logs.
- A two-channel relay module with active-low inputs compatible with ESP32 3.3 V logic.
- An iPhone with `remote-control-app` installed. For protocol debugging, you can also use an iBeacon advertising tool or BLE transmitter that supports custom UUID, major, and minor values.
- A development environment: VS Code with the [PlatformIO IDE extension](https://platformio.org/install/ide?install=vscode) is recommended. Alternatively, install [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html) to use the command line.

After installation, open the project root in VS Code. Run all commands below from the project root. When using the extension, run `pio` commands in the PlatformIO Core CLI terminal.

### 2. Wiring

| ESP32 Pin | Connection | Function |
| --- | --- | --- |
| GPIO25 | Control input of the open relay, such as IN1 | Goes low for approximately 500 milliseconds on an open command |
| GPIO26 | Control input of the close relay, such as IN2 | Goes low for approximately 500 milliseconds on a close command |
| GND | Control-side GND of the relay module | Common ground for a conventional non-isolated connection |

Power the relay module at its rated voltage. Connect GPIO pins only to compatible control inputs; do not drive relay coils or loads directly from GPIO pins. Connect the relay contacts according to the controlled device's control interface. The current firmware provides momentary open/close signals, equivalent to a short button press.

The default idle level is `HIGH`, and the active level is `LOW`. The two outputs are mutually exclusive: a pulse on one channel does not trigger the other channel at the same time.

### 3. Configure the Receiver UUID (Device Code)

For the first deployment, create the local configuration file from the template. If `include/local_config.h` already exists, edit it directly.

```bash
cp include/local_config.example.h include/local_config.h
```

Set your own `kNetIdUuid` in `include/local_config.h`. This is the device code you will later enter in `remote-control-app`, for example:

```cpp
#pragma once

constexpr char kNetIdUuid[] = "12345678-1234-5678-9abc-def012345678";
```

- The UUID must use the `8-4-4-4-12` format with 36 characters in total. Hexadecimal letters may be uppercase or lowercase.
- The device code in `remote-control-app` must match this value. The value above is for demonstration only; replace it with your own UUID.
- `include/local_config.h` is listed in `.gitignore`, so you do not need to commit your personal configuration.
- The UUID is compiled into the firmware. Rebuild and flash the firmware after changing it.

The UUID only filters advertisements. The current protocol provides no authentication or encryption.

### 4. Select the Serial Port, Build, and Flash

Connect the ESP32 and list the serial devices:

```bash
pio device list
```

Edit `platformio.ini` and set `upload_port` and `monitor_port` to the actual serial port shown in the device list. The repository currently uses the following configuration; adjust the port paths for your computer:

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
upload_port = /dev/tty.usbserial-A5069RR4
monitor_port = /dev/tty.usbserial-A5069RR4
monitor_speed = 115200
```

Typical port names include `/dev/cu.usbserial-xxxx` on macOS, `/dev/ttyUSB0` on Linux, and `COM3` on Windows. Always use the actual value from the device list.

Build the firmware:

```bash
pio run -e esp32dev
```

The first build requires an internet connection so PlatformIO can download the platform, toolchain, and Arduino framework.

Flash the development board:

```bash
pio run -e esp32dev -t upload
```

Open the serial monitor:

```bash
pio device monitor -b 115200
```

Press `Ctrl+C` to exit the monitor. Close it before flashing again to release the serial port. You can also select the `esp32dev` environment in the VS Code PlatformIO panel and use Build, Upload, and Monitor.

To see the complete startup log after opening the monitor, press the board's reset button. A normal startup produces output similar to:

```text
Booting BLE beacon relay scanner...
Configured netId UUID: 12345678-1234-5678-9abc-def012345678
Command mapping: major 1=open, major 2=close
Scan window=100ms, scan interval=100ms, duplicate ignore=3500ms
BLE passive scan started
```

### 5. Debugging: Send Open/Close Commands Manually

For everyday use, `remote-control-app` sends the commands. The following steps are only for debugging the receiver protocol with other tools.

Select **iBeacon advertising mode** in your debugging tool and configure these fields:

| Field | Value |
| --- | --- |
| UUID | Must match `kNetIdUuid` in `include/local_config.h` |
| major | `1` for open, `2` for close; other values are ignored |
| minor | `0`–`65535`, used as the command sequence number; use a new number for each new operation |
| Tx Power | Set as required by the transmitting tool; the receiver does not use this field to decide which action to take |

Example of a complete operation:

1. Set the matching UUID, `major=1`, and `minor=1`. Advertise briefly, for example for approximately 1 second, then stop.
2. When the receiver matches the advertisement, GPIO25 outputs a low pulse of approximately 500 milliseconds, and the serial log shows the open command.
3. Wait for the pulse to finish, set `major=2` and `minor=2`, then advertise briefly again and stop.
4. GPIO26 outputs a low pulse of approximately 500 milliseconds, and the serial log shows the close command.

Example log for an open command (RSSI varies with distance and the environment):

```text
Accepted beacon: netId=12345678-1234-5678-9abc-def012345678 major=1 minor=1 action=open rssi=-60
Executing open pulse on GPIO25
Open pulse complete
```

A close command produces `action=close`, `Executing close pulse on GPIO26`, and `Close pulse complete`. The receiver does not send an acknowledgement to the transmitter. Check the receiver logs and physical hardware response to confirm reception and execution.

#### Duplicate Advertisements and Consecutive Commands

- The receiver identifies duplicate commands by `(major, minor)` and stores up to 4 recent entries. While an entry is retained, the same pair will not execute again within 3500 milliseconds of being recorded.
- The deduplication window is not permanent: if the same advertisement continues beyond the window, it may trigger another action. Stop advertising after each operation.
- New commands are ignored without being queued while a relay action is pending or running. Commands ignored while busy are also recorded for deduplication. To retry, wait until the receiver is idle and use a new `minor`, or wait for the original entry's deduplication window to expire.
- Matching uses UUID, major, and minor. The receiver does not restrict transmitter addresses or use RSSI as a distance threshold for triggering.

#### Advertisement Format for Custom Transmitters

The receiver requires Manufacturer Data to be **exactly 25 bytes** long. This length includes the two-byte Company ID but excludes the outer BLE AD length and type fields:

```text
4C 00 | 02 15 | UUID (16 bytes) | major (2 bytes) | minor (2 bytes) | Tx Power (1 byte)
```

- The Company ID is `0x004C`, transmitted as `4C 00`, followed by the iBeacon identifier `02 15`.
- Send the UUID bytes in the order obtained by removing the hyphens from its standard text representation. Both major and minor use big-endian byte order. For example, a major value of 1 is encoded as `00 01`.
- If the transmitting tool takes Company ID as a separate parameter, enter `0x004C` there and start the remaining data with `02 15` to avoid adding Company ID twice.
- The receiver uses passive scanning. The iBeacon data must be in the advertising packet, not only in a Scan Response that requires active scanning to retrieve.

### 6. Troubleshooting

| Symptom | What to Check |
| --- | --- |
| The build cannot find `local_config.h`. | Copy the template as described in [Configure the Receiver UUID (Device Code)](#3-configure-the-receiver-uuid-device-code) and make sure the file is at `include/local_config.h`. |
| `pio: command not found` | Use the PlatformIO Core CLI terminal, or install PlatformIO Core and configure PATH. |
| Upload fails or the serial port cannot be found. | Confirm the port with `pio device list`. Check the USB data cable, drivers, and whether another program is using the port. Some boards require holding the BOOT button while flashing. |
| Serial output is garbled. | Set the monitor baud rate to `115200`. |
| `Invalid netId UUID in local_config.h` | Check the UUID length, hyphen positions, and hexadecimal characters, then reflash after correcting it. |
| `BLE passive scan failed to start` | The firmware retries approximately every 1 second. If failures continue, check power, reset the device, and watch subsequent logs. |
| Advertising produces no action and no command log. | Make sure the transmitter is sending iBeacon advertisements, the UUID matches, the format is correct, and the data is in the advertising packet. Nonmatching advertisements and duplicate commands are silently ignored. |
| `unsupported major` | Use only `1` or `2` for major on the transmitter. |
| `Ignoring beacon while relay busy` | Wait for the current action to finish, then retry with a new minor value. |
| The same command triggers repeatedly. | Check whether the transmitter keeps advertising. Stop advertising after each operation. |
| The log reports successful execution, but the relay does not respond. | Check GPIO25 / GPIO26 wiring, module power, a shared control-side ground, and compatibility with active-low inputs. The log only confirms that a GPIO pulse was executed; there is no feedback about the relay or load state. |

### 7. Files and Adjustable Parameters

| File | Purpose |
| --- | --- |
| `src/main.cpp` | iBeacon parsing, command deduplication, BLE scanning, and relay control |
| `include/local_config.example.h` | UUID configuration template |
| `include/local_config.h` | Local UUID configuration, created on first use and excluded from Git |
| `platformio.ini` | Board, framework, upload port, and serial monitor configuration |

To change the hardware configuration or timing, edit the constants near the top of `src/main.cpp` and reflash:

| Constant | Default | Meaning |
| --- | --- | --- |
| `kPinOpen` / `kPinClose` | `25` / `26` | Open / close output pins |
| `kRelayActiveLevel` / `kRelayIdleLevel` | `LOW` / `HIGH` | Relay active / idle levels |
| `kRelayPulseMs` | `500` | Output pulse duration in milliseconds |
| `kDuplicateIgnoreMs` | `3500` | Duplicate command suppression window in milliseconds |
| `kScanIntervalMs` / `kScanWindowMs` | `100` / `100` | BLE scan interval / window in milliseconds |
| `kMajorOpen` / `kMajorClose` | `1` / `2` | Major values for open / close commands |
