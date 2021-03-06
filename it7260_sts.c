/*
* single touch screen driver for it7260
*/
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <asm/io.h>
#include <linux/i2c.h>
#include <linux/timer.h>

/* buffer address */
#define CMD_BUF		0x20	/* command buffer (write only) */
#define SYS_CMD_BUF	0x40	/* systerm command buffer (write only) */
#define QUERY_BUF	0x80	/* query buffer (read only) */
#define CMD_RSP_BUF	0xA0	/* command response buffer (read only) */
#define SYS_CMD_RSP_BUF	0xC0	/* systerm command response buffer (read only) */
#define POINT_INFO_BUF	0xE0	/* point information buffer (read only) */

struct it7260_ts_priv {
	struct i2c_client *client;
	struct input_dev *input;
	struct delayed_work work;
	struct mutex mutex;
	int irq;
};

/**
* i2c_master_read_it7260 - issue two I2C message in master receive mode
* @client: handler to slave device
* @buf_index: buffer address
* @buf_data: where to store data read from slave
* @len_data: the bytes of buf_data to read 
*
* returns negative errno, or else the number of bytes read
*/
static int i2c_master_read_it7260(struct i2c_client *client, 
		unsigned char buf_index, unsigned char *buf_data, 
		unsigned short len_data)
{
	int ret;
	struct i2c_msg msgs[2] = {
		{
			.addr = client->addr,
			.flags = I2C_M_NOSTART,
			.len = 1,
			.buf = &buf_index,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = len_data,
			.buf = buf_data,
		}
	};

	ret = i2c_transfer(client->adapter, msgs, 2);

	return (ret == 2) ? len_data : ret;
}

/**
* i2c_master_write_it7260 - issue a single I2C message in master transmit mode
* @client: handler to slave device
* @buf_index: buffer address
* @buf_data: data that wile be write to the slave
* @len_data: the bytes of buf_data to write
*
* returns negative errno, or else the number of bytes written
*/
static int i2c_master_write_it7260(struct i2c_client *client, 
		unsigned char buf_index, unsigned char const *buf_data,
		unsigned short len_data)
{
	unsigned char buf[2];
	int ret;

	struct i2c_msg msgs[1] = {
		{
			.addr = client->addr,
			.flags = 0, /* default write flag */
			.len = len_data + 1, 
			.buf = buf,
		}
	};

	buf[0] = buf_index;
	memcpy(&buf[1], buf_data, len_data);

	ret = i2c_transfer(client->adapter, msgs, 1);
	
	return (ret == 1) ? sizeof(buf) : ret;
}

/**
* it7260_ts_poscheck - delayed work 
*
* get the informations of contacts from slave and report it
*/
static void it7260_ts_poscheck(struct work_struct *work)
{
	struct it7260_ts_priv *priv = container_of(work, 
							struct it7260_ts_priv, work.work);
	unsigned char buf[14];
	unsigned short xpos = 0, ypos = 0;
	unsigned char pressure;
	unsigned char query = 0;

	mutex_lock(&priv->mutex);

	i2c_master_read_it7260(priv->client, QUERY_BUF, &query, 1);
	if (!(query & 0x80)) {
		dev_info(&priv->client->dev, "no finger touch\n");
		goto out;
	}

	memset(&buf, 0, sizeof(buf));

	i2c_master_read_it7260(priv->client, POINT_INFO_BUF, buf, 14);

    /* touch key */
    if (buf[0] == 0x41) {
        dev_info(&priv->client->dev, "the key number %d\n", buf[1]);
        if (buf[1] == 0x04)
            input_report_key(priv->input, KEY_HOME, !!buf[2]);
        else if (buf[1] == 0x03)
            input_report_key(priv->input, KEY_MENU, !!buf[2]);
        else if (buf[1] == 0x02)
            input_report_key(priv->input, KEY_BACK, !!buf[2]);
        else if (buf[1] == 0x01)
            input_report_key(priv->input, KEY_POWER, !!buf[2]);
        else
            goto out;

        input_sync(priv->input);
        goto out;
    }

	xpos = ((buf[3] & 0x0F) << 8) | buf[2];
	ypos = ((buf[3] & 0xF0) << 4) | buf[4];
	pressure = buf[5];
	dev_info(&priv->client->dev, "xpos = %d, ypos = %d, pressure= %d\n", 
							ypos, xpos, pressure);

	input_report_key(priv->input, BTN_TOUCH, !!pressure);
	input_report_abs(priv->input, ABS_X, ypos);
	input_report_abs(priv->input, ABS_Y, xpos);
	input_report_abs(priv->input, ABS_PRESSURE, pressure);
	input_sync(priv->input);

out:
	mutex_unlock(&priv->mutex);
	enable_irq(priv->irq);
}

static irqreturn_t it7260_ts_isr(int irq, void *dev_id)
{
	struct it7260_ts_priv *priv = dev_id;

	disable_irq_nosync(irq);
	schedule_delayed_work(&priv->work, HZ / 20);

	return IRQ_HANDLED;
}

/**
* it7260_identify_capsensor - identify capacitance sensor model
*
* returns error -1, or else suc 0
*/
static int it7260_identify_capsensor(struct i2c_client *client)
{
	unsigned char buf[10];
	unsigned char query = 0;

	do {
		i2c_master_read_it7260(client, QUERY_BUF, &query, 1);
	} while (query & 0x01);

	/* 0x00: the command of identify cap sensor */
	buf[0] = 0x00;	
	i2c_master_write_it7260(client, CMD_BUF, buf, 1);

	do {
		i2c_master_read_it7260(client, QUERY_BUF, &query, 1);
	} while (query & 0x01);

	memset(&buf, 0, sizeof(buf));
	i2c_master_read_it7260(client, CMD_RSP_BUF, buf, 10);
	dev_info(&client->dev, "len = %d, %c%c%c\n", buf[0], buf[1], buf[2], buf[3]);
	if (buf[1] != 'I' || buf[2] != 'T' || buf[3] != 'E') 
		return -1;

	return 0;
}

static int it7260_ts_probe(struct i2c_client *client, 
			const struct i2c_device_id *idp)
{
	struct it7260_ts_priv *priv;
	struct input_dev *input;
	int error;

	error = it7260_identify_capsensor(client);
	if (error) {
		dev_err(&client->dev, "cannot identify the touch screen\n");
		goto err0;
	}

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		dev_err(&client->dev, "failed to allocate driver data\n");
		error = -ENOMEM;
		goto err0;
	}
	
	mutex_init(&priv->mutex);

	dev_set_drvdata(&client->dev, priv);

	input = input_allocate_device();
	if (!input) {
		dev_err(&client->dev, "failed to allocate input device\n");
		error = -ENOMEM;
		goto err1;
	}

	input->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS);
	input->keybit[BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH);

	input_set_capability(input, EV_KEY, KEY_MENU);
	input_set_capability(input, EV_KEY, KEY_BACK);
	input_set_capability(input, EV_KEY, KEY_HOME);
	input_set_capability(input, EV_KEY, KEY_POWER);

	input_set_abs_params(input, ABS_X, 0, 600, 0, 0);
	input_set_abs_params(input, ABS_Y, 0, 1024, 0, 0);
	input_set_abs_params(input, ABS_PRESSURE, 0, 16, 0, 0);

	input->name = "it7260 touch screen";
	input->phys = "I2C";
	input->id.bustype = BUS_I2C;

	input_set_drvdata(input, priv);

	priv->client = client;
	priv->input = input;
	INIT_DELAYED_WORK(&priv->work, it7260_ts_poscheck);
	priv->irq = client->irq;

	error = input_register_device(input);
	if (error) {
		dev_err(&client->dev, "failed to register input device\n");
		goto err1;
	}

	error = request_irq(priv->irq, it7260_ts_isr, IRQF_TRIGGER_LOW,
						client->name, priv);
	if (error) {
		dev_err(&client->dev, "unable to request touchscreen IRQ\n");
		goto err2;
	}

	device_init_wakeup(&client->dev, 1);

	return 0;
	
err2:
	input_unregister_device(input);
	input = NULL;
err1:
	input_free_device(input);
	kfree(priv);
err0:
	dev_set_drvdata(&client->dev, NULL);
	return error;
}

static int __devexit it7260_ts_remove(struct i2c_client *client)
{
	struct it7260_ts_priv *priv = dev_get_drvdata(&client->dev);

	free_irq(priv->irq, priv);
	input_unregister_device(priv->input);
	kfree(priv);

	dev_set_drvdata(&client->dev, NULL);

	return 0;
}

static int it7260_ts_suspend(struct i2c_client *client, pm_message_t mesg)
{
	int ret = -1;
	u8 suspend_cmd[] = {0x04, 0x00, 0x02};
	struct it7260_ts_priv *priv = i2c_get_clientdata(client);

	if (device_may_wakeup(&client->dev)) {
		enable_irq_wake(priv->irq);
		if (sizeof(suspend_cmd) == i2c_master_write_it7260(client, 
					CMD_BUF, suspend_cmd, 3)) 
			ret = 0;
	} 
	dev_err(&client->dev, "suspend touch screen\n");

	return ret;
}

static int it7260_ts_resume(struct i2c_client *client)
{
	int ret = -1;
	unsigned char query;
	struct it7260_ts_priv *priv = i2c_get_clientdata(client);

	if (device_may_wakeup(&client->dev)) {
		i2c_master_read_it7260(client, QUERY_BUF, &query, 1);
		disable_irq_wake(priv->irq);
		ret = 0;
	}
	dev_err(&client->dev, "resume touch screen\n");

	return ret;
}

static const struct i2c_device_id it7260_ts_id[] = {
	{"IT7260", 0},
	{"IT7250", 1},
	{}			/* should not omit */
};
MODULE_DEVICE_TABLE(i2c, it7260_ts_id);

static struct i2c_driver it7260_ts_driver = {
	.driver = {
		.name = "IT7260-ts",
	},
	.probe = it7260_ts_probe,
	.remove = __devexit_p(it7260_ts_remove),
	.suspend = it7260_ts_suspend,
	.resume = it7260_ts_resume,
	.id_table = it7260_ts_id,
};


static int __init it7260_ts_init(void)
{
	return i2c_add_driver(&it7260_ts_driver);	
}

static void __exit it7260_ts_exit(void)
{
	i2c_del_driver(&it7260_ts_driver);
}

module_init(it7260_ts_init);
module_exit(it7260_ts_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("CJOK <cjok.liao@gmail.com>");
MODULE_DESCRIPTION("it7260 touchscreen driver");

