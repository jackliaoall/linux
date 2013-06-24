/*
 * ad799x.c - Linux kernel module for hardware monitoring
 *
 * Copyright (C) 2009  Sigurd M. Andreassen <sigurdan at stud.ntnu.no>
 * Copyright (C) 2013  Kurt Van Dijck <linux@vandijck-laurijssen.be>
 *
 * Based on the lm90 driver. This driver was written for the ad7991
 * IC by Analog devices, but also supports ad7995 and ad7999. This
 * series of adc's has a 4 channel multiplexed input where one
 * input channel, number 3, can be configured as voltage reference.
 * Default voltage reference is the power pin Vcc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/slab.h>		/*      kzalloc()               */
#include <linux/sysfs.h>	/*      sysfs_create_group()    */
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/err.h>

/*     Each client has this additional data    */
struct ad799x_data {
	struct device *hwmon_dev;
	unsigned int reference;	/* [mV] */
	struct mutex lock;
};

/*     sysfs hook function
 *
 *     Returns bytes written to buf on success, -EINVAL if input value
 *     is invalid, -ERESTARTSYS on problems locking mutex and return value
 *     from smbus routine if call fails.
 *
 */
static ssize_t ad799x_read(struct device *dev, struct device_attribute *devattr,
			   char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct ad799x_data *ad799x = i2c_get_clientdata(client);
	int ret, command, vref;
	u8 adc[2];

	/* prepare command byte */
	command = 0x10 << to_sensor_dev_attr(devattr)->index;

	/* Writing ADC sample channel according to attribut */
	ret = i2c_smbus_read_i2c_block_data(client, command, 2, adc);

	if (ret != 2) {
		dev_dbg(&client->dev, "ad799x: Problems reading from adc"
			" @ 0x%x\n", client->addr);
		return ret;
	}

	/*      Lock mutex and fetch reference value    */
	if (mutex_lock_interruptible(&ad799x->lock))
		return -ERESTARTSYS;

	vref = ad799x->reference;

	mutex_unlock(&ad799x->lock);

	ret = (((adc[0] << 8) | adc[1]) & 0x0fff) * vref / 4095;
	return sprintf(buf, "%d\n", ret);
}

/*
 *     Returns bytes written to buffer on success, -EINVAL if input value
 *     is invalid and -ERESTARTSYS on problems locking mutex.
 *
 */
static ssize_t ad799x_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct ad799x_data *ad799x = i2c_get_clientdata(to_i2c_client(dev));
	int value;

	switch (to_sensor_dev_attr(attr)->index) {
	case 0 :
		/*      No other then ground level      */
		value = 0;
		break;
	case 1 :
		/*      Lock mutex and fetch reference value    */
		if (mutex_lock_interruptible(&ad799x->lock))
			return -ERESTARTSYS;

		value = ad799x->reference;

		mutex_unlock(&ad799x->lock);
		break;
	default:
		return -EINVAL;
	}

	return sprintf(buf, "%d\n", value);
}

/*
 *     Returns bytes written on success, -EINVAL if input value
 *     is invalid and -ERESTARTSYS on problems locking mutex.
 *
 */
static ssize_t ad799x_set(struct device *dev, struct device_attribute *devattr,
			  const char *buf, size_t count)
{
	struct ad799x_data *ad799x = i2c_get_clientdata(to_i2c_client(dev));
	unsigned long new_ref;

	/*      Fetch incoming value and check it for validity  */
	if (strict_strtoul(buf, 10, &new_ref))
		return -EINVAL;

	/*      Lock data struct and set new input reference value      */
	if (mutex_lock_interruptible(&ad799x->lock))
		return -ERESTARTSYS;

	ad799x->reference = new_ref;

	mutex_unlock(&ad799x->lock);

	return count;
}

/*     Tighing attribute names to functions    */
static SENSOR_DEVICE_ATTR(in0_input, S_IRUGO, ad799x_read, NULL, 0);
static SENSOR_DEVICE_ATTR(in1_input, S_IRUGO, ad799x_read, NULL, 1);
static SENSOR_DEVICE_ATTR(in2_input, S_IRUGO, ad799x_read, NULL, 2);
static SENSOR_DEVICE_ATTR(in3_input, S_IRUGO, ad799x_read, NULL, 3);
static SENSOR_DEVICE_ATTR(in_min, S_IRUGO, ad799x_show, NULL, 0);
static SENSOR_DEVICE_ATTR(in_max, S_IWUSR | S_IRUGO,
		ad799x_show, ad799x_set, 1);

/*     Sysfs attributes	*/
static struct attribute *ad799x_attributes[] = {
	&sensor_dev_attr_in0_input.dev_attr.attr,
	&sensor_dev_attr_in1_input.dev_attr.attr,
	&sensor_dev_attr_in2_input.dev_attr.attr,
	&sensor_dev_attr_in3_input.dev_attr.attr,
	&sensor_dev_attr_in_min.dev_attr.attr,
	&sensor_dev_attr_in_max.dev_attr.attr,
	NULL
};

/*     Attribute group */
static const struct attribute_group ad799x_attr_group = {
	.attrs = ad799x_attributes,
};

/*
 *     Returns 0 on success and error code on failure.
 *
 *
 */
static int ad799x_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	int err = 0;
	struct ad799x_data *ad799x;

	/*      Allocating memory to hold device/client specific values */
	if (!(ad799x = kzalloc(sizeof(struct ad799x_data), GFP_KERNEL))) {
		err = -ENOMEM;
		goto exit;
	}

	/*      Init dev data mutex     */
	mutex_init(&ad799x->lock);

	ad799x->hwmon_dev = hwmon_device_register(&client->dev);

	if (ad799x->hwmon_dev == NULL) {
		err = PTR_ERR(ad799x->hwmon_dev);
		goto hwmon_reg_failed;
	}

	/*      Tighing dev data to client struct       */
	dev_set_drvdata(&client->dev, ad799x);

	/*      Register sysfs hook     */
	if ((err = sysfs_create_group(&client->dev.kobj, &ad799x_attr_group)))
		goto sysfs_create_group_failed;

	/*      Default reference voltage 3300 mV       */
	if (mutex_lock_interruptible(&ad799x->lock))
		return -ERESTARTSYS;

	ad799x->reference = 3300;

	mutex_unlock(&ad799x->lock);

	return 0;

sysfs_create_group_failed:
	hwmon_device_unregister(ad799x->hwmon_dev);
hwmon_reg_failed:
	dev_set_drvdata(&client->dev, NULL);
	kfree(ad799x);
exit:
	return err;
}

/*     Removing different device registrations (buttom of probe func.) */
static int __exit ad799x_remove(struct i2c_client *client)
{
	struct ad799x_data *ad799x = i2c_get_clientdata(client);

	sysfs_remove_group(&client->dev.kobj, &ad799x_attr_group);

	hwmon_device_unregister(ad799x->hwmon_dev);

	dev_set_drvdata(&client->dev, NULL);
	kfree(ad799x);

	return 0;
}

/*     All supported devices   */
static struct i2c_device_id ad799x_idtable[] = {
	{ "ad7991", },
	{ "ad7993", },
	{ "ad7994", },
	{ "ad7995", },
	{ "ad7999", },
	{}
};

/*     Tighs id_table to device	*/
MODULE_DEVICE_TABLE(i2c, ad799x_idtable);

static struct i2c_driver ad799x_driver = {
	.driver = {
		.name   = "ad799x",
	},
	.probe          = ad799x_probe,
	.remove         = ad799x_remove,
	.id_table       = ad799x_idtable,
};

MODULE_AUTHOR("Sigurd Myhre Andreassen <sigurdan at stud.ntnu.no>");
MODULE_DESCRIPTION("Driver for the Norwegian Defence Research"
		   "Establishment (FFI). Analog Devices AD7991/5/9 driver");
MODULE_LICENSE("GPL");

module_i2c_driver(ad799x_driver);
