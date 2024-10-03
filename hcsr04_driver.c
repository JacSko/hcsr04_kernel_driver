#include <linux/module.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/list.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/property.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/completion.h>
#include <linux/delay.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jacek Skowronek");
MODULE_DESCRIPTION("HC-SR04 sensor driver");

#define MAX_DEVICES_COUNT 10
static unsigned int DEVICE_READ_TIMEOUT_MS = 100;
static unsigned int DRIVER_MAJOR;

module_param(DEVICE_READ_TIMEOUT_MS, uint, S_IRUGO);
MODULE_PARM_DESC(DEVICE_READ_TIMEOUT_MS, "Maximum timeout to wait for measurement in milliseconds");

static int hcsr04_probe(struct platform_device *pdev);
static int hcsr04_remove(struct platform_device *pdev);
static ssize_t hcsr04_read(struct file *File, char *user_buffer, size_t count, loff_t *offs);
static int hcsr04_open(struct inode* inode, struct file* filep);
static int hcsr04_release(struct inode* inode, struct file* filep);
static struct class device_class = {.name = "hcsr04_dev"};

struct hcsr04_data
{
   bool in_use;
   const char* name;
   dev_t device_no;
   struct platform_device* device;
   struct gpio_desc* echo_gpio;
   struct gpio_desc* trigger_gpio;
   unsigned int irq_number;
   struct completion completion;
   struct mutex lock;
   s64 measurement_start;
   s64 measurement_end;
};

static struct hcsr04_data hcsr04_devices [MAX_DEVICES_COUNT];
static struct of_device_id hcsr04_ids[] =
{
   {
		.compatible = "jskowronek,hcsr04",
	}, {}
};
MODULE_DEVICE_TABLE(of, hcsr04_ids);
DEFINE_MUTEX(device_list_mutex);

static struct platform_driver hcsr04_driver =
{
	.probe = hcsr04_probe,
	.remove = hcsr04_remove,
	.driver = {
		.name = "hcsr04_driver",
		.of_match_table = hcsr04_ids,
	},
};

static struct file_operations fops =
{
   .owner = THIS_MODULE,
   .read = hcsr04_read,
   .open = hcsr04_open,
   .release = hcsr04_release,
};

static irqreturn_t hcsr04_interrupt_handler(int irq, void* dev_id)
{
   struct hcsr04_data* device;
   device = dev_id;

   if (gpiod_get_value(device->echo_gpio) == 0)
   {
      if (device->measurement_start != -1)
      {
         device->measurement_end = ktime_get_boottime_ns();
         complete(&device->completion);
      }
      else
      {
         printk(KERN_ERR "Got end of measurement, but start is missing!\n");
      }
   }
   else
   {
      device->measurement_start = ktime_get_boottime_ns();
   }

   return IRQ_HANDLED;
}

struct hcsr04_data* hcsr04_get_by_devt(dev_t dev)
{
   struct hcsr04_data* result = NULL;
   int i;
   for (i = 0; i < MAX_DEVICES_COUNT; i++)
   {
      if (hcsr04_devices[i].device_no == dev)
      {
         result = &hcsr04_devices[i];
         break;
      }
   }
   return result;
}
static bool hcsr04_measurement_check(int distance_mm)
{
   static const int MAX_MEASURED_DISTANCE_MM = 4000;
   static const int MIN_MEASURED_DISTANCE_MM = 20;

   if (distance_mm < (MAX_MEASURED_DISTANCE_MM + 1000) && distance_mm > MIN_MEASURED_DISTANCE_MM - 15)
   {
      return true;
   }
   return false;
}
static ssize_t hcsr04_read(struct file *File, char *user_buffer, size_t count, loff_t *offs)
{
   int result;
   int minor;
   struct hcsr04_data* device;
   int time_difference;
   int distance_mm;
   device = File->private_data;
   mutex_lock(&device->lock);

   do
   {
      if (!device)
      {
         printk(KERN_ERR "[%s:?] Empty device received\n", __func__);
         result = ENODEV;
         break;
      }
      minor = device - hcsr04_devices;
      if (count != 2)
      {
         printk(KERN_ERR "[%s:%d] Invalid bytes count (only 2 bytes are supported)\n", __func__, minor);
         result = EINVAL;
         break;
      }
      if (!user_buffer)
      {
         printk(KERN_ERR "[%s:%d] Invalid buffer provided\n", __func__, minor);
         result = EINVAL;
         break;
      }
      device->irq_number = gpiod_to_irq(device->echo_gpio);
      result = request_irq(device->irq_number, hcsr04_interrupt_handler, IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING, device->name,  device);
      if (result != 0)
      {
         printk(KERN_ERR "[%s:%d] Cannot request interrupt with number %d, error %d\n", __func__, minor, device->irq_number, result);
         break;
      }
      reinit_completion(&device->completion);
      device->measurement_start = -1;
      device->measurement_end = -1;
      result = gpiod_direction_output(device->trigger_gpio, 1);
      if (result)
      {
         printk(KERN_ERR "[%s:%d] Cannot set TRIGGER line to high, error %d\n", __func__, minor, result);
         break;
      }

      usleep_range(10,20);

      result = gpiod_direction_output(device->trigger_gpio, 0);
      if (result)
      {
         printk(KERN_ERR "[%s:%d] Cannot set TRIGGER line to low, error %d\n", __func__, minor, result);
         break;
      }

      result = wait_for_completion_killable_timeout(&device->completion, msecs_to_jiffies(100));
      free_irq(device->irq_number, device);
      if (result <= 0)
      {
         printk(KERN_ERR "[%s:%d] Timeout waiting for sensor response, error %d\n", __func__, minor, result);
         break;
      }
      if (device->measurement_start == 1 || device->measurement_end == 1)
      {
         printk(KERN_ERR "[%s:%d] Missing start or/and end timestamp!\n", __func__, minor);
         result = ENOMSG;
         break;
      }
      time_difference = device->measurement_end - device->measurement_start;
      distance_mm = (time_difference / 1000) / 53;

      if (!hcsr04_measurement_check(distance_mm))
      {
         printk(KERN_ERR "[%s:%d] Invalid measurement: %d\n", __func__, minor, distance_mm);
         result = ENOMSG;
         break;
      }

      /* Max measured value should oscillate around 5000[mm] so it is enough to forward 2 bytes to user space */
      user_buffer[0] = (distance_mm >> 8) & 0xFF;
      user_buffer[1] = distance_mm & 0xFF;
      printk(KERN_ERR "[%s:%d] result: %d[mm]\n", __func__, minor, distance_mm);
      result = count;
   }
   while(0);

   mutex_unlock(&device->lock);
   return result;
}

static int hcsr04_open(struct inode* inode, struct file* filep)
{
   struct hcsr04_data* device;
   int result;
   mutex_lock(&device_list_mutex);

   do
   {
      device = hcsr04_get_by_devt(inode->i_rdev);
      if (!device)
      {
         printk(KERN_ERR "[%s:?] Empty device received\n", __func__);
         result = ENODEV;
         break;
      }
      mutex_lock(&device->lock);
      device->in_use = true;
      filep->private_data = device;
      mutex_unlock(&device->lock);
      result = 0;
   } while(0);

   mutex_unlock(&device_list_mutex);
   return result;
}

static int hcsr04_release(struct inode* inode, struct file* filep)
{
   struct hcsr04_data* device;
   int result;

   mutex_lock(&device_list_mutex);
   device = hcsr04_get_by_devt(inode->i_rdev);
   result = 0;
   if (!device)
   {
      printk(KERN_ERR "[%s:?] Empty device received\n", __func__);
      result = ENODEV;
   }
   filep->private_data = NULL;
   mutex_unlock(&device_list_mutex);
   return result;
}

static int __init hcsr04_init(void)
{
   int status;

   DRIVER_MAJOR = register_chrdev(0, "hcsr04", &fops);
   if (DRIVER_MAJOR < 0)
   {
      printk(KERN_ERR "[%s:?] Cannot register chardev\n", __func__);
      platform_driver_unregister(&hcsr04_driver);
      return DRIVER_MAJOR;
   }

   status = class_register(&device_class);
   if (status)
   {
      printk(KERN_ERR "[%s:?] Cannot register device class\n", __func__);
      platform_driver_unregister(&hcsr04_driver);
      unregister_chrdev(DRIVER_MAJOR, hcsr04_driver.driver.name);
      return status;
   }

   status = platform_driver_register(&hcsr04_driver);
   if (status)
   {
		printk(KERN_ERR "[%s:?] Could not load driver\n", __func__);
		return status;
	}
   return 0;
}

static void __exit hcsr04_exit(void)
{
   platform_driver_unregister(&hcsr04_driver);
   class_unregister(&device_class);
   unregister_chrdev(DRIVER_MAJOR, hcsr04_driver.driver.name);
}

struct hcsr04_data* hcsr04_get_data(struct platform_device* pdev)
{
   struct hcsr04_data* result = NULL;
   int i;
   for (i = 0; i < MAX_DEVICES_COUNT; i++)
   {
      if (hcsr04_devices[i].device == pdev)
      {
         result = &hcsr04_devices[i];
         break;
      }
   }
   return result;
}

struct hcsr04_data* hcsr04_get_not_used(void)
{
   struct hcsr04_data* result = NULL;
   int i;
   for (i = 0; i < MAX_DEVICES_COUNT; i++)
   {
      if (hcsr04_devices[i].in_use == false)
      {
         result = &hcsr04_devices[i];
         break;
      }
   }
   return result;
}

static int hcsr04_probe(struct platform_device *pdev)
{
   struct hcsr04_data* device;
   int minor;
   int result;

   device = hcsr04_get_not_used();
   if (device == NULL)
   {
      printk(KERN_ERR "[%s:?] Cannot find free space for new device!\n", __func__);
      return EBUSY;
   }
   minor = device - hcsr04_devices;
   if (device_property_read_string(&pdev->dev, "label", &device->name) != 0)
   {
      printk(KERN_ERR "[%s:%d] Cannot find device property 'label'\n", __func__, minor);
      return ENOENT;
   }
   if (!device_property_present(&pdev->dev, "echo-gpio"))
   {
      printk(KERN_ERR "[%s:%d] Cannot find device property 'echo-gpio'\n", __func__, minor);
      return ENOENT;
   }
   if (!device_property_present(&pdev->dev, "trigger-gpio"))
   {
      printk(KERN_ERR "[%s:%d] Cannot find device property 'trigger-gpio'\n", __func__, minor);
      return ENOENT;
   }
   mutex_init(&device->lock);
   device->device = pdev;

   mutex_lock(&device_list_mutex);
   printk(KERN_INFO "[%s:%d] Allocating device, name %s\n", __func__, minor, device->name);
   device->device_no = MKDEV(DRIVER_MAJOR, minor);
   result = 0;
   do
   {
      if (device_create(&device_class, &pdev->dev, device->device_no, device, "HCSR04_Driver%d",minor) == NULL)
      {
         printk(KERN_ERR "[%s:%d] Cannot create device!\n", __func__, minor);
         result = EBUSY;
         break;
      }

      /* setup GPIOs */
      device->echo_gpio = gpiod_get(&pdev->dev, "echo", GPIOD_IN);
      if (IS_ERR(device->echo_gpio))
      {
         printk(KERN_ERR "[%s:%d] Cannot get echo-gpio for device %s, error %ld!\n", __func__, minor, device->name, PTR_ERR(device->echo_gpio));
         result = -1 * PTR_ERR(device->echo_gpio);
         break;
      }
      device->trigger_gpio = gpiod_get(&pdev->dev, "trigger", GPIOD_OUT_LOW);
      if (IS_ERR(device->trigger_gpio))
      {
         printk(KERN_ERR "[%s:%d] Cannot get trigger-gpio for device %s, error %ld!\n", __func__, minor, device->name, PTR_ERR(device->trigger_gpio));
         result = -1 * PTR_ERR(device->trigger_gpio);
         break;
      }
   } while(0);

   device->in_use = true;
   init_completion(&device->completion);
   mutex_unlock(&device_list_mutex);

   return result;
}

static int hcsr04_remove(struct platform_device *pdev)
{
   struct hcsr04_data* device;

   mutex_lock(&device_list_mutex);
   device = hcsr04_get_data(pdev);
   if (!device)
   {
      printk(KERN_ERR "[%s:?] Empty device received\n", __func__);
   }
   printk(KERN_INFO "[%s:%d] Deallocating device, name %s\n", __func__, device - hcsr04_devices, device->name);
   device->in_use = false;
   device_destroy(&device_class, device->device_no);
   device->device = NULL;
   gpiod_put(device->echo_gpio);
   gpiod_put(device->trigger_gpio);
   device->echo_gpio = NULL;
   device->trigger_gpio = NULL;
   device->name = NULL;
   mutex_unlock(&device_list_mutex);

   return 0;
}

module_init(hcsr04_init);
module_exit(hcsr04_exit);
