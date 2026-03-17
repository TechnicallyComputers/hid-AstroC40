// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  HID driver for Astro C40 TR controller.
 *
 *  DS4-compatible mapping, touchpad, motion sensors (gyro+accel at bytes 13-24).
 *  Mapping reference: ds4drv/ds4drv/backends/hidraw.py HidrawAstroC40Device.
 */
#include <linux/bits.h>
#include <linux/device.h>
#include <linux/hid.h>
#include <linux/idr.h>
#include <linux/input/mt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/unaligned.h>
#include <linux/usb.h>
#include "hid-ids.h"

#define C40_DEVICE_NAME "Astro C40 TR"
static DEFINE_MUTEX(c40_devices_lock);
static LIST_HEAD(c40_devices_list);
static DEFINE_IDA(c40_player_id_allocator);
/* Base class for playstation devices. */
struct ps_device {
	struct list_head list;
	struct hid_device *hdev;
	spinlock_t lock;
	uint32_t player_id;
	struct power_supply_desc battery_desc;
	struct power_supply *battery;
	uint8_t battery_capacity;
	int battery_status;
	uint8_t mac_address[6]; /* Note: stored in little endian order. */
	uint32_t hw_version;
	uint32_t fw_version;
	int (*parse_report)(struct ps_device *dev, struct hid_report *report, u8 *data, int size);
};
/* Calibration data for playstation motion sensors. */
struct ps_calibration_data {
	int abs_code;
	short bias;
	int sens_numer;
	int sens_denom;
};
/* Astro C40 TR report layout */
#define C40_INPUT_REPORT_ID	0x01
#define C40_INPUT_REPORT_SIZE	64
/* C40 touchpad: header bytes indicate finger; 3-byte coords: X_hi,Y_hi,X_lo|Y_lo. 12-bit X/Y. */
#define C40_TOUCHPAD_WIDTH	1920
#define C40_TOUCHPAD_HEIGHT	942
/* Motion: bytes 10-12 = poll counter; 13-24 = gyro[3]+accel[3] as __le16 (DS4 order). */
#define C40_MOTION_OFFSET	13
#define DS4_ACC_RES_PER_G	1024
#define DS4_ACC_RANGE		(4 * DS4_ACC_RES_PER_G)
#define DS4_GYRO_RES_PER_DEG_S	16
#define DS4_GYRO_RANGE		(2048 * DS4_GYRO_RES_PER_DEG_S)

struct c40_device {
	struct ps_device base;
	struct input_dev *gamepad;
	struct input_dev *sensors;
	struct input_dev *touchpad;
	struct ps_calibration_data accel_calib_data[3];
	struct ps_calibration_data gyro_calib_data[3];
};
/*
 * Common gamepad buttons across DualShock 3 / 4 and DualSense.
 * Note: for device with a touchpad, touchpad button is not included
 *        as it will be part of the touchpad device.
 */
static const int ps_gamepad_buttons[] = {
	BTN_WEST, /* Square */
	BTN_NORTH, /* Triangle */
	BTN_EAST, /* Circle */
	BTN_SOUTH, /* Cross */
	BTN_TL, /* L1 */
	BTN_TR, /* R1 */
	BTN_TL2, /* L2 */
	BTN_TR2, /* R2 */
	BTN_SELECT, /* Create (PS5) / Share (PS4) */
	BTN_START, /* Option */
	BTN_THUMBL, /* L3 */
	BTN_THUMBR, /* R3 */
	BTN_MODE, /* PS Home */
};
static const struct {int x; int y; } ps_gamepad_hat_mapping[] = {
	{0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1},
	{0, 0},
};
/*
 * Add a new ps_device to ps_devices if it doesn't exist.
 * Return error on duplicate device, which can happen if the same
 * device is connected using both Bluetooth and USB.
 */
static int c40_devices_list_add(struct ps_device *dev)
{
	struct ps_device *entry;
	mutex_lock(&c40_devices_lock);
	list_for_each_entry(entry, &c40_devices_list, list) {
		if (!memcmp(entry->mac_address, dev->mac_address, sizeof(dev->mac_address))) {
			hid_err(dev->hdev, "Duplicate device found for MAC address %pMR.\n",
					dev->mac_address);
			mutex_unlock(&c40_devices_lock);
			return -EEXIST;
		}
	}
	list_add_tail(&dev->list, &c40_devices_list);
	mutex_unlock(&c40_devices_lock);
	return 0;
}
static int c40_devices_list_remove(struct ps_device *dev)
{
	mutex_lock(&c40_devices_lock);
	list_del(&dev->list);
	mutex_unlock(&c40_devices_lock);
	return 0;
}
static int c40_device_set_player_id(struct ps_device *dev)
{
	int ret = ida_alloc(&c40_player_id_allocator, GFP_KERNEL);
	if (ret < 0)
		return ret;
	dev->player_id = ret;
	return 0;
}
static void c40_device_release_player_id(struct ps_device *dev)
{
	ida_free(&c40_player_id_allocator, dev->player_id);
	dev->player_id = U32_MAX;
}
static struct input_dev *ps_allocate_input_dev(struct hid_device *hdev, const char *name_suffix)
{
	struct input_dev *input_dev;
	input_dev = devm_input_allocate_device(&hdev->dev);
	if (!input_dev)
		return ERR_PTR(-ENOMEM);
	input_dev->id.bustype = hdev->bus;
	input_dev->id.vendor = hdev->vendor;
	input_dev->id.product = hdev->product;
	input_dev->id.version = hdev->version;
	input_dev->uniq = hdev->uniq;
	if (name_suffix) {
		input_dev->name = devm_kasprintf(&hdev->dev, GFP_KERNEL, "%s %s", hdev->name,
				name_suffix);
		if (!input_dev->name)
			return ERR_PTR(-ENOMEM);
	} else {
		input_dev->name = hdev->name;
	}
	input_set_drvdata(input_dev, hdev);
	return input_dev;
}
static enum power_supply_property ps_power_supply_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_SCOPE,
};
static int ps_battery_get_property(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	struct ps_device *dev = power_supply_get_drvdata(psy);
	uint8_t battery_capacity;
	int battery_status;
	unsigned long flags;
	int ret = 0;
	spin_lock_irqsave(&dev->lock, flags);
	battery_capacity = dev->battery_capacity;
	battery_status = dev->battery_status;
	spin_unlock_irqrestore(&dev->lock, flags);
	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = battery_status;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = battery_capacity;
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_DEVICE;
		break;
	default:
		ret = -EINVAL;
		break;
	}
	return ret;
}
static int ps_device_register_battery(struct ps_device *dev)
{
	struct power_supply *battery;
	struct power_supply_config battery_cfg = { .drv_data = dev };
	int ret;
	dev->battery_desc.type = POWER_SUPPLY_TYPE_BATTERY;
	dev->battery_desc.properties = ps_power_supply_props;
	dev->battery_desc.num_properties = ARRAY_SIZE(ps_power_supply_props);
	dev->battery_desc.get_property = ps_battery_get_property;
	dev->battery_desc.name = devm_kasprintf(&dev->hdev->dev, GFP_KERNEL,
			"astroc40-battery-%pMR", dev->mac_address);
	if (!dev->battery_desc.name)
		return -ENOMEM;
	battery = devm_power_supply_register(&dev->hdev->dev, &dev->battery_desc, &battery_cfg);
	if (IS_ERR(battery)) {
		ret = PTR_ERR(battery);
		hid_err(dev->hdev, "Unable to register battery device: %d\n", ret);
		return ret;
	}
	dev->battery = battery;
	ret = power_supply_powers(dev->battery, &dev->hdev->dev);
	if (ret) {
		hid_err(dev->hdev, "Unable to activate battery device: %d\n", ret);
		return ret;
	}
	return 0;
}
static struct input_dev *ps_gamepad_create(struct hid_device *hdev,
		int (*play_effect)(struct input_dev *, void *, struct ff_effect *))
{
	struct input_dev *gamepad;
	unsigned int i;
	int ret;
	gamepad = ps_allocate_input_dev(hdev, NULL);
	if (IS_ERR(gamepad))
		return ERR_CAST(gamepad);
	input_set_abs_params(gamepad, ABS_X, 0, 255, 0, 0);
	input_set_abs_params(gamepad, ABS_Y, 0, 255, 0, 0);
	input_set_abs_params(gamepad, ABS_Z, 0, 255, 0, 0);
	input_set_abs_params(gamepad, ABS_RX, 0, 255, 0, 0);
	input_set_abs_params(gamepad, ABS_RY, 0, 255, 0, 0);
	input_set_abs_params(gamepad, ABS_RZ, 0, 255, 0, 0);
	input_set_abs_params(gamepad, ABS_HAT0X, -1, 1, 0, 0);
	input_set_abs_params(gamepad, ABS_HAT0Y, -1, 1, 0, 0);
	for (i = 0; i < ARRAY_SIZE(ps_gamepad_buttons); i++)
		input_set_capability(gamepad, EV_KEY, ps_gamepad_buttons[i]);
#if IS_ENABLED(CONFIG_PLAYSTATION_FF)
	if (play_effect) {
		input_set_capability(gamepad, EV_FF, FF_RUMBLE);
		input_ff_create_memless(gamepad, NULL, play_effect);
	}
#endif
	ret = input_register_device(gamepad);
	if (ret)
		return ERR_PTR(ret);
	return gamepad;
}
static struct input_dev *ps_sensors_create(struct hid_device *hdev, int accel_range, int accel_res,
		int gyro_range, int gyro_res)
{
	struct input_dev *sensors;
	int ret;
	sensors = ps_allocate_input_dev(hdev, "Motion Sensors");
	if (IS_ERR(sensors))
		return ERR_CAST(sensors);
	__set_bit(INPUT_PROP_ACCELEROMETER, sensors->propbit);
	__set_bit(EV_MSC, sensors->evbit);
	__set_bit(MSC_TIMESTAMP, sensors->mscbit);
	/* Accelerometer */
	input_set_abs_params(sensors, ABS_X, -accel_range, accel_range, 16, 0);
	input_set_abs_params(sensors, ABS_Y, -accel_range, accel_range, 16, 0);
	input_set_abs_params(sensors, ABS_Z, -accel_range, accel_range, 16, 0);
	input_abs_set_res(sensors, ABS_X, accel_res);
	input_abs_set_res(sensors, ABS_Y, accel_res);
	input_abs_set_res(sensors, ABS_Z, accel_res);
	/* Gyroscope */
	input_set_abs_params(sensors, ABS_RX, -gyro_range, gyro_range, 16, 0);
	input_set_abs_params(sensors, ABS_RY, -gyro_range, gyro_range, 16, 0);
	input_set_abs_params(sensors, ABS_RZ, -gyro_range, gyro_range, 16, 0);
	input_abs_set_res(sensors, ABS_RX, gyro_res);
	input_abs_set_res(sensors, ABS_RY, gyro_res);
	input_abs_set_res(sensors, ABS_RZ, gyro_res);
	ret = input_register_device(sensors);
	if (ret)
		return ERR_PTR(ret);
	return sensors;
}
static struct input_dev *ps_touchpad_create(struct hid_device *hdev, int width, int height,
		unsigned int num_contacts)
{
	struct input_dev *touchpad;
	int ret;
	touchpad = ps_allocate_input_dev(hdev, "Touchpad");
	if (IS_ERR(touchpad))
		return ERR_CAST(touchpad);
	/* Map button underneath touchpad to BTN_LEFT. */
	input_set_capability(touchpad, EV_KEY, BTN_LEFT);
	input_set_capability(touchpad, EV_KEY, BTN_TOOL_FINGER);
	input_set_capability(touchpad, EV_KEY, BTN_TOOL_DOUBLETAP);
	__set_bit(INPUT_PROP_BUTTONPAD, touchpad->propbit);
	input_set_abs_params(touchpad, ABS_MT_POSITION_X, 0, width - 1, 0, 0);
	input_set_abs_params(touchpad, ABS_MT_POSITION_Y, 0, height - 1, 0, 0);
	ret = input_mt_init_slots(touchpad, num_contacts, INPUT_MT_POINTER);
	if (ret)
		return ERR_PTR(ret);
	ret = input_register_device(touchpad);
	if (ret)
		return ERR_PTR(ret);
	return touchpad;
}
static ssize_t firmware_version_show(struct device *dev,
				struct device_attribute
				*attr, char *buf)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct ps_device *ps_dev = hid_get_drvdata(hdev);
	return sysfs_emit(buf, "0x%08x\n", ps_dev->fw_version);
}
static DEVICE_ATTR_RO(firmware_version);
static ssize_t hardware_version_show(struct device *dev,
				struct device_attribute
				*attr, char *buf)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct ps_device *ps_dev = hid_get_drvdata(hdev);
	return sysfs_emit(buf, "0x%08x\n", ps_dev->hw_version);
}
static DEVICE_ATTR_RO(hardware_version);
static struct attribute *ps_device_attributes[] = {
	&dev_attr_firmware_version.attr,
	&dev_attr_hardware_version.attr,
	NULL
};
static const struct attribute_group ps_device_attribute_group = {
	.attrs = ps_device_attributes,
};
static int astroc40_parse_report(struct ps_device *ps_dev, struct hid_report *report,
		u8 *data, int size)
{
	struct c40_device *c40 = container_of(ps_dev, struct c40_device, base);
	u8 hat, face, btn_lo, btn_hi;
	u16 buttons;
	u16 t0_x, t0_y, t1_x, t1_y;
	bool t0_active, t1_active;

	if (report->id != C40_INPUT_REPORT_ID || size < C40_INPUT_REPORT_SIZE)
		return 0;

	/* Sticks (DS4 mapping) */
	input_report_abs(c40->gamepad, ABS_X,  data[1]);
	input_report_abs(c40->gamepad, ABS_Y,  data[2]);
	input_report_abs(c40->gamepad, ABS_RX, data[3]);
	input_report_abs(c40->gamepad, ABS_RY, data[4]);
	input_report_abs(c40->gamepad, ABS_Z,  data[8]);
	input_report_abs(c40->gamepad, ABS_RZ, data[9]);

	/* Hat: C40 uses 0-7 for directions, 8=neutral. data[5] low nibble = hat, high nibble = face buttons */
	hat = data[5] & 0x0f;
	if (hat >= ARRAY_SIZE(ps_gamepad_hat_mapping))
		hat = 8;
	input_report_abs(c40->gamepad, ABS_HAT0X, ps_gamepad_hat_mapping[hat].x);
	input_report_abs(c40->gamepad, ABS_HAT0Y, ps_gamepad_hat_mapping[hat].y);

	/* Face buttons: data[5] bits 4-7 (upper nibble). Order: bit4=NORTH,5=SOUTH,6=EAST,7=WEST; adjust if wrong */
	face = data[5] >> 4;
	input_report_key(c40->gamepad, BTN_NORTH, !!(face & BIT(0)));
	input_report_key(c40->gamepad, BTN_SOUTH, !!(face & BIT(1)));
	input_report_key(c40->gamepad, BTN_EAST,  !!(face & BIT(2)));
	input_report_key(c40->gamepad, BTN_WEST,  !!(face & BIT(3)));

	/* data[6-7]: L1,R1,Share,Options,L3,R3,PS; bit9=touchpad_btn */
	btn_lo = data[6];
	btn_hi = data[7] & 0x3f;
	buttons = btn_lo | (btn_hi << 8);

	input_report_key(c40->gamepad, BTN_TL,     !!(buttons & BIT(0)));
	input_report_key(c40->gamepad, BTN_TR,     !!(buttons & BIT(1)));
	input_report_key(c40->gamepad, BTN_TR2,    !!(buttons & BIT(2)));
	input_report_key(c40->gamepad, BTN_TL2,    !!(buttons & BIT(3)));
	input_report_key(c40->gamepad, BTN_SELECT, !!(buttons & BIT(4)));
	input_report_key(c40->gamepad, BTN_START,  !!(buttons & BIT(5)));
	input_report_key(c40->gamepad, BTN_THUMBL, !!(buttons & BIT(6)));
	input_report_key(c40->gamepad, BTN_THUMBR, !!(buttons & BIT(7)));
	input_report_key(c40->gamepad, BTN_MODE,   !!(buttons & BIT(8)));
	input_sync(c40->gamepad);

	/* Motion sensors: bytes 13-24 = gyro[3] + accel[3] (__le16, DS4 order). No calibration. */
	{
		int i;
		s16 gyro[3], accel[3];

		for (i = 0; i < 3; i++) {
			gyro[i] = (s16)get_unaligned_le16(&data[C40_MOTION_OFFSET + i * 2]);
			accel[i] = (s16)get_unaligned_le16(&data[C40_MOTION_OFFSET + 6 + i * 2]);
		}
		for (i = 0; i < 3; i++) {
			int g = mult_frac(c40->gyro_calib_data[i].sens_numer,
					 (int)gyro[i], c40->gyro_calib_data[i].sens_denom);
			int a = mult_frac(c40->accel_calib_data[i].sens_numer,
					 (int)accel[i] - c40->accel_calib_data[i].bias,
					 c40->accel_calib_data[i].sens_denom);
			input_report_abs(c40->sensors, c40->gyro_calib_data[i].abs_code, g);
			input_report_abs(c40->sensors, c40->accel_calib_data[i].abs_code, a);
		}
	}
	input_sync(c40->sensors);

	/* Touchpad: DS4-style nibble layout, C40 nibble swap in shared byte.
	 * 4 bytes/finger: contact, x_lo, y_lo|x_hi, y_hi (low nibble=X_hi, high=Y_lo). */
	t0_active = data[35] != 0;
	t1_active = data[39] != 0;
	t0_x = data[36] | ((data[37] & 0x0f) << 8);
	t0_y = (data[38] << 4) | ((data[37] >> 4) & 0x0f);
	t1_x = data[40] | ((data[41] & 0x0f) << 8);
	t1_y = (data[42] << 4) | ((data[41] >> 4) & 0x0f);
	if (t0_active && t0_x == 0xfff && t0_y == 0xfff)
		t0_active = false;
	if (t1_active && t1_x == 0xfff && t1_y == 0xfff)
		t1_active = false;

	input_mt_slot(c40->touchpad, 0);
	input_mt_report_slot_state(c40->touchpad, MT_TOOL_FINGER, t0_active);
	if (t0_active) {
		input_report_abs(c40->touchpad, ABS_MT_POSITION_X, t0_x);
		input_report_abs(c40->touchpad, ABS_MT_POSITION_Y, t0_y);
	}
	input_mt_slot(c40->touchpad, 1);
	input_mt_report_slot_state(c40->touchpad, MT_TOOL_FINGER, t1_active);
	if (t1_active) {
		input_report_abs(c40->touchpad, ABS_MT_POSITION_X, t1_x);
		input_report_abs(c40->touchpad, ABS_MT_POSITION_Y, t1_y);
	}
	input_mt_sync_frame(c40->touchpad);
	input_report_key(c40->touchpad, BTN_TOOL_FINGER, t0_active && !t1_active);
	input_report_key(c40->touchpad, BTN_TOOL_DOUBLETAP, t0_active && t1_active);
	input_report_key(c40->touchpad, BTN_LEFT, !!(buttons & BIT(9)));
	input_sync(c40->touchpad);

	return 0;
}

static struct ps_device *astroc40_create(struct hid_device *hdev)
{
	struct c40_device *c40;
	struct ps_device *ps_dev;
	static atomic_t c40_mac_counter = ATOMIC_INIT(0);
	int ret;

	c40 = devm_kzalloc(&hdev->dev, sizeof(*c40), GFP_KERNEL);
	if (!c40)
		return ERR_PTR(-ENOMEM);

	ps_dev = &c40->base;
	ps_dev->hdev = hdev;
	spin_lock_init(&ps_dev->lock);
	ps_dev->battery_capacity = 0;
	ps_dev->battery_status = POWER_SUPPLY_STATUS_UNKNOWN;
	ps_dev->parse_report = astroc40_parse_report;

	/* Synthetic MAC; C40 does not support Sony feature reports */
	ps_dev->mac_address[0] = 0x02;
	ps_dev->mac_address[1] = 0x98;
	ps_dev->mac_address[2] = 0x86;
	ps_dev->mac_address[3] = (hdev->product >> 8) & 0xff;
	ps_dev->mac_address[4] = hdev->product & 0xff;
	ps_dev->mac_address[5] = atomic_inc_return(&c40_mac_counter) & 0xff;
	snprintf(hdev->uniq, sizeof(hdev->uniq), "%pMR", ps_dev->mac_address);

	/* Override device name for "Astro C40 TR" branding */
	strscpy(hdev->name, C40_DEVICE_NAME, sizeof(hdev->name));

	/* Default calibration: passthrough (C40 has no calibration feature report) */
	c40->gyro_calib_data[0].abs_code = ABS_RX;
	c40->gyro_calib_data[0].sens_denom = S16_MAX;
	c40->gyro_calib_data[0].sens_numer = DS4_GYRO_RANGE;
	c40->gyro_calib_data[1].abs_code = ABS_RY;
	c40->gyro_calib_data[1].sens_denom = S16_MAX;
	c40->gyro_calib_data[1].sens_numer = DS4_GYRO_RANGE;
	c40->gyro_calib_data[2].abs_code = ABS_RZ;
	c40->gyro_calib_data[2].sens_denom = S16_MAX;
	c40->gyro_calib_data[2].sens_numer = DS4_GYRO_RANGE;
	c40->accel_calib_data[0].abs_code = ABS_X;
	c40->accel_calib_data[0].sens_denom = S16_MAX;
	c40->accel_calib_data[0].sens_numer = DS4_ACC_RANGE;
	c40->accel_calib_data[1].abs_code = ABS_Y;
	c40->accel_calib_data[1].sens_denom = S16_MAX;
	c40->accel_calib_data[1].sens_numer = DS4_ACC_RANGE;
	c40->accel_calib_data[2].abs_code = ABS_Z;
	c40->accel_calib_data[2].sens_denom = S16_MAX;
	c40->accel_calib_data[2].sens_numer = DS4_ACC_RANGE;

	hid_set_drvdata(hdev, c40);

	ret = c40_devices_list_add(ps_dev);
	if (ret)
		return ERR_PTR(ret);

	c40->gamepad = ps_gamepad_create(hdev, NULL);
	if (IS_ERR(c40->gamepad)) {
		ret = PTR_ERR(c40->gamepad);
		goto err;
	}

	c40->sensors = ps_sensors_create(hdev, DS4_ACC_RANGE, DS4_ACC_RES_PER_G,
					 DS4_GYRO_RANGE, DS4_GYRO_RES_PER_DEG_S);
	if (IS_ERR(c40->sensors)) {
		ret = PTR_ERR(c40->sensors);
		goto err;
	}

	c40->touchpad = ps_touchpad_create(hdev, C40_TOUCHPAD_WIDTH, C40_TOUCHPAD_HEIGHT, 2);
	if (IS_ERR(c40->touchpad)) {
		ret = PTR_ERR(c40->touchpad);
		goto err;
	}

	ret = ps_device_register_battery(ps_dev);
	if (ret)
		goto err;

	ret = c40_device_set_player_id(ps_dev);
	if (ret) {
		hid_err(hdev, "Failed to assign player id for Astro C40: %d\n", ret);
		goto err;
	}

	hid_info(hdev, "Registered " C40_DEVICE_NAME " controller\n");
	return &c40->base;

err:
	c40_devices_list_remove(ps_dev);
	return ERR_PTR(ret);
}

static int ps_raw_event(struct hid_device *hdev, struct hid_report *report,
		u8 *data, int size)
{
	struct ps_device *dev = hid_get_drvdata(hdev);
	if (dev && dev->parse_report)
		return dev->parse_report(dev, report, data, size);
	return 0;
}
#define C40_HID_INTERFACE	4	/* C40 has HID on interfaces 3 and 4; use 4 only for inputs */

static int ps_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct ps_device *dev;
	int ret;

	/* C40 is USB composite: interfaces 3 and 4 are both gamepad HID. Bind only to 3. */
	if (hdev->bus == BUS_USB) {
		struct usb_interface *intf = to_usb_interface(hdev->dev.parent);
		if (intf->cur_altsetting->desc.bInterfaceNumber != C40_HID_INTERFACE)
			return -ENODEV;
	}

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "Parse failed\n");
		return ret;
	}
	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret) {
		hid_err(hdev, "Failed to start HID device\n");
		return ret;
	}
	ret = hid_hw_open(hdev);
	if (ret) {
		hid_err(hdev, "Failed to open HID device\n");
		goto err_stop;
	}
	dev = astroc40_create(hdev);
	if (IS_ERR(dev)) {
		hid_err(hdev, "Failed to create Astro C40.\n");
		ret = PTR_ERR(dev);
		goto err_close;
	}
	ret = devm_device_add_group(&hdev->dev, &ps_device_attribute_group);
	if (ret) {
		hid_err(hdev, "Failed to register sysfs nodes.\n");
		goto err_close;
	}
	return ret;
err_close:
	hid_hw_close(hdev);
err_stop:
	hid_hw_stop(hdev);
	return ret;
}
static void ps_remove(struct hid_device *hdev)
{
	struct ps_device *dev = hid_get_drvdata(hdev);
	c40_devices_list_remove(dev);
	c40_device_release_player_id(dev);
	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}
static const struct hid_device_id astroc40_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_ASTRO, USB_DEVICE_ID_ASTRO_C40) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ASTRO, USB_DEVICE_ID_ASTRO_C40_2) },
	{ }
};
MODULE_DEVICE_TABLE(hid, astroc40_devices);
static struct hid_driver astroc40_driver = {
	.name		= "astroc40",
	.id_table	= astroc40_devices,
	.probe		= ps_probe,
	.remove		= ps_remove,
	.raw_event	= ps_raw_event,
};
static int __init astroc40_init(void)
{
	return hid_register_driver(&astroc40_driver);
}
static void __exit astroc40_exit(void)
{
	hid_unregister_driver(&astroc40_driver);
	ida_destroy(&c40_player_id_allocator);
}
module_init(astroc40_init);
module_exit(astroc40_exit);
MODULE_AUTHOR("Astro Gaming");
MODULE_DESCRIPTION("HID Driver for Astro C40 TR controller");
MODULE_LICENSE("GPL");
