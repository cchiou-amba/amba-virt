# Physical Camera & 3A Bringup on Ambarella EVE-OS

This guide details the hardware architecture, driver insertion sequence, 3A image tuning database integration, and video pipeline orchestration required to bring up physical GMSL2 cameras (Maxim MAX96712 deserializer with OmniVision OS08A10 sensors) natively in EVE-OS Dom0 on Ambarella N1-655 platforms (`n1-655-devkit` and `n1-655-pro`).

Related documents:
- [EVE-BaseOS-AmbarellaDrivers.md](EVE-BaseOS-AmbarellaDrivers.md) (Out-of-tree module management, memory carveouts, and `/persist` layout)
- [EVE-Ambarella-Models.md](EVE-Ambarella-Models.md) (Hardware models and I/O adapter definitions)
- [Architecture.md](Architecture.md) (System virtualization architecture)

---

## 1. Hardware Topology & Camera Subsystem

The Ambarella N1-655 DevKit camera subsystem consists of a high-speed GMSL2 SerDes bridge connected to the SoC Video Input (VIN) controller over MIPI CSI-2:

```
[ OS08A10 8MP Sensor ] (Camera Module)
         │ (MIPI CSI-2)
[ MAX9295A Serializer ]
         │
         │ (GMSL2 Coaxial Cable with Power-Over-Coax / POC)
         ▼
[ MAX96712 Deserializer ] (Baseboard Hub, I2C bus @ vinbrg0)
         │
         │ (4-Lane MIPI CSI-2, 1080p / 4K Video Stream)
         ▼
[ Ambarella N1-655 VIN0 Controller ]
         │
         ▼
[ Image Processing Engine (IDSP / IENG) ] ───► [ DSP Video Encoder ]
         ▲
         │ (3A Statistics / Calibration Tables)
[ test_aaa_service (3A Daemon) ]
```

### Key Hardware Characteristics
- **Deserializer**: Maxim MAX96712 4-channel GMSL2 deserializer attached to SoC I2C bus `vinbrg0`.
- **Serializer / Sensor**: Maxim MAX9295A serializer paired with an OmniVision OS08A10 8-Megapixel CMOS image sensor.
- **Power-over-Coax (POC)**: Camera modules receive 12V power through the coaxial signal cables controlled by SoC GPIO lines:
  - **DevKit POC Power GPIOs**: GPIO 92, 93, 98, 99 (Active High).
  - All four POC lines must be asserted high (`value = 1`) to power on connected camera modules before the deserializer and sensor drivers probe the bus.

---

## 2. Platform Prerequisites & Memory Carveouts

The Ambarella Video (`iav.ko`) and Memory Allocation (`ambcma.ko`) drivers require contiguous physical memory carveouts defined in the platform device tree (`.dtb`).

### Mandatory Reserved Memory Carveouts

| Node | Physical Address Range | Size | Purpose |
|---|---|---|---|
| `/reserved-memory/iav@0` | `0x00a3000000`–`0x00ffffffff` | 1488 MiB | DSP image buffer and encoder memory (`dsp_buf_size=0x40000000`) |
| `/reserved-memory/iav@1` | `0x0028000000`–`0x003fffffff` | 384 MiB | IAV shared memory pool (pyramid buffers, statistics, canvas) |
| `/reserved-memory/cavalry@0` | `0x0100000000`–`0x03ffffffff` | 12 GiB | Cavalry neural processor memory pool |
| `/reserved-memory/cavalry@1` | `0x0026000000`–`0x0027ffffff` | 32 MiB | Cavalry shared DMA pool |
| `/reserved-memory/cavalry@2` | `0x0025c00000`–`0x0025ffffff` | 4 MiB | Cavalry ucode staging buffer |

### Device Tree Verification & Override
If the board boots with a generic device tree lacking `cavalry@0` or `vinbrg0..3`, `ambcma.ko` will fail to probe with `-ENODEV`. Deploy the verified device tree via partition 4 (`CONFIG` partition `/dev/mmcblk0p4`):

```bash
# Mount persistent config partition
mkdir -p /tmp/cfgmnt
mount /dev/mmcblk0p4 /tmp/cfgmnt

# Deploy verified platform DTB
cp /path/to/build/n1_655_devkit.dtb /tmp/cfgmnt/eve.dtb

# Direct GRUB to use eve.dtb
cat << 'EOF' > /tmp/cfgmnt/grub.cfg
set_global devicetree "($config_part)/eve.dtb"
EOF

umount /tmp/cfgmnt
reboot
```

After reboot, verify the active device tree:
```bash
cat /proc/device-tree/model
# Output must be: n1-655 cooper devkit
ls -d /proc/device-tree/reserved-memory/iav@* /proc/device-tree/reserved-memory/cavalry@*
```

---

## 3. Persistent Directory Layout

All modules, firmware binaries, calibration databases, and helper tools reside under the `/persist` partition:

```
/persist/
├── modules/
│   ├── hw_timer.ko             # Hardware timer
│   ├── ambcma.ko               # Contiguous memory allocator
│   ├── msg.ko, ambnl.ko        # DSP messaging and netlink
│   ├── dsp.ko                  # DSP control driver
│   ├── amba_opti_print.ko      # Print formatting driver
│   ├── imgproc.ko              # Image processing 3A interface
│   ├── iav.ko                  # Ambarella Video driver (/dev/iav)
│   ├── amba_otp.ko             # OTP fuse access driver
│   ├── cavalry.ko              # CVflow neural processor driver
│   ├── dsplog.ko               # Multi-core DSP logging
│   ├── vio_monitor.ko          # VIN/VOUT monitor driver
│   ├── ambrg.ko                # Sensor bridge core driver
│   ├── max96712.ko             # MAX96712 GMSL2 deserializer driver
│   └── os08a10_mipi_brg.ko     # OS08A10 sensor bridge driver
├── firmware/
│   ├── cavalry.bin             # Cavalry vector processor firmware
│   ├── orccode.bin             # DSP ORC code
│   ├── orcidsp0.bin            # DSP Image Engine microcode
│   ├── orcidsp1.bin            # DSP Video Engine microcode
│   └── default_binary.bin      # Default tuning binary
├── share/ambarella/idsp/
│   ├── aaa_iq_config.lua       # 3A runtime pipeline config
│   └── ipc/                    # Calibrated ADJ, AEB, and 3D Color Correction tables
├── bin/
│   ├── load_ucode              # DSP microcode loader
│   ├── dsp_monitor_service     # DSP monitoring daemon
│   ├── test_encode             # IAV state and encoding CLI
│   └── test_aaa_service        # 3A automatic exposure / white balance daemon
└── scripts/
    └── n1_655_vin0_1080p_linear_mainonly.lua  # Linear 1080p resource configuration
```

---

## 4. Driver Loading & Hardware Initialization Sequence

Driver insertion order is critical to prevent kernel faults. Follow this sequence precisely:

### Step 1: Base Driver Insertion & Firmware Path Configuration
Configure the Linux kernel firmware search path and load the core DSP memory and control drivers:

```bash
# Point firmware search path to persistent storage
echo -n "/persist/firmware" > /sys/module/firmware_class/parameters/path

# Insert core timing, CMA, and messaging drivers
insmod /persist/modules/hw_timer.ko
insmod /persist/modules/ambcma.ko
insmod /persist/modules/msg.ko
insmod /persist/modules/ambnl.ko
insmod /persist/modules/dsp.ko
insmod /persist/modules/amba_opti_print.ko
insmod /persist/modules/imgproc.ko
insmod /persist/modules/iav.ko
insmod /persist/modules/amba_otp.ko
insmod /persist/modules/cavalry.ko
insmod /persist/modules/dsplog.ko
```

### Step 2: Launch DSP Monitor Service & Load Microcode
The `dsp_monitor_service` daemon monitors DSP heartbeat and must be running when microcode is uploaded:

```bash
# Launch DSP monitor daemon in background
/persist/bin/dsp_monitor_service &

# Upload DSP ORC microcode to hardware
/persist/bin/load_ucode /persist/firmware
```

Verify DSP transition to `INIT` state in `dmesg`:
```text
dsp_init_dev: DSP driver probe successfully!
iav: IAV driver loaded successfully!
```

### Step 3: Transition IAV to IDLE (Critical Step)

> [!CAUTION]
> **Mandatory Ordering to Prevent Kernel Crash**:
> You must run `test_encode --idle --nopreview` **before** inserting the sensor bridge module (`os08a10_mipi_brg.ko`). Inserting `os08a10_mipi_brg.ko` while IAV is in uninitialized state causes `vin_get_controller()` to dereference a NULL pointer, leading to an immediate kernel panic.

```bash
/persist/bin/test_encode --idle --nopreview
```

Expected output:
```text
iav: enter idle state without preview.
```

### Step 4: Power On Cameras via POC GPIOs
Assert the active-high POC power GPIO lines to energize the SerDes serializer and camera sensor modules:

```bash
for g in 92 93 98 99; do
    if [ ! -d /sys/class/gpio/gpio$g ]; then
        echo $g > /sys/class/gpio/export
    fi
    echo out > /sys/class/gpio/gpio$g/direction
    echo 1 > /sys/class/gpio/gpio$g/value
done
```

### Step 5: Insert SerDes Bridge & Sensor Drivers
Once POC power is applied, insert the bridge controllers and sensor drivers with DevKit parameters:

```bash
insmod /persist/modules/vio_monitor.ko
insmod /persist/modules/ambrg.ko
# DevKit uses GPIO POC switching instead of MAX20087 I2C PMIC:
insmod /persist/modules/max96712.ko id=0x08040201 dts_addr=1 use_max20087=0
insmod /persist/modules/os08a10_mipi_brg.ko brg_id=0x2
```

Verify sensor probing in kernel log:
```bash
dmesg | tail -n 20
```
Expected output:
```text
max96712: MAX96712 deserializer probed on vinbrg0
os08a10_mipi_brg: OS08A10 sensor probed successfully on vin0
```

---

## 5. Calibrated 3A Tuning Database & Service

Physical image sensors require continuous Auto Exposure (AE), Auto White Balance (AWB), and Auto Focus (AF) adjustment (3A). Without calibrated 3A tuning tables, captured frames appear pitch-black or severely color-distorted.

### Step 1: Symlink Calibration Database
The Ambarella image tuning framework expects calibration binaries under `/usr/share/ambarella/idsp/`. Because `/usr` is read-only SquashFS in EVE Dom0, create a symlink to `/persist/share/ambarella/idsp`:

```bash
if [ ! -e /usr/share/ambarella/idsp ]; then
    mkdir -p /usr/share/ambarella
    ln -s /persist/share/ambarella/idsp /usr/share/ambarella/idsp
fi
```

### Step 2: Launch 3A Tuning Service
Start the `test_aaa_service` daemon in automatic mode (`-a`):

```bash
/persist/bin/test_aaa_service -a &
```

The daemon connects to `/dev/iav` and `/dev/imgproc`, monitors scene luminance via sensor statistics, and updates exposure and gain dynamically.

---

## 6. Linear Video Streaming vs. HDR Mode Deadlock

### The HDR Mode 5 Pitfall
On Ambarella N1-655 platforms, omitting the `encode_mode` parameter in the Lua resource configuration causes the DSP to default to **Encode Mode 5 (HDR Line Interleaved)**.

In Mode 5, the IDSP processing pipeline (`IENG`) expects multiple exposure frames (long, medium, short) per capture cycle. When operating with standard single-exposure linear cameras (such as the OS08A10 in linear mode), the DSP engine waits indefinitely for the second exposure line, resulting in an `iproc_prep.c:2274` timeout and pipeline lockup:
```text
#iav_error# [IENG] iproc_prep timeout!
```

### The Mode 0 Configuration
To stream from linear sensors, explicitly set `encode_mode = 0` (`DSP_NORMAL_ISO_MODE`) in the Lua resource configuration file:

```lua
-- /persist/scripts/n1_655_vin0_1080p_linear_mainonly.lua
vsrc_0 = {
    vsrc_id = 0,
    mode = "1080p",
    hdr_mode = "linear",
    fps = 30,
    bits = 0,
}

chan_0 = {
    id = 0,
    vsrc = vsrc_0,
    vsrc_ctx = 0,
    img_stats_src_chan = "chan_0",
    sensor_ctrl = 1,
    main = {
        output = {0, 0, 1920, 1080},
    },
}

stream_0 = {
    id = 0,
    max_size = {1920, 1080},
    max_M = 1,
    codec_enable = 2, -- H264/MJPEG
}

_resource_config_ = {
    encode_mode = 0, -- 0: Normal ISO (Linear), 5: HDR Line Interleaved
    version = 1,
    channels = { chan_0 },
    canvas = {
        {
            type = "encode",
            size = {0, 0},
            source = {"chan_0.main"},
            extra_dram_buf = 0,
        },
    },
    streams = { stream_0 },
}

return _resource_config_
```

### Known IDSP Pipeline Timeout (`iproc_prep.c:2274`)
On N1-655 platforms with DSP microcode build `2026/09/07` (hash `0x3f98a9a5`), even when `encode_mode = 0` (`DSP_NORMAL_ISO_MODE`) is explicitly specified, the IDSP microcode currently triggers an internal pipeline timeout assertion approximately 205–214 ms after starting the VIN stream:

```text
[  229.809800] #iav_error# dsp_check_assertion(770): DSP assertion happened!
               [IDSP0:2] Assertion failure at line:2274 of .../iproc_prep.c module 64
               IPROC:: iproc_proc_timeout_ack[chan_0]: pass 0 job 1 is_vp 0 TIMEOUT!!!
[  229.823615] do_process_msg(497): dsp_prof_id 1 --> 254
```

Once this assertion triggers (`dsp_prof_id = 254`), IDSP processing halts, leading to recurring `wait statistics timeout` messages and preventing frames from being delivered to the encode canvas (`wait_canvas_frames: [TIMEOUT] Wait Canvas 0 for 4 frames!`). Hardware-level investigations indicate this is an internal IDSP firmware signaling/synchronization condition under investigation by the Ambarella DSP team.

---

## 7. Verification & Video Stream Encoding

### Step 1: Transition IAV to PREVIEW Mode
Apply the linear resource configuration to transition the DSP into `PREVIEW` state:

```bash
/persist/bin/test_encode --resource-cfg /persist/scripts/n1_655_vin0_1080p_linear_mainonly.lua
```

Expected output:
```text
init_vin done
enable preview done
```

### Step 2: Trigger Video Encoding on Stream A
`test_encode` is an IAV state and codec configuration utility. It directs the hardware encoder to encode frames into the DRAM Bitstream Buffer (BSB):

```bash
# Encode Stream A (-A) at 1080p (-h 1080p) for 150 frames (-d 150, ~5 seconds at 30 fps):
/persist/bin/test_encode -A -h 1080p -e -d 150
```

> [!NOTE]
> - The `-d` (or `--duration`) parameter specifies the duration in **number of frames**, not seconds (e.g. 150 frames = 5 seconds at 30 fps).
> - `test_encode` does **not** accept a file output flag like `-f` (passing `-f` returns `unknown option found: [f]`).
> - In Ambarella architecture, `test_encode` controls the encoding pipeline in IAV; reading the bitstream out of the BSB buffer into disk files is performed by userspace streaming clients or recording daemons (e.g., `test_stream`).

---

## 8. Summary Checklist for Automation Scripts

When automating physical camera bringup in system initialization scripts or boot hooks:
1. Verify device tree reserved memory carveouts:
   - `iav@0`: base `0x00a3000000`, size `0x5d000000` (1488 MiB, bounded strictly below 4 GiB).
   - `iav@1`: base `0x0028000000`, size `0x18000000` (384 MiB).
   - `cavalry@0`: base `0x0100000000`, size `0x0300000000` (12 GiB).
2. Configure kernel firmware search path to `/persist/firmware`:
   `echo -n "/persist/firmware" > /sys/module/firmware_class/parameters/path`
3. Load base drivers in dependency order:
   `hw_timer`, `ambcma (ama_enable=1 dsp_buf_size=0x40000000)`, `msg`, `ambnl`, `dsp`, `amba_opti_print`, `imgproc`, `iav`, `amba_otp`, `dsplog`.
4. Upload DSP microcode:
   `/persist/bin/load_ucode /persist/firmware`
5. **Transition IAV to IDLE before loading sensor drivers**:
   `/persist/bin/test_encode --idle --nopreview`
6. Power on camera POC lines via GPIOs 92, 93, 98, 99:
   `for g in 92 93 98 99; do echo $g > /sys/class/gpio/export 2>/dev/null; echo out > /sys/class/gpio/gpio$g/direction; echo 1 > /sys/class/gpio/gpio$g/value; done`
7. Load SerDes bridge and camera sensor modules:
   - `vio_monitor.ko`
   - `ambrg.ko`
   - `max96712.ko id=0x08040201 dts_addr=1 use_max20087=0`
   - `os08a10_mipi_brg.ko brg_id=0x2`
8. Symlink tuning database and launch 3A daemon:
   `ln -s /persist/share/ambarella/idsp /usr/share/ambarella/idsp`
   `/persist/bin/test_aaa_service -a &`
9. Apply linear resource configuration:
   `/persist/bin/test_encode --resource-cfg /persist/scripts/n1_655_vin0_1080p_linear_mainonly.lua`
10. Trigger encoding:
   `/persist/bin/test_encode -A -h 1080p -e -d 150`
