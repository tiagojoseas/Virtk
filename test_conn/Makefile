obj-m := iw.o

KDIR    := /lib/modules/6.1.0-39-amd64/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean