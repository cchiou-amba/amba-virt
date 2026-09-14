#!/usr/bin/env python3
import sys, os, struct
import hivex

def get_or_create(h, parent, name):
    node = h.node_get_child(parent, name)
    if not node:
        node = h.node_add_child(parent, name)
    return node

def str_utf16(s):
    return (s + "\x00").encode("utf-16le")

def multi_sz(lst):
    return ("\x00".join(lst) + "\x00\x00").encode("utf-16le")

def dword(v):
    return struct.pack("<I", v)

def configure_system_hive(hive_path):
    print(f"Configuring SYSTEM hive at {hive_path}...")
    h = hivex.Hivex(hive_path, write=True)
    root = h.root()
    cs = h.node_get_child(root, "ControlSet001")
    if not cs:
        raise RuntimeError("ControlSet001 not found in SYSTEM hive")
    services = h.node_get_child(cs, "Services")
    if not services:
        raise RuntimeError("Services not found in SYSTEM hive")

    # 1. viostor Service (VirtIO SCSI / Block)
    print("  -> Configuring viostor service...")
    viostor = get_or_create(h, services, "viostor")
    h.node_set_values(viostor, [
        {"key": "DisplayName", "t": 1, "value": str_utf16("Red Hat VirtIO SCSI controller")},
        {"key": "ErrorControl", "t": 4, "value": dword(3)},
        {"key": "Group", "t": 1, "value": str_utf16("SCSI Miniport")},
        {"key": "ImagePath", "t": 2, "value": str_utf16("System32\\drivers\\viostor.sys")},
        {"key": "Start", "t": 4, "value": dword(0)},
        {"key": "Type", "t": 4, "value": dword(1)},
        {"key": "Tag", "t": 4, "value": dword(1)},
        {"key": "Owners", "t": 7, "value": multi_sz(["viostor.inf"])},
    ])
    params = get_or_create(h, viostor, "Parameters")
    h.node_set_values(params, [
        {"key": "BusType", "t": 4, "value": dword(1)},
        {"key": "DmaRemappingCompatible", "t": 4, "value": dword(0)},
    ])
    pnp = get_or_create(h, params, "PnpInterface")
    h.node_set_values(pnp, [{"key": "5", "t": 4, "value": dword(1)}])

    # 2. netkvm Service (VirtIO Network Adapter)
    print("  -> Configuring netkvm service...")
    netkvm = get_or_create(h, services, "netkvm")
    h.node_set_values(netkvm, [
        {"key": "DisplayName", "t": 1, "value": str_utf16("Red Hat VirtIO Ethernet Adapter Service")},
        {"key": "ErrorControl", "t": 4, "value": dword(1)},
        {"key": "Group", "t": 1, "value": str_utf16("NDIS")},
        {"key": "ImagePath", "t": 2, "value": str_utf16("System32\\drivers\\netkvm.sys")},
        {"key": "Start", "t": 4, "value": dword(0)},
        {"key": "Type", "t": 4, "value": dword(1)},
        {"key": "Owners", "t": 7, "value": multi_sz(["netkvm.inf"])},
    ])
    net_params = get_or_create(h, netkvm, "Parameters")
    h.node_set_values(net_params, [
        {"key": "DisableMSI", "t": 1, "value": str_utf16("0")},
        {"key": "EarlyDebug", "t": 1, "value": str_utf16("3")},
        {"key": "DmaRemappingCompatible", "t": 4, "value": dword(2)},
    ])

    # 3. VioGpuDod Service (VirtIO GPU Display Driver)
    print("  -> Configuring VioGpuDod service...")
    viogpu = get_or_create(h, services, "VioGpuDod")
    h.node_set_values(viogpu, [
        {"key": "DisplayName", "t": 1, "value": str_utf16("Red Hat VirtIO GPU DOD controller")},
        {"key": "ErrorControl", "t": 4, "value": dword(0)},
        {"key": "Group", "t": 1, "value": str_utf16("Video")},
        {"key": "ImagePath", "t": 2, "value": str_utf16("System32\\drivers\\viogpudo.sys")},
        {"key": "Start", "t": 4, "value": dword(0)},
        {"key": "Type", "t": 4, "value": dword(1)},
        {"key": "Owners", "t": 7, "value": multi_sz(["viogpudo.inf"])},
    ])
    gpu_params = get_or_create(h, viogpu, "Parameters")
    h.node_set_values(gpu_params, [
        {"key": "HWCursor", "t": 4, "value": dword(0)},
        {"key": "FlexResolution", "t": 4, "value": dword(1)},
        {"key": "UsePhysicalMemory", "t": 4, "value": dword(0)},
        {"key": "UsePresentProgress", "t": 4, "value": dword(0)},
    ])

    # 4. TermService (Remote Desktop)
    print("  -> Ensuring TermService starts automatically...")
    termsvc = get_or_create(h, services, "TermService")
    h.node_set_values(termsvc, [{"key": "Start", "t": 4, "value": dword(2)}])

    # 5. CriticalDeviceDatabase for boot-critical hardware matching
    print("  -> Injecting VirtIO devices into CriticalDeviceDatabase...")
    ctrl = get_or_create(h, cs, "Control")
    cdd = get_or_create(h, ctrl, "CriticalDeviceDatabase")
    scsi_guid = "{4D36E97B-E325-11CE-BFC1-08002BE10318}"
    net_guid = "{4D36E972-E325-11CE-BFC1-08002BE10318}"
    disp_guid = "{4D36E968-E325-11CE-BFC1-08002BE10318}"

    crit_devices = [
        ("PCI#VEN_1AF4&DEV_1001", scsi_guid, "viostor"),
        ("PCI#VEN_1AF4&DEV_1001&SUBSYS_00021AF4&REV_00", scsi_guid, "viostor"),
        ("PCI#VEN_1AF4&DEV_1042", scsi_guid, "viostor"),
        ("PCI#VEN_1AF4&DEV_1042&SUBSYS_11001AF4&REV_01", scsi_guid, "viostor"),
        ("PCI#VEN_1AF4&DEV_1042&SUBSYS_11001AF4", scsi_guid, "viostor"),
        ("PCI#CC_010000", scsi_guid, "viostor"),
        ("PCI#CC_0100", scsi_guid, "viostor"),
        ("pci#ven_1af4&dev_1001", scsi_guid, "viostor"),
        ("pci#ven_1af4&dev_1001&subsys_00021af4&rev_00", scsi_guid, "viostor"),
        ("pci#ven_1af4&dev_1042", scsi_guid, "viostor"),
        ("pci#ven_1af4&dev_1042&subsys_11001af4&rev_01", scsi_guid, "viostor"),
        ("pci#cc_010000", scsi_guid, "viostor"),
        ("pci#cc_0100", scsi_guid, "viostor"),

        ("PCI#VEN_1AF4&DEV_1000", net_guid, "netkvm"),
        ("PCI#VEN_1AF4&DEV_1000&SUBSYS_00011AF4&REV_00", net_guid, "netkvm"),
        ("PCI#VEN_1AF4&DEV_1041", net_guid, "netkvm"),
        ("PCI#VEN_1AF4&DEV_1041&SUBSYS_11001AF4&REV_01", net_guid, "netkvm"),
        ("PCI#CC_020000", net_guid, "netkvm"),
        ("PCI#CC_0200", net_guid, "netkvm"),
        ("pci#ven_1af4&dev_1000", net_guid, "netkvm"),
        ("pci#ven_1af4&dev_1000&subsys_00011af4&rev_00", net_guid, "netkvm"),
        ("pci#ven_1af4&dev_1041", net_guid, "netkvm"),
        ("pci#ven_1af4&dev_1041&subsys_11001af4&rev_01", net_guid, "netkvm"),
        ("pci#cc_020000", net_guid, "netkvm"),
        ("pci#cc_0200", net_guid, "netkvm"),

        ("PCI#VEN_1AF4&DEV_1050", disp_guid, "VioGpuDod"),
        ("PCI#VEN_1AF4&DEV_1050&SUBSYS_11001AF4&REV_01", disp_guid, "VioGpuDod"),
        ("PCI#CC_030000", disp_guid, "VioGpuDod"),
        ("PCI#CC_0300", disp_guid, "VioGpuDod"),
        ("pci#ven_1af4&dev_1050", disp_guid, "VioGpuDod"),
        ("pci#ven_1af4&dev_1050&subsys_11001af4&rev_01", disp_guid, "VioGpuDod"),
        ("pci#cc_030000", disp_guid, "VioGpuDod"),
        ("pci#cc_0300", disp_guid, "VioGpuDod"),
    ]
    for dev_id, class_guid, svc in crit_devices:
        node = get_or_create(h, cdd, dev_id)
        h.node_set_values(node, [
            {"key": "ClassGUID", "t": 1, "value": str_utf16(class_guid)},
            {"key": "Service", "t": 1, "value": str_utf16(svc)},
        ])

    # 6. Complete Windows 11 DriverDatabase entries for PnP matching
    print("  -> Injecting VirtIO devices into DriverDatabase...")
    dd = h.node_get_child(root, "DriverDatabase")
    if dd:
        devids = get_or_create(h, dd, "DeviceIds")
        pci = get_or_create(h, devids, "pci")
        inffiles = get_or_create(h, dd, "DriverInfFiles")
        pkgs = get_or_create(h, dd, "DriverPackages")

        import uuid
        def make_driver_version(class_guid_str):
            u = uuid.UUID(class_guid_str)
            ver = bytearray(48)
            struct.pack_into("<H", ver, 0, 0xffff)
            struct.pack_into("<H", ver, 2, 0x000c)
            ver[8:24] = u.bytes_le
            struct.pack_into("<Q", ver, 24, 0x01dcd5c5a38c8000)
            struct.pack_into("<Q", ver, 32, 0x000a000065f41ef0)
            return bytes(ver)

        driver_specs = [
            {
                "inf": "viostor.inf",
                "pkg": "viostor.inf_arm64_virtio",
                "cfg": "scsi_inst",
                "svc": "viostor",
                "guid": scsi_guid,
                "desc": "Red Hat VirtIO SCSI controller",
                "dev_ids": [
                    "VEN_1AF4&DEV_1042&SUBSYS_11001AF4&REV_01",
                    "VEN_1AF4&DEV_1042&SUBSYS_11001AF4",
                    "VEN_1AF4&DEV_1042&CC_010000",
                    "VEN_1AF4&DEV_1042&CC_0100",
                    "VEN_1AF4&DEV_1042&REV_01",
                    "VEN_1AF4&DEV_1042*",
                    "VEN_1AF4&DEV_1001&SUBSYS_00021AF4&REV_00",
                    "VEN_1AF4&DEV_1001*",
                    "CC_010000",
                    "CC_0100*",
                ],
                "extra_cfg": True,
            },
            {
                "inf": "netkvm.inf",
                "pkg": "netkvm.inf_arm64_virtio",
                "cfg": "kvmnet6.ndi",
                "svc": "netkvm",
                "guid": net_guid,
                "desc": "Red Hat VirtIO Ethernet Adapter",
                "dev_ids": [
                    "VEN_1AF4&DEV_1041&SUBSYS_11001AF4&REV_01",
                    "VEN_1AF4&DEV_1041&SUBSYS_11001AF4",
                    "VEN_1AF4&DEV_1041&CC_020000",
                    "VEN_1AF4&DEV_1041&CC_0200",
                    "VEN_1AF4&DEV_1041&REV_01",
                    "VEN_1AF4&DEV_1041*",
                    "VEN_1AF4&DEV_1000&SUBSYS_00011AF4&REV_00",
                    "VEN_1AF4&DEV_1000*",
                    "CC_020000",
                    "CC_0200*",
                ],
                "extra_cfg": False,
            },
            {
                "inf": "viogpudo.inf",
                "pkg": "viogpudo.inf_arm64_virtio",
                "cfg": "VioGpuDod_Inst",
                "svc": "VioGpuDod",
                "guid": disp_guid,
                "desc": "Red Hat VirtIO GPU DOD controller",
                "dev_ids": [
                    "VEN_1AF4&DEV_1050&SUBSYS_11001AF4&REV_01",
                    "VEN_1AF4&DEV_1050&SUBSYS_11001AF4",
                    "VEN_1AF4&DEV_1050&CC_030000",
                    "VEN_1AF4&DEV_1050&CC_0300",
                    "VEN_1AF4&DEV_1050&REV_01",
                    "VEN_1AF4&DEV_1050*",
                    "CC_030000",
                    "CC_0300*",
                ],
                "extra_cfg": False,
            },
        ]

        for spec in driver_specs:
            inf_name = spec["inf"]
            pkg_name = spec["pkg"]
            cfg_name = spec["cfg"]
            svc_name = spec["svc"]
            class_guid = spec["guid"]
            desc_text = spec["desc"]

            # 6a. DriverInfFiles
            inf_node = get_or_create(h, inffiles, inf_name)
            h.node_set_values(inf_node, [
                {"key": "", "t": 7, "value": multi_sz([pkg_name])},
                {"key": "Active", "t": 1, "value": str_utf16(pkg_name)},
                {"key": "Configurations", "t": 7, "value": multi_sz([cfg_name])},
            ])

            # 6b. DriverPackages
            pkg_node = get_or_create(h, pkgs, pkg_name)
            h.node_set_values(pkg_node, [
                {"key": "", "t": 1, "value": str_utf16(inf_name)},
                {"key": "Provider", "t": 1, "value": str_utf16("Red Hat Inc.")},
                {"key": "SignerScore", "t": 4, "value": dword(0x0d000003)},
                {"key": "StatusFlags", "t": 4, "value": dword(0x00001112)},
                {"key": "Version", "t": 3, "value": make_driver_version(class_guid)},
                {"key": "FileSize", "t": 11, "value": struct.pack("<Q", 50000)},
            ])

            cfgs_node = get_or_create(h, pkg_node, "Configurations")
            cfg_sub = get_or_create(h, cfgs_node, cfg_name)
            h.node_set_values(cfg_sub, [
                {"key": "Service", "t": 1, "value": str_utf16(svc_name)},
                {"key": "ConfigFlags", "t": 4, "value": dword(0)},
                {"key": "ConfigScope", "t": 4, "value": dword(0x00000f7f)},
                {"key": "IncludedInfs", "t": 7, "value": multi_sz(["pci.inf"])},
            ])

            # Services subkey under configuration is mandatory in Windows 11
            svc_node = get_or_create(h, cfg_sub, "Services")
            cfg_svc = get_or_create(h, svc_node, svc_name)

            if spec.get("extra_cfg"):
                # Viostor specific parameters
                v_par = get_or_create(h, cfg_svc, "Parameters")
                h.node_set_values(v_par, [
                    {"key": "BusType", "t": 4, "value": dword(1)},
                    {"key": "DmaRemappingCompatible", "t": 4, "value": dword(0)},
                ])
                v_pnp = get_or_create(h, v_par, "PnpInterface")
                h.node_set_values(v_pnp, [{"key": "5", "t": 4, "value": dword(1)}])

                dev_sub = get_or_create(h, cfg_sub, "Device")
                int_sub = get_or_create(h, dev_sub, "Interrupt Management")
                msi_sub = get_or_create(h, int_sub, "MessageSignaledInterruptProperties")
                h.node_set_values(msi_sub, [
                    {"key": "MSISupported", "t": 4, "value": dword(1)},
                    {"key": "MessageNumberLimit", "t": 4, "value": dword(257)},
                ])
                aff_sub = get_or_create(h, int_sub, "Affinity Policy")
                h.node_set_values(aff_sub, [{"key": "DevicePolicy", "t": 4, "value": dword(5)}])

            # 6c. Descriptors\PCI and DeviceIds\pci
            desc_node = get_or_create(h, pkg_node, "Descriptors")
            desc_pci = get_or_create(h, desc_node, "PCI")
            for dev_id in spec["dev_ids"]:
                d_node = get_or_create(h, desc_pci, dev_id)
                h.node_set_values(d_node, [
                    {"key": "Configuration", "t": 1, "value": str_utf16(cfg_name)},
                    {"key": "Description", "t": 1, "value": str_utf16(desc_text)},
                    {"key": "Manufacturer", "t": 1, "value": str_utf16("Red Hat Inc.")},
                ])
                clean_id = dev_id.rstrip("*")
                pci_entry = get_or_create(h, pci, clean_id)
                h.node_set_values(pci_entry, [{"key": inf_name, "t": 3, "value": b"\x01\xff\x00\x00"}])
                if clean_id != dev_id:
                    pci_entry_raw = get_or_create(h, pci, dev_id)
                    h.node_set_values(pci_entry_raw, [{"key": inf_name, "t": 3, "value": b"\x01\xff\x00\x00"}])

    # 7. Enable Remote Desktop in SYSTEM hive
    print("  -> Enabling RDP connections in Terminal Server settings...")
    ctrl = get_or_create(h, cs, "Control")
    ts = get_or_create(h, ctrl, "Terminal Server")
    h.node_set_values(ts, [{"key": "fDenyTSConnections", "t": 4, "value": dword(0)}])

    winstations = get_or_create(h, ts, "WinStations")
    rdp_tcp = get_or_create(h, winstations, "RDP-Tcp")
    h.node_set_values(rdp_tcp, [
        {"key": "UserAuthentication", "t": 4, "value": dword(0)},
        {"key": "SecurityLayer", "t": 4, "value": dword(1)},
        {"key": "fEnableWinStation", "t": 4, "value": dword(1)},
    ])

    # 8. Disable Firewall in SYSTEM hive
    print("  -> Disabling Windows Firewall across all profiles...")
    shared_access = get_or_create(h, services, "SharedAccess")
    params = get_or_create(h, shared_access, "Parameters")
    fw_policy = get_or_create(h, params, "FirewallPolicy")
    for profile in ["StandardProfile", "DomainProfile", "PublicProfile"]:
        pnode = get_or_create(h, fw_policy, profile)
        h.node_set_values(pnode, [{"key": "EnableFirewall", "t": 4, "value": dword(0)}])

    # 9. Normal Boot Mode (SetupType = 0, SystemSetupInProgress = 0)
    print("  -> Ensuring Normal Boot Mode in SYSTEM hive (SetupType = 0)...")
    setup = get_or_create(h, root, "Setup")
    h.node_set_values(setup, [
        {"key": "SetupType", "t": 4, "value": dword(0)},
        {"key": "SystemSetupInProgress", "t": 4, "value": dword(0)},
        {"key": "OOBEInProgress", "t": 4, "value": dword(0)},
        {"key": "CmdLine", "t": 1, "value": str_utf16("")},
    ])

    # 10. CrashControl: Disable AutoReboot on BSOD
    print("  -> Configuring CrashControl to prevent auto-reboot on BugCheck...")
    crash = get_or_create(h, ctrl, "CrashControl")
    h.node_set_values(crash, [
        {"key": "AutoReboot", "t": 4, "value": dword(0)},
        {"key": "CrashDumpEnabled", "t": 4, "value": dword(1)},
    ])

    h.commit(None)
    print("SYSTEM hive updated successfully.")

def configure_software_hive(hive_path):
    print(f"Configuring SOFTWARE hive at {hive_path}...")
    h = hivex.Hivex(hive_path, write=True)
    root = h.root()
    ms = h.node_get_child(root, "Microsoft")
    if not ms:
        raise RuntimeError("Microsoft not found in SOFTWARE hive")
    win = h.node_get_child(ms, "Windows")
    if not win:
        raise RuntimeError("Windows not found in SOFTWARE hive")
    cv = h.node_get_child(win, "CurrentVersion")
    if not cv:
        raise RuntimeError("CurrentVersion not found in SOFTWARE hive")

    # 1. DevicePath
    print("  -> Setting VirtIO DevicePath...")
    devpath = "%SystemRoot%\\inf;C:\\Drivers\\VirtIO;C:\\Drivers\\VirtIO\\viostor;C:\\Drivers\\VirtIO\\NetKVM;C:\\Drivers\\VirtIO\\viogpudo;C:\\Drivers\\VirtIO\\Balloon;C:\\Drivers\\VirtIO\\pvpanic;C:\\Drivers\\VirtIO\\vioserial\x00"
    h.node_set_value(cv, {"key": "DevicePath", "t": 2, "value": devpath.encode("utf-16le")})

    # 2. Winlogon AutoLogon
    print("  -> Configuring AutoAdminLogon for user 'windows'...")
    win_nt = get_or_create(h, ms, "Windows NT")
    win_nt_cv = get_or_create(h, win_nt, "CurrentVersion")
    winlogon = get_or_create(h, win_nt_cv, "Winlogon")
    h.node_set_values(winlogon, [
        {"key": "AutoAdminLogon", "t": 1, "value": str_utf16("1")},
        {"key": "DefaultUserName", "t": 1, "value": str_utf16("windows")},
        {"key": "DefaultPassword", "t": 1, "value": str_utf16("windows")},
        {"key": "AutoLogonCount", "t": 4, "value": dword(9999)},
        {"key": "ForceAutoLogon", "t": 1, "value": str_utf16("1")},
    ])

    # 3. Disable UAC elevation prompts
    print("  -> Disabling UAC prompts for unattended operation...")
    policies = get_or_create(h, cv, "Policies")
    sys_pol = get_or_create(h, policies, "System")
    h.node_set_values(sys_pol, [
        {"key": "EnableLUA", "t": 4, "value": dword(0)},
        {"key": "PromptOnSecureDesktop", "t": 4, "value": dword(0)},
        {"key": "ConsentPromptBehaviorAdmin", "t": 4, "value": dword(0)},
        {"key": "dontdisplaylastusername", "t": 4, "value": dword(0)},
        {"key": "LegalNoticeCaption", "t": 1, "value": str_utf16("")},
        {"key": "LegalNoticeText", "t": 1, "value": str_utf16("")},
    ])

    # 4. RunOnce fallback for firstboot.cmd
    print("  -> Adding RunOnce entry for firstboot.cmd...")
    runonce = get_or_create(h, cv, "RunOnce")
    h.node_set_values(runonce, [
        {"key": "FirstBoot", "t": 1, "value": str_utf16("cmd.exe /c C:\\Windows\\Setup\\Scripts\\firstboot.cmd")},
    ])

    h.commit(None)
    print("SOFTWARE hive updated successfully.")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <path_to_SYSTEM> <path_to_SOFTWARE>")
        sys.exit(1)
    configure_system_hive(sys.argv[1])
    configure_software_hive(sys.argv[2])
