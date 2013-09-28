/**	File: 		jz2440_mouse.c
  *	Created on: 	2013-09-28
  *	Author: 		pusuitxh
  *	Email:		pursuitxh@gmail.com
  */

#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/list.h>
#include <linux/kernel.h>
#include <linux/utsname.h>
#include <linux/module.h>
#include <linux/cdev.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include <linux/usb/ch9.h>
#include <linux/usb/gadget.h>
#include <linux/usb/composite.h>

#include "composite.c"
#include "usbstring.c"
#include "config.c"
#include "epautoconf.c"

#define DRIVER_DESC					"simulate jz2440 board into an usb mouse"
#define DRIVER_VERSION				"2013/09/28"

#define JZ_MOUSEG_VENDOR_NUM		0x0525
#define JZ_MOUSEG_PRODUCT_NUM		0x0001	

static struct usb_device_descriptor device_desc = {
	.bLength =		sizeof device_desc,
	.bDescriptorType =	USB_DT_DEVICE,

	.bcdUSB =		cpu_to_le16(0x0200),

	.bDeviceClass =		USB_CLASS_PER_INTERFACE,
	.bDeviceSubClass =	0,
	.bDeviceProtocol =	0,

	.idVendor =		cpu_to_le16(JZ_MOUSEG_VENDOR_NUM),
	.idProduct =		cpu_to_le16(JZ_MOUSEG_PRODUCT_NUM),
	
	.bNumConfigurations =	1,
};

/* string IDs are assigned dynamically */
#define STRING_MANUFACTURER_IDX		0
#define STRING_PRODUCT_IDX		1

static char manufacturer[50];

static struct usb_string strings_dev[] = {
	[STRING_MANUFACTURER_IDX].s = manufacturer,
	[STRING_PRODUCT_IDX].s = DRIVER_DESC,
	{  } /* end of list */
};

static struct usb_gadget_strings stringtab_dev = {
	.language	= 0x0409,	/* en-us */
	.strings	= strings_dev,
};

static struct usb_gadget_strings *dev_strings[] = {
	&stringtab_dev,
	NULL,
};

static int __init jz_mouse_bind(struct usb_composite_dev *cdev)
{
	printk(KERN_INFO "Enter %s function...\n", __FUNCTION__);

	return 0;
}

static int __exit jz_mouse_unbind(struct usb_composite_dev *cdev)
{
	return 0;
}


static int __init jz_mouseg_plat_driver_probe(struct platform_device *pdev)
{
	return 0;
}

static int __devexit jz_mouseg_plat_driver_remove(struct platform_device *pdev)
{
	return 0;
}


/* usb descriptor for a mouse */
static char usb_mouse_descriptor[52] = {
	0x05, 0x01,// USAGE_PAGE (Generic Desktop) 
	0x09, 0x02,// USAGE (Mouse) 
	0xa1, 0x01,// COLLECTION (Application)
	0x09, 0x01,//USAGE (Pointer) 
	0xa1, 0x00,//COLLECTION (Physical) 
	0x05, 0x09,//USAGE_PAGE (Button) 
	0x19, 0x01,//USAGE_MINIMUM (Button 1) 
	0x29, 0x03,//USAGE_MAXIMUM (Button 3) 
	0x15, 0x00,//LOGICAL_MINIMUM (0) 
	0x25, 0x01,//LOGICAL_MAXIMUM (1) 
	0x95, 0x03,//REPORT_COUNT (3) 
	0x75, 0x01,//REPORT_SIZE (1) 
	0x81, 0x02,//INPUT (DataVarAbs) 
	0x95, 0x01,//REPORT_COUNT (1) 
	0x75, 0x05,//REPORT_SIZE (5) 
	0x81, 0x03,//INPUT (CnstVarAbs) 
	0x05, 0x01,//USAGE_PAGE (Generic Desktop) 
	0x09, 0x30,//USAGE (X) 
	0x09, 0x31,//USAGE (Y) 
	0x09, 0x38,//USAGE (Wheel) 
	0x15, 0x81,//LOGICAL_MINIMUM (-127) 
	0x25, 0x7f,//LOGICAL_MAXIMUM (127) 
	0x75, 0x08,//REPORT_SIZE (8)  
	0x95, 0x03,//REPORT_COUNT (3)  
	0x81, 0x06,//INPUT (DataVarRel) 
	0xc0,//END_COLLECTION 
	0xc0// END_COLLECTION 
};


static struct platform_device jz_mouse = {
    .name = "jz_mouseg",
    .id            = 0,
    .num_resources = 0,
    .resource    = 0,
    .dev.platform_data = &usb_mouse_descriptor,
};

static struct usb_composite_driver jz_mouseg_driver = {
	.name		= "g_jz_mouse",
	.dev		= &device_desc,
	.strings	= dev_strings,
	.max_speed	= USB_SPEED_HIGH,
	.unbind		= __exit_p(jz_mouse_unbind),
};

static struct platform_driver jz_mouseg_plat_driver = {
	.remove		= __devexit_p(jz_mouseg_plat_driver_remove),
	.driver		= {
		.owner	= THIS_MODULE,
		.name	= "jz_mouseg",
	},
};

static int __init jz_mouseg_init(void)
{
	int status;

	status = platform_device_register(&jz_mouse);
    if (status < 0){
        platform_device_unregister(&jz_mouse);
        return status;
    }
	
	status = platform_driver_probe(&jz_mouseg_plat_driver,
				jz_mouseg_plat_driver_probe);
	if (status < 0)
		return status;

	status = usb_composite_probe(&jz_mouseg_driver, jz_mouse_bind);
	if (status < 0)
		platform_driver_unregister(&jz_mouseg_plat_driver);

	printk(KERN_INFO "%s: init ok!\n", __FUNCTION__);
	return status;
}
module_init(jz_mouseg_init);

static void __exit jz_mouseg_cleanup(void)
{
	platform_device_unregister(&jz_mouse);
	platform_driver_unregister(&jz_mouseg_plat_driver);
	usb_composite_unregister(&jz_mouseg_driver);
}
module_exit(jz_mouseg_cleanup);

MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_AUTHOR("pursuitxh");
MODULE_LICENSE("GPL");
