/*
 * Asus PC WMI hotkey driver
 *
 * Copyright(C) 2010 Intel Corporation.
 * Copyright(C) 2010-2011 Corentin Chary <corentin.chary@gmail.com>
 *
 * Portions based on wistron_btns.c:
 * Copyright (C) 2005 Miloslav Trmac <mitr@volny.cz>
 * Copyright (C) 2005 Bernhard Rosenkraenzer <bero@arklinux.org>
 * Copyright (C) 2005 Dmitry Torokhov <dtor@mail.ru>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/input/sparse-keymap.h>
#include <linux/fb.h>
#include <linux/backlight.h>
#include <linux/leds.h>
#include <linux/rfkill.h>
#include <linux/pci.h>
#include <linux/pci_hotplug.h>
#include <linux/power_supply.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/platform_device.h>
#include <linux/acpi.h>
#include <linux/dmi.h>

#include <linux/version.h>
#if (LINUX_VERSION_CODE > KERNEL_VERSION(5, 6, 0))
#include <linux/units.h>
#else
#define ABSOLUTE_ZERO_MILLICELSIUS -273150
#define MILLIDEGREE_PER_DECIDEGREE 100

static inline long milli_kelvin_to_millicelsius(long t)
{
	return t + ABSOLUTE_ZERO_MILLICELSIUS;
}

static inline long deci_kelvin_to_millicelsius(long t)
{
	return milli_kelvin_to_millicelsius(t * MILLIDEGREE_PER_DECIDEGREE);
}
#endif

#include <acpi/battery.h>
#include <acpi/video.h>

#include "faustus.h"

MODULE_AUTHOR("Corentin Chary <corentin.chary@gmail.com>, "
	      "Yong Wang <yong.y.wang@intel.com>");
MODULE_DESCRIPTION("Backport of Asus Generic WMI Driver");
MODULE_LICENSE("GPL");

static bool let_it_burn = 0;
module_param(let_it_burn, bool, 0);
MODULE_PARM_DESC(let_it_burn, "Disable DMI check, force load");

static bool report_key_events = 0;
module_param(report_key_events, bool, 0644);
MODULE_PARM_DESC(report_key_events, "Forward fan mode key events");

#define ASUS_WMI_MGMT_GUID	"97845ED0-4E6D-11DE-8A39-0800200C9A66"

#define NOTIFY_BRNUP_MIN		0x11
#define NOTIFY_BRNUP_MAX		0x1f
#define NOTIFY_BRNDOWN_MIN		0x20
#define NOTIFY_BRNDOWN_MAX		0x2e
#define NOTIFY_FNLOCK_TOGGLE		0x4e
#define NOTIFY_WNDWSLOCK_TOGGLE		0x4f
#define NOTIFY_KBD_DOCK_CHANGE		0x75
#define NOTIFY_KBD_BRTUP		0xc4
#define NOTIFY_KBD_BRTDWN		0xc5
#define NOTIFY_KBD_BRTTOGGLE		0xc7
#define NOTIFY_KBD_FBM			0x99
#define NOTIFY_KBD_TTP			0xae
#define NOTIFY_KBD_AURA_LFT		0xb2
#define NOTIFY_KBD_AURA_RGHT		0xb3
#define NOTIFY_LID_FLIP			0xfa

#define ASUS_WMI_FNLOCK_BIOS_DISABLED	BIT(0)

#define ASUS_CPU_FAN_DESC			"cpu_fan"
#define ASUS_GPU_FAN_DESC			"gpu_fan"
#define ASUS_FAN_MFUN			0x13
#define ASUS_FAN_SFUN_READ		0x06
#define ASUS_FAN_SFUN_WRITE		0x07

/* Based on standard hwmon pwmX_enable values */
#define ASUS_FAN_CTRL_FULLSPEED		0
#define ASUS_FAN_CTRL_MANUAL		1
#define ASUS_FAN_CTRL_AUTO		2

#define ASUS_FAN_BOOST_MODE_NORMAL		0
#define ASUS_FAN_BOOST_MODE_OVERBOOST		1
#define ASUS_FAN_BOOST_MODE_OVERBOOST_MASK	0x01
#define ASUS_FAN_BOOST_MODE_SILENT		2
#define ASUS_FAN_BOOST_MODE_SILENT_MASK		0x02
#define ASUS_FAN_BOOST_MODES_MASK		0x03

#define ASUS_THROTTLE_THERMAL_POLICY_DEFAULT	0
#define ASUS_THROTTLE_THERMAL_POLICY_OVERBOOST	1
#define ASUS_THROTTLE_THERMAL_POLICY_SILENT	2

#define USB_INTEL_XUSB2PR		0xD0
#define PCI_DEVICE_ID_INTEL_LYNXPOINT_LP_XHCI	0x9c31

#define ASUS_ACPI_UID_ASUSWMI		"ASUSWMI"
#define ASUS_ACPI_UID_ATK		"ATK"

#define WMI_EVENT_QUEUE_SIZE		0x10
#define WMI_EVENT_QUEUE_END		0x1
#define WMI_EVENT_MASK			0xFFFF
/* The WMI hotkey event value is always the same. */
#define WMI_EVENT_VALUE_ATK		0xFF

#define WMI_EVENT_MASK			0xFFFF

static const char * const ashs_ids[] = { "ATK4001", "ATK4002", NULL };

static bool ashs_present(void)
{
	int i = 0;
	while (ashs_ids[i]) {
		if (acpi_dev_found(ashs_ids[i++]))
			return true;
	}
	return false;
}

struct bios_args {
	u32 arg0;
	u32 arg1;
	u32 arg2; /* At least TUF Gaming series uses 3 dword input buffer. */
	u32 arg4;
	u32 arg5;
} __packed;

/*
 * Struct that's used for all methods called via AGFN. Naming is
 * identically to the AML code.
 */
struct agfn_args {
	u16 mfun; /* probably "Multi-function" to be called */
	u16 sfun; /* probably "Sub-function" to be called */
	u16 len;  /* size of the hole struct, including subfunction fields */
	u8 stas;  /* not used by now */
	u8 err;   /* zero on success */
} __packed;

/* struct used for calling fan read and write methods */
struct agfn_fan_args {
	struct agfn_args agfn;	/* common fields */
	u8 fan;			/* fan number: 0: set auto mode 1: 1st fan */
	u32 speed;		/* read: RPM/100 - write: 0-255 */
} __packed;

/*
 * <platform>/    - debugfs root directory
 *   dev_id      - current dev_id
 *   ctrl_param  - current ctrl_param
 *   method_id   - current method_id
 *   devs        - call DEVS(dev_id, ctrl_param) and print result
 *   dsts        - call DSTS(dev_id)  and print result
 *   call        - call method_id(dev_id, ctrl_param) and print result
 */
struct asus_wmi_debug {
	struct dentry *root;
	u32 method_id;
	u32 dev_id;
	u32 ctrl_param;
};

struct asus_rfkill {
	struct asus_wmi *asus;
	struct rfkill *rfkill;
	u32 dev_id;
};

struct asus_kbbl_rgb {
	u8 kbbl_red;
	u8 kbbl_green;
	u8 kbbl_blue;
	u8 kbbl_mode;
	u8 kbbl_speed;
	u8 kbbl_auraspeed;
	u8 kbbl_auramode;

	u8 kbbl_set_red;
	u8 kbbl_set_green;
	u8 kbbl_set_blue;
	u8 kbbl_set_mode;
	u8 kbbl_set_speed;
	u8 kbbl_set_flags;
	u8 kbbl_set_auraspeed;
	u8 kbbl_set_auramode;
};

enum fan_type {
	FAN_TYPE_NONE = 0,
	FAN_TYPE_AGFN,		/* deprecated on newer platforms */
	FAN_TYPE_SPEC83,	/* starting in Spec 8.3, use CPU_FAN_CTRL */
};

struct asus_wmi {
	int dsts_id;
	int spec;
	int sfun;
	bool wmi_event_queue;

	struct input_dev *inputdev;
	struct backlight_device *backlight_device;
	struct platform_device *platform_device;

	struct led_classdev wlan_led;
	int wlan_led_wk;
	struct led_classdev tpd_led;
	int tpd_led_wk;
	struct led_classdev kbd_led;
	int kbd_led_wk;
	struct led_classdev lightbar_led;
	int lightbar_led_wk;
	struct workqueue_struct *led_workqueue;
	struct work_struct tpd_led_work;
	struct work_struct wlan_led_work;
	struct work_struct lightbar_led_work;

	struct asus_rfkill wlan;
	struct asus_rfkill bluetooth;
	struct asus_rfkill wimax;
	struct asus_rfkill wwan3g;
	struct asus_rfkill gps;
	struct asus_rfkill uwb;

	enum fan_type fan_type;
	int fan_pwm_mode;
	int agfn_pwm;

	bool fan_boost_mode_available;
	u8 fan_boost_mode_mask;
	u8 fan_boost_mode;

	bool throttle_thermal_policy_available;
	u8 throttle_thermal_policy_mode;

	// The RSOC controls the maximum charging percentage.
	bool battery_rsoc_available;

	bool kbbl_rgb_available;
	struct asus_kbbl_rgb kbbl_rgb;

	struct hotplug_slot hotplug_slot;
	struct mutex hotplug_lock;
	struct mutex wmi_lock;
	struct workqueue_struct *hotplug_workqueue;
	struct work_struct hotplug_work;

	bool fnlock_locked;
	bool wndwslock_locked;

	struct asus_wmi_debug debug;

	struct asus_wmi_driver *driver;
};

/* WMI ************************************************************************/

static int asus_wmi_evaluate_method5(u32 method_id,
		u32 arg0, u32 arg1, u32 arg2, u32 arg4, u32 arg5, u32 *retval)
{
	struct bios_args args = {
		.arg0 = arg0,
		.arg1 = arg1,
		.arg2 = arg2,
		.arg4 = arg4,
		.arg5 = arg5,
	};
	struct acpi_buffer input = { (acpi_size) sizeof(args), &args };
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	acpi_status status;
	union acpi_object *obj;
	u32 tmp = 0;

	status = wmi_evaluate_method(ASUS_WMI_MGMT_GUID, 0, method_id,
				     &input, &output);

	if (ACPI_FAILURE(status))
		return -EIO;

	obj = (union acpi_object *)output.pointer;
	if (obj && obj->type == ACPI_TYPE_INTEGER)
		tmp = (u32) obj->integer.value;

	if (retval)
		*retval = tmp;

	kfree(obj);

	if (tmp == ASUS_WMI_UNSUPPORTED_METHOD)
		return -ENODEV;

	return 0;
}

static int asus_wmi_evaluate_method3(u32 method_id,
		u32 arg0, u32 arg1, u32 arg2, u32 *retval)
{
	return asus_wmi_evaluate_method5(method_id, arg0, arg1, arg2, 0, 0, retval);
}

static int asus_wmi_evaluate_method(u32 method_id, u32 arg0, u32 arg1, u32 *retval)
{
	return asus_wmi_evaluate_method3(method_id, arg0, arg1, 0, retval);
}

static int asus_wmi_evaluate_method_agfn(const struct acpi_buffer args)
{
	struct acpi_buffer input;
	u64 phys_addr;
	u32 retval;
	u32 status;

	/*
	 * Copy to dma capable address otherwise memory corruption occurs as
	 * bios has to be able to access it.
	 */
	input.pointer = kmemdup(args.pointer, args.length, GFP_DMA | GFP_KERNEL);
	input.length = args.length;
	if (!input.pointer)
		return -ENOMEM;
	phys_addr = virt_to_phys(input.pointer);

	status = asus_wmi_evaluate_method(ASUS_WMI_METHODID_AGFN,
					phys_addr, 0, &retval);
	if (!status)
		memcpy(args.pointer, input.pointer, args.length);

	kfree(input.pointer);
	if (status)
		return -ENXIO;

	return retval;
}

static int asus_wmi_get_devstate(struct asus_wmi *asus, u32 dev_id, u32 *retval)
{
	return asus_wmi_evaluate_method(asus->dsts_id, dev_id, 0, retval);
}

static int asus_wmi_set_devstate(u32 dev_id, u32 ctrl_param,
				 u32 *retval)
{
	return asus_wmi_evaluate_method(ASUS_WMI_METHODID_DEVS, dev_id,
					ctrl_param, retval);
}

/* Helper for special devices with magic return codes */
static int asus_wmi_get_devstate_bits(struct asus_wmi *asus,
				      u32 dev_id, u32 mask)
{
	u32 retval = 0;
	int err;

	err = asus_wmi_get_devstate(asus, dev_id, &retval);
	if (err < 0)
		return err;

	if (!(retval & ASUS_WMI_DSTS_PRESENCE_BIT))
		return -ENODEV;

	if (mask == ASUS_WMI_DSTS_STATUS_BIT) {
		if (retval & ASUS_WMI_DSTS_UNKNOWN_BIT)
			return -ENODEV;
	}

	return retval & mask;
}

static int asus_wmi_get_devstate_simple(struct asus_wmi *asus, u32 dev_id)
{
	return asus_wmi_get_devstate_bits(asus, dev_id,
					  ASUS_WMI_DSTS_STATUS_BIT);
}

static bool asus_wmi_dev_is_present(struct asus_wmi *asus, u32 dev_id)
{
	u32 retval;
	int status = asus_wmi_get_devstate(asus, dev_id, &retval);

	return status == 0 && (retval & ASUS_WMI_DSTS_PRESENCE_BIT);
}

/* Input **********************************************************************/

static int asus_wmi_input_init(struct asus_wmi *asus)
{
	int err, result;

	asus->inputdev = input_allocate_device();
	if (!asus->inputdev)
		return -ENOMEM;

	asus->inputdev->name = asus->driver->input_name;
	asus->inputdev->phys = asus->driver->input_phys;
	asus->inputdev->id.bustype = BUS_HOST;
	asus->inputdev->dev.parent = &asus->platform_device->dev;
	set_bit(EV_REP, asus->inputdev->evbit);

	err = sparse_keymap_setup(asus->inputdev, asus->driver->keymap, NULL);
	if (err)
		goto err_free_dev;

	if (asus->driver->quirks->use_kbd_dock_devid) {
		result = asus_wmi_get_devstate_simple(asus, ASUS_WMI_DEVID_KBD_DOCK);
		if (result >= 0) {
			input_set_capability(asus->inputdev, EV_SW, SW_TABLET_MODE);
			input_report_switch(asus->inputdev, SW_TABLET_MODE, !result);
		} else if (result != -ENODEV) {
			pr_err("Error checking for keyboard-dock: %d\n", result);
		}
	}

	if (asus->driver->quirks->use_lid_flip_devid) {
		result = asus_wmi_get_devstate_simple(asus, ASUS_WMI_DEVID_LID_FLIP);
		if (result < 0)
			asus->driver->quirks->use_lid_flip_devid = 0;
		if (result >= 0) {
			input_set_capability(asus->inputdev, EV_SW, SW_TABLET_MODE);
			input_report_switch(asus->inputdev, SW_TABLET_MODE, result);
		} else if (result == -ENODEV) {
			pr_err("This device has lid_flip quirk but got ENODEV checking it. This is a bug.");
		} else {
			pr_err("Error checking for lid-flip: %d\n", result);
		}
	}

	err = input_register_device(asus->inputdev);
	if (err)
		goto err_free_dev;

	return 0;

err_free_dev:
	input_free_device(asus->inputdev);
	return err;
}

static void asus_wmi_input_exit(struct asus_wmi *asus)
{
	if (asus->inputdev)
		input_unregister_device(asus->inputdev);

	asus->inputdev = NULL;
}

/* Tablet mode ****************************************************************/

static void lid_flip_tablet_mode_get_state(struct asus_wmi *asus)
{
	int result = asus_wmi_get_devstate_simple(asus, ASUS_WMI_DEVID_LID_FLIP);

	if (result >= 0) {
		input_report_switch(asus->inputdev, SW_TABLET_MODE, result);
		input_sync(asus->inputdev);
	}
}

/* Battery ********************************************************************/

/* The battery maximum charging percentage */
static int charge_end_threshold;

static ssize_t charge_control_end_threshold_store(struct device *dev,
						  struct device_attribute *attr,
						  const char *buf, size_t count)
{
	int value, ret, rv;

	ret = kstrtouint(buf, 10, &value);
	if (ret)
		return ret;

	if (value < 0 || value > 100)
		return -EINVAL;

	ret = asus_wmi_set_devstate(ASUS_WMI_DEVID_RSOC, value, &rv);
	if (ret)
		return ret;

	if (rv != 1)
		return -EIO;

	/* There isn't any method in the DSDT to read the threshold, so we
	 * save the threshold.
	 */
	charge_end_threshold = value;
	return count;
}

static ssize_t charge_control_end_threshold_show(struct device *device,
						 struct device_attribute *attr,
						 char *buf)
{
	return sprintf(buf, "%d\n", charge_end_threshold);
}

static DEVICE_ATTR_RW(charge_control_end_threshold);

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 2, 0))
static int asus_wmi_battery_add(struct power_supply *battery, struct acpi_battery_hook *hook)
#else
static int asus_wmi_battery_add(struct power_supply *battery)
#endif
{
	/* The WMI method does not provide a way to specific a battery, so we
	 * just assume it is the first battery.
	 * Note: On some newer ASUS laptops (Zenbook UM431DA), the primary/first
	 * battery is named BATT.
	 */
	if (strcmp(battery->desc->name, "BAT0") != 0 &&
	    strcmp(battery->desc->name, "BAT1") != 0 &&
	    strcmp(battery->desc->name, "BATC") != 0 &&
	    strcmp(battery->desc->name, "BATT") != 0)
		return -ENODEV;

	if (device_create_file(&battery->dev,
	    &dev_attr_charge_control_end_threshold))
		return -ENODEV;

	/* The charge threshold is only reset when the system is power cycled,
	 * and we can't get the current threshold so let set it to 100% when
	 * a battery is added.
	 */
	asus_wmi_set_devstate(ASUS_WMI_DEVID_RSOC, 100, NULL);
	charge_end_threshold = 100;

	return 0;
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 2, 0))
static int asus_wmi_battery_remove(struct power_supply *battery, struct acpi_battery_hook *hook)
#else
static int asus_wmi_battery_remove(struct power_supply *battery)
#endif
{
	device_remove_file(&battery->dev,
			   &dev_attr_charge_control_end_threshold);
	return 0;
}

static struct acpi_battery_hook battery_hook = {
	.add_battery = asus_wmi_battery_add,
	.remove_battery = asus_wmi_battery_remove,
	.name = "ASUS Battery Extension",
};

static void asus_wmi_battery_init(struct asus_wmi *asus)
{
	asus->battery_rsoc_available = false;
	if (asus_wmi_dev_is_present(asus, ASUS_WMI_DEVID_RSOC)) {
		asus->battery_rsoc_available = true;
		battery_hook_register(&battery_hook);
	}
}

static void asus_wmi_battery_exit(struct asus_wmi *asus)
{
	if (asus->battery_rsoc_available)
		battery_hook_unregister(&battery_hook);
}

/* LEDs ***********************************************************************/

/*
 * These functions actually update the LED's, and are called from a
 * workqueue. By doing this as separate work rather than when the LED
 * subsystem asks, we avoid messing with the Asus ACPI stuff during a
 * potentially bad time, such as a timer interrupt.
 */
static void tpd_led_update(struct work_struct *work)
{
	int ctrl_param;
	struct asus_wmi *asus;

	asus = container_of(work, struct asus_wmi, tpd_led_work);

	ctrl_param = asus->tpd_led_wk;
	asus_wmi_set_devstate(ASUS_WMI_DEVID_TOUCHPAD_LED, ctrl_param, NULL);
}

static void tpd_led_set(struct led_classdev *led_cdev,
			enum led_brightness value)
{
	struct asus_wmi *asus;

	asus = container_of(led_cdev, struct asus_wmi, tpd_led);

	asus->tpd_led_wk = !!value;
	queue_work(asus->led_workqueue, &asus->tpd_led_work);
}

static int read_tpd_led_state(struct asus_wmi *asus)
{
	return asus_wmi_get_devstate_simple(asus, ASUS_WMI_DEVID_TOUCHPAD_LED);
}

static enum led_brightness tpd_led_get(struct led_classdev *led_cdev)
{
	struct asus_wmi *asus;

	asus = container_of(led_cdev, struct asus_wmi, tpd_led);

	return read_tpd_led_state(asus);
}

static void kbd_led_update(struct asus_wmi *asus)
{
	int ctrl_param = 0;

	ctrl_param = 0x80 | (asus->kbd_led_wk & 0x7F);
	asus_wmi_set_devstate(ASUS_WMI_DEVID_KBD_BACKLIGHT, ctrl_param, NULL);
}

static int kbd_led_read(struct asus_wmi *asus, int *level, int *env)
{
	int retval;

	/*
	 * bits 0-2: level
	 * bit 7: light on/off
	 * bit 8-10: environment (0: dark, 1: normal, 2: light)
	 * bit 17: status unknown
	 */
	retval = asus_wmi_get_devstate_bits(asus, ASUS_WMI_DEVID_KBD_BACKLIGHT,
					    0xFFFF);

	/* Unknown status is considered as off */
	if (retval == 0x8000)
		retval = 0;

	if (retval < 0)
		return retval;

	if (level)
		*level = retval & 0x7F;
	if (env)
		*env = (retval >> 8) & 0x7F;
	return 0;
}

static void do_kbd_led_set(struct led_classdev *led_cdev, int value)
{
	struct asus_wmi *asus;
	int max_level;

	asus = container_of(led_cdev, struct asus_wmi, kbd_led);
	max_level = asus->kbd_led.max_brightness;

	asus->kbd_led_wk = clamp_val(value, 0, max_level);
	kbd_led_update(asus);
}

static void kbd_led_set(struct led_classdev *led_cdev,
			enum led_brightness value)
{
	/* Prevent disabling keyboard backlight on module unregister */
	if (led_cdev->flags & LED_UNREGISTERING)
		return;

	do_kbd_led_set(led_cdev, value);
}

static void kbd_led_set_by_kbd(struct asus_wmi *asus, enum led_brightness value)
{
	struct led_classdev *led_cdev = &asus->kbd_led;

	do_kbd_led_set(led_cdev, value);
	led_classdev_notify_brightness_hw_changed(led_cdev, asus->kbd_led_wk);
}

static enum led_brightness kbd_led_get(struct led_classdev *led_cdev)
{
	struct asus_wmi *asus;
	int retval, value;

	asus = container_of(led_cdev, struct asus_wmi, kbd_led);

	retval = kbd_led_read(asus, &value, NULL);
	if (retval < 0)
		return retval;

	return value;
}

static int wlan_led_unknown_state(struct asus_wmi *asus)
{
	u32 result;

	asus_wmi_get_devstate(asus, ASUS_WMI_DEVID_WIRELESS_LED, &result);

	return result & ASUS_WMI_DSTS_UNKNOWN_BIT;
}

static void wlan_led_update(struct work_struct *work)
{
	int ctrl_param;
	struct asus_wmi *asus;

	asus = container_of(work, struct asus_wmi, wlan_led_work);

	ctrl_param = asus->wlan_led_wk;
	asus_wmi_set_devstate(ASUS_WMI_DEVID_WIRELESS_LED, ctrl_param, NULL);
}

static void wlan_led_set(struct led_classdev *led_cdev,
			 enum led_brightness value)
{
	struct asus_wmi *asus;

	asus = container_of(led_cdev, struct asus_wmi, wlan_led);

	asus->wlan_led_wk = !!value;
	queue_work(asus->led_workqueue, &asus->wlan_led_work);
}

static enum led_brightness wlan_led_get(struct led_classdev *led_cdev)
{
	struct asus_wmi *asus;
	u32 result;

	asus = container_of(led_cdev, struct asus_wmi, wlan_led);
	asus_wmi_get_devstate(asus, ASUS_WMI_DEVID_WIRELESS_LED, &result);

	return result & ASUS_WMI_DSTS_BRIGHTNESS_MASK;
}

static void lightbar_led_update(struct work_struct *work)
{
	struct asus_wmi *asus;
	int ctrl_param;

	asus = container_of(work, struct asus_wmi, lightbar_led_work);

	ctrl_param = asus->lightbar_led_wk;
	asus_wmi_set_devstate(ASUS_WMI_DEVID_LIGHTBAR, ctrl_param, NULL);
}

static void lightbar_led_set(struct led_classdev *led_cdev,
			     enum led_brightness value)
{
	struct asus_wmi *asus;

	asus = container_of(led_cdev, struct asus_wmi, lightbar_led);

	asus->lightbar_led_wk = !!value;
	queue_work(asus->led_workqueue, &asus->lightbar_led_work);
}

static enum led_brightness lightbar_led_get(struct led_classdev *led_cdev)
{
	struct asus_wmi *asus;
	u32 result;

	asus = container_of(led_cdev, struct asus_wmi, lightbar_led);
	asus_wmi_get_devstate(asus, ASUS_WMI_DEVID_LIGHTBAR, &result);

	return result & ASUS_WMI_DSTS_LIGHTBAR_MASK;
}

static void asus_wmi_led_exit(struct asus_wmi *asus)
{
	led_classdev_unregister(&asus->kbd_led);
	led_classdev_unregister(&asus->tpd_led);
	led_classdev_unregister(&asus->wlan_led);
	led_classdev_unregister(&asus->lightbar_led);

	if (asus->led_workqueue)
		destroy_workqueue(asus->led_workqueue);
}

static int asus_wmi_led_init(struct asus_wmi *asus)
{
	int rv = 0, led_val;

	asus->led_workqueue = create_singlethread_workqueue("led_workqueue");
	if (!asus->led_workqueue)
		return -ENOMEM;

	if (read_tpd_led_state(asus) >= 0) {
		INIT_WORK(&asus->tpd_led_work, tpd_led_update);

		asus->tpd_led.name = "asus::touchpad";
		asus->tpd_led.brightness_set = tpd_led_set;
		asus->tpd_led.brightness_get = tpd_led_get;
		asus->tpd_led.max_brightness = 1;

		rv = led_classdev_register(&asus->platform_device->dev,
					   &asus->tpd_led);
		if (rv)
			goto error;
	}

	if (!kbd_led_read(asus, &led_val, NULL)) {
		asus->kbd_led_wk = led_val;
		asus->kbd_led.name = "asus::kbd_backlight";
		asus->kbd_led.flags = LED_BRIGHT_HW_CHANGED;
		asus->kbd_led.brightness_set = kbd_led_set;
		asus->kbd_led.brightness_get = kbd_led_get;
		asus->kbd_led.max_brightness = 3;

		rv = led_classdev_register(&asus->platform_device->dev,
					   &asus->kbd_led);
		if (rv)
			goto error;
	}

	if (asus_wmi_dev_is_present(asus, ASUS_WMI_DEVID_WIRELESS_LED)
			&& (asus->driver->quirks->wapf > 0)) {
		INIT_WORK(&asus->wlan_led_work, wlan_led_update);

		asus->wlan_led.name = "asus::wlan";
		asus->wlan_led.brightness_set = wlan_led_set;
		if (!wlan_led_unknown_state(asus))
			asus->wlan_led.brightness_get = wlan_led_get;
		asus->wlan_led.flags = LED_CORE_SUSPENDRESUME;
		asus->wlan_led.max_brightness = 1;
		asus->wlan_led.default_trigger = "asus-wlan";

		rv = led_classdev_register(&asus->platform_device->dev,
					   &asus->wlan_led);
		if (rv)
			goto error;
	}

	if (asus_wmi_dev_is_present(asus, ASUS_WMI_DEVID_LIGHTBAR)) {
		INIT_WORK(&asus->lightbar_led_work, lightbar_led_update);

		asus->lightbar_led.name = "asus::lightbar";
		asus->lightbar_led.brightness_set = lightbar_led_set;
		asus->lightbar_led.brightness_get = lightbar_led_get;
		asus->lightbar_led.max_brightness = 1;

		rv = led_classdev_register(&asus->platform_device->dev,
					   &asus->lightbar_led);
	}

error:
	if (rv)
		asus_wmi_led_exit(asus);

	return rv;
}

/* RGB keyboard backlight *****************************************************/

static ssize_t show_u8(u8 value, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%02x\n", value);
}

static ssize_t store_u8(u8 *value, const char *buf, int count)
{
	int err;
	u8 result;

	err = kstrtou8(buf, 16, &result);
	if (err < 0) {
		pr_warn("Trying to store invalid value\n");
		return err;
	}

	*value = result;

	return count;
}

static ssize_t kbbl_red_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct asus_wmi *asus = dev_get_drvdata(dev);

	return show_u8(asus->kbbl_rgb.kbbl_red, buf);
}

static ssize_t kbbl_red_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct asus_wmi *asus = dev_get_drvdata(dev);

	return store_u8(&asus->kbbl_rgb.kbbl_set_red, buf, count);
}

static ssize_t kbbl_green_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct asus_wmi *asus = dev_get_drvdata(dev);

	return show_u8(asus->kbbl_rgb.kbbl_green, buf);
}

static ssize_t kbbl_green_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct asus_wmi *asus = dev_get_drvdata(dev);

	return store_u8(&asus->kbbl_rgb.kbbl_set_green, buf, count);
}

static ssize_t kbbl_blue_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct asus_wmi *asus = dev_get_drvdata(dev);

	return show_u8(asus->kbbl_rgb.kbbl_blue, buf);
}

static ssize_t kbbl_blue_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct asus_wmi *asus = dev_get_drvdata(dev);

	return store_u8(&asus->kbbl_rgb.kbbl_set_blue, buf, count);
}

static ssize_t kbbl_mode_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct asus_wmi *asus = dev_get_drvdata(dev);

	return show_u8(asus->kbbl_rgb.kbbl_mode, buf);
}

static ssize_t kbbl_mode_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct asus_wmi *asus = dev_get_drvdata(dev);

	return store_u8(&asus->kbbl_rgb.kbbl_set_mode, buf, count);
}

static ssize_t kbbl_auramode_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct asus_wmi *asus = dev_get_drvdata(dev);

	return show_u8(asus->kbbl_rgb.kbbl_auramode, buf);
}

static ssize_t kbbl_auramode_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct asus_wmi *asus = dev_get_drvdata(dev);

	return store_u8(&asus->kbbl_rgb.kbbl_set_auramode, buf, count);
}

static ssize_t kbbl_auraspeed_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct asus_wmi *asus = dev_get_drvdata(dev);

	return show_u8(asus->kbbl_rgb.kbbl_auraspeed, buf);
}

static ssize_t kbbl_auraspeed_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct asus_wmi *asus = dev_get_drvdata(dev);

	return store_u8(&asus->kbbl_rgb.kbbl_set_auraspeed, buf, count);
}

static ssize_t kbbl_speed_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct asus_wmi *asus = dev_get_drvdata(dev);

	return show_u8(asus->kbbl_rgb.kbbl_speed, buf);
}

static ssize_t kbbl_speed_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct asus_wmi *asus = dev_get_drvdata(dev);

	return store_u8(&asus->kbbl_rgb.kbbl_set_speed, buf, count);
}

static ssize_t kbbl_flags_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct asus_wmi *asus = dev_get_drvdata(dev);

	return show_u8(asus->kbbl_rgb.kbbl_set_flags, buf);
}

static ssize_t kbbl_flags_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct asus_wmi *asus = dev_get_drvdata(dev);

	return store_u8(&asus->kbbl_rgb.kbbl_set_flags, buf, count);
}

static ssize_t kbbl_set_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE,
			"Write to configure RGB keyboard backlight\n");
}

static int kbbl_rgb_write(struct asus_wmi *asus, int persistent)
{
	int err;
	u32 retval;
	u8 speed_byte;
	u8 mode_byte;
	u8 speed;
	u8 mode;

	speed = asus->kbbl_rgb.kbbl_set_speed;
	switch (speed) {
	case 0:
	default:
		speed_byte = 0xe1; // slow
		speed = 0;
		break;
	case 1:
		speed_byte = 0xeb; // medium
		break;
	case 2:
		speed_byte = 0xf5; // fast
		break;
	}

	mode = asus->kbbl_rgb.kbbl_set_mode;
	switch (mode) {
	case 0:
	default:
		mode_byte = 0x00; // static color
		mode = 0;
		break;
	case 1:
		mode_byte = 0x01; // breathing
		break;
	case 2:
		mode_byte = 0x02; // color cycle
		break;
	case 3:
		mode_byte = 0x0a; // strobing
		break;
	}

	err = asus_wmi_evaluate_method3(ASUS_WMI_METHODID_DEVS,
		ASUS_WMI_DEVID_KBD_RGB,
		(persistent ? 0xb4 : 0xb3) |
		(mode_byte << 8) |
		(asus->kbbl_rgb.kbbl_set_red << 16) |
		(asus->kbbl_rgb.kbbl_set_green << 24),
		(asus->kbbl_rgb.kbbl_set_blue) |
		(speed_byte << 8), &retval);
	if (err) {
		pr_warn("RGB keyboard device 1, write error: %d\n", err);
		return err;
	}

	if (retval != 1) {
		pr_warn("RGB keyboard device 1, write error (retval): %x\n",
				retval);
		return -EIO;
	}

	err = asus_wmi_evaluate_method3(ASUS_WMI_METHODID_DEVS,
		ASUS_WMI_DEVID_KBD_RGB2,
		(0xbd) |
		(asus->kbbl_rgb.kbbl_set_flags << 16) |
		(persistent ? 0x0100 : 0x0000), 0, &retval);
	if (err) {
		pr_warn("RGB keyboard device 2, write error: %d\n", err);
		return err;
	}

	if (retval != 1) {
		pr_warn("RGB keyboard device 2, write error (retval): %x\n",
				retval);
		return -EIO;
	}

	asus->kbbl_rgb.kbbl_red = asus->kbbl_rgb.kbbl_set_red;
	asus->kbbl_rgb.kbbl_green = asus->kbbl_rgb.kbbl_set_green;
	asus->kbbl_rgb.kbbl_blue = asus->kbbl_rgb.kbbl_set_blue;
	asus->kbbl_rgb.kbbl_mode = mode;
	asus->kbbl_rgb.kbbl_speed = speed;
	asus->kbbl_rgb.kbbl_auraspeed = asus->kbbl_rgb.kbbl_set_auraspeed;
	//asus->kbbl_rgb.kbbl_auramode = asus->kbbl_rgb.kbbl_set_auramode;
	asus->kbbl_rgb.kbbl_auramode = (asus->kbbl_rgb.kbbl_set_auramode <= 3)?
		asus->kbbl_rgb.kbbl_set_auramode : 0;

	return 0;
}

static ssize_t kbbl_set_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	u8 value;
	struct asus_wmi *asus;
	int result;

	asus = dev_get_drvdata(dev);
	result = store_u8(&value, buf, count);
	if (result < 0)
		return result;

	if (value == 1)
		kbbl_rgb_write(asus, 1);
	else if (value == 2)
		kbbl_rgb_write(asus, 0);

	return count;
}

/* RGB values: 00 .. ff */
static DEVICE_ATTR_RW(kbbl_red);
static DEVICE_ATTR_RW(kbbl_green);
static DEVICE_ATTR_RW(kbbl_blue);

/*
 * Color modes: 0 - static color, 1 - breathing, 2 - color cycle, 3 - strobing
 */
static DEVICE_ATTR_RW(kbbl_mode);

/* Speed for modes 1 and 2: 0 - slow, 1 - medium, 2 - fast */
static DEVICE_ATTR_RW(kbbl_speed);

/*
 * Enable: 02 - on boot (until module load) | 08 - awake | 20 - sleep
 * (2a or ff to enable everything)
 *
 * Logically 80 would be shutdown, but no visible effects of this option
 * were observed so far
 */
static DEVICE_ATTR_RW(kbbl_flags);

/* Write data: 1 - permanently, 2 - temporarily (reset after reboot) */
static DEVICE_ATTR_RW(kbbl_set);

/* Speed for aura hotkeys color change */
static DEVICE_ATTR_RW(kbbl_auraspeed);

/* Set mode switching for aura hotkeys: 0 - color cycle, 1 - color mode selection, 
 * 2 - speed for modes, 3 - white balance */
static DEVICE_ATTR_RW(kbbl_auramode);

static struct attribute *rgbkb_sysfs_attributes[] = {
	&dev_attr_kbbl_red.attr,
	&dev_attr_kbbl_green.attr,
	&dev_attr_kbbl_blue.attr,
	&dev_attr_kbbl_mode.attr,
	&dev_attr_kbbl_speed.attr,
	&dev_attr_kbbl_flags.attr,
	&dev_attr_kbbl_set.attr,
	&dev_attr_kbbl_auraspeed.attr,
	&dev_attr_kbbl_auramode.attr,
	NULL,
};

static const struct attribute_group kbbl_attribute_group = {
	.name = "kbbl",
	.attrs = rgbkb_sysfs_attributes
};

static int kbbl_rgb_init(struct asus_wmi *asus)
{
	int err;

	err = asus_wmi_get_devstate_simple(asus, ASUS_WMI_DEVID_KBD_RGB);
	if (err) {
		if (err == -ENODEV)
			return 0;
		else
			return err;
	}

	err = asus_wmi_get_devstate_simple(asus, ASUS_WMI_DEVID_KBD_RGB2);
	if (err) {
		if (err == -ENODEV)
			return 0;
		else
			return err;
	}

	asus->kbbl_rgb_available = true;
	return sysfs_create_group(&asus->platform_device->dev.kobj,
			&kbbl_attribute_group);
}

static void kbbl_rgb_exit(struct asus_wmi *asus)
{
	if (asus->kbbl_rgb_available) {
		sysfs_remove_group(&asus->platform_device->dev.kobj,
				&kbbl_attribute_group);
	}
}

/* RF *************************************************************************/

/*
 * PCI hotplug (for wlan rfkill)
 */
static bool asus_wlan_rfkill_blocked(struct asus_wmi *asus)
{
	int result = asus_wmi_get_devstate_simple(asus, ASUS_WMI_DEVID_WLAN);

	if (result < 0)
		return false;
	return !result;
}

static void asus_rfkill_hotplug(struct asus_wmi *asus)
{
	struct pci_dev *dev;
	struct pci_bus *bus;
	bool blocked;
	bool absent;
	u32 l;

	mutex_lock(&asus->wmi_lock);
	blocked = asus_wlan_rfkill_blocked(asus);
	mutex_unlock(&asus->wmi_lock);

	mutex_lock(&asus->hotplug_lock);
	pci_lock_rescan_remove();

	if (asus->wlan.rfkill)
		rfkill_set_sw_state(asus->wlan.rfkill, blocked);

	if (asus->hotplug_slot.ops) {
		bus = pci_find_bus(0, 1);
		if (!bus) {
			pr_warn("Unable to find PCI bus 1?\n");
			goto out_unlock;
		}

		if (pci_bus_read_config_dword(bus, 0, PCI_VENDOR_ID, &l)) {
			pr_err("Unable to read PCI config space?\n");
			goto out_unlock;
		}
		absent = (l == 0xffffffff);

		if (blocked != absent) {
			pr_warn("BIOS says wireless lan is %s, "
				"but the pci device is %s\n",
				blocked ? "blocked" : "unblocked",
				absent ? "absent" : "present");
			pr_warn("skipped wireless hotplug as probably "
				"inappropriate for this model\n");
			goto out_unlock;
		}

		if (!blocked) {
			dev = pci_get_slot(bus, 0);
			if (dev) {
				/* Device already present */
				pci_dev_put(dev);
				goto out_unlock;
			}
			dev = pci_scan_single_device(bus, 0);
			if (dev) {
				pci_bus_assign_resources(bus);
				pci_bus_add_device(dev);
			}
		} else {
			dev = pci_get_slot(bus, 0);
			if (dev) {
				pci_stop_and_remove_bus_device(dev);
				pci_dev_put(dev);
			}
		}
	}

out_unlock:
	pci_unlock_rescan_remove();
	mutex_unlock(&asus->hotplug_lock);
}

static void asus_rfkill_notify(acpi_handle handle, u32 event, void *data)
{
	struct asus_wmi *asus = data;

	if (event != ACPI_NOTIFY_BUS_CHECK)
		return;

	/*
	 * We can't call directly asus_rfkill_hotplug because most
	 * of the time WMBC is still being executed and not reetrant.
	 * There is currently no way to tell ACPICA that  we want this
	 * method to be serialized, we schedule a asus_rfkill_hotplug
	 * call later, in a safer context.
	 */
	queue_work(asus->hotplug_workqueue, &asus->hotplug_work);
}

static int asus_register_rfkill_notifier(struct asus_wmi *asus, char *node)
{
	acpi_status status;
	acpi_handle handle;

	status = acpi_get_handle(NULL, node, &handle);
	if (ACPI_FAILURE(status))
		return -ENODEV;

	status = acpi_install_notify_handler(handle, ACPI_SYSTEM_NOTIFY,
					     asus_rfkill_notify, asus);
	if (ACPI_FAILURE(status))
		pr_warn("Failed to register notify on %s\n", node);

	return 0;
}

static void asus_unregister_rfkill_notifier(struct asus_wmi *asus, char *node)
{
	acpi_status status = AE_OK;
	acpi_handle handle;

	status = acpi_get_handle(NULL, node, &handle);
	if (ACPI_FAILURE(status))
		return;

	status = acpi_remove_notify_handler(handle, ACPI_SYSTEM_NOTIFY,
					    asus_rfkill_notify);
	if (ACPI_FAILURE(status))
		pr_err("Error removing rfkill notify handler %s\n", node);
}

static int asus_get_adapter_status(struct hotplug_slot *hotplug_slot,
				   u8 *value)
{
	struct asus_wmi *asus = container_of(hotplug_slot,
					     struct asus_wmi, hotplug_slot);
	int result = asus_wmi_get_devstate_simple(asus, ASUS_WMI_DEVID_WLAN);

	if (result < 0)
		return result;

	*value = !!result;
	return 0;
}

static const struct hotplug_slot_ops asus_hotplug_slot_ops = {
	.get_adapter_status = asus_get_adapter_status,
	.get_power_status = asus_get_adapter_status,
};

static void asus_hotplug_work(struct work_struct *work)
{
	struct asus_wmi *asus;

	asus = container_of(work, struct asus_wmi, hotplug_work);
	asus_rfkill_hotplug(asus);
}

static int asus_setup_pci_hotplug(struct asus_wmi *asus)
{
	int ret = -ENOMEM;
	struct pci_bus *bus = pci_find_bus(0, 1);

	if (!bus) {
		pr_err("Unable to find wifi PCI bus\n");
		return -ENODEV;
	}

	asus->hotplug_workqueue =
	    create_singlethread_workqueue("hotplug_workqueue");
	if (!asus->hotplug_workqueue)
		goto error_workqueue;

	INIT_WORK(&asus->hotplug_work, asus_hotplug_work);

	asus->hotplug_slot.ops = &asus_hotplug_slot_ops;

	ret = pci_hp_register(&asus->hotplug_slot, bus, 0, "asus-wifi");
	if (ret) {
		pr_err("Unable to register hotplug slot - %d\n", ret);
		goto error_register;
	}

	return 0;

error_register:
	asus->hotplug_slot.ops = NULL;
	destroy_workqueue(asus->hotplug_workqueue);
error_workqueue:
	return ret;
}

/*
 * Rfkill devices
 */
static int asus_rfkill_set(void *data, bool blocked)
{
	struct asus_rfkill *priv = data;
	u32 ctrl_param = !blocked;
	u32 dev_id = priv->dev_id;

	/*
	 * If the user bit is set, BIOS can't set and record the wlan status,
	 * it will report the value read from id ASUS_WMI_DEVID_WLAN_LED
	 * while we query the wlan status through WMI(ASUS_WMI_DEVID_WLAN).
	 * So, we have to record wlan status in id ASUS_WMI_DEVID_WLAN_LED
	 * while setting the wlan status through WMI.
	 * This is also the behavior that windows app will do.
	 */
	if ((dev_id == ASUS_WMI_DEVID_WLAN) &&
	     priv->asus->driver->wlan_ctrl_by_user)
		dev_id = ASUS_WMI_DEVID_WLAN_LED;

	return asus_wmi_set_devstate(dev_id, ctrl_param, NULL);
}

static void asus_rfkill_query(struct rfkill *rfkill, void *data)
{
	struct asus_rfkill *priv = data;
	int result;

	result = asus_wmi_get_devstate_simple(priv->asus, priv->dev_id);

	if (result < 0)
		return;

	rfkill_set_sw_state(priv->rfkill, !result);
}

static int asus_rfkill_wlan_set(void *data, bool blocked)
{
	struct asus_rfkill *priv = data;
	struct asus_wmi *asus = priv->asus;
	int ret;

	/*
	 * This handler is enabled only if hotplug is enabled.
	 * In this case, the asus_wmi_set_devstate() will
	 * trigger a wmi notification and we need to wait
	 * this call to finish before being able to call
	 * any wmi method
	 */
	mutex_lock(&asus->wmi_lock);
	ret = asus_rfkill_set(data, blocked);
	mutex_unlock(&asus->wmi_lock);
	return ret;
}

static const struct rfkill_ops asus_rfkill_wlan_ops = {
	.set_block = asus_rfkill_wlan_set,
	.query = asus_rfkill_query,
};

static const struct rfkill_ops asus_rfkill_ops = {
	.set_block = asus_rfkill_set,
	.query = asus_rfkill_query,
};

static int asus_new_rfkill(struct asus_wmi *asus,
			   struct asus_rfkill *arfkill,
			   const char *name, enum rfkill_type type, int dev_id)
{
	int result = asus_wmi_get_devstate_simple(asus, dev_id);
	struct rfkill **rfkill = &arfkill->rfkill;

	if (result < 0)
		return result;

	arfkill->dev_id = dev_id;
	arfkill->asus = asus;

	if (dev_id == ASUS_WMI_DEVID_WLAN &&
	    asus->driver->quirks->hotplug_wireless)
		*rfkill = rfkill_alloc(name, &asus->platform_device->dev, type,
				       &asus_rfkill_wlan_ops, arfkill);
	else
		*rfkill = rfkill_alloc(name, &asus->platform_device->dev, type,
				       &asus_rfkill_ops, arfkill);

	if (!*rfkill)
		return -EINVAL;

	if ((dev_id == ASUS_WMI_DEVID_WLAN) &&
			(asus->driver->quirks->wapf > 0))
		rfkill_set_led_trigger_name(*rfkill, "asus-wlan");

	rfkill_init_sw_state(*rfkill, !result);
	result = rfkill_register(*rfkill);
	if (result) {
		rfkill_destroy(*rfkill);
		*rfkill = NULL;
		return result;
	}
	return 0;
}

static void asus_wmi_rfkill_exit(struct asus_wmi *asus)
{
	if (asus->driver->wlan_ctrl_by_user && ashs_present())
		return;

	asus_unregister_rfkill_notifier(asus, "\\_SB.PCI0.P0P5");
	asus_unregister_rfkill_notifier(asus, "\\_SB.PCI0.P0P6");
	asus_unregister_rfkill_notifier(asus, "\\_SB.PCI0.P0P7");
	if (asus->wlan.rfkill) {
		rfkill_unregister(asus->wlan.rfkill);
		rfkill_destroy(asus->wlan.rfkill);
		asus->wlan.rfkill = NULL;
	}
	/*
	 * Refresh pci hotplug in case the rfkill state was changed after
	 * asus_unregister_rfkill_notifier()
	 */
	asus_rfkill_hotplug(asus);
	if (asus->hotplug_slot.ops)
		pci_hp_deregister(&asus->hotplug_slot);
	if (asus->hotplug_workqueue)
		destroy_workqueue(asus->hotplug_workqueue);

	if (asus->bluetooth.rfkill) {
		rfkill_unregister(asus->bluetooth.rfkill);
		rfkill_destroy(asus->bluetooth.rfkill);
		asus->bluetooth.rfkill = NULL;
	}
	if (asus->wimax.rfkill) {
		rfkill_unregister(asus->wimax.rfkill);
		rfkill_destroy(asus->wimax.rfkill);
		asus->wimax.rfkill = NULL;
	}
	if (asus->wwan3g.rfkill) {
		rfkill_unregister(asus->wwan3g.rfkill);
		rfkill_destroy(asus->wwan3g.rfkill);
		asus->wwan3g.rfkill = NULL;
	}
	if (asus->gps.rfkill) {
		rfkill_unregister(asus->gps.rfkill);
		rfkill_destroy(asus->gps.rfkill);
		asus->gps.rfkill = NULL;
	}
	if (asus->uwb.rfkill) {
		rfkill_unregister(asus->uwb.rfkill);
		rfkill_destroy(asus->uwb.rfkill);
		asus->uwb.rfkill = NULL;
	}
}

static int asus_wmi_rfkill_init(struct asus_wmi *asus)
{
	int result = 0;

	mutex_init(&asus->hotplug_lock);
	mutex_init(&asus->wmi_lock);

	result = asus_new_rfkill(asus, &asus->wlan, "asus-wlan",
				 RFKILL_TYPE_WLAN, ASUS_WMI_DEVID_WLAN);

	if (result && result != -ENODEV)
		goto exit;

	result = asus_new_rfkill(asus, &asus->bluetooth,
				 "asus-bluetooth", RFKILL_TYPE_BLUETOOTH,
				 ASUS_WMI_DEVID_BLUETOOTH);

	if (result && result != -ENODEV)
		goto exit;

	result = asus_new_rfkill(asus, &asus->wimax, "asus-wimax",
				 RFKILL_TYPE_WIMAX, ASUS_WMI_DEVID_WIMAX);

	if (result && result != -ENODEV)
		goto exit;

	result = asus_new_rfkill(asus, &asus->wwan3g, "asus-wwan3g",
				 RFKILL_TYPE_WWAN, ASUS_WMI_DEVID_WWAN3G);

	if (result && result != -ENODEV)
		goto exit;

	result = asus_new_rfkill(asus, &asus->gps, "asus-gps",
				 RFKILL_TYPE_GPS, ASUS_WMI_DEVID_GPS);

	if (result && result != -ENODEV)
		goto exit;

	result = asus_new_rfkill(asus, &asus->uwb, "asus-uwb",
				 RFKILL_TYPE_UWB, ASUS_WMI_DEVID_UWB);

	if (result && result != -ENODEV)
		goto exit;

	if (!asus->driver->quirks->hotplug_wireless)
		goto exit;

	result = asus_setup_pci_hotplug(asus);
	/*
	 * If we get -EBUSY then something else is handling the PCI hotplug -
	 * don't fail in this case
	 */
	if (result == -EBUSY)
		result = 0;

	asus_register_rfkill_notifier(asus, "\\_SB.PCI0.P0P5");
	asus_register_rfkill_notifier(asus, "\\_SB.PCI0.P0P6");
	asus_register_rfkill_notifier(asus, "\\_SB.PCI0.P0P7");
	/*
	 * Refresh pci hotplug in case the rfkill state was changed during
	 * setup.
	 */
	asus_rfkill_hotplug(asus);

exit:
	if (result && result != -ENODEV)
		asus_wmi_rfkill_exit(asus);

	if (result == -ENODEV)
		result = 0;

	return result;
}

/* Quirks *********************************************************************/

static void asus_wmi_set_xusb2pr(struct asus_wmi *asus)
{
	struct pci_dev *xhci_pdev;
	u32 orig_ports_available;
	u32 ports_available = asus->driver->quirks->xusb2pr;

	xhci_pdev = pci_get_device(PCI_VENDOR_ID_INTEL,
			PCI_DEVICE_ID_INTEL_LYNXPOINT_LP_XHCI,
			NULL);

	if (!xhci_pdev)
		return;

	pci_read_config_dword(xhci_pdev, USB_INTEL_XUSB2PR,
				&orig_ports_available);

	pci_write_config_dword(xhci_pdev, USB_INTEL_XUSB2PR,
				cpu_to_le32(ports_available));

	pr_info("set USB_INTEL_XUSB2PR old: 0x%04x, new: 0x%04x\n",
			orig_ports_available, ports_available);
}

/*
 * Some devices dont support or have borcken get_als method
 * but still support set method.
 */
static void asus_wmi_set_als(void)
{
	asus_wmi_set_devstate(ASUS_WMI_DEVID_ALS_ENABLE, 1, NULL);
}

/* Hwmon device ***************************************************************/

static int asus_agfn_fan_speed_read(struct asus_wmi *asus, int fan,
					  int *speed)
{
	struct agfn_fan_args args = {
		.agfn.len = sizeof(args),
		.agfn.mfun = ASUS_FAN_MFUN,
		.agfn.sfun = ASUS_FAN_SFUN_READ,
		.fan = fan,
		.speed = 0,
	};
	struct acpi_buffer input = { (acpi_size) sizeof(args), &args };
	int status;

	if (fan != 1)
		return -EINVAL;

	status = asus_wmi_evaluate_method_agfn(input);

	if (status || args.agfn.err)
		return -ENXIO;

	if (speed)
		*speed = args.speed;

	return 0;
}

static int asus_agfn_fan_speed_write(struct asus_wmi *asus, int fan,
				     int *speed)
{
	struct agfn_fan_args args = {
		.agfn.len = sizeof(args),
		.agfn.mfun = ASUS_FAN_MFUN,
		.agfn.sfun = ASUS_FAN_SFUN_WRITE,
		.fan = fan,
		.speed = speed ?  *speed : 0,
	};
	struct acpi_buffer input = { (acpi_size) sizeof(args), &args };
	int status;

	/* 1: for setting 1st fan's speed 0: setting auto mode */
	if (fan != 1 && fan != 0)
		return -EINVAL;

	status = asus_wmi_evaluate_method_agfn(input);

	if (status || args.agfn.err)
		return -ENXIO;

	if (speed && fan == 1)
		asus->agfn_pwm = *speed;

	return 0;
}

/*
 * Check if we can read the speed of one fan. If true we assume we can also
 * control it.
 */
static bool asus_wmi_has_agfn_fan(struct asus_wmi *asus)
{
	int status;
	int speed;
	u32 value;

	status = asus_agfn_fan_speed_read(asus, 1, &speed);
	if (status != 0)
		return false;

	status = asus_wmi_get_devstate(asus, ASUS_WMI_DEVID_FAN_CTRL, &value);
	if (status != 0)
		return false;

	/*
	 * We need to find a better way, probably using sfun,
	 * bits or spec ...
	 * Currently we disable it if:
	 * - ASUS_WMI_UNSUPPORTED_METHOD is returned
	 * - reverved bits are non-zero
	 * - sfun and presence bit are not set
	 */
	return !(value == ASUS_WMI_UNSUPPORTED_METHOD || value & 0xFFF80000
		 || (!asus->sfun && !(value & ASUS_WMI_DSTS_PRESENCE_BIT)));
}

static int asus_fan_set_auto(struct asus_wmi *asus)
{
	int status;
	u32 retval;

	switch (asus->fan_type) {
	case FAN_TYPE_SPEC83:
		status = asus_wmi_set_devstate(ASUS_WMI_DEVID_CPU_FAN_CTRL,
					       0, &retval);
		if (status)
			return status;

		if (retval != 1)
			return -EIO;
		break;

	case FAN_TYPE_AGFN:
		status = asus_agfn_fan_speed_write(asus, 0, NULL);
		if (status)
			return -ENXIO;
		break;

	default:
		return -ENXIO;
	}


	return 0;
}

static ssize_t pwm1_show(struct device *dev,
			       struct device_attribute *attr,
			       char *buf)
{
	struct asus_wmi *asus = dev_get_drvdata(dev);
	int err;
	int value;

	/* If we already set a value then just return it */
	if (asus->agfn_pwm >= 0)
		return sprintf(buf, "%d\n", asus->agfn_pwm);

	/*
	 * If we haven't set already set a value through the AGFN interface,
	 * we read a current value through the (now-deprecated) FAN_CTRL device.
	 */
	err = asus_wmi_get_devstate(asus, ASUS_WMI_DEVID_FAN_CTRL, &value);
	if (err < 0)
		return err;

	value &= 0xFF;

	if (value == 1) /* Low Speed */
		value = 85;
	else if (value == 2)
		value = 170;
	else if (value == 3)
		value = 255;
	else if (value) {
		pr_err("Unknown fan speed %#x\n", value);
		value = -1;
	}

	return sprintf(buf, "%d\n", value);
}

static ssize_t pwm1_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count) {
	struct asus_wmi *asus = dev_get_drvdata(dev);
	int value;
	int state;
	int ret;

	ret = kstrtouint(buf, 10, &value);
	if (ret)
		return ret;

	value = clamp(value, 0, 255);

	state = asus_agfn_fan_speed_write(asus, 1, &value);
	if (state)
		pr_warn("Setting fan speed failed: %d\n", state);
	else
		asus->fan_pwm_mode = ASUS_FAN_CTRL_MANUAL;

	return count;
}

static ssize_t fan1_input_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct asus_wmi *asus = dev_get_drvdata(dev);
	int value;
	int ret;

	switch (asus->fan_type) {
	case FAN_TYPE_SPEC83:
		ret = asus_wmi_get_devstate(asus, ASUS_WMI_DEVID_CPU_FAN_CTRL,
					    &value);
		if (ret < 0)
			return ret;

		value &= 0xffff;
		break;

	case FAN_TYPE_AGFN:
		/* no speed readable on manual mode */
		if (asus->fan_pwm_mode == ASUS_FAN_CTRL_MANUAL)
			return -ENXIO;

		ret = asus_agfn_fan_speed_read(asus, 1, &value);
		if (ret) {
			pr_warn("reading fan speed failed: %d\n", ret);
			return -ENXIO;
		}
		break;

	default:
		return -ENXIO;
	}

	return sprintf(buf, "%d\n", value < 0 ? -1 : value*100);
}

static ssize_t fan2_input_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct asus_wmi *asus = dev_get_drvdata(dev);
	int value;
	int ret;

	switch (asus->fan_type) {
	case FAN_TYPE_SPEC83:
		ret = asus_wmi_get_devstate(asus, ASUS_WMI_DEVID_GPU_FAN_CTRL,
					    &value);
		if (ret < 0)
			return ret;

		value &= 0xffff;
		break;

	case FAN_TYPE_AGFN:
		/* no speed readable on manual mode */
		if (asus->fan_pwm_mode == ASUS_FAN_CTRL_MANUAL)
			return -ENXIO;

		ret = asus_agfn_fan_speed_read(asus, 1, &value);
		if (ret) {
			pr_warn("reading fan speed failed: %d\n", ret);
			return -ENXIO;
		}
		break;

	default:
		return -ENXIO;
	}

	return sprintf(buf, "%d\n", value < 0 ? -1 : value*100);
}

static ssize_t pwm1_enable_show(struct device *dev,
						 struct device_attribute *attr,
						 char *buf)
{
	struct asus_wmi *asus = dev_get_drvdata(dev);

	/*
	 * Just read back the cached pwm mode.
	 *
	 * For the CPU_FAN device, the spec indicates that we should be
	 * able to read the device status and consult bit 19 to see if we
	 * are in Full On or Automatic mode. However, this does not work
	 * in practice on X532FL at least (the bit is always 0) and there's
	 * also nothing in the DSDT to indicate that this behaviour exists.
	 */
	return sprintf(buf, "%d\n", asus->fan_pwm_mode);
}

static ssize_t pwm1_enable_store(struct device *dev,
						  struct device_attribute *attr,
						  const char *buf, size_t count)
{
	struct asus_wmi *asus = dev_get_drvdata(dev);
	int status = 0;
	int state;
	int value;
	int ret;
	u32 retval;

	ret = kstrtouint(buf, 10, &state);
	if (ret)
		return ret;

	if (asus->fan_type == FAN_TYPE_SPEC83) {
		switch (state) { /* standard documented hwmon values */
		case ASUS_FAN_CTRL_FULLSPEED:
			value = 1;
			break;
		case ASUS_FAN_CTRL_AUTO:
			value = 0;
			break;
		default:
			return -EINVAL;
		}

		ret = asus_wmi_set_devstate(ASUS_WMI_DEVID_CPU_FAN_CTRL,
					    value, &retval);
		if (ret)
			return ret;

		if (retval != 1)
			return -EIO;
	} else if (asus->fan_type == FAN_TYPE_AGFN) {
		switch (state) {
		case ASUS_FAN_CTRL_MANUAL:
			break;

		case ASUS_FAN_CTRL_AUTO:
			status = asus_fan_set_auto(asus);
			if (status)
				return status;
			break;

		default:
			return -EINVAL;
		}
	}

	asus->fan_pwm_mode = state;
	return count;
}

static ssize_t fan1_label_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	return sprintf(buf, "%s\n", ASUS_CPU_FAN_DESC);
}

static ssize_t fan2_label_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	return sprintf(buf, "%s\n", ASUS_GPU_FAN_DESC);
}

static ssize_t asus_hwmon_temp1(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	struct asus_wmi *asus = dev_get_drvdata(dev);
	u32 value;
	int err;

	err = asus_wmi_get_devstate(asus, ASUS_WMI_DEVID_THERMAL_CTRL, &value);
	if (err < 0)
		return err;

	return sprintf(buf, "%ld\n",
		       deci_kelvin_to_millicelsius(value & 0xFFFF));
}

/* Fan1 */
static DEVICE_ATTR_RW(pwm1);
static DEVICE_ATTR_RW(pwm1_enable);
static DEVICE_ATTR_RO(fan1_input);
static DEVICE_ATTR_RO(fan1_label);

/* Fan2 */
static DEVICE_ATTR_RO(fan2_input);
static DEVICE_ATTR_RO(fan2_label);

/* Temperature */
static DEVICE_ATTR(temp1_input, S_IRUGO, asus_hwmon_temp1, NULL);

static struct attribute *hwmon_attributes[] = {
	&dev_attr_pwm1.attr,
	&dev_attr_pwm1_enable.attr,
	&dev_attr_fan1_input.attr,
	&dev_attr_fan1_label.attr,
	&dev_attr_fan2_input.attr,
        &dev_attr_fan2_label.attr,


	&dev_attr_temp1_input.attr,
	NULL
};

static umode_t asus_hwmon_sysfs_is_visible(struct kobject *kobj,
					  struct attribute *attr, int idx)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct asus_wmi *asus = dev_get_drvdata(dev->parent);
	u32 value = ASUS_WMI_UNSUPPORTED_METHOD;

	if (attr == &dev_attr_pwm1.attr) {
		if (asus->fan_type != FAN_TYPE_AGFN)
			return 0;
	} else if (attr == &dev_attr_fan1_input.attr
	    || attr == &dev_attr_fan1_label.attr
	    || attr == &dev_attr_fan2_input.attr
	    || attr == &dev_attr_fan2_label.attr
	    || attr == &dev_attr_pwm1_enable.attr) {
		if (asus->fan_type == FAN_TYPE_NONE)
			return 0;
	} else if (attr == &dev_attr_temp1_input.attr) {
		int err = asus_wmi_get_devstate(asus,
						ASUS_WMI_DEVID_THERMAL_CTRL,
						&value);

		if (err < 0)
			return 0; /* can't return negative here */

		/*
		 * If the temperature value in deci-Kelvin is near the absolute
		 * zero temperature, something is clearly wrong
		 */
		if (value == 0 || value == 1)
			return 0;
	}

	return attr->mode;
}

static const struct attribute_group hwmon_attribute_group = {
	.is_visible = asus_hwmon_sysfs_is_visible,
	.attrs = hwmon_attributes
};
__ATTRIBUTE_GROUPS(hwmon_attribute);

static int asus_wmi_hwmon_init(struct asus_wmi *asus)
{
	struct device *dev = &asus->platform_device->dev;
	struct device *hwmon;

	hwmon = devm_hwmon_device_register_with_groups(dev, "asus", asus,
			hwmon_attribute_groups);

	if (IS_ERR(hwmon)) {
		pr_err("Could not register asus hwmon device\n");
		return PTR_ERR(hwmon);
	}
	return 0;
}

static int asus_wmi_fan_init(struct asus_wmi *asus)
{
	asus->fan_type = FAN_TYPE_NONE;
	asus->agfn_pwm = -1;

	if (asus_wmi_dev_is_present(asus, ASUS_WMI_DEVID_CPU_FAN_CTRL))
		asus->fan_type = FAN_TYPE_SPEC83;
	else if (asus_wmi_has_agfn_fan(asus))
		asus->fan_type = FAN_TYPE_AGFN;
	else
		asus->fan_type = FAN_TYPE_SPEC83;
	if (asus->fan_type == FAN_TYPE_NONE)
		return -ENODEV;

	asus_fan_set_auto(asus);
	asus->fan_pwm_mode = ASUS_FAN_CTRL_AUTO;
	return 0;
}

/* Fan mode *******************************************************************/

static int fan_boost_mode_check_present(struct asus_wmi *asus)
{
	u32 result;
	int err;

	asus->fan_boost_mode_available = false;

	err = asus_wmi_get_devstate(asus, ASUS_WMI_DEVID_FAN_BOOST_MODE,
				    &result);
	if (err) {
		if (err == -ENODEV)
			return 0;
		else
			return err;
	}

	if ((result & ASUS_WMI_DSTS_PRESENCE_BIT) &&
			(result & ASUS_FAN_BOOST_MODES_MASK)) {
		asus->fan_boost_mode_available = true;
		asus->fan_boost_mode_mask = result & ASUS_FAN_BOOST_MODES_MASK;
	}

	return 0;
}

static int fan_boost_mode_write(struct asus_wmi *asus)
{
	int err;
	u8 value;
	u32 retval;

	value = asus->fan_boost_mode;

	pr_info("Set fan boost mode: %u\n", value);
	err = asus_wmi_set_devstate(ASUS_WMI_DEVID_FAN_BOOST_MODE, value,
				    &retval);

	sysfs_notify(&asus->platform_device->dev.kobj, NULL,
			"fan_boost_mode");

	if (err) {
		pr_warn("Failed to set fan boost mode: %d\n", err);
		return err;
	}

	if (retval != 1) {
		pr_warn("Failed to set fan boost mode (retval): 0x%x\n",
			retval);
		return -EIO;
	}

	return 0;
}

static int fan_boost_mode_switch_next(struct asus_wmi *asus)
{
	u8 mask = asus->fan_boost_mode_mask;

	if (asus->fan_boost_mode == ASUS_FAN_BOOST_MODE_NORMAL) {
		if (mask & ASUS_FAN_BOOST_MODE_OVERBOOST_MASK)
			asus->fan_boost_mode = ASUS_FAN_BOOST_MODE_OVERBOOST;
		else if (mask & ASUS_FAN_BOOST_MODE_SILENT_MASK)
			asus->fan_boost_mode = ASUS_FAN_BOOST_MODE_SILENT;
	} else if (asus->fan_boost_mode == ASUS_FAN_BOOST_MODE_OVERBOOST) {
		if (mask & ASUS_FAN_BOOST_MODE_SILENT_MASK)
			asus->fan_boost_mode = ASUS_FAN_BOOST_MODE_SILENT;
		else
			asus->fan_boost_mode = ASUS_FAN_BOOST_MODE_NORMAL;
	} else {
		asus->fan_boost_mode = ASUS_FAN_BOOST_MODE_NORMAL;
	}

	return fan_boost_mode_write(asus);
}

static ssize_t fan_boost_mode_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct asus_wmi *asus = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%d\n", asus->fan_boost_mode);
}

static ssize_t fan_boost_mode_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	u8 new_mode;
	struct asus_wmi *asus = dev_get_drvdata(dev);
	u8 mask = asus->fan_boost_mode_mask;

	int successfully_converted = kstrtou8(buf, 10, &new_mode);
	if (successfully_converted < 0) {
		pr_warn("Trying to store invalid value\n");
		return -EINVAL;
	}

	if (new_mode == ASUS_FAN_BOOST_MODE_OVERBOOST) {
		if (!(mask & ASUS_FAN_BOOST_MODE_OVERBOOST_MASK))
			return -EINVAL;
	} else if (new_mode == ASUS_FAN_BOOST_MODE_SILENT) {
		if (!(mask & ASUS_FAN_BOOST_MODE_SILENT_MASK))
			return -EINVAL;
	} else if (new_mode != ASUS_FAN_BOOST_MODE_NORMAL) {
		return -EINVAL;
	}

	asus->fan_boost_mode = new_mode;
	fan_boost_mode_write(asus);

	return count;
}

// Fan boost mode: 0 - normal, 1 - overboost, 2 - silent
static DEVICE_ATTR_RW(fan_boost_mode);

/* Throttle thermal policy ****************************************************/

static int throttle_thermal_policy_check_present(struct asus_wmi *asus)
{
	u32 result;
	int err;

	asus->throttle_thermal_policy_available = false;

	err = asus_wmi_get_devstate(asus,
				    ASUS_WMI_DEVID_THROTTLE_THERMAL_POLICY,
				    &result);
	if (err) {
		if (err == -ENODEV)
			return 0;
		return err;
	}

	if (result & ASUS_WMI_DSTS_PRESENCE_BIT)
		asus->throttle_thermal_policy_available = true;

	return 0;
}

static int throttle_thermal_policy_write(struct asus_wmi *asus)
{
	int err;
	u8 value;
	u32 retval;

	value = asus->throttle_thermal_policy_mode;

	err = asus_wmi_set_devstate(ASUS_WMI_DEVID_THROTTLE_THERMAL_POLICY,
				    value, &retval);

	sysfs_notify(&asus->platform_device->dev.kobj, NULL,
			"throttle_thermal_policy");

	if (err) {
		pr_warn("Failed to set throttle thermal policy: %d\n", err);
		return err;
	}

	if (retval != 1) {
		pr_warn("Failed to set throttle thermal policy (retval): 0x%x\n",
			retval);
		return -EIO;
	}

	return 0;
}

static int throttle_thermal_policy_set_default(struct asus_wmi *asus)
{
	if (!asus->throttle_thermal_policy_available)
		return 0;

	asus->throttle_thermal_policy_mode = ASUS_THROTTLE_THERMAL_POLICY_DEFAULT;
	return throttle_thermal_policy_write(asus);
}

static int throttle_thermal_policy_switch_next(struct asus_wmi *asus)
{
	u8 new_mode = asus->throttle_thermal_policy_mode + 1;

	if (new_mode > ASUS_THROTTLE_THERMAL_POLICY_SILENT)
		new_mode = ASUS_THROTTLE_THERMAL_POLICY_DEFAULT;

	asus->throttle_thermal_policy_mode = new_mode;
	return throttle_thermal_policy_write(asus);
}

static ssize_t throttle_thermal_policy_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct asus_wmi *asus = dev_get_drvdata(dev);
	u8 mode = asus->throttle_thermal_policy_mode;

	return scnprintf(buf, PAGE_SIZE, "%d\n", mode);
}

static ssize_t throttle_thermal_policy_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	int result;
	u8 new_mode;
	struct asus_wmi *asus = dev_get_drvdata(dev);

	result = kstrtou8(buf, 10, &new_mode);
	if (result < 0)
		return result;

	if (new_mode > ASUS_THROTTLE_THERMAL_POLICY_SILENT)
		return -EINVAL;

	asus->throttle_thermal_policy_mode = new_mode;
	throttle_thermal_policy_write(asus);

	return count;
}

// Throttle thermal policy: 0 - default, 1 - overboost, 2 - silent
static DEVICE_ATTR_RW(throttle_thermal_policy);

/* Backlight ******************************************************************/

static int read_backlight_power(struct asus_wmi *asus)
{
	int ret;

	if (asus->driver->quirks->store_backlight_power)
		ret = !asus->driver->panel_power;
	else
		ret = asus_wmi_get_devstate_simple(asus,
						   ASUS_WMI_DEVID_BACKLIGHT);

	if (ret < 0)
		return ret;

	return ret ? FB_BLANK_UNBLANK : FB_BLANK_POWERDOWN;
}

static int read_brightness_max(struct asus_wmi *asus)
{
	u32 retval;
	int err;

	err = asus_wmi_get_devstate(asus, ASUS_WMI_DEVID_BRIGHTNESS, &retval);
	if (err < 0)
		return err;

	retval = retval & ASUS_WMI_DSTS_MAX_BRIGTH_MASK;
	retval >>= 8;

	if (!retval)
		return -ENODEV;

	return retval;
}

static int read_brightness(struct backlight_device *bd)
{
	struct asus_wmi *asus = bl_get_data(bd);
	u32 retval;
	int err;

	err = asus_wmi_get_devstate(asus, ASUS_WMI_DEVID_BRIGHTNESS, &retval);
	if (err < 0)
		return err;

	return retval & ASUS_WMI_DSTS_BRIGHTNESS_MASK;
}

static u32 get_scalar_command(struct backlight_device *bd)
{
	struct asus_wmi *asus = bl_get_data(bd);
	u32 ctrl_param = 0;

	if ((asus->driver->brightness < bd->props.brightness) ||
	    bd->props.brightness == bd->props.max_brightness)
		ctrl_param = 0x00008001;
	else if ((asus->driver->brightness > bd->props.brightness) ||
		 bd->props.brightness == 0)
		ctrl_param = 0x00008000;

	asus->driver->brightness = bd->props.brightness;

	return ctrl_param;
}

static int update_bl_status(struct backlight_device *bd)
{
	struct asus_wmi *asus = bl_get_data(bd);
	u32 ctrl_param;
	int power, err = 0;

	power = read_backlight_power(asus);
	if (power != -ENODEV && bd->props.power != power) {
		ctrl_param = !!(bd->props.power == FB_BLANK_UNBLANK);
		err = asus_wmi_set_devstate(ASUS_WMI_DEVID_BACKLIGHT,
					    ctrl_param, NULL);
		if (asus->driver->quirks->store_backlight_power)
			asus->driver->panel_power = bd->props.power;

		/* When using scalar brightness, updating the brightness
		 * will mess with the backlight power */
		if (asus->driver->quirks->scalar_panel_brightness)
			return err;
	}

	if (asus->driver->quirks->scalar_panel_brightness)
		ctrl_param = get_scalar_command(bd);
	else
		ctrl_param = bd->props.brightness;

	err = asus_wmi_set_devstate(ASUS_WMI_DEVID_BRIGHTNESS,
				    ctrl_param, NULL);

	return err;
}

static const struct backlight_ops asus_wmi_bl_ops = {
	.get_brightness = read_brightness,
	.update_status = update_bl_status,
};

static int asus_wmi_backlight_notify(struct asus_wmi *asus, int code)
{
	struct backlight_device *bd = asus->backlight_device;
	int old = bd->props.brightness;
	int new = old;

	if (code >= NOTIFY_BRNUP_MIN && code <= NOTIFY_BRNUP_MAX)
		new = code - NOTIFY_BRNUP_MIN + 1;
	else if (code >= NOTIFY_BRNDOWN_MIN && code <= NOTIFY_BRNDOWN_MAX)
		new = code - NOTIFY_BRNDOWN_MIN;

	bd->props.brightness = new;
	backlight_update_status(bd);
	backlight_force_update(bd, BACKLIGHT_UPDATE_HOTKEY);

	return old;
}

static int asus_wmi_backlight_init(struct asus_wmi *asus)
{
	struct backlight_device *bd;
	struct backlight_properties props;
	int max;
	int power;

	max = read_brightness_max(asus);
	if (max < 0)
		return max;

	power = read_backlight_power(asus);
	if (power == -ENODEV)
		power = FB_BLANK_UNBLANK;
	else if (power < 0)
		return power;

	memset(&props, 0, sizeof(struct backlight_properties));
	props.type = BACKLIGHT_PLATFORM;
	props.max_brightness = max;
	bd = backlight_device_register(asus->driver->name,
				       &asus->platform_device->dev, asus,
				       &asus_wmi_bl_ops, &props);
	if (IS_ERR(bd)) {
		pr_err("Could not register backlight device\n");
		return PTR_ERR(bd);
	}

	asus->backlight_device = bd;

	if (asus->driver->quirks->store_backlight_power)
		asus->driver->panel_power = power;

	bd->props.brightness = read_brightness(bd);
	bd->props.power = power;
	backlight_update_status(bd);

	asus->driver->brightness = bd->props.brightness;

	return 0;
}

static void asus_wmi_backlight_exit(struct asus_wmi *asus)
{
	backlight_device_unregister(asus->backlight_device);

	asus->backlight_device = NULL;
}

static int is_display_toggle(int code)
{
	/* display toggle keys */
	if ((code >= 0x61 && code <= 0x67) ||
	    (code >= 0x8c && code <= 0x93) ||
	    (code >= 0xa0 && code <= 0xa7) ||
	    (code >= 0xd0 && code <= 0xd5))
		return 1;

	return 0;
}

/* Fn-lock ********************************************************************/

static bool asus_wmi_has_fnlock_key(struct asus_wmi *asus)
{
	u32 result;

	asus_wmi_get_devstate(asus, ASUS_WMI_DEVID_FNLOCK, &result);

	return (result & ASUS_WMI_DSTS_PRESENCE_BIT) &&
		!(result & ASUS_WMI_FNLOCK_BIOS_DISABLED);
}

static void asus_wmi_fnlock_update(struct asus_wmi *asus)
{
	int mode = asus->fnlock_locked;

	asus_wmi_set_devstate(ASUS_WMI_DEVID_FNLOCK, mode, NULL);
}

/* WMI events *****************************************************************/

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0))
static int asus_wmi_get_event_code(union acpi_object *obj)
{
	int code;

	if (obj && obj->type == ACPI_TYPE_INTEGER)
		code = (int)(obj->integer.value & WMI_EVENT_MASK);
	else
		code = -EIO;

	return code;
}
#else 
static int asus_wmi_get_event_code(u32 value)
{
	struct acpi_buffer response = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	acpi_status status;
	int code;

	status = wmi_get_event_data(value, &response);
	if (ACPI_FAILURE(status)) {
		pr_warn("Failed to get WMI notify code: %s\n",
				acpi_format_exception(status));
		return -EIO;
	}

	obj = (union acpi_object *)response.pointer;

	if (obj && obj->type == ACPI_TYPE_INTEGER)
		code = (int)(obj->integer.value & WMI_EVENT_MASK);
	else
		code = -EIO;

	kfree(obj);
	return code;
}
#endif

static void asus_wmi_handle_aura_event(struct asus_wmi *asus, int direction)
{
	int color1, color2, color3, speed;
	
	speed = (asus->kbbl_rgb.kbbl_auraspeed)? asus->kbbl_rgb.kbbl_auraspeed : 5; // default to 5
	asus->kbbl_rgb.kbbl_auramode = (asus->kbbl_rgb.kbbl_set_auramode <= 3)?
		asus->kbbl_rgb.kbbl_set_auramode : 0;

	if (asus->kbbl_rgb.kbbl_auramode == 2) {
		if (!direction && asus->kbbl_rgb.kbbl_speed+1 <= 2) {
			asus->kbbl_rgb.kbbl_set_speed = asus->kbbl_rgb.kbbl_speed+=1;
		} else if (!direction) {
			asus->kbbl_rgb.kbbl_set_speed = 0;
		}
		if (direction && asus->kbbl_rgb.kbbl_speed-1 >= 0) {
			asus->kbbl_rgb.kbbl_set_speed = asus->kbbl_rgb.kbbl_speed-=1;
		} else if (direction) {
			asus->kbbl_rgb.kbbl_set_speed = 2;
		}
	}

	if (asus->kbbl_rgb.kbbl_auramode == 1) {
		if (!direction && asus->kbbl_rgb.kbbl_mode+1 <= 3) {
			asus->kbbl_rgb.kbbl_set_mode = asus->kbbl_rgb.kbbl_mode+=1;
		} else if (!direction) {
			asus->kbbl_rgb.kbbl_set_mode = 0;
		}
		if (direction && asus->kbbl_rgb.kbbl_mode-1 >= 0) {
			asus->kbbl_rgb.kbbl_set_mode = asus->kbbl_rgb.kbbl_mode-=1;
		} else if (direction) {
			asus->kbbl_rgb.kbbl_set_mode = 3;
		}
	}		

	if (direction) { // LEFT
		color1 = asus->kbbl_rgb.kbbl_red;
		color2 = asus->kbbl_rgb.kbbl_green;
		color3 = asus->kbbl_rgb.kbbl_blue;
	} else { // RIGHT
		color1 = asus->kbbl_rgb.kbbl_red;
		color2 = asus->kbbl_rgb.kbbl_blue;
		color3 = asus->kbbl_rgb.kbbl_green;
	}

	if (asus->kbbl_rgb.kbbl_auramode == 3) {
		if (asus->kbbl_rgb.kbbl_mode == 2) // Don't run manual color cycle if keyboard mode is auto color cycle
			return;
		if (!direction) {
			if (color1 != 255 && color2 != 255 && color1+speed <= 255 && color2+speed <= 255) {
				color1+=speed;
				color2+=speed;
			} else if (color2 != 255 && color3 != 255 && color2+speed <= 255 && color3+speed <= 255) {
				color2+=speed;
				color3+=speed;
			} else if (color1 != 255 && color3 != 255 && color1+speed <= 255 && color3+speed <= 255) {
				color1+=speed;
				color3+=speed;
			} else if (color2 == 255 && color3 == 255 && color1+speed != 255) {
				color1+=speed;
			} else if (color1 == 255 && color3 == 255 && color2+speed != 255) {
				color2+=speed;
			} else if (color1 == 255 && color2 == 255 && color3+speed != 255) {
				color3+=speed;
			}
		}
		if ((direction) && color1 != 0 && color2 != 0 && color3 != 0) {
			if (color1 != 255)
				color1-=speed;
			if (color2 != 255)
				color2-=speed;
			if (color3 != 255)
				color3-=speed;
		}  else if (color1 == color2 && color2 == color3) {
			color1-=5; // let's just change 2 values...
			color2-=5;
		}

	} else if (!asus->kbbl_rgb.kbbl_auramode) {
		if (asus->kbbl_rgb.kbbl_mode == 2) // Don't run manual color cycle if keyboard mode is auto color cycle
			return;
		if (color1==255 && color2<255 && color3<255) {
			if (color3!=0 && color3-speed>=0) {
				color3-=speed;
			} else if (color2+speed <= 255) {
				color2+=speed;
			} else {
				color3 = 0;
				if (color2 != 0)
					color2 = 255;
			}
		} else if (color2==255 && color1==255) {
			color1 -= speed;
		} else if (color2==255 && color1<255 && color3<255) {
			if (color1==0 && color3+speed<=255) {
				color3+=speed;
			} else if (color1-speed >= 0) {
				color1-=speed;
			} else {
				color1 = 0;
				if (color3 != 0)
					color3 = 255;
			}
		} else if (color3==255 && color2==255) {
			color2-=speed;
		} else if (color3==255 && color1<255 && color2 < 255) {
			if (color2 == 0 && color1+speed<=255) {
				color1+=speed;
			} else if (color2-speed>=0) {
				color2-=speed;
			} else {
				color2 = 0;
				if (color1 != 0)
					color1 = 255;
			}
		} else if (color1== 255 && color3 == 255) {
			color3 -= speed;
		}
	}

	if (direction && (!asus->kbbl_rgb.kbbl_auramode
			|| asus->kbbl_rgb.kbbl_auramode == 3)) {
		asus->kbbl_rgb.kbbl_set_red = color1;
		asus->kbbl_rgb.kbbl_set_green  = color2;
		asus->kbbl_rgb.kbbl_set_blue = color3;
		//pr_info("RED: %d GREEN: %d BLUE: %d", color1, color2, color3);
	} else if (!direction && (!asus->kbbl_rgb.kbbl_auramode
			|| asus->kbbl_rgb.kbbl_auramode == 3)) {
		asus->kbbl_rgb.kbbl_set_red = color1;
		asus->kbbl_rgb.kbbl_set_blue  = color2;
		asus->kbbl_rgb.kbbl_set_green = color3;
		//pr_info("RED: %d GREEN: %d BLUE: %d", color1, color3, color2);
	}

	if (!asus->kbbl_rgb.kbbl_set_flags) {
		asus->kbbl_rgb.kbbl_set_flags = 42; // default to 2a...
		asus->kbbl_rgb.kbbl_set_red = 255; // initializaton
	}
	kbbl_rgb_write(asus, 1);
	return;
}

static void asus_wmi_handle_event_code(int code, struct asus_wmi *asus)
{
	unsigned int key_value = 1;
	bool autorelease = 1;
	int result, orig_code;

	orig_code = code;

	if (asus->driver->key_filter) {
		asus->driver->key_filter(asus->driver, &code, &key_value,
					 &autorelease);
		if (code == ASUS_WMI_KEY_IGNORE)
			return;
	}

	if (code >= NOTIFY_BRNUP_MIN && code <= NOTIFY_BRNUP_MAX)
		code = ASUS_WMI_BRN_UP;
	else if (code >= NOTIFY_BRNDOWN_MIN && code <= NOTIFY_BRNDOWN_MAX)
		code = ASUS_WMI_BRN_DOWN;

	if (code == ASUS_WMI_BRN_DOWN || code == ASUS_WMI_BRN_UP) {
		if (acpi_video_get_backlight_type() == acpi_backlight_vendor) {
			asus_wmi_backlight_notify(asus, orig_code);
			return;
		}
	}

	if (code == NOTIFY_KBD_BRTUP) {
		kbd_led_set_by_kbd(asus, asus->kbd_led_wk + 1);
		return;
	}
	if (code == NOTIFY_KBD_BRTDWN) {
		kbd_led_set_by_kbd(asus, asus->kbd_led_wk - 1);
		return;
	}


	if (code == NOTIFY_KBD_AURA_LFT) {
		asus_wmi_handle_aura_event(asus, 1);
		return;
	}
	if (code == NOTIFY_KBD_AURA_RGHT) {
		asus_wmi_handle_aura_event(asus, 0);
		return;
	}

	if (code == NOTIFY_KBD_BRTTOGGLE) {
		if (asus->kbd_led_wk == asus->kbd_led.max_brightness)
			kbd_led_set_by_kbd(asus, 0);
		else
			kbd_led_set_by_kbd(asus, asus->kbd_led_wk + 1);
		return;
	}

	if (code == NOTIFY_FNLOCK_TOGGLE) {
		asus->fnlock_locked = !asus->fnlock_locked;
		asus_wmi_fnlock_update(asus);
		return;
	}
	if (code == NOTIFY_WNDWSLOCK_TOGGLE) {
		return; // TODO: figure out the DEVID for this...
	}

	if (asus->driver->quirks->use_kbd_dock_devid && code == NOTIFY_KBD_DOCK_CHANGE) {
		result = asus_wmi_get_devstate_simple(asus,
						      ASUS_WMI_DEVID_KBD_DOCK);
		if (result >= 0) {
			input_report_switch(asus->inputdev, SW_TABLET_MODE,
					    !result);
			input_sync(asus->inputdev);
		}
		return;
	}

	if (asus->driver->quirks->use_lid_flip_devid && code == NOTIFY_LID_FLIP) {
		lid_flip_tablet_mode_get_state(asus);
		return;
	}

	if (asus->fan_boost_mode_available && code == NOTIFY_KBD_FBM) {
		fan_boost_mode_switch_next(asus);
		if (!report_key_events)
			return;
	}

	if (asus->throttle_thermal_policy_available && code == NOTIFY_KBD_TTP) {
		throttle_thermal_policy_switch_next(asus);
		if (!report_key_events)
			return;
	}

	if (is_display_toggle(code) && asus->driver->quirks->no_display_toggle)
		return;

	if (!sparse_keymap_report_event(asus->inputdev, code,
					key_value, autorelease))
		pr_info("Unknown key %x pressed\n", code);
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0))
static void asus_wmi_notify(union acpi_object *obj, void *context)
#else 
static void asus_wmi_notify(u32 value, void *context)
#endif
{
	struct asus_wmi *asus = context;
	int code;
	int i;

	for (i = 0; i < WMI_EVENT_QUEUE_SIZE + 1; i++) {
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0))
		code = asus_wmi_get_event_code(obj);
#else 
		code = asus_wmi_get_event_code(value);
#endif
		if (code < 0) {
			pr_warn("Failed to get notify code: %d\n", code);
			return;
		}

		if (code == WMI_EVENT_QUEUE_END || code == WMI_EVENT_MASK)
			return;

		asus_wmi_handle_event_code(code, asus);

		/*
		 * Double check that queue is present:
		 * ATK (with queue) uses 0xff, ASUSWMI (without) 0xd2.
		 */
		if (!asus->wmi_event_queue
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0))
			|| code != WMI_EVENT_VALUE_ATK)
#else 
			|| value != WMI_EVENT_VALUE_ATK)
#endif
			return;
	}

	pr_warn("Failed to process event queue, last code: 0x%x\n", code);
}

static int asus_wmi_notify_queue_flush(struct asus_wmi *asus)
{
	int code;
	int i;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0))
	union acpi_object *obj;
#endif

	for (i = 0; i < WMI_EVENT_QUEUE_SIZE + 1; i++) {
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0))
		code = asus_wmi_get_event_code(obj);
#else 
		code = asus_wmi_get_event_code(WMI_EVENT_VALUE_ATK);
#endif
		if (code < 0) {
			pr_warn("Failed to get event during flush: %d\n", code);
			return code;
		}

		if (code == WMI_EVENT_QUEUE_END || code == WMI_EVENT_MASK)
			return 0;
	}

	pr_warn("Failed to flush event queue\n");
	return -EIO;
}

/* Sysfs **********************************************************************/

static ssize_t store_sys_wmi(struct asus_wmi *asus, int devid,
			     const char *buf, size_t count)
{
	u32 retval;
	int err, value;

	value = asus_wmi_get_devstate_simple(asus, devid);
	if (value < 0)
		return value;

	err = kstrtoint(buf, 0, &value);
	if (err)
		return err;

	err = asus_wmi_set_devstate(devid, value, &retval);
	if (err < 0)
		return err;

	return count;
}

static ssize_t show_sys_wmi(struct asus_wmi *asus, int devid, char *buf)
{
	int value = asus_wmi_get_devstate_simple(asus, devid);

	if (value < 0)
		return value;

	return sprintf(buf, "%d\n", value);
}

#define ASUS_WMI_CREATE_DEVICE_ATTR(_name, _mode, _cm)			\
	static ssize_t show_##_name(struct device *dev,			\
				    struct device_attribute *attr,	\
				    char *buf)				\
	{								\
		struct asus_wmi *asus = dev_get_drvdata(dev);		\
									\
		return show_sys_wmi(asus, _cm, buf);			\
	}								\
	static ssize_t store_##_name(struct device *dev,		\
				     struct device_attribute *attr,	\
				     const char *buf, size_t count)	\
	{								\
		struct asus_wmi *asus = dev_get_drvdata(dev);		\
									\
		return store_sys_wmi(asus, _cm, buf, count);		\
	}								\
	static struct device_attribute dev_attr_##_name = {		\
		.attr = {						\
			.name = __stringify(_name),			\
			.mode = _mode },				\
		.show   = show_##_name,					\
		.store  = store_##_name,				\
	}

ASUS_WMI_CREATE_DEVICE_ATTR(touchpad, 0644, ASUS_WMI_DEVID_TOUCHPAD);
ASUS_WMI_CREATE_DEVICE_ATTR(camera, 0644, ASUS_WMI_DEVID_CAMERA);
ASUS_WMI_CREATE_DEVICE_ATTR(cardr, 0644, ASUS_WMI_DEVID_CARDREADER);
ASUS_WMI_CREATE_DEVICE_ATTR(lid_resume, 0644, ASUS_WMI_DEVID_LID_RESUME);
ASUS_WMI_CREATE_DEVICE_ATTR(als_enable, 0644, ASUS_WMI_DEVID_ALS_ENABLE);

static ssize_t cpufv_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	int value, rv;

	rv = kstrtoint(buf, 0, &value);
	if (rv)
		return rv;

	if (value < 0 || value > 2)
		return -EINVAL;

	rv = asus_wmi_evaluate_method(ASUS_WMI_METHODID_CFVS, value, 0, NULL);
	if (rv < 0)
		return rv;

	return count;
}

static DEVICE_ATTR_WO(cpufv);

static struct attribute *platform_attributes[] = {
	&dev_attr_cpufv.attr,
	&dev_attr_camera.attr,
	&dev_attr_cardr.attr,
	&dev_attr_touchpad.attr,
	&dev_attr_lid_resume.attr,
	&dev_attr_als_enable.attr,
	&dev_attr_fan_boost_mode.attr,
	&dev_attr_throttle_thermal_policy.attr,
	NULL
};

static umode_t asus_sysfs_is_visible(struct kobject *kobj,
				    struct attribute *attr, int idx)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct asus_wmi *asus = dev_get_drvdata(dev);
	bool ok = true;
	int devid = -1;

	if (attr == &dev_attr_camera.attr)
		devid = ASUS_WMI_DEVID_CAMERA;
	else if (attr == &dev_attr_cardr.attr)
		devid = ASUS_WMI_DEVID_CARDREADER;
	else if (attr == &dev_attr_touchpad.attr)
		devid = ASUS_WMI_DEVID_TOUCHPAD;
	else if (attr == &dev_attr_lid_resume.attr)
		devid = ASUS_WMI_DEVID_LID_RESUME;
	else if (attr == &dev_attr_als_enable.attr)
		devid = ASUS_WMI_DEVID_ALS_ENABLE;
	else if (attr == &dev_attr_fan_boost_mode.attr)
		ok = asus->fan_boost_mode_available;
	else if (attr == &dev_attr_throttle_thermal_policy.attr)
		ok = asus->throttle_thermal_policy_available;

	if (devid != -1)
		ok = !(asus_wmi_get_devstate_simple(asus, devid) < 0);

	return ok ? attr->mode : 0;
}

static const struct attribute_group platform_attribute_group = {
	.is_visible = asus_sysfs_is_visible,
	.attrs = platform_attributes
};

static void asus_wmi_sysfs_exit(struct platform_device *device)
{
	sysfs_remove_group(&device->dev.kobj, &platform_attribute_group);
}

static int asus_wmi_sysfs_init(struct platform_device *device)
{
	return sysfs_create_group(&device->dev.kobj, &platform_attribute_group);
}

/* Platform device ************************************************************/

static int asus_wmi_platform_init(struct asus_wmi *asus)
{
	struct device *dev = &asus->platform_device->dev;
	int rv;

	/* INIT enable hotkeys on some models */
	if (!asus_wmi_evaluate_method(ASUS_WMI_METHODID_INIT, 0, 0, &rv))
		pr_info("Initialization: %#x\n", rv);

	/* We don't know yet what to do with this version... */
	if (!asus_wmi_evaluate_method(ASUS_WMI_METHODID_SPEC, 0, 0x9, &rv)) {
		pr_info("BIOS WMI version: %d.%d\n", rv >> 16, rv & 0xFF);
		asus->spec = rv;
	}

	/*
	 * The SFUN method probably allows the original driver to get the list
	 * of features supported by a given model. For now, 0x0100 or 0x0800
	 * bit signifies that the laptop is equipped with a Wi-Fi MiniPCI card.
	 * The significance of others is yet to be found.
	 */
	if (!asus_wmi_evaluate_method(ASUS_WMI_METHODID_SFUN, 0, 0, &rv)) {
		pr_info("SFUN value: %#x\n", rv);
		asus->sfun = rv;
	}

	/*
	 * Eee PC and Notebooks seems to have different method_id for DSTS,
	 * but it may also be related to the BIOS's SPEC.
	 * Note, on most Eeepc, there is no way to check if a method exist
	 * or note, while on notebooks, they returns 0xFFFFFFFE on failure,
	 * but once again, SPEC may probably be used for that kind of things.
	 *
	 * Additionally at least TUF Gaming series laptops return nothing for
	 * unknown methods, so the detection in this way is not possible.
	 *
	 * There is strong indication that only ACPI WMI devices that have _UID
	 * equal to "ASUSWMI" use DCTS whereas those with "ATK" use DSTS.
	 */
	// NOTE[backport]: Always use DSTS
	dev_info(dev, "Use DSTS\n");
	asus->dsts_id = ASUS_WMI_METHODID_DSTS;

	/*
	 * Some devices can have multiple event codes stored in a queue before
	 * the module load if it was unloaded intermittently after calling
	 * the INIT method (enables event handling). The WMI notify handler is
	 * expected to retrieve all event codes until a retrieved code equals
	 * queue end marker (One or Ones). Old codes are flushed from the queue
	 * upon module load. Not enabling this when it should be has minimal
	 * visible impact so fall back if anything goes wrong.
	 */
	// NOTE[backport]: Always enable event queue
	dev_info(dev, "Enable event queue\n");

	if (!asus_wmi_notify_queue_flush(asus))
		asus->wmi_event_queue = true;

	/* CWAP allow to define the behavior of the Fn+F2 key,
	 * this method doesn't seems to be present on Eee PCs */
	if (asus->driver->quirks->wapf >= 0)
		asus_wmi_set_devstate(ASUS_WMI_DEVID_CWAP,
				      asus->driver->quirks->wapf, NULL);

	return 0;
}

/* debugfs ********************************************************************/

struct asus_wmi_debugfs_node {
	struct asus_wmi *asus;
	char *name;
	int (*show) (struct seq_file *m, void *data);
};

static int show_dsts(struct seq_file *m, void *data)
{
	struct asus_wmi *asus = m->private;
	int err;
	u32 retval = -1;

	err = asus_wmi_get_devstate(asus, asus->debug.dev_id, &retval);
	if (err < 0)
		return err;

	seq_printf(m, "DSTS(%#x) = %#x\n", asus->debug.dev_id, retval);

	return 0;
}

static int show_devs(struct seq_file *m, void *data)
{
	struct asus_wmi *asus = m->private;
	int err;
	u32 retval = -1;

	err = asus_wmi_set_devstate(asus->debug.dev_id, asus->debug.ctrl_param,
				    &retval);
	if (err < 0)
		return err;

	seq_printf(m, "DEVS(%#x, %#x) = %#x\n", asus->debug.dev_id,
		   asus->debug.ctrl_param, retval);

	return 0;
}

static int show_call(struct seq_file *m, void *data)
{
	struct asus_wmi *asus = m->private;
	struct bios_args args = {
		.arg0 = asus->debug.dev_id,
		.arg1 = asus->debug.ctrl_param,
	};
	struct acpi_buffer input = { (acpi_size) sizeof(args), &args };
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	acpi_status status;

	status = wmi_evaluate_method(ASUS_WMI_MGMT_GUID,
				     0, asus->debug.method_id,
				     &input, &output);

	if (ACPI_FAILURE(status))
		return -EIO;

	obj = (union acpi_object *)output.pointer;
	if (obj && obj->type == ACPI_TYPE_INTEGER)
		seq_printf(m, "%#x(%#x, %#x) = %#x\n", asus->debug.method_id,
			   asus->debug.dev_id, asus->debug.ctrl_param,
			   (u32) obj->integer.value);
	else
		seq_printf(m, "%#x(%#x, %#x) = t:%d\n", asus->debug.method_id,
			   asus->debug.dev_id, asus->debug.ctrl_param,
			   obj ? obj->type : -1);

	kfree(obj);

	return 0;
}

static struct asus_wmi_debugfs_node asus_wmi_debug_files[] = {
	{NULL, "devs", show_devs},
	{NULL, "dsts", show_dsts},
	{NULL, "call", show_call},
};

static int asus_wmi_debugfs_open(struct inode *inode, struct file *file)
{
	struct asus_wmi_debugfs_node *node = inode->i_private;

	return single_open(file, node->show, node->asus);
}

static const struct file_operations asus_wmi_debugfs_io_ops = {
	.owner = THIS_MODULE,
	.open = asus_wmi_debugfs_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static void asus_wmi_debugfs_exit(struct asus_wmi *asus)
{
	debugfs_remove_recursive(asus->debug.root);
}

static void asus_wmi_debugfs_init(struct asus_wmi *asus)
{
	int i;

	asus->debug.root = debugfs_create_dir(asus->driver->name, NULL);

	debugfs_create_x32("method_id", S_IRUGO | S_IWUSR, asus->debug.root,
			   &asus->debug.method_id);

	debugfs_create_x32("dev_id", S_IRUGO | S_IWUSR, asus->debug.root,
			   &asus->debug.dev_id);

	debugfs_create_x32("ctrl_param", S_IRUGO | S_IWUSR, asus->debug.root,
			   &asus->debug.ctrl_param);

	for (i = 0; i < ARRAY_SIZE(asus_wmi_debug_files); i++) {
		struct asus_wmi_debugfs_node *node = &asus_wmi_debug_files[i];

		node->asus = asus;
		debugfs_create_file(node->name, S_IFREG | S_IRUGO,
				    asus->debug.root, node,
				    &asus_wmi_debugfs_io_ops);
	}
}

/* Init / exit ****************************************************************/

/// asus-nb-wmi
/*
 * Asus Notebooks WMI hotkey driver
 *
 * Copyright(C) 2010 Corentin Chary <corentin.chary@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#define ASUS_NB_WMI_EVENT_GUID	"0B3CBB35-E3C2-45ED-91C2-4C5A6D195D1C"

static const struct key_entry asus_nb_wmi_keymap[] = {
	{ KE_KEY, ASUS_WMI_BRN_DOWN, { KEY_BRIGHTNESSDOWN } },
	{ KE_KEY, ASUS_WMI_BRN_UP, { KEY_BRIGHTNESSUP } },
	{ KE_KEY, 0x30, { KEY_VOLUMEUP } },
	{ KE_KEY, 0x31, { KEY_VOLUMEDOWN } },
	{ KE_KEY, 0x32, { KEY_MUTE } },
	{ KE_KEY, 0x35, { KEY_SCREENLOCK } },
	{ KE_KEY, 0x40, { KEY_PREVIOUSSONG } },
	{ KE_KEY, 0x41, { KEY_NEXTSONG } },
	{ KE_KEY, 0x43, { KEY_STOPCD } }, /* Stop/Eject */
	{ KE_KEY, 0x45, { KEY_PLAYPAUSE } },
	{ KE_KEY, 0x4c, { KEY_MEDIA } }, /* WMP Key */
	{ KE_KEY, 0x50, { KEY_EMAIL } },
	{ KE_KEY, 0x51, { KEY_WWW } },
	{ KE_KEY, 0x55, { KEY_CALC } },
	{ KE_IGNORE, 0x57, },  /* Battery mode */
	{ KE_IGNORE, 0x58, },  /* AC mode */
	{ KE_KEY, 0x5C, { KEY_F15 } },  /* Power Gear key */
	{ KE_KEY, 0x5D, { KEY_WLAN } }, /* Wireless console Toggle */
	{ KE_KEY, 0x5E, { KEY_WLAN } }, /* Wireless console Enable */
	{ KE_KEY, 0x5F, { KEY_WLAN } }, /* Wireless console Disable */
	{ KE_KEY, 0x60, { KEY_TOUCHPAD_ON } },
	{ KE_KEY, 0x61, { KEY_SWITCHVIDEOMODE } }, /* SDSP LCD only */
	{ KE_KEY, 0x62, { KEY_SWITCHVIDEOMODE } }, /* SDSP CRT only */
	{ KE_KEY, 0x63, { KEY_SWITCHVIDEOMODE } }, /* SDSP LCD + CRT */
	{ KE_KEY, 0x64, { KEY_SWITCHVIDEOMODE } }, /* SDSP TV */
	{ KE_KEY, 0x65, { KEY_SWITCHVIDEOMODE } }, /* SDSP LCD + TV */
	{ KE_KEY, 0x66, { KEY_SWITCHVIDEOMODE } }, /* SDSP CRT + TV */
	{ KE_KEY, 0x67, { KEY_SWITCHVIDEOMODE } }, /* SDSP LCD + CRT + TV */
	{ KE_KEY, 0x6B, { KEY_TOUCHPAD_TOGGLE } },
	{ KE_IGNORE, 0x6E, },  /* Low Battery notification */
	{ KE_KEY, 0x71, { KEY_F13 } }, /* General-purpose button */
	{ KE_IGNORE, 0x79, },  /* Charger type dectection notification */
	{ KE_KEY, 0x7a, { KEY_ALS_TOGGLE } }, /* Ambient Light Sensor Toggle */
	{ KE_KEY, 0x7c, { KEY_MICMUTE } },
	{ KE_KEY, 0x7D, { KEY_BLUETOOTH } }, /* Bluetooth Enable */
	{ KE_KEY, 0x7E, { KEY_BLUETOOTH } }, /* Bluetooth Disable */
	{ KE_KEY, 0x82, { KEY_CAMERA } },
	{ KE_KEY, 0x88, { KEY_RFKILL  } }, /* Radio Toggle Key */
	{ KE_KEY, 0x8A, { KEY_PROG1 } }, /* Color enhancement mode */
	{ KE_KEY, 0x8C, { KEY_SWITCHVIDEOMODE } }, /* SDSP DVI only */
	{ KE_KEY, 0x8D, { KEY_SWITCHVIDEOMODE } }, /* SDSP LCD + DVI */
	{ KE_KEY, 0x8E, { KEY_SWITCHVIDEOMODE } }, /* SDSP CRT + DVI */
	{ KE_KEY, 0x8F, { KEY_SWITCHVIDEOMODE } }, /* SDSP TV + DVI */
	{ KE_KEY, 0x90, { KEY_SWITCHVIDEOMODE } }, /* SDSP LCD + CRT + DVI */
	{ KE_KEY, 0x91, { KEY_SWITCHVIDEOMODE } }, /* SDSP LCD + TV + DVI */
	{ KE_KEY, 0x92, { KEY_SWITCHVIDEOMODE } }, /* SDSP CRT + TV + DVI */
	{ KE_KEY, 0x93, { KEY_SWITCHVIDEOMODE } }, /* SDSP LCD + CRT + TV + DVI */
	{ KE_KEY, 0x95, { KEY_MEDIA } },
	{ KE_KEY, NOTIFY_KBD_FBM, { KEY_FN_F5 } },
	//{ KE_KEY, 0x99, { KEY_PHONE } }, /* Conflicts with fan mode switch */
	{ KE_KEY, 0xA0, { KEY_SWITCHVIDEOMODE } }, /* SDSP HDMI only */
	{ KE_KEY, 0xA1, { KEY_SWITCHVIDEOMODE } }, /* SDSP LCD + HDMI */
	{ KE_KEY, 0xA2, { KEY_SWITCHVIDEOMODE } }, /* SDSP CRT + HDMI */
	{ KE_KEY, 0xA3, { KEY_SWITCHVIDEOMODE } }, /* SDSP TV + HDMI */
	{ KE_KEY, 0xA4, { KEY_SWITCHVIDEOMODE } }, /* SDSP LCD + CRT + HDMI */
	{ KE_KEY, 0xA5, { KEY_SWITCHVIDEOMODE } }, /* SDSP LCD + TV + HDMI */
	{ KE_KEY, 0xA6, { KEY_SWITCHVIDEOMODE } }, /* SDSP CRT + TV + HDMI */
	{ KE_KEY, 0xA7, { KEY_SWITCHVIDEOMODE } }, /* SDSP LCD + CRT + TV + HDMI */
	{ KE_KEY, NOTIFY_KBD_TTP, { KEY_FN_F5 } },
	{ KE_KEY, 0xB5, { KEY_CALC } },
	{ KE_KEY, 0xC4, { KEY_KBDILLUMUP } },
	{ KE_KEY, 0xC5, { KEY_KBDILLUMDOWN } },
	{ KE_IGNORE, 0xC6, },  /* Ambient Light Sensor notification */
	{ KE_KEY, 0xFA, { KEY_PROG2 } },           /* Lid flip action */
	{ KE_END, 0},
};

static struct quirk_entry quirk_asus_unknown = {
	.wapf = 0,
	.wmi_backlight_set_devstate = true,
};

static struct asus_wmi_driver asus_nb_wmi_driver = {
	.name = KBUILD_MODNAME,
	.owner = THIS_MODULE,
	.event_guid = ASUS_NB_WMI_EVENT_GUID,
	.keymap = asus_nb_wmi_keymap,
	.input_name = "Asus WMI hotkeys",
	.input_phys = KBUILD_MODNAME "/input0",
	.quirks = &quirk_asus_unknown
};
///

static int asus_wmi_add(struct platform_device *pdev)
{
	struct asus_wmi *asus;
#if (LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 1))
	const char *chassis_type;
#endif
	acpi_status status;
	int err;
	u32 result;

	asus = kzalloc(sizeof(struct asus_wmi), GFP_KERNEL);
	if (!asus)
		return -ENOMEM;

	asus->driver = &asus_nb_wmi_driver;
	asus->platform_device = pdev;
	asus->driver->platform_device = pdev;

	platform_set_drvdata(asus->platform_device, asus);

	err = asus_wmi_platform_init(asus);
	if (err)
		goto fail_platform;

	err = fan_boost_mode_check_present(asus);
	if (err)
		goto fail_fan_boost_mode;

	err = throttle_thermal_policy_check_present(asus);
	if (err)
		goto fail_throttle_thermal_policy;
	else
		throttle_thermal_policy_set_default(asus);

	err = asus_wmi_sysfs_init(asus->platform_device);
	if (err)
		goto fail_sysfs;

	err = asus_wmi_input_init(asus);
	if (err)
		goto fail_input;

	err = asus_wmi_fan_init(asus); /* probably no problems on error */

	err = asus_wmi_hwmon_init(asus);
	if (err)
		goto fail_hwmon;

	err = asus_wmi_led_init(asus);
	if (err)
		goto fail_leds;

	err = kbbl_rgb_init(asus);
	if (err)
		goto fail_rgbkb;

	asus_wmi_get_devstate(asus, ASUS_WMI_DEVID_WLAN, &result);
	if (result & (ASUS_WMI_DSTS_PRESENCE_BIT | ASUS_WMI_DSTS_USER_BIT))
		asus->driver->wlan_ctrl_by_user = 1;

	if (!(asus->driver->wlan_ctrl_by_user && ashs_present())) {
		err = asus_wmi_rfkill_init(asus);
		if (err)
			goto fail_rfkill;
	}

	if (asus->driver->quirks->wmi_force_als_set)
		asus_wmi_set_als();
#if (LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 1))
	/* Some Asus desktop boards export an acpi-video backlight interface,
	   stop this from showing up */
	chassis_type = dmi_get_system_info(DMI_CHASSIS_TYPE);

	if (chassis_type && !strcmp(chassis_type, "3"))
		acpi_video_set_dmi_backlight_type(acpi_backlight_vendor);

	if (asus->driver->quirks->wmi_backlight_power)
		acpi_video_set_dmi_backlight_type(acpi_backlight_vendor);

	if (asus->driver->quirks->wmi_backlight_native)
		acpi_video_set_dmi_backlight_type(acpi_backlight_native);
#endif

	if (asus->driver->quirks->xusb2pr)
		asus_wmi_set_xusb2pr(asus);

	if (acpi_video_get_backlight_type() == acpi_backlight_vendor) {
		err = asus_wmi_backlight_init(asus);
		if (err && err != -ENODEV)
			goto fail_backlight;
	} else if (asus->driver->quirks->wmi_backlight_set_devstate)
		err = asus_wmi_set_devstate(ASUS_WMI_DEVID_BACKLIGHT, 2, NULL);

	if (asus_wmi_has_fnlock_key(asus)) {
		asus->fnlock_locked = true;
		asus_wmi_fnlock_update(asus);
	}

	status = wmi_install_notify_handler(asus->driver->event_guid,
					    asus_wmi_notify, asus);
	if (ACPI_FAILURE(status)) {
		pr_err("Unable to register notify handler - %d\n", status);
		err = -ENODEV;
		goto fail_wmi_handler;
	}

	asus_wmi_debugfs_init(asus);

	return 0;

fail_wmi_handler:
	asus_wmi_backlight_exit(asus);
fail_backlight:
	asus_wmi_rfkill_exit(asus);
fail_rfkill:
	kbbl_rgb_exit(asus);
fail_rgbkb:
	asus_wmi_led_exit(asus);
fail_leds:
fail_hwmon:
	asus_wmi_input_exit(asus);
fail_input:
	asus_wmi_sysfs_exit(asus->platform_device);
fail_sysfs:
fail_throttle_thermal_policy:
fail_fan_boost_mode:
fail_platform:
	kfree(asus);
	return err;
}
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0))
static void asus_wmi_remove(struct platform_device *device)
#else
static int asus_wmi_remove(struct platform_device *device)
#endif
{
	struct asus_wmi *asus;

	asus = platform_get_drvdata(device);
	wmi_remove_notify_handler(asus->driver->event_guid);
	asus_wmi_backlight_exit(asus);
	asus_wmi_input_exit(asus);
	asus_wmi_led_exit(asus);
	kbbl_rgb_exit(asus);
	asus_wmi_rfkill_exit(asus);
	asus_wmi_debugfs_exit(asus);
	asus_wmi_sysfs_exit(asus->platform_device);
	asus_fan_set_auto(asus);

	kfree(asus);
#if (LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0))
	return 0;
#endif
}

/* Platform driver - hibernate/resume callbacks *******************************/

static int asus_hotk_thaw(struct device *device)
{
	struct asus_wmi *asus = dev_get_drvdata(device);

	if (asus->wlan.rfkill) {
		bool wlan;

		/*
		 * Work around bios bug - acpi _PTS turns off the wireless led
		 * during suspend.  Normally it restores it on resume, but
		 * we should kick it ourselves in case hibernation is aborted.
		 */
		wlan = asus_wmi_get_devstate_simple(asus, ASUS_WMI_DEVID_WLAN);
		asus_wmi_set_devstate(ASUS_WMI_DEVID_WLAN, wlan, NULL);
	}

	return 0;
}

static int asus_hotk_resume(struct device *device)
{
	struct asus_wmi *asus = dev_get_drvdata(device);

	if (!IS_ERR_OR_NULL(asus->kbd_led.dev))
		kbd_led_update(asus);

	if (asus_wmi_has_fnlock_key(asus))
		asus_wmi_fnlock_update(asus);

	if (asus->driver->quirks->use_lid_flip_devid)
		lid_flip_tablet_mode_get_state(asus);

	return 0;
}

static int asus_hotk_restore(struct device *device)
{
	struct asus_wmi *asus = dev_get_drvdata(device);
	int bl;

	/* Refresh both wlan rfkill state and pci hotplug */
	if (asus->wlan.rfkill)
		asus_rfkill_hotplug(asus);

	if (asus->bluetooth.rfkill) {
		bl = !asus_wmi_get_devstate_simple(asus,
						   ASUS_WMI_DEVID_BLUETOOTH);
		rfkill_set_sw_state(asus->bluetooth.rfkill, bl);
	}
	if (asus->wimax.rfkill) {
		bl = !asus_wmi_get_devstate_simple(asus, ASUS_WMI_DEVID_WIMAX);
		rfkill_set_sw_state(asus->wimax.rfkill, bl);
	}
	if (asus->wwan3g.rfkill) {
		bl = !asus_wmi_get_devstate_simple(asus, ASUS_WMI_DEVID_WWAN3G);
		rfkill_set_sw_state(asus->wwan3g.rfkill, bl);
	}
	if (asus->gps.rfkill) {
		bl = !asus_wmi_get_devstate_simple(asus, ASUS_WMI_DEVID_GPS);
		rfkill_set_sw_state(asus->gps.rfkill, bl);
	}
	if (asus->uwb.rfkill) {
		bl = !asus_wmi_get_devstate_simple(asus, ASUS_WMI_DEVID_UWB);
		rfkill_set_sw_state(asus->uwb.rfkill, bl);
	}
	if (!IS_ERR_OR_NULL(asus->kbd_led.dev))
		kbd_led_update(asus);

	if (asus_wmi_has_fnlock_key(asus))
		asus_wmi_fnlock_update(asus);

	if (asus->driver->quirks->use_lid_flip_devid)
		lid_flip_tablet_mode_get_state(asus);

	return 0;
}

static const struct dev_pm_ops asus_pm_ops = {
	.thaw = asus_hotk_thaw,
	.restore = asus_hotk_restore,
	.resume = asus_hotk_resume,
};

/// Faustus -------------------------------------------------------------------
static struct platform_device* atw_platform_dev;

// Platform driver ************************************************************

static struct platform_driver atw_platform_driver = {
	.probe = asus_wmi_add,
	.remove = asus_wmi_remove,
	.driver = {
		.name = KBUILD_MODNAME,
		.owner = THIS_MODULE,
		.pm = &asus_pm_ops,
	}
};

// Probing ********************************************************************

static int __init dmi_check_callback(const struct dmi_system_id *id)
{
	pr_info("DMI check: %s\n", id->ident);
	return 1;
}

static const struct dmi_system_id atw_dmi_list[] __initconst = {
	{
		.callback = dmi_check_callback,
		.ident = "FX505GM",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "ASUSTeK COMPUTER INC."),
			DMI_MATCH(DMI_PRODUCT_NAME, "FX505GM"),
		},
	},
	{
		.callback = dmi_check_callback,
		.ident = "FX705GM",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "ASUSTeK COMPUTER INC."),
			DMI_MATCH(DMI_PRODUCT_NAME, "FX705GM"),
		},
	},
	{
		.callback = dmi_check_callback,
		.ident = "FX505DD",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "ASUSTeK COMPUTER INC."),
			DMI_MATCH(DMI_PRODUCT_NAME, "FX505DD"),
		},
	},
	{
		.callback = dmi_check_callback,
		.ident = "FX505DY",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "ASUSTeK COMPUTER INC."),
			DMI_MATCH(DMI_PRODUCT_NAME, "FX505DY"),
		},
	},
	{
		.callback = dmi_check_callback,
		.ident = "FX505GE",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "ASUSTeK COMPUTER INC."),
			DMI_MATCH(DMI_PRODUCT_NAME, "FX505GE"),
		},
	},
	{
		.callback = dmi_check_callback,
		.ident = "FX705GE",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "ASUSTeK COMPUTER INC."),
			DMI_MATCH(DMI_PRODUCT_NAME, "FX705GE"),
		},
	},
	{
		.callback = dmi_check_callback,
		.ident = "FX705DY",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "ASUSTeK COMPUTER INC."),
			DMI_MATCH(DMI_PRODUCT_NAME, "FX705DY"),
		},
	},
	{
		.callback = dmi_check_callback,
		.ident = "FX505GD",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "ASUSTeK COMPUTER INC."),
			DMI_MATCH(DMI_PRODUCT_NAME, "FX505GD"),
		},
	},
	{
		.callback = dmi_check_callback,
		.ident = "FX505DT",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "ASUSTeK COMPUTER INC."),
			DMI_MATCH(DMI_PRODUCT_NAME, "FX505DT"),
		},
	},
	{
		.callback = dmi_check_callback,
		.ident = "FX705DT",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "ASUSTeK COMPUTER INC."),
			DMI_MATCH(DMI_PRODUCT_NAME, "FX705DT"),
		},
	},
	{
		.callback = dmi_check_callback,
		.ident = "FX505DU",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "ASUSTeK COMPUTER INC."),
			DMI_MATCH(DMI_PRODUCT_NAME, "FX505DU"),
		},
	},
	{
		.callback = dmi_check_callback,
		.ident = "FX705DU",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "ASUSTeK COMPUTER INC."),
			DMI_MATCH(DMI_PRODUCT_NAME, "FX705DU"),
		},
	},
	{
		.callback = dmi_check_callback,
		.ident = "FX505DV",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "ASUSTeK COMPUTER INC."),
			DMI_MATCH(DMI_PRODUCT_NAME, "FX505DV"),
		},
	},
	{
		.callback = dmi_check_callback,
		.ident = "FX505GT",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "ASUSTeK COMPUTER INC."),
			DMI_MATCH(DMI_PRODUCT_NAME, "FX505GT"),
		},
	},
	{
		.callback = dmi_check_callback,
		.ident = "FA506IV",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "ASUSTeK COMPUTER INC."),
			DMI_MATCH(DMI_PRODUCT_NAME, "FA506IV"),
		},
	},
	{
		.callback = dmi_check_callback,
		.ident = "FA506II",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "ASUSTeK COMPUTER INC."),
			DMI_MATCH(DMI_PRODUCT_NAME, "FA506II"),
		},
	},
	{
		.callback = dmi_check_callback,
		.ident = "FA506IU",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "ASUSTeK COMPUTER INC."),
			DMI_MATCH(DMI_PRODUCT_NAME, "FA506IU"),
		},
	},
	{
		.callback = dmi_check_callback,
		.ident = "FA506IA",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "ASUSTeK COMPUTER INC."),
			DMI_MATCH(DMI_PRODUCT_NAME, "FA506IA"),
		},
	},
	{
		.callback = dmi_check_callback,
		.ident = "FA706II",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "ASUSTeK COMPUTER INC."),
			DMI_MATCH(DMI_PRODUCT_NAME, "FA706II"),
		},
	},
	{
		.callback = dmi_check_callback,
		.ident = "FA706IU",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "ASUSTeK COMPUTER INC."),
			DMI_MATCH(DMI_PRODUCT_NAME, "FA706IU"),
		},
	},
    {
		.callback = dmi_check_callback,
		.ident = "FA706IH",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "ASUSTeK COMPUTER INC."),
			DMI_MATCH(DMI_PRODUCT_NAME, "FA706IH"),
		},
	},
	{
		.callback = dmi_check_callback,
		.ident = "FX506IU",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "ASUSTeK COMPUTER INC."),
			DMI_MATCH(DMI_PRODUCT_NAME, "FX506IU"),
		},
	},
	{
		.callback = dmi_check_callback,
		.ident = "FX506LI",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "ASUSTeK COMPUTER INC."),
			DMI_MATCH(DMI_PRODUCT_NAME, "FX506LI"),
		},
	},
	{
		.callback = dmi_check_callback,
		.ident = "FX506LHB.311",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "ASUSTeK COMPUTER INC."),
			DMI_MATCH(DMI_PRODUCT_NAME, "FX506LHB.311"),
		},
	},
};

static int __init atw_init(void)
{
	int status;

	if (!let_it_burn) {
		if (!dmi_check_system(atw_dmi_list)) {
			return -ENODEV;
		}
	} else {
		pr_info("Omitting DMI verification\n");
	}

	if (!wmi_has_guid(ASUS_WMI_MGMT_GUID)) {
		pr_info("Method WMI GUID not found\n");
		return -ENODEV;
	}

	if (!wmi_has_guid(ASUS_NB_WMI_EVENT_GUID)) {
		pr_info("Event WMI GUID not found\n");
		return -ENODEV;
	}


	atw_platform_dev = platform_device_register_simple(
			KBUILD_MODNAME, -1, NULL, 0);
	if (IS_ERR(atw_platform_dev)) {
		status = PTR_ERR(atw_platform_dev);
		goto fail_dev;
	}

	status = platform_driver_probe(&atw_platform_driver, asus_wmi_add);
	if (status) {
		pr_err("Can't probe platform driver: %d\n", status);
		goto fail_driver;
	}

	return 0;

fail_driver:
	platform_device_unregister(atw_platform_dev);
fail_dev:
	return status;
}

static void __exit atw_cleanup(void)
{
	pr_info("Faustus unloading..");
	platform_driver_unregister(&atw_platform_driver);
	platform_device_unregister(atw_platform_dev);
}
 
module_init(atw_init);
module_exit(atw_cleanup);
