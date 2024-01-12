# %%
import usb.core
import usb.util
from time import sleep
from tqdm import *

# %%
def read(address):
    global dev

    data = [0] * 8
    data[0] = 0xb5
    data[1] = (address & 0xFF00) >> 8
    data[2] = address & 0xFF

    # bmRequestType: 0x21 - USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE
    # bRequest:      0x09 - HID_REQ_SET_REPORT
    dev.ctrl_transfer(0x21, 0x09, 0x0300, 0, data)


    # bmRequestType: 0xa1 - USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE
    # bRequest:      0x01 - HID_REQ_GET_REPORT
    # we want to read 8 bytes
    data = dev.ctrl_transfer(0xa1, 0x01, 0x0300, 0, 8)

    # data now looks like this
    # idx |  0 | 1 | 2 | 3 | 4,5,6,7
    # val | b5 | H | L | x | follow-up bytes
    #            ^   ^   ^
    #           address  |
    # high and low byte  |
    #                    byte we wanted to read
    
    return data[3]

# %%
def write6(address, six_bytes):
    global dev
    
    data = [0] * 2
    data[0] = 0xa6
    data[1] = address # on a write request, there is only one address byte...?
    data += six_bytes

    # bmRequestType: 0x21 - USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE
    # bRequest:      0x09 - HID_REQ_SET_REPORT
    dev.ctrl_transfer(0x21, 0x09, 0x0300, 0, data)



# %%
def power_on():
    write6(0x07, [1,2,0,0,0,0])

def power_off():
    write6(0x07, [0,0,0,0,0,0])
    # After powering off and on again a new call of set_resulution seems to be necessary

#detect if hdmi cable is connected
def detect():
    return read(0x32)


def set_resolution():
    global width, height, hz, mode, pixfmt
    
    write6(0x04, [0,0,0,0,0,0])
    read(0x30)
    read(0x33)
    read(0xc620)
    write6(0x03, [3,0,0,0,0,0])

    data = [
        (width & 0xFF00) >> 8,
        width & 0xFF,
        (height & 0xFF00) >> 8,
        height & 0xFF,
        (pixfmt & 0xFF00) >> 8,
        pixfmt & 0xFF
    ]
    write6(0x01, data)

    data = [
        (mode & 0xFF00) >> 8,
        mode & 0xFF,
        (width & 0xFF00) >> 8,
        width & 0xFF,
        (height & 0xFF00) >> 8,
        height & 0xFF
    ]
    write6(0x02, data)

    write6(0x04, [1,0,0,0,0,0])
    write6(0x05, [1,0,0,0,0,0])

# %%
# find our device
# use this udev rule on permission problems
# SUBSYSTEM=="usb", ATTRS{idVendor}=="534d", ATTRS{idProduct}=="6021", MODE="0666"

def get_device():
    dev = usb.core.find(idVendor=0x534d, idProduct=0x6021)

    if dev is None:
        raise ValueError('Device not found')

    # The kernel automatically binds usbhid to the control endpoint, we need to unbind it to do control transfers.
    
    if dev.is_kernel_driver_active(0): #control
        dev.detach_kernel_driver(0)
    if dev.is_kernel_driver_active(3): #bulk
        dev.detach_kernel_driver(3)

    return dev

def get_endpoint(dev):
    cfg = dev[0]
    intf = cfg[(3,0)]
    ep = intf[0]
    return ep


# %%
dev = get_device()
power_on()

# %%

def read16(address):
    return read(address) + (read(address+1) << 8)

resolutions = []
for i in trange(256):
    global width, height, hz, mode, pixfmt
    width = 1000
    height = 1000
    hz = 60
    mode = i << 8
    pixfmt = 0x2200
    power_off()
    power_on()
    set_resolution()

    data = []
    data.append(read16(0xf384)) # hactive
    data.append(read16(0xf388)) # vactive
    data.append(read(0xf182)) # hz


    data.append(read16(0xf398)) # htotal
    data.append(read16(0xf39a)) # vtotal
    data.append(read16(0xf39c)) # hsync
    data.append(read16(0xf39e)) # vsync
    data.append(read16(0xf3a0)) # hbackporch + hsync
    data.append(read16(0xf3a2)) # htotal - hfrontporch
    data.append(read16(0xf3a4)) # vbackporch + vsync
    data.append(read16(0xf3a6)) # vtotal - vfrontporch
    resolutions.append(data)
    
# %%
for i in range(len(resolutions)):
    if i == 0 or (resolutions[i] != resolutions[i-1]):
        print(f"MS912X_MODE({resolutions[i][0]:4}, {resolutions[i][1]:4}, {resolutions[i][2]:2}, 0x{(i<<8):04x}, MS912X_PIXFMT_UYVY),")