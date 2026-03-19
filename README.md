This driver enables all the features benefitted to an official DS4 controller on Linux by the new hid-playstation driver, only it includes all the native mappings of the AstroC40 instead.  You can now see 3 devices through evdev for the Astro C40 TR, Motion Sensors, and and Touchpad.  Because it uses the official DS4 profile I expect it to work the same as the DS4 in games, but it depends how the game is calling things and checkings things too.  The touchpad works on the Linux desktop natively with multitouch support on the trackpad of the Astro C40 just like with a DS4 controller.    

I have not yet tested this with a dongle, I don't own one to test, but I have one being delivered.

Use DKMS to install kernel module driver for Astro C40 based on the official playstation controller driver named hid-playstation.  

This work was also constructed through research thanks to [ds4drv](https://github.com/chrippa/ds4drv), [evtest-qt](https://github.com/Grumbel/evtest-qt), [antimicrox](https://github.com/AntiMicroX/antimicrox), and [sc-controller](https://github.com/kozec/sc-controller).

This project was built with Cursor AI.

# hid-astroc40-dkms

Standalone HID kernel driver for the **Astro C40 TR** controller. Builds via DKMS without kernel source modifications.

## Features

- Gamepad (sticks, D-pad, face buttons, triggers, Share, Options, L3/R3, PS)
- Touchpad (2-finger support, native Linux desktop trackpad features & behaviour)
- Motion sensors (gyro + accelerometer)
- Rumble (force feedback)
- Battery power_supply (capacity/status; parsing from reports not yet implemented)

## Requirements

- Linux kernel headers for your running kernel
- DKMS

## Install

```bash
# From the package directory
sudo dkms add .
sudo dkms install hid-astroc40/1.0
```

Or install the package (e.g. `hid-astroc40-dkms`) if provided as a distro package.

## Manual build (without DKMS)

```bash
make
sudo insmod hid-astroc40.ko
```

## Supported devices

- USB VID 0x9886 (Astro Gaming), PID 0x0024
- USB VID 0x9886 (Astro Gaming), PID 0x0025

## Uninstall

```bash
sudo dkms remove hid-astroc40/1.0 --all
```
