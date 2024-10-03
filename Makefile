obj-m += hcsr04_driver.o

all: module dt

module:
	make -C /lib/modules/$(shell uname -r)/build M=${PWD} modules
dt: hcsr04_overlay.dts
	dtc -@ -I dts -O dtb -o hcsr04_overlay.dtbo hcsr04_overlay.dts
clean:
	make -C /lib/modules/$(shell uname -r)/build M=${PWD} clean
	rm -rf hcsr04_overlay.dtbo
