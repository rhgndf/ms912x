# Part 1: The protocol

Install the windows driver and use wireshark to capture the USB traffic.

## 1.1 Register reads

The first prominent thing is a whole lot of usb CONTROL IN transfers. The requests seem to be from 0xc000 and up with a single byte reply. Collating the responses gives the EDID of the monitor.

From there we can see that this request is a register read. Inspecting the capture shows that register 0x31 is 0 if unplugged and 1 if plugged in.

## 1.2 Resolution changes

In windows settings, change the resolution and record the traffic. A set of commands followed with 6 bytes of CONTROL OUT requests. There are some 16 bit values indicating the resolution and a 8 bit mode number, with other values the same. Replicating this sequence changes the resolution successfully.

## 1.3 Framebuffer

The image is sent using a usb BULK OUT transfer. There is a fixed header, followed by a very long buffer then a fixed terminating 8 byte sequence. Inspecting the smaller packet shows which fields in the header is the x and y position and the width and height of the image. The bytes in the buffer seems to alternate like ABAB which suggests a possible type of yuv422 format. Displaying a fully blue image in the screen confirms this. Encoding an image in this way and sending it to the screen works.

# Part 2: Extras

## 2.1 More resolutions

In 1.2, there wasn't enough resolutions gathered. We could do a register from 0-FFFF after setting two different resolutions. This is done using a modified version of s12wu's code in `dump-regs.py`. Looking at the difference, these two sets of changed registers look useful:

```
0000f120  60 67 d0 e4 08 00 01 01  3d 00 00 0e 00 ff 03 00  | | 0000f120  60 67 d0 e4 08 00 03 01  21 54 55 05 00 ff 03 00  |
0000f130  00 21 00 02 00 04 03 ff  00 fc 00 15 86 71 1e 00  | | 0000f130  01 21 00 02 00 04 03 ff  00 f2 00 01 06 73 1e 00  |
0000f200  00 2e 01 00 80 07 38 04  00 00 00 00 20 03 58 02  | | 0000f200  00 2e 01 00 20 03 58 02  00 00 00 00 20 03 58 02  |
0000f250  00 00 00 00 00 00 10 c0  03 c0 03 c5 00 00 00 00  | | 0000f250  00 00 00 00 00 00 10 90  01 90 01 c5 00 00 00 00  |
0000f260  df c1 f0 00 00 00 00 00  00 10 c0 03 11 3c c0 03  | | 0000f260  df c1 f0 00 00 00 00 00  00 10 90 01 11 3c 90 01  |
0000f2b0  00 00 00 00 00 00 26 00  03 03 03 00 00 00 02 05  | | 0000f2b0  00 00 00 00 00 00 3e 00  00 00 03 00 00 00 02 05  |
0000f380  00 00 80 07 80 07 38 04  38 04 00 00 00 00 00 10  | | 0000f380  00 00 20 03 20 03 58 02  58 02 00 00 00 00 00 10  |
0000f390  00 10 80 80 00 00 12 00  98 08 65 04 2c 00 05 00  | | 0000f390  00 10 80 80 00 00 12 00  20 04 74 02 80 00 04 00  |
0000f3a0  c0 00 40 08 29 00 61 04  c0 00 40 08 29 00 61 04  | | 0000f3a0  d8 00 f8 03 1b 00 73 02  d8 00 f8 03 1b 00 73 02  |
0000f3b0  00 00 00 00 00 00 00 00  00 88 61 04 04 00 00 00  | | 0000f3b0  00 00 00 00 00 00 00 00  00 88 73 02 01 00 00 00  |
0000f440  00 77 06 00 10 80 80 00  00 00 00 00 00 00 00 00  | | 0000f440  00 ff 03 00 10 80 80 00  00 00 00 00 00 00 00 00  |
```

Comparing the values with those at with those at https://tomverbeure.github.io/video_timings_calculator, we can see what some of the values mean: 

| Register | Description |
|----------|-------------|
| 0xf204 | hactive |
| 0xf206 | vactive |
| 0xf26d | hz? | 
| 0xf382 | hactive |
| 0xf384 | hactive |
| 0xf386 | vactive |
| 0xf388 | vactive |
| 0xf398 | htotal |
| 0xf39a | vtotal |
| 0xf39c | hsync |
| 0xf39e | vsync |
| 0xf3a0 | hbackporch + hsync |
| 0xf3a2 | htotal - hfrontporch |
| 0xf3a4 | vbackporch + vsync |
| 0xf3a6 | vtotal - vfrontporch |

Next, dump the registers for all the resolutions and output them in `dump-resolutions.py`. This is used to populate the driver with all the resolutions.