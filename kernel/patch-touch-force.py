#!/usr/bin/env python3
"""The S9's force press: a hard press at the bottom centre, as KEY_HOMEPAGE.

The touchscreen controller (S6SY761, G960 firmware) has Samsung's "sponge"
library, which reads the pressure sensor and sends a gesture event (EID 2,
type 2) when the screen is pressed hard where the home button is: byte 1 is 0
pressed, 1 released. Mainline drops every event that isn't a coordinate, and
never switches the feature on. This switches it on after every SENSE_ON (input
open, runtime resume) the way Samsung's sec_ts does (sponge parameter 0, the
mode, bit 6; then the notify command), and reports it as KEY_HOMEPAGE on an
input device of its own, "s6sy761-force", so the touchscreen's own device is
unchanged and the shell can tell it from a real home key (ztmetro.c
FORCE_PRESS_DEVICE). Found and tested on the phone: ROADMAP 7.3.

force_level (sysfs, 0-5; 0 is the firmware's own) is the sponge's pressure
level, Samsung's sensitivity setting. Screen off it does nothing: the
controller is in application sleep then (the low-power mode is ROADMAP 3.9).
"""
from pathlib import Path
import sys

p = Path(sys.argv[1]) / 'drivers/input/touchscreen/s6sy761.c'
s = p.read_text()
MARK = 'zboot: force press'
if MARK in s:
    print('patch-touch-force: already applied')
    sys.exit(0)


def rep(old, new):
    global s
    if s.count(old) != 1:
        sys.exit(f'patch-touch-force: anchor changed: {old[:60]!r}')
    s = s.replace(old, new)


rep('/* events */', '''/* zboot: force press. Samsung's sponge library on the controller: a parameter
 * write (offset, then data) takes effect on the notify command. */
#define S6SY761_SPONGE_GET_INFO		0x90
#define S6SY761_SPONGE_WRITE		0x91
#define S6SY761_SPONGE_NOTIFY		0x93
#define S6SY761_SPONGE_MODE		0x00	/* parameter offset */
#define S6SY761_SPONGE_PRESSURE_LEVEL	0x5e
#define S6SY761_SPONGE_FORCE_KEY	0x40	/* in the mode */
#define S6SY761_EVENT_ID_GESTURE	0x02
#define S6SY761_GESTURE_FORCE		0x02
#define S6SY761_FORCE_DEV_NAME		"s6sy761-force"

/* events */''')

rep('''	u16 devid;
	u8 tx_channel;
};''', '''	u16 devid;
	u8 tx_channel;

	struct input_dev *force_input;	/* NULL: the controller has no sponge */
	bool force_down;
	u8 force_level;			/* 0: the firmware's own */
};''')

rep('''static void s6sy761_handle_events(struct s6sy761_data *sdata, u8 n_events)''', '''/* A force press: byte 0 is the event id (bits 0-1) and its type (bits 2-5),
 * byte 1 is 0 pressed, 1 released. A press while one is down, or a release
 * with none, is not passed on. */
static void s6sy761_handle_gesture(struct s6sy761_data *sdata, u8 *event)
{
	bool down;

	if (!sdata->force_input || ((event[0] >> 2) & 0xf) != S6SY761_GESTURE_FORCE)
		return;
	down = event[1] == 0;
	if (down == sdata->force_down)
		return;
	sdata->force_down = down;
	input_report_key(sdata->force_input, KEY_HOMEPAGE, down);
	input_sync(sdata->force_input);
}

/* Let go of a press the controller won't now say was released (it sleeps). */
static void s6sy761_force_release(struct s6sy761_data *sdata)
{
	if (sdata->force_input && sdata->force_down) {
		sdata->force_down = false;
		input_report_key(sdata->force_input, KEY_HOMEPAGE, 0);
		input_sync(sdata->force_input);
	}
}

static int s6sy761_sponge_write(struct s6sy761_data *sdata, u8 offset, u8 value)
{
	u8 buf[3] = { offset, 0x00, value };
	int ret;

	ret = i2c_smbus_write_i2c_block_data(sdata->client, S6SY761_SPONGE_WRITE, sizeof(buf), buf);
	if (!ret)
		ret = i2c_smbus_write_byte(sdata->client, S6SY761_SPONGE_NOTIFY);
	return ret;
}

/* After every SENSE_ON: the setting isn't known to survive application sleep. */
static void s6sy761_force_on(struct s6sy761_data *sdata)
{
	int ret;

	if (!sdata->force_input)
		return;
	ret = s6sy761_sponge_write(sdata, S6SY761_SPONGE_MODE, S6SY761_SPONGE_FORCE_KEY);
	if (!ret && sdata->force_level)
		ret = s6sy761_sponge_write(sdata, S6SY761_SPONGE_PRESSURE_LEVEL, sdata->force_level);
	if (ret)
		dev_warn(&sdata->client->dev, "force press not switched on: %d\\n", ret);
}

static void s6sy761_handle_events(struct s6sy761_data *sdata, u8 n_events)''')

rep('''		case S6SY761_EVENT_ID_STATUS:
			break;
''', '''		case S6SY761_EVENT_ID_STATUS:
			break;

		case S6SY761_EVENT_ID_GESTURE:
			s6sy761_handle_gesture(sdata, event);
			break;
''')

# input open: after SENSE_ON
rep('''	ret = i2c_smbus_write_byte(sdata->client, S6SY761_SENSE_ON);
	if (ret)
		pm_runtime_put(&sdata->client->dev);
	return ret;''', '''	ret = i2c_smbus_write_byte(sdata->client, S6SY761_SENSE_ON);
	if (ret)
		pm_runtime_put(&sdata->client->dev);
	else
		s6sy761_force_on(sdata);
	return ret;''')

# runtime suspend: a press held as the controller sleeps is let go
rep('''static int s6sy761_runtime_suspend(struct device *dev)
{
	struct s6sy761_data *sdata = dev_get_drvdata(dev);
	int ret;
''', '''static int s6sy761_runtime_suspend(struct device *dev)
{
	struct s6sy761_data *sdata = dev_get_drvdata(dev);
	int ret;

	s6sy761_force_release(sdata);''')

# runtime resume: after SENSE_ON
rep('''		S6SY761_APPLICATION_MODE, S6SY761_APP_NORMAL);
	if (ret)
		return ret;
	return i2c_smbus_write_byte(sdata->client, S6SY761_SENSE_ON);''', '''		S6SY761_APPLICATION_MODE, S6SY761_APP_NORMAL);
	if (ret)
		return ret;
	ret = i2c_smbus_write_byte(sdata->client, S6SY761_SENSE_ON);
	if (!ret)
		s6sy761_force_on(sdata);
	return ret;''')

# input close: let go of a held press too
rep('''	ret = i2c_smbus_write_byte(sdata->client, S6SY761_SENSE_OFF);
	if (ret)
		dev_err(&sdata->client->dev, "failed to turn off sensing\\n");''', '''	s6sy761_force_release(sdata);
	ret = i2c_smbus_write_byte(sdata->client, S6SY761_SENSE_OFF);
	if (ret)
		dev_err(&sdata->client->dev, "failed to turn off sensing\\n");''')

# sysfs: force_level
rep('''static DEVICE_ATTR(devid, 0444, s6sy761_sysfs_devid, NULL);''', '''static DEVICE_ATTR(devid, 0444, s6sy761_sysfs_devid, NULL);

/* the force press's sensitivity: 1-5, or 0 for the firmware's own */
static ssize_t force_level_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct s6sy761_data *sdata = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\\n", sdata->force_level);
}

static ssize_t force_level_store(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct s6sy761_data *sdata = dev_get_drvdata(dev);
	u8 level;
	int ret = kstrtou8(buf, 10, &level);

	if (ret)
		return ret;
	if (level > 5)
		return -EINVAL;
	if (!sdata->force_input)
		return -ENODEV;
	sdata->force_level = level;
	/* now if it is awake; otherwise at its next SENSE_ON */
	if (pm_runtime_get_if_active(dev) > 0) {
		if (level)
			ret = s6sy761_sponge_write(sdata, S6SY761_SPONGE_PRESSURE_LEVEL, level);
		pm_runtime_put(dev);
	}
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(force_level);''')
rep('''	&dev_attr_devid.attr,
	NULL''', '''	&dev_attr_devid.attr,
	&dev_attr_force_level.attr,
	NULL''')

# probe: the force device, when the controller has the sponge library; before the
# touchscreen's own device, whose open (it can come at once) switches it on
rep('''	err = input_register_device(sdata->input);''', '''	{
		/* the sponge's info starts with the model it was built for
		 * ("G960"); stock checks it the same way before using it */
		u8 info[10] = { 0 };

		err = i2c_smbus_read_i2c_block_data(client, S6SY761_SPONGE_GET_INFO, sizeof(info), info);
		if (err == sizeof(info) && info[0] >= 'A' && info[0] <= 'Z') {
			sdata->force_input = devm_input_allocate_device(&client->dev);
			if (!sdata->force_input)
				return -ENOMEM;
			sdata->force_input->name = S6SY761_FORCE_DEV_NAME;
			sdata->force_input->id.bustype = BUS_I2C;
			input_set_capability(sdata->force_input, EV_KEY, KEY_HOMEPAGE);
			err = input_register_device(sdata->force_input);
			if (err)
				return err;
			dev_info(&client->dev, "force press: sponge %.4s\\n", info);
		} else {
			dev_info(&client->dev, "no force press: sponge info %d\\n", err);
		}
	}

	err = input_register_device(sdata->input);''')

p.write_text(s)
print('patch-touch-force: force press as KEY_HOMEPAGE on s6sy761-force')
