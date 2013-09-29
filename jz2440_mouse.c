/**
 * File:       jz_mouse.c
 * Created on: 2013-09-28
 * Author:     pursuitxh
 * Email:      pursuitxh@gmail.com
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
#include <mach/regs-gpio.h>
#include <plat/gpio-fns.h>

#include <linux/usb/ch9.h>
#include <linux/usb/gadget.h>
#include <linux/usb/composite.h>
#include <linux/hid.h>

#include "composite.c"
#include "usbstring.c"
#include "config.c"
#include "epautoconf.c"

#define DRIVER_DESC	"simulate jz2440 board into an usb mouse"
#define DRIVER_VERSION	"2013/09/28"

#define JZ_MOUSEG_VENDOR_NUM	0x0525
#define JZ_MOUSEG_PRODUCT_NUM	0x0001

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

static struct usb_configuration config_driver = {
	.label			= "JZ_Mouse Gadget",
	.bConfigurationValue	= 1,
	/* .iConfiguration = DYNAMIC */
	.bmAttributes		= USB_CONFIG_ATT_SELFPOWER,
};

#define CT_FUNC_HID_IDX	0

static struct usb_string ct_func_string_defs[] = {
	[CT_FUNC_HID_IDX].s	= "HID Interface",
	{},			/* end of list */
};

static struct usb_gadget_strings ct_func_string_table = {
	.language	= 0x0409,	/* en-US */
	.strings	= ct_func_string_defs,
};

static struct usb_gadget_strings *ct_func_strings[] = {
	&ct_func_string_table,
	NULL,
};

static struct usb_interface_descriptor jzmouseg_interface_desc = {
	.bLength		= sizeof jzmouseg_interface_desc,
	.bDescriptorType	= USB_DT_INTERFACE,
	/* .bInterfaceNumber	= DYNAMIC */
	.bAlternateSetting	= 0,
	.bNumEndpoints		= 1,
	.bInterfaceClass	= USB_CLASS_HID,
	/* .bInterfaceSubClass	= DYNAMIC */
	/* .bInterfaceProtocol	= DYNAMIC */
	/* .iInterface		= DYNAMIC */
};
static struct hid_descriptor jzmouseg_desc = {
	.bLength			= sizeof jzmouseg_desc,
	.bDescriptorType		= HID_DT_HID,
	.bcdHID				= 0x0101,
	.bCountryCode			= 0x00,
	.bNumDescriptors		= 0x1,
	/*.desc[0].bDescriptorType	= DYNAMIC */
	/*.desc[0].wDescriptorLenght	= DYNAMIC */
};

/* High-Speed Support */
static struct usb_endpoint_descriptor jzmouseg_hs_in_ep_desc = {
	.bLength		= USB_DT_ENDPOINT_SIZE,
	.bDescriptorType	= USB_DT_ENDPOINT,
	.bEndpointAddress	= USB_DIR_IN,
	.bmAttributes		= USB_ENDPOINT_XFER_INT,
	/*.wMaxPacketSize	= DYNAMIC */
	.bInterval		= 4, /* FIXME: Add this field in the
				      * HID gadget configuration?
				      * (struct hidg_func_descriptor)
				      */
};

static struct usb_descriptor_header *jzmouseg_hs_descriptors[] = {
	(struct usb_descriptor_header *)&jzmouseg_interface_desc,
	(struct usb_descriptor_header *)&jzmouseg_desc,
	(struct usb_descriptor_header *)&jzmouseg_hs_in_ep_desc,
	NULL,
};

/* Full-Speed Support */
static struct usb_endpoint_descriptor jzmouseg_fs_in_ep_desc = {
	.bLength		= USB_DT_ENDPOINT_SIZE,
	.bDescriptorType	= USB_DT_ENDPOINT,
	.bEndpointAddress	= USB_DIR_IN,
	.bmAttributes		= USB_ENDPOINT_XFER_INT,
	/*.wMaxPacketSize	= DYNAMIC */
	.bInterval		= 10, /* FIXME: Add this field in the
				       * HID gadget configuration?
				       * (struct hidg_func_descriptor)
				       */
};

static struct usb_descriptor_header *jzmouseg_fs_descriptors[] = {
	(struct usb_descriptor_header *)&jzmouseg_interface_desc,
	(struct usb_descriptor_header *)&jzmouseg_desc,
	(struct usb_descriptor_header *)&jzmouseg_fs_in_ep_desc,
	NULL,
};

struct jzmouseg_func_descriptor {
	unsigned char		subclass;
	unsigned char		protocol;
	unsigned short		report_length;
	unsigned short		report_desc_length;
	unsigned char		report_desc[];
};

struct f_jzmouseg {
	/* configuration */
	unsigned char			bInterfaceSubClass;
	unsigned char			bInterfaceProtocol;
	unsigned short			report_desc_length;
	char				*report_desc;
	unsigned short			report_length;

	/* recv report */
	char				*set_report_buff;
	unsigned short			set_report_length;
	spinlock_t			spinlock;
	wait_queue_head_t		read_queue;

	/* send report */
	struct mutex			lock;
	bool				write_pending;
	wait_queue_head_t		write_queue;
	struct usb_request		*req;

	struct cdev			cdev;
	struct usb_function		func;
	struct usb_ep			*in_ep;
};

struct jzmouseg_func_descriptor *fdesc = NULL;
static int major = 0;
static int minor = 0;
static struct class *jzmouseg_class;

static inline struct f_jzmouseg *func_to_jzmouseg(struct usb_function *f)
{
	return container_of(f, struct f_jzmouseg, func);
}


const struct file_operations f_jzmouseg_fops = {
#if 0
	.owner		= THIS_MODULE,
	.open		= f_jzmouseg_open,
	.release	= f_jzmouseg_release,
	.write		= f_jzmouseg_write,
	.read		= f_jzmouseg_read,
	.poll		= f_jzmouseg_poll,
	.llseek		= noop_llseek,
#endif
};

static int __init g_jz_mouse_setup(struct usb_gadget *g, int count)
{
	int status;
	dev_t dev;

	jzmouseg_class = class_create(THIS_MODULE, "jzmouseg");

	status = alloc_chrdev_region(&dev, 0, count, "jzmouseg");
	if (!status) {
		major = MAJOR(dev);
		minor = count;
	}

	return status;
}

static int __init jzmouseg_bind(struct usb_configuration *c, struct usb_function *f)
{
	struct usb_ep		*ep;
	struct f_jzmouseg	*jzmouseg = func_to_jzmouseg(f);
	int			status;
	dev_t			dev;

	printk(KERN_INFO "Enter %s function...\n", __func__);

	/* allocate instance-specific interface IDs, and patch descriptors */
	status = usb_interface_id(c, f);
	if (status < 0)
		goto fail;
	jzmouseg_interface_desc.bInterfaceNumber = status;


	/* allocate instance-specific endpoints */
	status = -ENODEV;
	ep = usb_ep_autoconfig(c->cdev->gadget, &jzmouseg_fs_in_ep_desc);
	if (!ep)
		goto fail;
	ep->driver_data = c->cdev;	/* claim */
	jzmouseg->in_ep = ep;

	/* preallocate request and buffer */
	status = -ENOMEM;
	jzmouseg->req = usb_ep_alloc_request(jzmouseg->in_ep, GFP_KERNEL);
	if (!jzmouseg->req)
		goto fail;


	jzmouseg->req->buf = kmalloc(jzmouseg->report_length, GFP_KERNEL);
	if (!jzmouseg->req->buf)
		goto fail;

	/* set descriptor dynamic values */
	jzmouseg_interface_desc.bInterfaceSubClass = jzmouseg->bInterfaceSubClass;
	jzmouseg_interface_desc.bInterfaceProtocol = jzmouseg->bInterfaceProtocol;
	jzmouseg_hs_in_ep_desc.wMaxPacketSize = cpu_to_le16(jzmouseg->report_length);
	jzmouseg_fs_in_ep_desc.wMaxPacketSize = cpu_to_le16(jzmouseg->report_length);
	jzmouseg_desc.desc[0].bDescriptorType = HID_DT_REPORT;
	jzmouseg_desc.desc[0].wDescriptorLength =
		cpu_to_le16(jzmouseg->report_desc_length);

	jzmouseg->set_report_buff = NULL;

	/* copy descriptors */
	f->descriptors = usb_copy_descriptors(jzmouseg_fs_descriptors);
	if (!f->descriptors)
		goto fail;

	if (gadget_is_dualspeed(c->cdev->gadget)) {
		jzmouseg_hs_in_ep_desc.bEndpointAddress =
				jzmouseg_fs_in_ep_desc.bEndpointAddress;
		f->hs_descriptors = usb_copy_descriptors(jzmouseg_hs_descriptors);
		if (!f->hs_descriptors)
			goto fail;
	}

	mutex_init(&jzmouseg->lock);
	spin_lock_init(&jzmouseg->spinlock);
	init_waitqueue_head(&jzmouseg->write_queue);
	init_waitqueue_head(&jzmouseg->read_queue);

	/* create char device */
	cdev_init(&jzmouseg->cdev, &f_jzmouseg_fops);
	dev = MKDEV(major, minor);
	status = cdev_add(&jzmouseg->cdev, dev, 1);
	if (status)
		goto fail;

	device_create(jzmouseg_class, NULL, dev, NULL, "%s%d", "jzmouseg", 0);

	return 0;

fail:
	ERROR(f->config->cdev, "hidg_bind FAILED\n");
	if (jzmouseg->req != NULL) {
		kfree(jzmouseg->req->buf);
		if (jzmouseg->in_ep != NULL)
			usb_ep_free_request(jzmouseg->in_ep, jzmouseg->req);
	}

	usb_free_descriptors(f->hs_descriptors);
	usb_free_descriptors(f->descriptors);

	return status;
}

static void jzmouseg_unbind(struct usb_configuration *c, struct usb_function *f)
{

}

static int jzmouseg_set_alt(struct usb_function *f, unsigned intf, unsigned alt)
{

	return 0;
}

static void jzmouseg_disable(struct usb_function *f)
{

}

static int jzmouseg_setup(struct usb_function *f,
		const struct usb_ctrlrequest *ctrl)
{
	struct f_jzmouseg		*jzmouseg = func_to_jzmouseg(f);
	struct usb_composite_dev	*cdev = f->config->cdev;
	struct usb_request		*req  = cdev->req;
	int status = 0;
	__u16 value, length;

	value	= __le16_to_cpu(ctrl->wValue);
	length	= __le16_to_cpu(ctrl->wLength);

	INFO(cdev, "hid_setup crtl_request : bRequestType:0x%x bRequest:0x%x "
		"Value:0x%x\n", ctrl->bRequestType, ctrl->bRequest, value);

	switch ((ctrl->bRequestType << 8) | ctrl->bRequest) {
	case ((USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE) << 8
		  | HID_REQ_GET_REPORT):
		INFO(cdev, "get_report\n");

		/* send an empty report */
		length = min_t(unsigned, length, jzmouseg->report_length);
		memset(req->buf, 0x0, length);

		goto respond;
		break;

	case ((USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE) << 8
		  | HID_REQ_GET_PROTOCOL):
		INFO(cdev, "get_protocol\n");
		goto stall;
		break;

	case ((USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE) << 8
		  | HID_REQ_SET_REPORT):
		INFO(cdev, "set_report | wLenght=%d\n", ctrl->wLength);
		req->context  = jzmouseg;
		//req->complete = jzmouseg_set_report_complete;
		goto respond;
		break;

	case ((USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE) << 8
		  | HID_REQ_SET_PROTOCOL):
		INFO(cdev, "set_protocol\n");
		goto stall;
		break;

	case ((USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_INTERFACE) << 8
		  | USB_REQ_GET_DESCRIPTOR):
		switch (value >> 8) {
		case HID_DT_HID:
			INFO(cdev, "USB_REQ_GET_DESCRIPTOR: HID\n");
			length = min_t(unsigned short, length,
						   jzmouseg_desc.bLength);
			memcpy(req->buf, &jzmouseg_desc, length);
			goto respond;
			break;
		case HID_DT_REPORT:
			INFO(cdev, "USB_REQ_GET_DESCRIPTOR: REPORT\n");
			length = min_t(unsigned short, length,
					jzmouseg->report_desc_length);
			memcpy(req->buf, jzmouseg->report_desc, length);
			goto respond;
			break;

		default:
			INFO(cdev, "Unknown decriptor request 0x%x\n",
				 value >> 8);
			goto stall;
			break;
		}
		break;

	default:
		INFO(cdev, "Unknown request 0x%x\n",
			 ctrl->bRequest);
		goto stall;
		break;
	}

stall:
	return -EOPNOTSUPP;

respond:
	req->zero = 0;
	req->length = length;
	status = usb_ep_queue(cdev->gadget->ep0, req, GFP_ATOMIC);
	if (status < 0)
		ERROR(cdev, "usb_ep_queue error on ep0 %d\n", value);
	return status;
}

static int __init jz_mouse_bind_config(struct usb_configuration *c)
{
	struct f_jzmouseg *jzmouseg;
	int status;

	printk(KERN_INFO "Enter %s function...\n", __func__);

	/* allocate and initialize one new instance */
	jzmouseg = kzalloc(sizeof *jzmouseg, GFP_KERNEL);
	if (!jzmouseg)
		return -ENOMEM;

	jzmouseg->bInterfaceSubClass = fdesc->subclass;
	jzmouseg->bInterfaceProtocol = fdesc->protocol;
	jzmouseg->report_length = fdesc->report_length;
	jzmouseg->report_desc_length = fdesc->report_desc_length;
	jzmouseg->report_desc = kmemdup(fdesc->report_desc,
				    fdesc->report_desc_length,
				    GFP_KERNEL);
	if (!jzmouseg->report_desc) {
		kfree(jzmouseg);
		return -ENOMEM;
	}

	jzmouseg->func.name    = "jzmouse";
	jzmouseg->func.strings = ct_func_strings;
	jzmouseg->func.bind    = jzmouseg_bind;
	jzmouseg->func.unbind  = jzmouseg_unbind;
	jzmouseg->func.set_alt = jzmouseg_set_alt;
	jzmouseg->func.disable = jzmouseg_disable;
	jzmouseg->func.setup   = jzmouseg_setup;

	status = usb_add_function(c, &jzmouseg->func);
	if (status)
		kfree(jzmouseg);

	return status;
}

static int __init jz_mouse_bind(struct usb_composite_dev *cdev)
{
	printk(KERN_INFO "Enter %s function...\n", __func__);

	struct usb_gadget *gadget = cdev->gadget;
	int status, gcnum;

	/* set up jz_mouse */
	status = g_jz_mouse_setup(cdev->gadget, 0);
	if (status < 0)
		return status;

	gcnum = usb_gadget_controller_number(gadget);
	if (gcnum >= 0)
		device_desc.bcdDevice = cpu_to_le16(0x0300 | gcnum);
	else
		device_desc.bcdDevice = cpu_to_le16(0x0300 | 0x0099);


	/* device descriptor strings: manufacturer, product */
	snprintf(manufacturer, sizeof manufacturer, "%s %s with %s",
		init_utsname()->sysname, init_utsname()->release,
		gadget->name);
	status = usb_string_id(cdev);
	if (status < 0)
		return status;
	strings_dev[STRING_MANUFACTURER_IDX].id = status;
	device_desc.iManufacturer = status;

	status = usb_string_id(cdev);
	if (status < 0)
		return status;
	strings_dev[STRING_PRODUCT_IDX].id = status;
	device_desc.iProduct = status;

	/* register our configuration */
	status = usb_add_config(cdev, &config_driver, jz_mouse_bind_config);
	if (status < 0)
		return status;

	dev_info(&gadget->dev, DRIVER_DESC ", version: " DRIVER_VERSION "\n");

	return 0;

}

static int __exit jz_mouse_unbind(struct usb_composite_dev *cdev)
{
	return 0;
}


static int __init jz_mouseg_plat_driver_probe(struct platform_device *pdev)
{
	fdesc = pdev->dev.platform_data;

	return 0;
}

static int __devexit jz_mouseg_plat_driver_remove(struct platform_device *pdev)
{
	return 0;
}


/* usb descriptor for a mouse */
static struct jzmouseg_func_descriptor jzmouse_data = {
	.subclass 		= 0, /* No subclass */
	.protocol 		= 2, /* mouse */
	.report_length 		= 8,
	.report_desc_length	= 52,
	.report_desc 		= {
		0x05, 0x01, /* USAGE_PAGE (Generic Desktop) */
		0x09, 0x02, /* USAGE (Mouse) */
		0xa1, 0x01, /* COLLECTION (Application) */
		0x09, 0x01, /* USAGE (Pointer) */
		0xa1, 0x00, /* COLLECTION (Physical) */
		0x05, 0x09, /* USAGE_PAGE (Button) */
		0x19, 0x01, /* USAGE_MINIMUM (Button 1) */
		0x29, 0x03, /* USAGE_MAXIMUM (Button 3) */
		0x15, 0x00, /* LOGICAL_MINIMUM (0) */
		0x25, 0x01, /* LOGICAL_MAXIMUM (1) */
		0x95, 0x03, /* REPORT_COUNT (3) */
		0x75, 0x01, /* REPORT_SIZE (1) */
		0x81, 0x02, /* INPUT (DataVarAbs) */
		0x95, 0x01, /* REPORT_COUNT (1) */
		0x75, 0x05, /* REPORT_SIZE (5) */
		0x81, 0x03, /* INPUT (CnstVarAbs) */
		0x05, 0x01, /* USAGE_PAGE (Generic Desktop) */
		0x09, 0x30, /* USAGE (X) */
		0x09, 0x31, /* USAGE (Y) */
		0x09, 0x38, /* USAGE (Wheel) */
		0x15, 0x81, /* LOGICAL_MINIMUM (-127) */
		0x25, 0x7f, /* LOGICAL_MAXIMUM (127) */
		0x75, 0x08, /* REPORT_SIZE (8) */
		0x95, 0x03, /* REPORT_COUNT (3) */
		0x81, 0x06, /* INPUT (DataVarRel) */
		0xc0, 	    /* END_COLLECTION */
		0xc0        /* END_COLLECTION */
	}
};

static struct platform_device jz_mouse = {
	.name 			= "jz_mouseg",
	.id			= 0,
	.num_resources		= 0,
	.resource    		= 0,
	.dev.platform_data 	= &jzmouse_data,
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

	s3c2410_gpio_cfgpin(S3C2410_GPC(5),S3C2410_GPIO_OUTPUT);
	s3c2410_gpio_setpin(S3C2410_GPC(5),0);

	status = platform_device_register(&jz_mouse);
	if (status < 0){
		platform_device_unregister(&jz_mouse);
		return status;
	}

	status = platform_driver_probe(&jz_mouseg_plat_driver,
				jz_mouseg_plat_driver_probe);
	if (status < 0) {
		platform_device_unregister(&jz_mouse);
		return status;
	}

	status = usb_composite_probe(&jz_mouseg_driver, jz_mouse_bind);
	if (status < 0) {
		platform_device_unregister(&jz_mouse);
		platform_driver_unregister(&jz_mouseg_plat_driver);
	}
	
	/* Enable usb device function */
	s3c2410_gpio_setpin(S3C2410_GPC(5),1);

	printk(KERN_INFO "%s: success!\n", __FUNCTION__);

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
