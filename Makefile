KVERSION=$(shell uname -r)

obj-m += clear_refs_ranges.o

all:
	make -C /lib/modules/$(KVERSION)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(KVERSION)/build M=$(PWD) clean
	rm -f test
