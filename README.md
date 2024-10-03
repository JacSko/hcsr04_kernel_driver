# HC-SR04 Linux Kernel Driver

## Overview

This project is a Linux kernel driver designed for interfacing with HC-SR04 ultrasonic distance sensors. The driver supports multiple devices, allowing users to read distance measurements from multiple HC-SR04 sensors connected to the system.
Tested on RaspberryPi 3B with kernel 5.10.103.

## Features

- Support for multiple HC-SR04 sensors (configuring via device tree overlay)
- Read distance in millimeters
- Kernel-level integration for efficient data processing
- Simple API for userspace applications

## Configuration of device tree overlay
You can define up to 10 HC-SR04 devices in device tree overlay file (hcsr04_overlay.dts). Every device have to contain below properties, otherwise driver startup will be aborted on 'insmod' stage
```bash
my_device {
  compatible = "jskowronek,hcsr04";
	status = "okay";
	label = "sensor1";
	echo-gpio = <&gpio 21 0>;
	trigger-gpio = <&gpio 20 0>;
};
```
## Building
1. Install linux kernel headers for flawless compilation

```bash
$ sudo apt update
$ sudo apt upgrade
$ sudo apt install raspberrypi-kernel-headers
```

2. Make sure You have build utilities installed (make, gcc, etc.). Usually they are embedded into default RaspberryOS image.
3. Adjust device tree overlay according to your system setup (see next chapters for more details)
4. Build kernel module and device tree overlay by simply running make command.

```bash
$ make
```

## Installing
1. Load device tree overlay (*.dtbo)
```bash
$ sudo dtoverlay hcsr04_overlay.dtbo
```
2. Insert kernel module
```bash
$ sudo insmod hcsr04_driver.ko
```
2.1 Default timeout for device response is set to 100[ms]. You can overwrite this kernel parameter before build (in hcsr04_driver.c) or at loading time by using:
```bash
$ sudo insmod hcsr04_driver.ko DEVICE_READ_TIMEOUT_MS=<time in [ms]>
```
3. Verify if module was loaded correctly by reviewing dmesg output. If You see below logs, driver is up and running.
```bash
[ 8472.498327] [hcsr04_probe:0] Allocating device, name sensor1
[ 8472.498966] [hcsr04_probe:1] Allocating device, name sensor2
```
Alternatively, You can also check if driver is listed in kernel modules list:
```bash
$ sudo lsmod | grep hc
hcsr04_driver          16384  0
```
4. You can remove module from kernel at any time by typing:
```bash
$ sudo rmmod hcsr04_driver.ko
```
## Usage
Sensor readout is possible using the devices created by driver at /dev/ location
```bash
$ sudo ls -la /dev/ | grep HCSR04_*
crw-------   1 root root    239,   0 Oct  3 18:50 HCSR04_Driver0
crw-------   1 root root    239,   1 Oct  3 18:50 HCSR04_Driver1
```
## Testing
To verify driver's operation and hardware connections, there is a simple test application written in C++ that performs data readout on defined device.
1. Build application
```bash
$ g++ -o test_app test_app.cpp
```
2. Run application
```bash
$ sudo ./test_app /dev/HCSR04_Driver0
Trying to read device /dev/HCSR04_Driver0
distance: 64[mm]
```

## Hardware connection (basing on RaspberryPi)
HC-SR04 device can be connected to any GPIO-capable pin from pin header, but it has to be noted, that ECHO pin is operating on 5V level (according to supply voltage provided to sensor's Vcc).
In that case, before connecting ECHO pin simple voltage divider must be connected to fit input voltage to RaspberryPi GPIO's.
The easiest way is to use 2 resistors connected as below:

![image](https://github.com/user-attachments/assets/18fa0895-fd03-4a4e-8c79-0c318da8f182)

