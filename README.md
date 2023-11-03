# ms912x driver for Linux

Linux kernel driver for MacroSilicon USB to VGA/HDMI adapter. VID/PID is 534d:6021. Device is USB2.0

For kernel 6.1 checkout branch kernel-6.1

TODOs:

- Detect connector type (VGA, HDMI, etc...)
- More resolutions
- Error handling
- Is RGB to YUV conversion needed?

## Development 

Driver is written by analyzing wireshark captures of the device.

## DKMS

Run `sudo dkms install .`

