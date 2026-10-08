# Physical Camera & 3A Bringup on Ambarella EVE-OS

> [!NOTE]
> **Retained Qualification Record (Historical Baseline — Not a Runnable Product Procedure)**:
> This document records the historical hardware qualification achieved on 2026-10-01 on `n1-655-devkit`. A cold reset, a self-contained stage built by `tools/bringup/scripts/stage_fresh_bringup.sh`, and `tools/bringup/scripts/run_sensor_bringup_fresh.sh` produced 900 decodable 1920x1080 H.264 frames at 30 fps via Port A, clearing the previous 7-frame IDSP stall (`vin0_idsp_sof` counted continuously; DSP state stayed `0x0`).
>
> The `/persist/fresh-bringup` and `/persist/{modules,bin,lib,firmware}` paths described in this document are retained strictly as a historical qualification record and diagnostic baseline. They are **NOT** a runnable product procedure. Direct staging onto `/persist` has been retired and purged. In the product architecture, drivers and firmware are packaged in Debian packages (`amba-virt-camera`), and pipeline orchestration is driven by `amba-virt-server`. Do not stage files into `/persist/fresh-bringup` or execute manual bringup scripts on production boards.

This guide details the hardware architecture, memory carveout requirements, driver loading sequence, 3A image tuning database integration, and video pipeline orchestration required to bring up physical GMSL2 cameras (Maxim MAX96712 deserializer with OmniVision OS08A10 sensors) natively in EVE-OS Dom0 on Ambarella N1-655 platforms (`n1-655-pro` and `n1-655-devkit`).

Related documents:
- [EVE-BaseOS-AmbarellaDrivers.md](EVE-BaseOS-AmbarellaDrivers.md) (Out-of-tree module management, memory carveouts, and `/persist` layout)
- [EVE-Ambarella-Models.md](EVE-Ambarella-Models.md) (Hardware models and I/O adapter definitions)
- [Architecture.md](Architecture.md) (System virtualization architecture)

---

## 1. Hardware Topology & Camera Subsystem

The Ambarella N1-655 camera subsystem consists of a high-speed GMSL2 SerDes bridge connected to the SoC Video Input (VIN) controller over MIPI CSI-2:

```
[ OS08A10 8MP Sensor ] (Camera Module on Port A)
         │ (MIPI CSI-2)
[ MAX9295A Serializer ] (id: 0x91)
         │
         │ (GMSL2 Coaxial Cable with Power-Over-Coax / POC)
         ▼
[ MAX96712 Deserializer ] (Baseboard Hub, Port A / Link 0, id: 0xa0)
         │
         │ (4-Lane MIPI CSI-2, 1080p / 4K Video Stream)
         ▼
[ Ambarella N1-655 VIN0 Controller ] (vinc:0)
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
- **Active Physical Link**: **Port A (Link 0 / Bridge ID `0x1`)** bound to **`vinc:0`** (Physical VIN Controller 0).
- **Power-over-Coax (POC)**: Camera modules receive 12V power through coaxial signal cables controlled by SoC GPIO lines:
  - **POC Power GPIOs**: GPIO 92, 93, 98, 99 (Active High).
  - All four POC lines must be asserted high (`value = 1`) to power on connected camera modules before SerDes and sensor drivers probe the bus.

---

## 2. Platform Prerequisites & Physical Memory Layout

All Ambarella DSP microcode engines (`orccode.bin`, `orcidsp0.bin`, `orcidsp1.bin`, `orcvin0.bin`, `orcvin1.bin`) operate strictly within a **32-bit physical address space (`< 0x100000000` / 4 GiB)**. All memory buffers passed to DSP hardware blocks must reside below this 4 GiB boundary.

### ipcam-mini Memory Map

U-Boot writes this map from the detected DRAM size. Both current boards detect 32 GiB. `iav@1` and `iav@0` are contiguous and end at 4 GiB. `cavalry@0` starts there. This boot layout does not qualify sensor, VIN, IDSP, or Cavalry DMA.

| Memory / Carveout Node | Physical Address Range | Size | Allocation Policy | Functional Purpose |
| :--- | :--- | :--- | :--- | :--- |
| `/memory` | `0x0000000000`–`0x07ffffffff` | 32 GiB | `device_type = "memory"` | Detected DRAM |
| `/chosen/sys-dram-size` | `0x00000008 0x00000000` | 32 GiB | `u64` property | Size `ambcma.ko` validates |
| `/reserved-memory/virtio_reserved@2000000` | `0x0002000000`–`0x00021fffff` | 2 MiB | `no-map;` | VirtIO device configuration window |
| `/reserved-memory/cavalry@2` | `0x1fc00000`–`0x1fffffff` | 4 MiB | `no-map;` | Cavalry ucode staging |
| `/reserved-memory/cavalry@1` | none | 0 | `no-map;` | No shared-pool `reg` in this profile |
| `/reserved-memory/disp@0` | `0x20000000`–`0x27ffffff` | 128 MiB | `no-map;` | Display buffer |
| `/reserved-memory/iav@1` | `0x28000000`–`0x3fffffff` | 384 MiB | `no-map;` | `IDSP_SHARED` |
| `/reserved-memory/iav@0` | `0x40000000`–`0xffffffff` | 3072 MiB | `no-map;` | `IDSP_PRIVATE` |
| `/reserved-memory/linux,cma` | above `0x400000000` | 512 MiB | `reusable;` | Kernel CMA in Linux high |
| `/reserved-memory/cavalry@0` | `0x100000000`–`0x3ffffffff` | 12 GiB | `no-map;` | Cavalry private pool |

Do not install a static DTB with `set_global devicetree`. GRUB would replace the tree U-Boot patched. Confirm the running tree with `od -An -tx1` on `/proc/device-tree/memory/reg`, `/proc/device-tree/chosen/sys-dram-size`, and the reserved `iav` and `cavalry` nodes.

---

## 3. Persistent Directory Layout [RETAINED HISTORICAL QUALIFICATION RECORD]

All modules, firmware binaries, calibration databases, and helper tools were historically staged under `/persist` during initial bringup (this layout is now retired):

```
/persist/
├── modules/
│   ├── hw_timer.ko             # Hardware timer
│   ├── ambcma.ko               # Contiguous memory allocator (AMA mode)
│   ├── msg.ko, ambnl.ko        # DSP messaging and Netlink connector
│   ├── dsp.ko                  # DSP control driver
│   ├── amba_opti_print.ko      # Print formatting driver
│   ├── imgproc.ko              # Image processing 3A interface
│   ├── iav.ko                  # Ambarella Video driver (/dev/iav)
│   ├── amba_otp.ko             # OTP fuse access driver
│   ├── cavalry.ko              # CVflow neural processor driver
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

## 4. Driver Loading & Hardware Bringup Sequence [RETAINED HISTORICAL QUALIFICATION RECORD]

This sequence records the historical verification steps executed on 2026-10-01:

### Step 1: Base Driver Insertion
Configure kernel firmware path and load core DSP memory and control drivers:

```bash
# Point firmware search path to persistent storage
echo -n "/persist/firmware" > /sys/module/firmware_class/parameters/path

# Insert core timing, CMA, and messaging drivers
insmod /persist/modules/hw_timer.ko
# dsp_buf_size must fit in iav@0 (3072 MiB, ending at 4 GiB).
# The old 1664 MiB value belonged to a different map and is not qualified here.
insmod /persist/modules/ambcma.ko ama_enable=1 dsp_buf_size=0x40000000
insmod /persist/modules/cavalry.ko
insmod /persist/modules/msg.ko
insmod /persist/modules/ambnl.ko
insmod /persist/modules/dsp.ko
insmod /persist/modules/amba_opti_print.ko
insmod /persist/modules/imgproc.ko
insmod /persist/modules/iav.ko
insmod /persist/modules/amba_otp.ko
```

### Step 2: Ensure Device Nodes & Launch DSP Monitor
```bash
# Ensure character device nodes exist
[ ! -c /dev/iav ] && mknod /dev/iav c 508 1 && chmod 666 /dev/iav
[ ! -c /dev/ucode ] && mknod /dev/ucode c 508 0 && chmod 666 /dev/ucode
[ ! -c /dev/cavalry ] && mknod /dev/cavalry c 507 0 && chmod 666 /dev/cavalry
[ ! -c /dev/amba_otp ] && mknod /dev/amba_otp c 503 0 && chmod 600 /dev/amba_otp

# Launch DSP monitor service via container dynamic linker
ROOTFS="/persist/clear/volumes/fa59bede-7dda-4e93-8fc7-96d238800bf5#0.container/rootfs"
LD_SO="$ROOTFS/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1"
LIB_PATH="$ROOTFS/usr/lib/aarch64-linux-gnu:$ROOTFS/lib/aarch64-linux-gnu:$ROOTFS/usr/lib:$ROOTFS/lib:/persist/lib"
$LD_SO --library-path "$LIB_PATH" /persist/bin/dsp_monitor_service > /tmp/dsp_monitor.log 2>&1 &

# Upload DSP microcode
/persist/bin/load_ucode /persist/firmware
```

### Step 3: Transition IAV to IDLE
```bash
/persist/bin/test_encode --idle --nopreview
```

### Step 4: Power On Cameras & Insert SerDes / Sensor Drivers
```bash
# Power on POC lines via SoC GPIOs
for g in 92 93 98 99; do
    echo $g > /sys/class/gpio/export 2>/dev/null || true
    echo out > /sys/class/gpio/gpio$g/direction || true
    echo 1 > /sys/class/gpio/gpio$g/value || true
done

# Insert SerDes bridge and OS08A10 sensor on Port A (Bridge ID 0x1)
insmod /persist/modules/vio_monitor.ko
insmod /persist/modules/ambrg.ko
insmod /persist/modules/max96712.ko id=0x01 dts_addr=1 use_max20087=0
insmod /persist/modules/os08a10_mipi_brg.ko brg_id=0x1
```

Verify sensor probe:
```bash
/persist/bin/test_encode --show-vsrc-info
```
Expected output:
```text
VIN Controller[0]:
	vsrc[0] : sensor info  :  os08a10 (vinc_id: 0, sensor_id: 0x1013, status: active)
```

---

## 5. 3A Tuning Database & Service

Physical image sensors require continuous Auto Exposure (AE) and Auto White Balance (AWB) tuning.

### Step 1: Symlink Calibration Database
```bash
mkdir -p /usr/share/ambarella
ln -sf /persist/share/ambarella/idsp /usr/share/ambarella/idsp 2>/dev/null
ln -sf /persist/firmware/default_binary.bin /usr/share/ambarella/idsp/default_binary.bin 2>/dev/null
```

### Step 2: Launch 3A Tuning Service
```bash
nohup /persist/bin/test_aaa_service -a < /dev/null > /persist/aaa.log 2>&1 &
```

Verify `test_aaa_service` completed initialization in `/persist/aaa.log`:
```text
>>> Connection established with kernel.
>>> AAA prepare start.
[IMG_API] Loading default_binary from /usr/share/ambarella/idsp/default_binary.bin
>>> [chan-0]ADJ parameter version : 3 
>>> [chan-0]AEB parameter version : 1 
>>> AAA prepare done.
```

---

## 6. Official Linear Video Resource Configuration

To stream from linear sensors, use the official Cooper BSP Lua resource configuration (`/persist/scripts/n1_655_vin0_1080p_linear.lua`):

```lua
-- /persist/scripts/n1_655_vin0_1080p_linear.lua
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
	max_padding_width = 0,
	idsp_fps = 0,
	lens_warp = 0,
	max_main_input_width = 0,
	mctf_cmpr = 1,
	c2y_burst_tile = 1,
	extra_downscale = 0,
	high_perf_enable = 1,
	main = {
		max_output = {0, 0},
		input      = {0, 0, 0, 0},
		output     = {0, 0, 1920, 1080},
	},
	second = {
		max_output = {0, 0},
		input      = {0, 0, 0, 0},
		output     = {0, 0, 720, 480},
	},
	third = {
		max_output = {0, 0},
		input      = {0, 0, 1920, 1080},
		output     = {0, 0, 1920, 1080},
	},
	fourth = {
		max_output = {0, 0},
		input      = {0, 0, 1920, 1080},
		output     = {0, 0, 1920, 1080},
	},
	fifth = {
		max_output = {0, 0},
		input      = {0, 0, 0, 0},
		output     = {0, 0, 1280, 720},
	},
	pyramid = {
		input_buf_id = 4,
		scale_type = 0,
		buf_addr = 0x0,
		buf_size = 0x0,
		manual_feed = 0,
		item_num = 0,
		layer_map = 0x7f,
		layers = {
			{ crop_win = {0, 0, 0, 0} },
			{ crop_win = {0, 0, 0, 0} },
			{ crop_win = {0, 0, 0, 0} },
			{ crop_win = {0, 0, 0, 0} },
			{ crop_win = {0, 0, 0, 0} },
			{ crop_win = {0, 0, 0, 0} },
			{ crop_win = {0, 0, 0, 0} },
		},
	},
}

stream_0 = {
	id = 0,
	max_size = {1920, 1080},
	max_M = 1,
	fast_seek_enable = 0,
	two_ref_enable = 0,
	max_svct_layers_minus_1 = 0,
	max_num_minus_1_ltrs = 0,
	codec_enable = 2,
}

stream_1 = {
	id = 1,
	max_size = {1920, 1080},
	max_M = 1,
	fast_seek_enable = 0,
	two_ref_enable = 0,
	max_svct_layers_minus_1 = 0,
	max_num_minus_1_ltrs = 0,
	codec_enable = 2,
}

stream_2 = {
	id = 2,
	max_size = {720, 480},
	max_M = 1,
	fast_seek_enable = 0,
	two_ref_enable = 0,
	max_svct_layers_minus_1 = 0,
	max_num_minus_1_ltrs = 0,
	codec_enable = 2,
}

_resource_config_ = {
	version = 1,
	log_level = 0,
	channels = { chan_0 },
	canvas = {
		{
			type = "encode",
			size = {0, 0},
			source = {"chan_0.main"},
			extra_dram_buf = 0,
		},
		{
			type = "encode",
			size = {0, 0},
			source = {"chan_0.second"},
			extra_dram_buf = 0,
		},
		{
			type = "prev",
			size = {0, 0},
			source = {"chan_0.third"},
			vout_id = 0,
			vout_YUV422 = 0,
			extra_dram_buf = 0,
		},
		{
			type = "encode",
			size = {0, 0},
			source = {"chan_0.fourth"},
			extra_dram_buf = 0,
		},
	},
	streams = { stream_0, stream_1, stream_2 },
}

return _resource_config_
```

---

## 7. Execution & Status

### Applying Configuration
```bash
/persist/bin/test_encode --resource-cfg /persist/scripts/n1_655_vin0_1080p_linear.lua
```

### Video Encoding Command (Post-Preview)
```bash
/persist/bin/test_encode -A -h 1080p -e -d 150
```
*(150 frames = 5 seconds at 30 fps encoded into DRAM Bitstream Buffer).*
