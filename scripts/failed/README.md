# Failed in-place reconfigure

Do **not** run these scripts against gmwtus. They exit immediately.

Tried on `ubuntu_24_04-container` (Devkit + Pro instances):

- `push_app.sh` — `edge-app update --manifest` to add `cavalry` /
  `gpio0` / `iav`. After stripping `show --detail` UI fields, gmwtus
  returned `InvalidFieldFormat: has active app instances: new
  Interfaces cannot be added`.
- `--stop` on both containers left them Halted / Inactive.
  `appInstCount` stayed **2**. Halted still blocks new bundle
  interfaces. This is controller validation, not an account ACL.
- `set_adapters` cannot attach names missing from the edge-app
  template (`Interface X not found in edge app template`).
- Deleting instance records to empty `appInstCount` would drop
  implicit volumes (`…_0_m_0`). Do not.

The HVM (`ubuntu_24_04`) was not the problem and does not need I/O
changes. vsock is already there.

**Do this instead:** create a **new** edge-app with those interfaces
already in the manifest, then create a **new** instance with
`--adapter=` at create. That is a new rootfs. Leave the old containers
(eth0 only). Recipe:
[doc/EVE-ReconfigureEdgeApps.md](../../doc/EVE-ReconfigureEdgeApps.md).

Keep using `scripts/create_app.sh` and `scripts/create_instance.sh`.
