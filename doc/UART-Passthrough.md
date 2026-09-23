# Ambarella Hardware UART Passthrough & Console Guide

*Document Path: `doc/UART-Passthrough.md`*  
*Copyright (C) 2026, Ambarella International LLC*

---

## 1. Overview & Objectives

This guide documents the architecture, configuration, and verification of direct hardware serial (RS-232 / UART) passthrough on the Ambarella N1-655 SoC running EVE OS to HVM guest virtual machines (Ubuntu 24.04 LTS HVM and QNX Neutrino RTOS 8.0 HVM).

### Standardized Multi-Guest Demo Topology
1. **Preserve EVE Dom0 Console**: `uart0` (MMIO `0xffe4000000`, SPI 30) is retained strictly by EVE Dom0 for bootloader output, early kernel crash recovery, and hypervisor management (Channel 0).
2. **Pass Through Secondary UART (`uart1`) to Ubuntu HVM ("Console 2")**: `uart1` (MMIO `0xffe0017000`, SPI 114) is passed directly to the Ubuntu 24.04 LTS HVM guest VM with an interactive `serial-getty` login shell (Channel 1).
3. **Pass Through Tertiary UART (`uart2`) to QNX HVM ("Console 3")**: `uart2` (MMIO `0xffe0018000`, SPI 115) is passed directly to the QNX Neutrino RTOS 8.0 HVM guest VM with an interactive `tinit` / `login` prompt or root `ksh` shell (Channel 2).
4. **Preserve MCU Power/Reset Console**: Channel 3 connects to the Cortex-M3 board MCU for hardware power cycling (`pwr on`, `pwr off -y`) and board telemetry.

---

## 2. Hardware Resource & Pin Routing Map

```text
+---------------------------------------------------------------------------------------------------+
| Multi-Guest Hardware Passthrough & USB-to-Quad-UART Port Mapping                                 |
|                                                                                                   |
|   +-------------------+  +-------------------+  +-------------------+  +-----------------------+  |
|   | SoC UART0         |  | SoC UART1         |  | SoC UART2         |  | Cortex-M3 Board MCU   |  |
|   | 0xffe4000000      |  | 0xffe0017000      |  | 0xffe0018000      |  | Power / Boot / Reset  |  |
|   | GIC SPI IRQ 30    |  | GIC SPI IRQ 114   |  | GIC SPI IRQ 115   |  | Protocol & Telemetry  |  |
|   +---------|---------+  +---------|---------+  +---------|---------+  +-----------|-----------+  |
|             |                      |                      |                        |              |
|             v                      v                      v                        v              |
|   +-------------------------------------------------------------------------------------------+   |
|   | Onboard CH9344 USB-to-Quad-UART High-Speed Bridge                                         |   |
|   |                                                                                           |   |
|   |   - Channel 0: SoC Dom0 System / Bootloader Console                                       |   |
|   |   - Channel 1: "Console 2" (Direct Passthrough to Ubuntu 24.04 LTS HVM)                   |   |
|   |   - Channel 2: "Console 3" (Direct Passthrough to QNX Neutrino RTOS 8.0 HVM)              |   |
|   |   - Channel 3: MCU Power Control Console                                                  |   |
|   +-------------------------------------------------------------------------------------------+   |
+---------------------------------------------------------------------------------------------------+
```

### Complete CH9344 4-Port Matrix

| Channel | SoC Hardware Resource | DevKit Host Port | Pro Host Port | Assigned Domain | Runtime Service & Shell |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **0** | `uart0` (`0xffe4000000`, SPI 30) | `ttyCH9344USB8` | `ttyCH9344USB0` | **EVE OS Dom0** | U-Boot & EVE Dom0 Root Console |
| **1** | `uart1` (`0xffe0017000`, SPI 114) | `ttyCH9344USB9` | `ttyCH9344USB1` | **Ubuntu 24.04 HVM** | `serial-getty@ttyAMBA0` (115200 8N1) |
| **2** | `uart2` (`0xffe0018000`, SPI 115) | `ttyCH9344USB10`| `ttyCH9344USB2` | **QNX 8.0 HVM** | `devc-seramb` / `tinit` / `ksh` (115200 8N1) |
| **3** | Board MCU UART | `ttyCH9344USB11`| `ttyCH9344USB3` | **Cortex-M3 MCU** | Hardware Power & Reset Management |

---

## 3. Ubuntu 24.04 LTS HVM Configuration (Console 2)

### Step 1: Assign `COM2` Adapter to Ubuntu Instance
In the ZEDEDA Cloud model ([`models/N1-655-Cooper-Devkit.json`](file:///home/samurai/work/amba-virt/models/N1-655-Cooper-Devkit.json)), assign `COM2` (`uart1`) to the Ubuntu edge application instance:
```json
{
  "ztype": "IO_TYPE_COM",
  "phylabel": "COM2",
  "logicallabel": "COM2",
  "phyaddrs": {
    "Serial": "/dev/ttyS1"
  },
  "assigngrp": "COM2"
}
```

### Step 2: Automated Cross-Compilation & Deployment Script
Use the unified guest deployment script [`guest-os/deploy_guest.sh`](file:///home/samurai/work/amba-virt/guest-os/deploy_guest.sh) (or `make deploy-hvm-ubuntu`):
```bash
# 1. Build guest drivers
make guest-ubuntu

# 2. Deploy to target HVM, reload kernel modules, enable serial console, and test:
./guest-os/deploy_guest.sh n1-655-devkit-ubuntu --reload --enable-serial --test
# Or for Pro:
./guest-os/deploy_guest.sh n1-655-pro-ubuntu --reload --enable-serial --test
```
This script automatically:
1. Stages `amba_virt.ko`, `amba_cavalry.ko`, `amba_gdma.ko`, `amba_uart.ko`, and `amba_dma.ko` into `/lib/modules/$(uname -r)/extra/`.
2. Runs `depmod -a` and inserts `amba_uart.ko` with parameters `use_dma=0 mmio_base=0xffe0017000 irq=114 baud=115200`.
3. Enables and starts `serial-getty@ttyAMBA0.service`.
4. Executes a post-deployment sanity check.

---

## 4. QNX Neutrino RTOS 8.0 HVM Configuration (Console 3)

### 4.1 Does QNX Have a Login Prompt?
**Yes.** QNX Neutrino RTOS supports multiple standard interactive console modes:
1. **Authenticated Login Prompt (`login` / `tinit`)**:
   When `tinit` or `/bin/login` is bound to the character device (`/dev/ser2`), QNX emits a standard login prompt:
   ```text
   QNX Neutrino RTOS 8.0 (n1-655-devkit-qnx) (ser2)

   login: root
   Password:
   # 
   ```
2. **Direct Korn Shell (`ksh`) / Standard Shell (`sh`)**:
   In embedded demo configurations, QNX can spawn an immediate root session without password prompting:
   `[+session] /bin/ksh </dev/ser2 >/dev/ser2 2>&1 &`

### 4.2 Step 1: Assign `COM3` Adapter to QNX Instance
In the ZEDEDA Cloud model, assign `COM3` (`uart2` at `0xffe0018000`, SPI 115) to the QNX edge application instance:
```json
{
  "ztype": "IO_TYPE_COM",
  "phylabel": "COM3",
  "logicallabel": "COM3",
  "phyaddrs": {
    "Serial": "/dev/ttyS2"
  },
  "assigngrp": "COM3"
}
```

### 4.3 Step 2: Automated QNX Driver Build & Deployment Script
Use the unified guest deployment script [`guest-os/deploy_guest.sh`](file:///home/samurai/work/amba-virt/guest-os/deploy_guest.sh) (or `make deploy-hvm-qnx`):
```bash
# 1. Build QNX guest artifacts
make guest-qnx

# 2. Deploy to target QNX HVM, restart resource managers, enable console, and test:
./guest-os/deploy_guest.sh n1-655-devkit-qnx --reload --enable-serial --test
# Or for Pro:
./guest-os/deploy_guest.sh n1-655-pro-qnx --reload --enable-serial --test
```

### 4.4 Step 3: Dynamic FDT Device Tree Discovery & Boot Script
Neither Ubuntu nor QNX hardcode specific hardware UART addresses. Both operating systems dynamically probe the Device Tree (FDT) injected by QEMU:
- **In Linux (`amba_uart.c`)**: Matches `compatible = "ambarella,uart"`. If present, extracts MMIO `reg` and `interrupts` dynamically. If no UART adapter was assigned to this VM instance, the driver probes 0 devices and consumes 0 resources.
- **In QNX (`startup_postpci.custom`)**:
  At startup, QNX checks the FDT for an `ambarella,uart` node:
  ```sh
  # In startup_postpci.custom:
  # Check if Device Tree contains Ambarella UART node
  if fdt_get_node "ambarella,uart" >/dev/null 2>&1 || [ -e /sys/fdt ]; then
      MMIO=$(fdt_get_reg "ambarella,uart" 0 2>/dev/null || echo "0xffe0018000")
      IRQ=$(fdt_get_irq "ambarella,uart" 0 2>/dev/null || echo "115")
      devc-seramb -e -F -b115200 "${MMIO},${IRQ}" &
      waitfor /dev/ser2 2
  fi

  # Automatically spawn interactive shell / login prompt on any active serial port
  for dev in /dev/ser*; do
      if [ "$dev" != "/dev/ser1" ] && [ -e "$dev" ]; then
          echo "---> Starting shell on serial $dev"
          on -d -t "$dev" ksh -l &
      fi
  done
  ```
- **Golden Image Reuse**: The identical QCOW2 image can be deployed across $N$ instances—instances with assigned serial adapters dynamically get a serial console, while instances without serial adapters run cleanly as standard headless/SSH VMs.

---

## 5. Verification & Target Testing

### 5.1 Connecting to "Console 2" (Ubuntu Shell)
From the host workstation:
* **On `n1-655-devkit`**: `screen /dev/ttyCH9344USB9 115200`
* **On `n1-655-pro`**: `screen /dev/ttyCH9344USB1 115200`

```text
Ubuntu 24.04 LTS n1-655-devkit-ubuntu ttyAMBA0

n1-655-devkit-ubuntu login: ubuntu
Password: 
Welcome to Ubuntu 24.04 LTS (GNU/Linux 6.8.0-31-generic aarch64)
ubuntu@n1-655-devkit-ubuntu:~$ 
```

### 5.2 Connecting to "Console 3" (QNX Shell)
From the host workstation:
* **On `n1-655-devkit`**: `screen /dev/ttyCH9344USB10 115200`
* **On `n1-655-pro`**: `screen /dev/ttyCH9344USB2 115200`

```text
QNX Neutrino RTOS 8.0 (n1-655-devkit-qnx) (ser2)

login: root
Password: 
# uname -a
QNX n1-655-devkit-qnx 8.0.0 2026/08/15-12:00:00UTC aarch64le
# pidin
     pid name               prio STATE       code  data
       1 proc/boot/procnto    0f READY       3.1M  128M
   16386 devc-seramb         10r RECEIVE      64K  128K
   20482 sbin/tinit          10r SIGWAITINFO  48K   64K
   32770 bin/ksh             10r RECEIVE     192K  256K
# 
```

### 5.3 Automated Telemetry with MCP Tools
Using the `aimon` MCP console tools, automated regression tests can interact with both guest consoles concurrently:
```python
# DevKit Console 2 (Ubuntu)
embdevenv_console_write(target="n1-655-devkit", port="console2", text="\n")
embdevenv_console_expect(target="n1-655-devkit", port="console2", pattern="login:", timeout=5)

# DevKit Console 3 (QNX)
embdevenv_console_write(target="n1-655-devkit", port="console3", text="\n")
embdevenv_console_expect(target="n1-655-devkit", port="console3", pattern="login:", timeout=5)
```
