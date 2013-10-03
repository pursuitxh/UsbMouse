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
#include <asm/irq.h>
#include <linux/interrupt.h>
#include <asm/io.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/irq.h>
#include <mach/regs-gpio.h>
#include <plat/gpio-fns.h>

#include <linux/usb/ch9.h>
#include <linux/usb/gadget.h>
#include <linux/usb/composite.h>
#include <linux/usb/g_hid.h>
#include <linux/hid.h>

#include "composite.c"
#include "usbstring.c"
#include "config.c"
#include "epautoconf.c"

#define DRIVER_DESC	"simulate jz2440 board into an usb mouse"
#define DRIVER_VERSION	"2013/09/28"

#define JZ_MOUSEG_VENDOR_NUM	0x0525 /* XXX NetChip */
#define JZ_MOUSEG_PRODUCT_NUM	0x0001

/* JZ2440 Device descriptor */
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
	.bInterval		= 4,
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
	.bInterval		= 10,
};

static struct usb_descriptor_header *jzmouseg_fs_descriptors[] = {
	(struct usb_descriptor_header *)&jzmouseg_interface_desc,
	(struct usb_descriptor_header *)&jzmouseg_desc,
	(struct usb_descriptor_header *)&jzmouseg_fs_in_ep_desc,
	NULL,
};

struct f_jzmouseg {
	/* configuration */
	unsigned char			bInterfaceSubClass;
	unsigned char			bInterfaceProtocol;
	unsigned short			report_desc_length;
	char				*report_desc;
	unsigned short			report_length;

	struct usb_request		*req;

	struct cdev			cdev;
	struct usb_function		func;
	struct usb_ep			*in_ep;
};

struct hidg_func_descriptor	*fdesc;

#define JZMOUSEG_MAJOR	0
#define JZMOUSEG_DRIVER_NAME "jzmouseg"
static int jzmouseg_major = JZMOUSEG_MAJOR;
static struct class *jzmouseg_class;

static volatile unsigned long *gpfcon;
static volatile unsigned long *gpfdat;
static volatile unsigned long *gpgcon;
static volatile unsigned long *gpgdat;
static struct timer_list buttons_timer;
static struct fasync_struct *button_async;

struct pin_desc{
	unsigned int irq;
	unsigned int pin;
	unsigned int key_val;
	const char *name;
};

/* Key Down: 0x01, 0x02, 0x03, 0x04 */
/* Key Up:   0x81, 0x82, 0x83, 0x84 */
static unsigned char key_val;

struct pin_desc pins_desc[4] = {
	{IRQ_EINT0, S3C2410_GPF(0), 0x01, "S2"},
	{IRQ_EINT2, S3C2410_GPF(2), 0x02, "S3"},
	{IRQ_EINT11, S3C2410_GPG(3), 0x03, "S4"},
	{IRQ_EINT19, S3C2410_GPG(11), 0x04, "S5"},
};

static struct pin_desc *irq_pd;

static inline struct f_jzmouseg *func_to_jzmouseg(struct usb_function *f)
{
	return container_of(f, struct f_jzmouseg, func);
}

static void buttons_timer_function(unsigned long data)
{
	struct pin_desc * pindesc = irq_pd;
	unsigned int pinval;

	if (!pindesc)
		return;

	pinval = s3c2410_gpio_getpin(pindesc->pin);

	if (pinval)
		key_val = 0x80 | pindesc->key_val;
	else
		key_val = pindesc->key_val;

	kill_fasync (&button_async, SIGIO, POLL_IN);
}

static irqreturn_t buttons_irq(int irq, void *dev_id)
{
	irq_pd = (struct pin_desc *)dev_id;
	mod_timer(&buttons_timer, jiffies+HZ/100);
	return IRQ_RETVAL(IRQ_HANDLED);
}

static void f_jzmouseg_req_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct f_jzmouseg *jzmouseg = (struct f_jzmouseg *)ep->driver_data;

	if (req->status != 0) {
		ERROR(jzmouseg->func.config->cdev,
			"End Point Request ERROR: %d\n", req->status);
	}
}

static ssize_t f_jzmouseg_read(struct file *file, char __user *buffer,
			size_t count, loff_t *ptr)
{
	struct f_jzmouseg	*jzmouseg     = file->private_data;
	ssize_t status;
	unsigned char mouse_buf[4] = {0, 0, 0, 0};

	if (count != 1)
		return -EINVAL;



	if(key_val == 0x01) {
		mouse_buf[0] |= 0x01;
	} else if(key_val == 0x02) {
		mouse_buf[0] |= 0x02;
	} else if(key_val == 0x03) {
		mouse_buf[0] |= 0x04;
	}
	memcpy(jzmouseg->req->buf, &mouse_buf, 4);
	jzmouseg->req->status   = 0;
	jzmouseg->req->zero     = 0;
	jzmouseg->req->length   = 4;
	jzmouseg->req->complete = f_jzmouseg_req_complete;
	jzmouseg->req->context  = jzmouseg;

	status = usb_ep_queue(jzmouseg->in_ep, jzmouseg->req, GFP_ATOMIC);

	if (status < 0)
		ERROR(jzmouseg->func.config->cdev,
			"usb_ep_queue error on int endpoint %zd\n", status);

	status = copy_to_user(buffer, &key_val, 1);
	return status? -EFAULT : 0;
}

static int f_jzmouseg_fasync (int fd, struct file *filp, int on)
{
	pr_info("Enter %s function!\n", __func__);
	return fasync_helper (fd, filp, on, &button_async);
}

static int f_jzmouseg_release(struct inode *inode, struct file *fd)
{
	int i;
	fd->private_data = NULL;

	for (i = 0; i < 4; i++)
		free_irq(pins_desc[i].irq, &pins_desc[i]);

	return 0;
}

static int f_jzmouseg_open(struct inode *inode, struct file *fd)
{
	int retval;
	int i;

	struct f_jzmouseg *jzmouseg =
		container_of(inode->i_cdev, struct f_jzmouseg, cdev);

	fd->private_data = jzmouseg;

	for (i = 0; i < 4; i++)
		retval = request_irq(pins_desc[i].irq,  buttons_irq,
				(IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING),
				pins_desc[i].name, &pins_desc[i]);
	if (retval) {
		i--;
		for (; i>=0; i++)
			free_irq(pins_desc[i].irq, &pins_desc[i]);
	}

	return 0;
}

const struct file_operations f_jzmouseg_fops = {
	.owner		= THIS_MODULE,
	.open		= f_jzmouseg_open,
	.read		= f_jzmouseg_read,
	.fasync		= f_jzmouseg_fasync,
	.release	= f_jzmouseg_release,

};

static int __init gjzmouse_setup(struct f_jzmouseg *jzmouseg)
{
	int retval;
	dev_t devnum = MKDEV(JZMOUSEG_MAJOR, 0);

	if (jzmouseg_major) {
		retval = register_chrdev_region(devnum, 1, JZMOUSEG_DRIVER_NAME);
		pr_info("static jzmouseg major is: %d\n", jzmouseg_major);
	} else {
		retval = alloc_chrdev_region(&devnum, 0, 1, JZMOUSEG_DRIVER_NAME);
		jzmouseg_major = MAJOR(devnum);
		pr_info("automatic jzmouseg major is: %d\n", jzmouseg_major);
	}

	if (retval < 0) {
		pr_err("jzmouseg driver: can't get major number...\n");
		return retval;
	}

	jzmouseg_class = class_create(THIS_MODULE, "jz2440_hid");
	device_create(jzmouseg_class, NULL, devnum, NULL, JZMOUSEG_DRIVER_NAME);

	cdev_init(&jzmouseg->cdev, &f_jzmouseg_fops);
	jzmouseg->cdev.owner = THIS_MODULE;
	jzmouseg->cdev.ops = &f_jzmouseg_fops;

	retval = cdev_add(&jzmouseg->cdev, devnum, 1);
	if (retval)
		pr_err("Error %d adding jzmouseg\n", retval);

	init_timer(&buttons_timer);
	buttons_timer.function = buttons_timer_function;
	add_timer(&buttons_timer);

	gpfcon = (volatile unsigned long *)ioremap(0x56000050, 16);
	gpfdat = gpfcon + 1;

	gpgcon = (volatile unsigned long *)ioremap(0x56000060, 16);
	gpgdat = gpgcon + 1;

	return retval;
}

static int __init jzmouseg_bind(struct usb_configuration *c, struct usb_function *f)
{
	struct usb_ep		*ep;
	struct f_jzmouseg	*jzmouseg = func_to_jzmouseg(f);
	int			status;

	pr_info("Enter %s function!\n", __func__);

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

	/* setup jzmouse device */
	status = gjzmouse_setup(jzmouseg);
	if (status)
		goto fail;

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

static void gjzmouse_cleanup(void)
{
	device_destroy(jzmouseg_class, MKDEV(jzmouseg_major, 0));
	class_destroy(jzmouseg_class);
	unregister_chrdev_region(MKDEV(jzmouseg_major, 0), 1);
	iounmap(gpfcon);
	iounmap(gpgcon);

	pr_info("gjzmouse module removed successed!\n");
}

static void jzmouseg_unbind(struct usb_configuration *c, struct usb_function *f)
{
	struct f_jzmouseg *jzmouseg = func_to_jzmouseg(f);

	gjzmouse_cleanup();
	/* disable/free request and end point */
	usb_ep_disable(jzmouseg->in_ep);
	usb_ep_dequeue(jzmouseg->in_ep, jzmouseg->req);
	kfree(jzmouseg->req->buf);
	usb_ep_free_request(jzmouseg->in_ep, jzmouseg->req);

	/* free descriptors copies */
	usb_free_descriptors(f->hs_descriptors);
	usb_free_descriptors(f->descriptors);

	kfree(jzmouseg->report_desc);
	kfree(jzmouseg);
}

static int jzmouseg_set_alt(struct usb_function *f, unsigned intf, unsigned alt)
{
	struct usb_composite_dev *cdev = f->config->cdev;
	struct f_jzmouseg *jzmouseg = func_to_jzmouseg(f);
	int status = 0;

	INFO(cdev, "hidg_set_alt intf:%d alt:%d\n", intf, alt);

	if (jzmouseg->in_ep != NULL) {
		/* restart endpoint */
		if (jzmouseg->in_ep->driver_data != NULL)
			usb_ep_disable(jzmouseg->in_ep);

		status = config_ep_by_speed(f->config->cdev->gadget, f,
						jzmouseg->in_ep);
		if (status) {
			ERROR(cdev, "config_ep_by_speed FAILED!\n");
			goto fail;
		}
		status = usb_ep_enable(jzmouseg->in_ep);
		if (status < 0) {
			ERROR(cdev, "Enable endpoint FAILED!\n");
			goto fail;
		}
		jzmouseg->in_ep->driver_data = jzmouseg;
	}
fail:
	return status;
}

static void jzmouseg_disable(struct usb_function *f)
{
	struct f_jzmouseg *jzmouseg = func_to_jzmouseg(f);

	usb_ep_disable(jzmouseg->in_ep);
	jzmouseg->in_ep->driver_data = NULL;
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

	INFO(cdev, "jzmouse_setup crtl_request : bRequestType:0x%x bRequest:0x%x "
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

static int __init jzmouse_bind_config(struct usb_configuration *c)
{
	struct f_jzmouseg *jzmouseg;
	int status;

	pr_info("Enter %s function!\n", __func__);

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


static int __init jzmouse_bind(struct usb_composite_dev *cdev)
{
	struct usb_gadget *gadget = cdev->gadget;
	int status, gcnum;

	pr_info("Enter %s function...\n", __func__);

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
	status = usb_add_config(cdev, &config_driver, jzmouse_bind_config);
	if (status < 0)
		return status;

	dev_info(&gadget->dev, DRIVER_DESC ", version: " DRIVER_VERSION "\n");

	return 0;
}

static int __exit jzmouse_unbind(struct usb_composite_dev *cdev)
{
	return 0;
}

static int __init jzmouseg_plat_driver_probe(struct platform_device *pdev)
{
	 fdesc = pdev->dev.platform_data;

	/* Enable JZ2440 USB Device Controller */
	s3c2410_gpio_cfgpin(S3C2410_GPC(5),S3C2410_GPIO_OUTPUT);
	s3c2410_gpio_setpin(S3C2410_GPC(5),1);

	return 0;
}

static int __devexit jzmouseg_plat_driver_remove(struct platform_device *pdev)
{
	fdesc = NULL;

	/* Disable JZ2440 USB Device Controller */
	s3c2410_gpio_setpin(S3C2410_GPC(5),0);

	return 0;
}

static struct usb_composite_driver jzmouseg_driver = {
	.name		= "g_jzmouse",
	.dev		= &device_desc,
	.strings	= dev_strings,
	.max_speed	= USB_SPEED_HIGH,
	.unbind		= __exit_p(jzmouse_unbind),
};

static struct platform_driver jzmouseg_plat_driver = {
	.remove		= __devexit_p(jzmouseg_plat_driver_remove),
	.driver		= {
		.owner	= THIS_MODULE,
		.name	= "jz_mouseg",
	},
};

static int __init jzmouseg_init(void)
{
	int status;

	status = platform_driver_probe(&jzmouseg_plat_driver,
				jzmouseg_plat_driver_probe);
	if (status < 0)
		return status;

	status = usb_composite_probe(&jzmouseg_driver, jzmouse_bind);
	if (status < 0)
		platform_driver_unregister(&jzmouseg_plat_driver);

	pr_info("%s: success!\n", __func__);

	return status;
}
module_init(jzmouseg_init);

static void __exit jzmouseg_cleanup(void)
{
	platform_driver_unregister(&jzmouseg_plat_driver);
	usb_composite_unregister(&jzmouseg_driver);
}
module_exit(jzmouseg_cleanup);

MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_AUTHOR("pursuitxh");
MODULE_LICENSE("GPL");
