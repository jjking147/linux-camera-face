
KERNELDIR := /home/fyj/imx6ull/imx6ull_linux/
 
CURRENR_PATH := $(shell pwd)
 
obj-m :=ov5640.o mx6s_capture.o
 
build: kernel_modules
 
kernel_modules:
	$(MAKE) -C $(KERNELDIR) M=$(CURRENR_PATH) modules
 
clean:
	$(MAKE) -C $(KERNELDIR) M=$(CURRENR_PATH) clean
