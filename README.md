# Sixaxis/DS4 Pairer – Windows Command-Line Tool

This tool lets you **read** or **set** the Bluetooth pairing MAC address of a Sony Sixaxis, DualShock 3, or DualShock 4 controller directly on Windows, without extra drivers or complex tools.  
It uses the Windows HID API (`hid.dll`) and `setupapi` to communicate over USB.
---

## 📦 Features

- Works on **Windows** with stock drivers.
- Supports:
    - DualShock 4 controllers (PID `0x05C4`, `0x09CC`, `0x0CE6`, `0x0CDA`)
    - DS4 Wireless USB Adapter (PID `0x0BA0`)
- **Read** current paired Bluetooth MAC address.
- **Set** a new Bluetooth MAC address.

---

## 🛠 Build Instructions

### Using MSVC Developer Command Prompt
```powershell
cl /EHsc /W4 pair_sixaxis_win.c /link setupapi.lib hid.lib
```

## 🛠 Build Instructions

### Using CMake
```bash
# From project root
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --target sixaxispairer_gui --config Debug
```

🚀 Usage
```cmd
sixaxispairer.exe
```
or
```cmd
sixaxispairer.exe 11:22:33:44:55:66
```
Connect your controller via USB.

To check the current pairing MAC, run without arguments.

To set a new pairing MAC:

Replace 11:22:33:44:55:66 with your PC’s Bluetooth adapter MAC address.

Run with the MAC as argument.

Unplug/replug the controller.

Optionally read back to verify the change.

💡 Notes
The tool automatically selects the first matching Sony VID/PID it finds.

DS4 devices use Feature Report ID 0x12; Sixaxis/DS3 use 0xF5.

MAC bytes may be stored in reverse order in DS4 firmware.


To change it:
Enter your PC’s Bluetooth adapter MAC in the XX:XX:XX:XX:XX:XX format.

Click Set.
Unplug/replug the controller to apply changes.

Optional: Read again to confirm the new MAC.

💡 Notes
If multiple Sony devices appear, select the controller, not the dongle.
Some devices require unplug/replug after setting the MAC for the change to stick.
Make sure your Bluetooth adapter is powered on and its MAC address is correct.

📜 License
This project is provided as-is for educational and personal use.
No warranties or guarantees are implied.

🔍 Troubleshooting
Read failed → Try replugging the controller and reading again.

MAC not updating → Make sure you are not setting the MAC of the dongle by mistake.

No devices found → Confirm the controller is connected via USB and drivers are installed.