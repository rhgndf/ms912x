# ms912x driver for Linux

Linux kernel driver for MacroSilicon USB to VGA/HDMI adapter.

There are two variants:
 - VID/PID is 534d:6021. Device is USB 2
 - VID/PID is 345f:9132. Device is USB 3

- Improved performance and Driver adaptation for Linux kernel 6.15. by Andrey Rodríguez Araya

- For kernel 6.16 checkout branch kernel-6.16
- For kernel 6.15 checkout branch kernel-6.15
- For kernel 6.12 checkout branch kernel-6.12
- For kernel 6.11 ?
- For kernel 6.8 - 6.10 checkout branch kernel-6.8

TODOs:

- Detect connector type (VGA, HDMI, etc...)
- More resolutions
- Error handling
- Is RGB to YUV conversion needed?

## Development 

Driver is written by analyzing wireshark captures of the device.

## DKMS

Run `sudo dkms install .`

- make clean
- make all -j
- sudo rmmod ms912x # It will not work if the device is in use.
- sudo modprobe drm_shmem_helper
- sudo insmod ms912x.ko

Forked From: https://github.com/rhgndf/ms912x


