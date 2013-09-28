KERN_DIR = /home/pursuitxh/Workspace/project/linux-3.2.50

all:
	make -C $(KERN_DIR) M=`pwd` modules 

clean:
	make -C $(KERN_DIR) M=`pwd` modules clean
	rm -rf modules.order

obj-m	+= jz2440_mouse.o
