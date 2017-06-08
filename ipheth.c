/*
 * Copyright (C) 2017 Walker Wei<walker0411@163.com>. All rights reserved.
 * Copyright (C) 2009 Diego Giagio. All rights reserved.
 * Copyright (C) 2000-2005 by David Brownell
 * Copyright (C) 2003-2005 David Hollis <dhollis@davehollis.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Thanks to Diego Giagio for figuring out the programming details for
 * the Apple iPhone Ethernet driver.
 * This program is based on usbnet which is written by David Brownell.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/mii.h>
#include <linux/usb.h>
#include <linux/usb/cdc.h>
#include <linux/usb/usbnet.h>

#define USB_VENDOR_APPLE        0x05ac
#define USB_PRODUCT_IPHONE      0x1290
#define USB_PRODUCT_IPHONE_3G   0x1292
#define USB_PRODUCT_IPHONE_3GS  0x1294
#define USB_PRODUCT_IPHONE_4    0x1297
#define USB_PRODUCT_IPHONE_4_VZW 0x129c
#define USB_PRODUCT_IPHONE_4S   0x12a0
#define USB_PRODUCT_IPHONE_5   0x12a8
#define USB_PRODUCT_IPAD       0x129a
#define USB_PRODUCT_IPAD_MINI  0x12ab

#define IPHETH_USBINTF_CLASS    255
#define IPHETH_USBINTF_SUBCLASS 253
#define IPHETH_USBINTF_PROTO    1

#define IPHETH_INTFNUM          2
#define IPHETH_ALT_INTFNUM      1

#define IPHETH_BUF_SIZE         1516
#define IPHETH_IP_ALIGN         2      /* padding at front of URB */

#define IPHETH_CTRL_ENDP        0x00
#define IPHETH_CTRL_BUF_SIZE    0x40
#define IPHETH_CTRL_TIMEOUT     (5 * HZ)

#define IPHETH_CMD_GET_MACADDR   0x00
#define IPHETH_CMD_CARRIER_CHECK 0x45

#define IPHETH_CARRIER_CHECK_TIMEOUT round_jiffies_relative(1 * HZ)
#define IPHETH_CARRIER_ON       0x04

static struct ipheth_local local;

struct ipheth_local {
	struct usbnet *dev;
	struct mutex local_mutex;
	struct delayed_work carrier_work;
};

/* same as usbnet_netdev_ops but MTU change not allowed */
static const struct net_device_ops ipheth_netdev_ops = {
	.ndo_open		= usbnet_open,
	.ndo_stop		= usbnet_stop,
	.ndo_start_xmit		= usbnet_start_xmit,
	.ndo_tx_timeout		= usbnet_tx_timeout,
	.ndo_set_mac_address 	= eth_mac_addr,
	.ndo_validate_addr	= eth_validate_addr,
};

void ipheth_status(struct usbnet *dev, struct urb *urb)
{
	netdev_dbg(dev->net, "ipheth status urb, len %d stat %d\n",
			urb->actual_length, urb->status);
}
EXPORT_SYMBOL_GPL(ipheth_status);

static int ipheth_carrier_set(struct usbnet *dev)
{
	struct usb_device *udev = NULL;
	int retval = 0;
	unsigned char ctrl_buf[IPHETH_CTRL_BUF_SIZE] = {0};

	if (NULL == dev)
	{
		err("%s: struct usbnet *dev is NULL.", __func__);
		retval = -EINVAL;
		return retval;
	}

	udev = dev->udev;
	retval = usb_control_msg(udev,
			usb_rcvctrlpipe(udev, IPHETH_CTRL_ENDP),
			IPHETH_CMD_CARRIER_CHECK, /* request */
			0xc0, /* request type */
			0x00, /* value */
			0x02, /* index */
			(void *)ctrl_buf,
			IPHETH_CTRL_BUF_SIZE,
			IPHETH_CTRL_TIMEOUT);
	if (retval < 0) {
		err("%s: usb_control_msg: %d", __func__, retval);
	}
	else
	{
		if (ctrl_buf[0] == IPHETH_CARRIER_ON)
			netif_carrier_on(dev->net);
		else
			netif_carrier_off(dev->net);
	}

	return retval;
}

static void ipheth_carrier_check_work(struct work_struct *work)
{
	struct usbnet *dev = NULL;

	mutex_lock(&(local.local_mutex));
	dev = local.dev;
	ipheth_carrier_set(dev);
	schedule_delayed_work(&(local.carrier_work), IPHETH_CARRIER_CHECK_TIMEOUT);
	mutex_unlock(&(local.local_mutex));
}

static int ipheth_get_macaddr(struct usbnet *dev)
{
	struct usb_device *udev = dev->udev;
	struct net_device *net = dev->net;
	int retval;
	unsigned char ctrl_buf[IPHETH_CTRL_BUF_SIZE] = {0};

	retval = usb_control_msg(udev,
				 usb_rcvctrlpipe(udev, IPHETH_CTRL_ENDP),
				 IPHETH_CMD_GET_MACADDR, /* request */
				 0xc0, /* request type */
				 0x00, /* value */
				 0x02, /* index */
				 (void *)ctrl_buf,
				 IPHETH_CTRL_BUF_SIZE,
				 IPHETH_CTRL_TIMEOUT);
	if (retval < 0) {
		err("%s: usb_control_msg: %d", __func__, retval);
	} else if (retval < ETH_ALEN) {
		err("%s: usb_control_msg: short packet: %d bytes",
			__func__, retval);
		retval = -EINVAL;
	} else {
		memcpy(net->dev_addr, (const void *)ctrl_buf, ETH_ALEN);
		memcpy(net->perm_addr, (const void *)ctrl_buf, ETH_ALEN);
		retval = 0;
	}

	return retval;
}

static int ipheth_bind(struct usbnet *dev, struct usb_interface *intf)
{
	int retval = 0;//success
	int i = 0;
	struct usb_host_interface *hintf;
	struct usb_endpoint_descriptor *endp;
	u8 bulk_in = 0;
	u8 bulk_out = 0;

	/* Set up endpoints */
	hintf = usb_altnum_to_altsetting(intf, IPHETH_ALT_INTFNUM);
	if (hintf == NULL) {
		retval = -ENODEV;
		err("Unable to find alternate settings interface, retval=%d", retval);
		return retval;
	}

	for (i = 0; i < hintf->desc.bNumEndpoints; i++) {
		endp = &hintf->endpoint[i].desc;
		if (usb_endpoint_is_bulk_in(endp))
			bulk_in = endp->bEndpointAddress;
		else if (usb_endpoint_is_bulk_out(endp))
			bulk_out = endp->bEndpointAddress;
	}

	if (!(bulk_in && bulk_out)) {
		retval = -ENODEV;
		err("Unable to find endpoints, retval=%d", retval);
		return retval;
	}
	else
	{
		dev->in = usb_rcvbulkpipe(dev->udev, bulk_in & USB_ENDPOINT_NUMBER_MASK);
		dev->out= usb_sndbulkpipe(dev->udev, bulk_out & USB_ENDPOINT_NUMBER_MASK);
	}

	//fix bulk recv buffer size
	dev->rx_urb_size = IPHETH_BUF_SIZE;
	//dev->hard_mtu = IPHETH_BUF_SIZE;//need this?

	//get hardware addr
	retval = ipheth_get_macaddr(dev);
	if (0 != retval)
	{
		err("Unable to get macaddr, retval=%d", retval);
		return retval;
	}

	//carrier check:check the remote tethering enable or not.
	mutex_init (&(local.local_mutex));
	INIT_DELAYED_WORK(&(local.carrier_work), ipheth_carrier_check_work);
	local.dev = dev;

	return retval;
}
EXPORT_SYMBOL_GPL(ipheth_bind);

static int ipheth_reset(struct usbnet *dev)
{
	int retval = 0;//success
	struct work_struct *work = NULL;

	//Attention: this func's section should not be in "reset" function,
	//I put it here because it still need to be called during the usbnet_open().
	usb_set_interface(dev->udev, IPHETH_INTFNUM, IPHETH_ALT_INTFNUM);
	ipheth_carrier_check_work(work);

	return retval;
}
EXPORT_SYMBOL_GPL(ipheth_reset);

static int ipheth_stop(struct usbnet *dev)
{
	mutex_lock(&(local.local_mutex));
	cancel_delayed_work_sync(&local.carrier_work);
	mutex_unlock(&(local.local_mutex));

	return 0;
}
EXPORT_SYMBOL_GPL(ipheth_stop);

int ipheth_rx_fixup(struct usbnet *dev, struct sk_buff *skb)
{
	skb_pull(skb, IPHETH_IP_ALIGN);
	return 1;
}
EXPORT_SYMBOL_GPL(ipheth_rx_fixup);

static const struct driver_info ipheth_info = {
	.description =	"ipheth device",
	.flags =	FLAG_ETHER | FLAG_NO_SETINT,
	.status =	ipheth_status,
	.bind = 	ipheth_bind,
	.reset = 	ipheth_reset,
	.stop =		ipheth_stop,
	.rx_fixup = ipheth_rx_fixup,
};

/*-------------------------------------------------------------------------*/
static struct usb_device_id ipheth_table[] = {
	{ USB_DEVICE_AND_INTERFACE_INFO(
		USB_VENDOR_APPLE, USB_PRODUCT_IPHONE,
		IPHETH_USBINTF_CLASS, IPHETH_USBINTF_SUBCLASS,
		IPHETH_USBINTF_PROTO),
		.driver_info = (unsigned long) &ipheth_info,},
	{ USB_DEVICE_AND_INTERFACE_INFO(
		USB_VENDOR_APPLE, USB_PRODUCT_IPHONE_3G,
		IPHETH_USBINTF_CLASS, IPHETH_USBINTF_SUBCLASS,
		IPHETH_USBINTF_PROTO),
		.driver_info = (unsigned long) &ipheth_info,},
	{ USB_DEVICE_AND_INTERFACE_INFO(
		USB_VENDOR_APPLE, USB_PRODUCT_IPHONE_3GS,
		IPHETH_USBINTF_CLASS, IPHETH_USBINTF_SUBCLASS,
		IPHETH_USBINTF_PROTO),
		.driver_info = (unsigned long) &ipheth_info,},
	{ USB_DEVICE_AND_INTERFACE_INFO(
		USB_VENDOR_APPLE, USB_PRODUCT_IPHONE_4,
		IPHETH_USBINTF_CLASS, IPHETH_USBINTF_SUBCLASS,
		IPHETH_USBINTF_PROTO),
		.driver_info = (unsigned long) &ipheth_info,},
	{ USB_DEVICE_AND_INTERFACE_INFO(
		USB_VENDOR_APPLE, USB_PRODUCT_IPHONE_4_VZW,
		IPHETH_USBINTF_CLASS, IPHETH_USBINTF_SUBCLASS,
		IPHETH_USBINTF_PROTO),
		.driver_info = (unsigned long) &ipheth_info,},
	{ USB_DEVICE_AND_INTERFACE_INFO(
		USB_VENDOR_APPLE, USB_PRODUCT_IPHONE_4S,
		IPHETH_USBINTF_CLASS, IPHETH_USBINTF_SUBCLASS,
		IPHETH_USBINTF_PROTO),
		.driver_info = (unsigned long) &ipheth_info,},
	{ USB_DEVICE_AND_INTERFACE_INFO(
		USB_VENDOR_APPLE, USB_PRODUCT_IPAD,
		IPHETH_USBINTF_CLASS, IPHETH_USBINTF_SUBCLASS,
		IPHETH_USBINTF_PROTO),
		.driver_info = (unsigned long) &ipheth_info,},
	{ USB_DEVICE_AND_INTERFACE_INFO(
		USB_VENDOR_APPLE, USB_PRODUCT_IPAD_MINI,
		IPHETH_USBINTF_CLASS, IPHETH_USBINTF_SUBCLASS,
		IPHETH_USBINTF_PROTO),
		.driver_info = (unsigned long) &ipheth_info,},
	{ USB_DEVICE_AND_INTERFACE_INFO(
		USB_VENDOR_APPLE, USB_PRODUCT_IPHONE_5,
		IPHETH_USBINTF_CLASS, IPHETH_USBINTF_SUBCLASS,
		IPHETH_USBINTF_PROTO),
		.driver_info = (unsigned long) &ipheth_info,},
	{ }
};
MODULE_DEVICE_TABLE(usb, ipheth_table);

static struct usb_driver ipheth_driver = {
	.name =		"ipheth",
	.id_table =	ipheth_table,
	.probe =	usbnet_probe,
	.disconnect =	usbnet_disconnect,
	.suspend =	usbnet_suspend,
	.resume =	usbnet_resume,
};
module_usb_driver(ipheth_driver);

MODULE_AUTHOR("Walker Wei");
MODULE_DESCRIPTION("Apple iPhone USB Ethernet driver");
MODULE_LICENSE("GPL");
