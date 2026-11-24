# Mu-Silicium for Nubia RedMagic 8S Pro (`nx729j`) — Kettle Linux Branch

This branch (`nx729j-kettle`) is a clean downstream fork of [Project-Silicium/Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium) customized for booting mainline Linux / [Kettle Linux](https://github.com/kettlelinux/kettlelinux) on the **ZTE Nubia RedMagic 8S Pro (`nx729j`, SM8550)**.

The upstream `main` branch and all other device targets are kept intact so upstream `Mu-Silicium` updates can be merged or rebased cleanly.

## Summary of Changes on `nx729j-kettle`

1. **`QcomPkg/MassStorage` single-LUN fix**
   Fixes an off-by-one loop bound in `AssignStorageHandles()` that prevented USB Mass Storage from exposing a LUN when the controller only offered 1 assignable LUN.
2. **`nx729j` hardware reset & FDT publishing**
   - Loads Qualcomm's `ResetRuntimeDxe` before `ResetSystemRuntimeDxe` in `APRIORI.inc` so shutdown powers the device off instead of rebooting.
   - Enables `DtPlatformDxe` and embeds `Platforms/ZTE/nx729jPkg/FdtBlob/sm8550-zte-nx729j.dtb` (compiled from the Kettle `nx729j` DTS, including SM8550 `reserved-memory` carveouts for `llcc_lpi_region`, `chipinfo-region`, and `oem-vm-gap-region`).
3. **Kettle boot UI, dynamic RAM/initrd handoff, and diagnostics**
   - Dynamically detects full DDR capacity and patches the FDT `/memory` node at boot.
   - Registers ESP Linux boot entries and publishes the initramfs via `EFI_LOAD_FILE2_PROTOCOL`.
   - Adds Kettle branding bitmaps, scaled portrait console/menu rendering, and a Volume Down settings screen.
   - Adds `KettleDebugRingLib` + `KettleLogDxe` (scoped to `nx729j.dsc` via `KETTLE_DEBUG_CAPTURE`) for persistent boot diagnostics in `logfs`.

## Building

Follow the standard upstream [Building.md](Building.md) prerequisites on Linux:

```bash
git clone --recursive -b nx729j-kettle https://github.com/TimmyTurner51/Mu-Silicium.git
cd Mu-Silicium
./setup_env.sh -p apt
python3 build_uefi.py -d nx729j
```

This produces `Mu-nx729j.img`, which can be flashed to `boot_a` / `boot_b`.

### Replacing the Embedded DTB

If you compile a new `sm8550-nx729j.dtb` and replace `Platforms/ZTE/nx729jPkg/FdtBlob/sm8550-zte-nx729j.dtb`, make sure it has at least **4096 bytes of free padding** so `KettlePatchFdtMemory()` has space to rewrite the `/memory` node at boot:

```bash
dtc -I dtb -O dtb -p 4096 -o Platforms/ZTE/nx729jPkg/FdtBlob/sm8550-zte-nx729j.dtb path/to/sm8550-nx729j.dtb
```

## Credits & Disclosure

- Upstream UEFI framework and SM8550/Kailua platform support: [Project Silicium](https://github.com/Project-Silicium/Mu-Silicium) and [Microsoft Project Mu](https://microsoft.github.io/mu/).
- Original `nx729jPkg` platform port: [Robotix](https://github.com/Robotix22/) and [Nikka](https://github.com/NikkaGames/).
- Target OS & branding assets: [Kettle Linux](https://github.com/kettlelinux/kettlelinux).
- **AI-assisted development disclosure:** The bring-up modifications on this `nx729j-kettle` branch were developed with AI assistance and verified on physical RedMagic 8S Pro (`nx729j`) hardware.

## License

Inherits upstream licensing: `BSD-2-Clause` / `BSD-2-Clause-Patent` (and `GPLv2+` for any upstream GPL components).
