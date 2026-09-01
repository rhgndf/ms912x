# ms912x driver for Linux

Linux kernel driver for MacroSilicon USB to VGA/HDMI adapter.

There are three variants:
- VID/PID is 534d:6021. Device is USB 2
- VID/PID is 534d:0821. Device is USB 2
- VID/PID is 345f:9132. Device is USB 3

## Supported kernels

Use the branch matching the target kernel:

- Linux 5.10 LTS: `kernel-5.10`
- Linux 5.15 LTS: `kernel-5.15`
- Linux 6.1 LTS: `kernel-6.1`
- Linux 6.6 LTS: `kernel-6.6`
- Linux 6.12 LTS: `kernel-6.12`
- Linux 6.18 LTS: `kernel-6.18`
- Linux 7.2: `kernel-7.2`

`main` tracks current development.

TODOs:

- Detect connector type (VGA, HDMI, etc...)
- More resolutions
- Error handling
- Is RGB to YUV conversion needed?

## Development 

Driver is written by analyzing wireshark captures of the device.

## DKMS

Run `sudo dkms install .`

