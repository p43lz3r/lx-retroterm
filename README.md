# LX-RETROTERM

A serial terminal for retro hardware — Arduino, ESP32, RC2014 Z80, and similar devices.

ttcore-port is a Linux port of [TeraTerm 5](https://github.com/TeraTermProject/teraterm) as a standalone C99 library (`libttcore`) with a GTK3 graphical interface. It runs entirely without Windows dependencies.

## Features & Design

The terminal emulation features, serial communication options, and file transfer protocols implemented in this project are based on [TeraTerm 5](https://github.com/TeraTermProject/teraterm) by the TeraTerm Project. Not all TeraTerm features are present — a detailed coverage analysis is in progress and will be documented in `COVERAGE.md`.

## Requirements

You need a Linux system with GTK3 development libraries. Debian, Ubuntu, and MX Linux are recommended.

Install the required packages:

```bash
sudo apt install gcc cmake make libgtk-3-dev libglib2.0-dev pkg-config
```

**Compiler:** gcc 10 or newer is required. Tested with gcc 14.2.0 on MX Linux.

## Build instructions

1. Clone the repository:

   ```bash
   git clone https://github.com/p43lz3r/lx-retroterm.git
   cd lx-retroterm
   ```

2. Create a build directory and compile:

   ```bash
   mkdir build && cd build
   cmake ..
   make -j$(nproc)
   ```

3. Run the application:

   ```bash
   ./gui/ttcore-gui
   ```

That's it. No `sudo make install` needed — the binary runs directly from the build directory.

## Usage

The GUI has three main areas: a **toolbar** at the top (port selection, connect/disconnect, baud rate), the **terminal area** in the center, and a **menu bar** for all features.

### Connecting to a device

1. Select your serial port from the toolbar dropdown (e.g., `/dev/ttyACM0`)
2. Choose the baud rate (default: 115200)
3. Click **Connect** (or press `Ctrl+Shift+O`)

The terminal is now live. Type to send, received data appears immediately.

### Keyboard shortcuts

All shortcuts use `Ctrl+Shift` to avoid conflicts with terminal input:

| Shortcut         | Action                |
|------------------|-----------------------|
| Ctrl+Shift+O     | Connect / Disconnect  |
| Ctrl+Shift+D     | Rescan ports          |
| Ctrl+Shift+N     | Next device           |
| Ctrl+Shift+L     | Clear screen          |
| Ctrl+Shift+C     | Copy selection        |
| Ctrl+Shift+G     | Start / Stop log      |
| Ctrl+Shift+T     | Send text             |
| Ctrl+Shift+F     | Send text file        |
| Ctrl+Shift+H     | Send Intel HEX        |
| Ctrl+Shift+X     | XMODEM send           |
| Ctrl+Shift+R     | XMODEM receive        |
| Ctrl+Shift+S     | Serial config         |
| Ctrl+Shift+B     | Toggle toolbar        |

The full list is also available in **Help > Keyboard Shortcuts**.

### File transfers

- **Transfer > Send File (XMODEM)** — send a file using XMODEM (CRC or 1K-CRC)
- **Transfer > Receive File (XMODEM)** — receive a file using XMODEM
- **Transfer > Send Text File** — send a text file line by line with configurable delay
- **Transfer > Send Intel HEX** — upload Intel HEX files to embedded targets

## Hardware tested

| Device          | Interface     | Notes                              |
|-----------------|---------------|------------------------------------|
| Arduino Uno     | USB-Serial    | ttyACM0, 115200 baud               |
| ESP32-S3        | USB-CDC       | ttyACM0, 115200 baud               |
| RC2014 Z80      | FTDI USB      | ttyUSB0, 115200 baud, RomWBW CP/M  |

## Quality

- 1092 unit tests across 8 modules — all passing
- Valgrind: 0 errors, 0 memory leaks

## License

MIT License. See [LICENSE](LICENSE).

Based on [TeraTerm 5](https://github.com/TeraTermProject/teraterm) by the amazing TeraTerm Project.
