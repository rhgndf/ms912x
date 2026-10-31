# Linux Driver for MacroSilicon USB Display Adapters (MS912x / MS912C)

**[Read in English](README.md)** | **[Leia em Português](README.pt-BR.md)**

---

Linux DRM/KMS kernel driver with **DKMS** support for external USB-to-VGA and USB-to-HDMI display adapters based on **MacroSilicon MS912x** chipsets.

Many of these adapters ship with an internal flash storage volume (~8 MB) containing drivers only for Windows (`.exe`), macOS (`.dmg`), and Android (`.apk`). This driver adds native Linux kernel support, enabling external displays to work seamlessly in desktop environments (GNOME, Cinnamon, KDE, XFCE, etc.).

---

## Supported Devices

| USB ID (`lsusb`) | Bus Speed | Device / Chipset |
|---|---|---|
| `534d:6021` | USB 2.0 | MacroSilicon VGA / HDMI Display Adapter |
| `534d:0821` | USB 2.0 | MacroSilicon Display Adapter |
| `345f:9132` | USB 3.0 | MacroSilicon MS913x Display Adapter |
| `345f:9133` | USB 3.0 | MacroSilicon MS913x Display Adapter |
| `345f:9135` | USB 3.0 | MacroSilicon MS913x Display Adapter |

---

## Features

- **Modern Kernel Compatibility**: Fixes DRM atomic helper signatures (`struct drm_atomic_state`) for smooth compilation on Linux kernels 6.x and 7.x.
- **DKMS Support**: Automatically rebuilds and installs the driver whenever the system updates to a new Linux kernel.
- **Automated Setup Script**: One-command compilation, DKMS registration, and module activation.

---

## Installation

### 1. Install Build Dependencies

On Debian, Ubuntu, Linux Mint, and derivatives:

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r) dkms
```

On Fedora / RHEL:

```bash
sudo dnf install -y gcc make kernel-devel dkms
```

On Arch Linux:

```bash
sudo pacman -S --needed base-devel linux-headers dkms
```

### 2. Clone & Install

```bash
git clone https://github.com/rhgndf/ms912x.git
cd ms912x
sudo bash install.sh
```

*(If you prefer Portuguese terminal output, you can run `sudo bash instalar.sh` instead).*

The script will build the kernel module, register it with DKMS, and immediately load `ms912x`. Your external monitor should turn on right away.

---

## Uninstallation

To remove the driver from your system:

```bash
sudo dkms remove -m ms912x -v 0.1 --all
sudo rm -f /lib/modules/$(uname -r)/updates/ms912x.ko
sudo depmod -a
```

---

## License

GPL-2.0. Based on community reverse engineering of MacroSilicon hardware protocols.
