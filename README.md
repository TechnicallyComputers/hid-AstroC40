This driver enables all the features benefitted to an official DS4 controller on Linux by the new hid-playstation driver, only it includes all the native mappings of the AstroC40 instead.  You can now see 3 devices through evdev for the Astro C40 TR, Motion Sensors, and and Touchpad.  Because it uses the official DS4 profile I expect it to work the same as the DS4 in games, but it depends how the game is calling things and checkings things too.  The touchpad works on the Linux desktop natively with multitouch support on the trackpad of the Astro C40 just like with a DS4 controller.    

I have not yet tested this with a dongle, I don't own one to test, but I have one being delivered.

Use DKMS to install kernel module driver for Astro C40 based on the official playstation controller driver named hid-playstation.  
  
```
cd hid-astroc40-dkmssudo dkms add -m hid-astroc40 -v 1.0sudo dkms build -m hid-astroc40 -v 1.0sudo dkms install -m hid-astroc40 -v 1.0  
```
Or in one step:  

```  
sudo dkms install -m hid-astroc40 -v 1.0  
```
Check  

```
dkms statuslsmod | grep hid_astroc40
```

This work was also constructed through research thanks to [ds4drv](https://github.com/chrippa/ds4drv), [evtest-qt](https://github.com/Grumbel/evtest-qt), [antimicrox](https://github.com/AntiMicroX/antimicrox), and [sc-controller](https://github.com/kozec/sc-controller).

This project was built with Cursor AI.
