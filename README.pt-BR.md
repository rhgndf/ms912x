# Driver Linux para Adaptadores de Vídeo USB MacroSilicon (MS912x / MS912C)

**[Read in English](README.md)** | **[Leia em Português](README.pt-BR.md)**

---

Driver de kernel Linux (DRM/KMS) com suporte a **DKMS** para adaptadores externos USB para HDMI/VGA baseados nos chips **MacroSilicon MS912x**.

Esses adaptadores costumam vir de fábrica com uma pequena partição de armazenamento (~8 MB) contendo instaladores apenas para Windows (`.exe`), macOS (`.dmg`) e Android (`.apk`). Este driver adiciona suporte nativo no Linux, fazendo com que o monitor conectado pelo adaptador USB seja reconhecido normalmente nas configurações de tela do sistema (Cinnamon, GNOME, KDE, XFCE, etc.).

---

## Dispositivos suportados

| ID USB (`lsusb`) | Barramento | Chip / Dispositivo |
|---|---|---|
| `534d:6021` | USB 2.0 | MacroSilicon VGA / HDMI Display Adapter |
| `534d:0821` | USB 2.0 | MacroSilicon Display Adapter |
| `345f:9132` | USB 3.0 | MacroSilicon MS913x Display Adapter |
| `345f:9133` | USB 3.0 | MacroSilicon MS913x Display Adapter |
| `345f:9135` | USB 3.0 | MacroSilicon MS913x Display Adapter |

---

## Destaques desta versão

- **Compatibilidade com Kernel 6.x e 7.x**: correções nas assinaturas de callbacks DRM (`drm_atomic_state` nos helpers de CRTC e Plane).
- **Instalação com DKMS**: o driver é recompilado automaticamente pelo sistema quando o Linux atualiza a versão do kernel.
- **Script tudo-em-um**: compilação, instalação e carregamento em um único comando.

---

## Como instalar

### 1. Pré-requisitos de compilação

No Debian, Ubuntu, Linux Mint e derivados:

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r) dkms
```

No Fedora / RHEL:

```bash
sudo dnf install -y gcc make kernel-devel dkms
```

No Arch Linux:

```bash
sudo pacman -S --needed base-devel linux-headers dkms
```

### 2. Clonar e instalar

```bash
git clone https://github.com/rhgndf/ms912x.git
cd ms912x
sudo bash instalar.sh
```

*(Caso prefira mensagens em inglês no terminal, execute `sudo bash install.sh`).*

O script compilará o módulo, configurará o DKMS e ativará o driver imediatamente. Seu monitor externo acenderá na hora.

---

## Desinstalação

Caso queira remover o driver do sistema:

```bash
sudo dkms remove -m ms912x -v 0.1 --all
sudo rm -f /lib/modules/$(uname -r)/updates/ms912x.ko
sudo depmod -a
```

---

## Licença

GPL-2.0. Baseado no trabalho de engenharia reversa da comunidade para os chips MacroSilicon.
