## Why does this fork exist?

If you're anything like me, you thought it would be fun to use your Moga Pro controller to play older DirectInput games. My specific use case was Final Fantasy 8's PC port, but you've probably got one of your own in mind. That's cool, I'm not here to judge. When you start getting into DInput, the Moga (or any modern controller) really has way more axes and buttons than most games were expecting you to have back in the day. I, to my frustration, discovered that Final Fantasy 8 only supports up to 10 buttons, and MogaSerial maps L2/R2 as buttons 11 and 12 when you set them to report as buttons. That's no good.

Older games also largely didn't expect that you'd have both a joystick and an analog stick on your gamepad; popular controllers like the Gravis Game Pad and Gravis Game Pad Pro all reported their directional pad as the traditional PC joystick's...well, joystick input. Final Fantasy 8 is no exception, regrettably.

Does vJoy have a way to remap button numbers from one to another?

No, really, that's a legitimate question. I looked into it for a while and I couldn't find any easy way to do it. There are other pieces of sofware that will install yet another virtual input device, and map inputs on your vJoy device to inputs on the new device, but...seriously? I'm already two pieces of software into trying to use this controller on Windows, and I just want to take some child soldiers on an adventure to stop an evil sorceress from compressing time.

Naturally, my solution was to modify MogaSerial to just...do the things I wanted it to do. Namely, these modifications, when MogaSerial is connected in vJoy mode, do two things:

1) Swap the button numbers of L2/R2 with the button numbers of L3/R3. L3/R3 now report at 11 and 12, L2/R2 now report as 9 and 10.
2) Report the directional pad directions as extreme inputs on the left analog stick axes. The analog stick still works, but if you press the directional pad at the same time, the directional pad value will override it.

This also adds two checkbox toggles to the UI, one for each of these hacks, so you can turn them on or off if you just want to use MogaSerial the way it was before.

These modifications do nothing in XInput mode.

Full disclosure: I have literally never done MFC programming before, so if I've committed a horrible MFC faux pas or two in the way that I've done anything...well, that's why. It should come as no surprise that this modification comes with absolutely no warranty, though...I guess drop me a line if you have any questions? My initial thought was to add a separate dialog that would let you swap anny control mapping you wanted to any DirectInput button or axis, but, again, I don't do MFC. If anyone feels like doing it, it should be fairly straightforward.

The solution has also been, for better or worse, updated to VS2019. I like constexpr and ternaries, leave me alone.

## Moga Serial to Windows Interface

The Moga line of Android controllers are neat pieces of kit, with one glaring issue.  They don't work right in Windows!  They could easily be a great all-in-one wireless controller, but no native driver meant being forced to use generic HID mode, and unfortunately this comes with a whole host of problems.

Connection issues aside, the L2/R2 triggers are nonfunctional by default.  The Moga identifies the triggers via the HID codes `AXIS_GAS` and `AXIS_BRAKE`, which Windows DirectInput doesn't recognize and therefore ignores, despite the controller actually reporting trigger values.  Fortunately there are alternatives!

MogaSerial is a solution that connects to the Moga gamepad directly via its mode A serial interface.  It can then feed controller data into either the vJoy driver for full DirectInput support, or into the SCP driver for native XInput support.  With this, the Moga can finally serve as a fully-functional wireless controller across mobile platforms, laptops, and desktops alike.

![MogaSerial](http://i63.tinypic.com/30b2rz6.png)

-----
### Download

The latest build of MogaSerial is 1.5.1, released April 26, 2016.  
![>](http://i64.tinypic.com/voad5u.png) [MogaSerial-v151.zip](https://github.com/Zel-os/MogaSerial/releases/download/v1.5.1/MogaSerial-v151.zip) - x86 for Windows 7, 8, and 10 


-----
### Setup

The Moga controller doesn't need to be paired with Windows, but if so it will appear on the Bluetooth device list by default.  Pin is `1234`, if needed.  

One (or both) of the interface drivers need to be installed:

##### XInput - SCP virtual bus driver
For modern games and Steam's Big Picture mode, the SCP driver emulates an Xbox 360 controller and provides full native XInput support.

This driver is included in the MogaSerial download.  Run ScpDriver.exe and click `Install`.  If you'd prefer to build the driver yourself, it can be obtained from <http://github.com/nefarius/ScpServer>.  

>If you're running Windows 7, also download and install the official [Xbox 360 Controller driver](http://www.microsoft.com/hardware/en-us/d/xbox-360-controller-for-windows).  This is pre-installed on both Windows 8 and 10.

##### DirectInput - vJoy virtual device driver
If you want full trigger support in older DirectInput games, or want to use your Moga alongside other controllers with [x360ce](http://www.x360ce.com/), the vJoy driver is a better option.  

Download and install vJoy 2.1.6 (or later) from <http://vjoystick.sourceforge.net/>.  Then run the vJoyConf configuration tool and set a controller as follows:

 - Axes: `X` `Y` `Z` `Rx` `Ry` `Rz`
 - Buttons: `12`
 - POV Hat: `1 Continuous`

> If you will only use the triggers as axes, buttons can be 10.  
> If you will only use the triggers as buttons, Z and Rz can be omitted.  
> Button mode for triggers ought to be compatible with the older Moga Pocket.


-----
#### Usage

Make sure Bluetooth is enabled, and ensure the Moga is switched to **MODE A**.

Then just select your controller from the Bluetooth drop-down and click the Moga button in the lower-right.  After a couple seconds, it will connect and begin feeding data to the selected driver.  If your Moga is not in the list, click the orange refresh button to re-scan for local Bluetooth devices, and after a few seconds it ought to appear.

The Xbox Guide button is emulated in XInput mode with SCP.  Press `Start + Select` together to trigger it.
  
Trigger mode is for vJoy only.  This determines how Windows will see `L2` and `R2`, either as the `Z` and `Rz` axis, as a combined `Z` axis, or as buttons `11` and `12`.  When using the SCP driver with DirectInput games, only combined trigger mode is available.

If the Moga disconnects due to sleeping, being shut off, or a Bluetooth error, the program will reset the connection, wait a few seconds, and try to reconnect.  Click the Moga button again to stop the controller interface.


-----
#### Notes

- Some basic troubleshooting information is on the GitHub [project wiki](https://github.com/Zel-os/MogaSerial/wiki).

- The Moga responds to polling at approximately 100 updates per second.

- Curiously, there seems to be no way to get battery status through the serial interface.  It's reported as a byte code when in HID mode B, but not here.

- I only own a Moga Power Pro for testing, but MogaSerial ought to work with all Moga-brand Android controllers.  From what I can tell, the serial communication protocol hasn't changed between models.  Rebel and Ace support would require further work, as well as assistance from somebody who owns one.


------------------------
##### Changes

* 1.5.1
  * Debug message added to show input lag from MogaSerial's perspective.
* 1.5.0
  * Added support for the SCP driver to get native XInput functionality.
* 1.4.0
  * GUI window now minimizes to the system tray.
* 1.3.2
  * Tweaks to try and address the reported input lag problem.  
* 1.3.0
  * Program settings are now stored in the system registry under HKCU\Software\MogaSerial.
  * Added 'Combined Axes' option for the trigger mode.  This mimics how the XBox360 controller behaves under DirectInput.
* 1.2.0
  * Debug switch added to display raw controller output. 
  * First public release of the MFC GUI version of MogaSerial.

------------------------

Thanks to badfontkeming@gmail.com for first making a similar tool that works with vJoy and the Moga in HID mode B, and inspiring me to finish this project here.  Thanks as well to the ZeeMouse author for showing that a serial mode A connection ought to be straightforward.  
<http://ngemu.com/threads/moga-pro-power-triggers-not-detected.170401/>


Moga controllers are (c) PowerA and MogaAnywhere.com  
This application is (c) Jake Montgomery - jmont@sonic.net
