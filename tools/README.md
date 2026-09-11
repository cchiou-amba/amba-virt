# Ambarella Host Tools

This directory contains host-side utilities and binaries for development, board bring-up, and flash programming on x86-64 Linux workstations.

## Directory Layout

| Path | Description |
|---|---|
| [`tools/bin/usb-matrix`](bin/usb-matrix) | Ambarella USB flash programming and boot utility for x86-64 Linux |

---

## USB Flash Programmer (`usb-matrix`)

`tools/bin/usb-matrix` is an x86-64 Linux ELF executable used for USB flash programming, firmware flashing, and bootstrapping Ambarella SoCs (including Cooper / N1-655).

### Host System Requirements

- **Operating System**: Linux x86-64 (tested on Ubuntu 20.04 / 22.04 / 24.04 LTS)
- **Library Dependencies**:
  - `libusb-1.0` (`libusb-1.0-0` or `libusb-1.0-0-dev`)
  - `libudev` (`libudev1`)
  - `glibc` 2.7+
- **Privileges**: Root access (`sudo`) or appropriate `udev` rules granting non-root access to Ambarella USB vendor IDs.

To install required libraries on Debian/Ubuntu:
```bash
sudo apt-get update && sudo apt-get install -y libusb-1.0-0 libudev1
```

### Supported SoC Targets

`usb-matrix` includes built-in profiles for Ambarella SoCs:
- **`cv3ad655`** (Ambarella N1-655 Cooper DevKit / Pro)
- **`cv3ad685`**
- **`cv72`**
- **`cv75`**
- **`cv28`**
- **`cv25`**
- **`Generic`**

### Command-Line Options

```text
Usage: tools/bin/usb-matrix [option]
-h, --help                  display this help
-v, --version               show version
-V, --verbose               more log
-c, --chip [CHIP]           specify the chip (cv3ad655, cv72, cv3, cv28 ...)
                            if chip is not specified, scan all devices in support lists
-T, --rescan-pause [SEC]    pause time to do next device scanning (default: 3 secs)
-f, --fw BIN[@ADDR]         boot firmware programmer (burn firmware)
                            if ADDR is not specified, the boot address is from the header of the binary
-u, --ust BIN               boot usbstrap image
-U, --ads FILE              specify the .ads file (cv2x only)
-b, --bld BIN[@ADDR]        boot bld image
-l, --list                  scan and list devices
-k, --kernel BIN@ADDR       load kernel image at specified memory address
-d, --dtb BIN@ADDR          load dtb image at specified memory address
```

### Common Usage Workflows

#### 1. Scan and List Connected Devices

Inspect connected USB devices and verify the Ambarella SoC is detected in USB download/programming mode:

```bash
./tools/bin/usb-matrix -l
```

#### 2. Burn / Program Firmware Image

Flash firmware onto the target board (e.g., CV3AD655 / N1-655):

```bash
sudo ./tools/bin/usb-matrix -c cv3ad655 -f /path/to/firmware.bin
```

If a specific memory load address is required:

```bash
sudo ./tools/bin/usb-matrix -c cv3ad655 -f /path/to/firmware.bin@0x00000000
```

#### 3. Boot Stage Images via USB

Boot a bootloader (BLD) image directly:

```bash
sudo ./tools/bin/usb-matrix -c cv3ad655 -b /path/to/bld.bin
```

Boot a `usbstrap` image:

```bash
sudo ./tools/bin/usb-matrix -c cv3ad655 -u /path/to/usbstrap.bin
```

Load and boot a kernel and device tree (DTB) to RAM:

```bash
sudo ./tools/bin/usb-matrix -c cv3ad655 -k /path/to/Image@0x00200000 -d /path/to/board.dtb@0x08000000
```

---

## Board Preparation for USB Programming

To enter USB programming / recovery mode on Ambarella N1-655 boards:
1. Configure the boot mode dip switches / strap jumpers to USB boot mode (refer to hardware schematics or carrier board documentation).
2. Connect the target board's USB OTG / programming port to the x86 Linux host machine using a standard USB cable.
3. Power on the board.
4. Verify detection using `lsusb` and `./tools/bin/usb-matrix -l`.
