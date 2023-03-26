// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020, NVIDIA CORPORATION.  All rights reserved.
 */

#define pr_fmt(f) KBUILD_MODNAME ": " f "\n"

#include <linux/acpi.h>
#include <linux/backlight.h>
#include <linux/dmi.h>
#include <linux/fixp-arith.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/wmi.h>

/**
 * enum wmi_brightness_method - WMI method IDs
 * @WMI_BRIGHTNESS_METHOD_LEVEL:  Get/Set EC brightness level status
 * @WMI_BRIGHTNESS_METHOD_SOURCE: Get/Set EC Brightness Source
 */
enum wmi_brightness_method {
	WMI_BRIGHTNESS_METHOD_LEVEL = 1,
	WMI_BRIGHTNESS_METHOD_SOURCE = 2,
	WMI_BRIGHTNESS_METHOD_MAX
};

/**
 * enum wmi_brightness_mode - Operation mode for WMI-wrapped method
 * @WMI_BRIGHTNESS_MODE_GET:            Get the current brightness level/source.
 * @WMI_BRIGHTNESS_MODE_SET:            Set the brightness level.
 * @WMI_BRIGHTNESS_MODE_GET_MAX_LEVEL:  Get the maximum brightness level. This
 *                                      is only valid when the WMI method is
 *                                      %WMI_BRIGHTNESS_METHOD_LEVEL.
 */
enum wmi_brightness_mode {
	WMI_BRIGHTNESS_MODE_GET = 0,
	WMI_BRIGHTNESS_MODE_SET = 1,
	WMI_BRIGHTNESS_MODE_GET_MAX_LEVEL = 2,
	WMI_BRIGHTNESS_MODE_MAX
};

/**
 * enum wmi_brightness_source - Backlight brightness control source selection
 * @WMI_BRIGHTNESS_SOURCE_GPU: Backlight brightness is controlled by the GPU.
 * @WMI_BRIGHTNESS_SOURCE_EC:  Backlight brightness is controlled by the
 *                             system's Embedded Controller (EC).
 * @WMI_BRIGHTNESS_SOURCE_AUX: Backlight brightness is controlled over the
 *                             DisplayPort AUX channel.
 */
enum wmi_brightness_source {
	WMI_BRIGHTNESS_SOURCE_GPU = 1,
	WMI_BRIGHTNESS_SOURCE_EC = 2,
	WMI_BRIGHTNESS_SOURCE_AUX = 3,
	WMI_BRIGHTNESS_SOURCE_MAX
};

/**
 * struct wmi_brightness_args - arguments for the WMI-wrapped ACPI method
 * @mode:    Pass in an &enum wmi_brightness_mode value to select between
 *           getting or setting a value.
 * @val:     In parameter for value to set when using %WMI_BRIGHTNESS_MODE_SET
 *           mode. Not used in conjunction with %WMI_BRIGHTNESS_MODE_GET or
 *           %WMI_BRIGHTNESS_MODE_GET_MAX_LEVEL mode.
 * @ret:     Out parameter returning retrieved value when operating in
 *           %WMI_BRIGHTNESS_MODE_GET or %WMI_BRIGHTNESS_MODE_GET_MAX_LEVEL
 *           mode. Not used in %WMI_BRIGHTNESS_MODE_SET mode.
 * @ignored: Padding; not used. The ACPI method expects a 24 byte params struct.
 *
 * This is the parameters structure for the WmiBrightnessNotify ACPI method as
 * wrapped by WMI. The value passed in to @val or returned by @ret will be a
 * brightness value when the WMI method ID is %WMI_BRIGHTNESS_METHOD_LEVEL, or
 * an &enum wmi_brightness_source value with %WMI_BRIGHTNESS_METHOD_SOURCE.
 */
struct wmi_brightness_args {
	u32 mode;
	u32 val;
	u32 ret;
	u32 ignored[3];
};

/**
 * struct nvidia_wmi_ec_backlight_priv - driver private data
 * @bl_dev:       the associated backlight device
 * @proxy_target: backlight device which receives relayed brightness changes
 * @notifier:     notifier block for resume callback
 */
struct nvidia_wmi_ec_backlight_priv {
	struct backlight_device *bl_dev;
	struct backlight_device *proxy_target;
	struct notifier_block nb;
};

static char *backlight_proxy_target;
module_param(backlight_proxy_target, charp, 0444);
MODULE_PARM_DESC(backlight_proxy_target, "Relay brightness change requests to the named backlight driver, on systems which erroneously report EC backlight control.");

static int max_reprobe_attempts = 128;
module_param(max_reprobe_attempts, int, 0444);
MODULE_PARM_DESC(max_reprobe_attempts, "Limit of reprobe attempts when relaying brightness change requests.");

static bool restore_level_on_resume;
module_param(restore_level_on_resume, bool, 0444);
MODULE_PARM_DESC(restore_level_on_resume, "Restore the backlight level when resuming from suspend, on systems which reset the EC's backlight level on resume.");

/* Bit field values for quirks table */

#define NVIDIA_WMI_EC_BACKLIGHT_QUIRK_RESTORE_LEVEL_ON_RESUME   BIT(0)

/* bits 1-7: reserved for future quirks; bits 8+: proxy target device names */

#define NVIDIA_WMI_EC_BACKLIGHT_QUIRK_PROXY_TO_AMDGPU       BIT(8)

#define QUIRK(name) NVIDIA_WMI_EC_BACKLIGHT_QUIRK_##name
#define HAS_QUIRK(data, name) (((long) data) & QUIRK(name))

static int assign_quirks(const struct dmi_system_id *id)
{
	if (HAS_QUIRK(id->driver_data, RESTORE_LEVEL_ON_RESUME))
		restore_level_on_resume = 1;

	/* If the module parameter is set, override the quirks table */
	if (!backlight_proxy_target) {
		if (HAS_QUIRK(id->driver_data, PROXY_TO_AMDGPU))
			backlight_proxy_target = "amdgpu_bl0";
	}

	return true;
}

#define QUIRK_ENTRY(vendor, product, quirks) {          \
	.callback = assign_quirks,                      \
	.matches = {                                    \
		DMI_MATCH(DMI_SYS_VENDOR, vendor),      \
		DMI_MATCH(DMI_PRODUCT_VERSION, product) \
	},                                              \
	.driver_data = (void *)(quirks)                 \
}

static const struct dmi_system_id quirks_table[] = {
	QUIRK_ENTRY(
		/* This quirk is preset as of firmware revision HACN31WW */
		"LENOVO", "Legion S7 15ACH6",
		QUIRK(RESTORE_LEVEL_ON_RESUME) | QUIRK(PROXY_TO_AMDGPU)
	),
	{ }
};

/**
 * wmi_brightness_notify() - helper function for calling WMI-wrapped ACPI method
 * @w:    Pointer to the struct wmi_device identified by %WMI_BRIGHTNESS_GUID
 * @id:   The WMI method ID to call (e.g. %WMI_BRIGHTNESS_METHOD_LEVEL or
 *        %WMI_BRIGHTNESS_METHOD_SOURCE)
 * @mode: The operation to perform on the method (e.g. %WMI_BRIGHTNESS_MODE_SET
 *        or %WMI_BRIGHTNESS_MODE_GET)
 * @val:  Pointer to a value passed in by the caller when @mode is
 *        %WMI_BRIGHTNESS_MODE_SET, or a value passed out to caller when @mode
 *        is %WMI_BRIGHTNESS_MODE_GET or %WMI_BRIGHTNESS_MODE_GET_MAX_LEVEL.
 *
 * Returns 0 on success, or a negative error number on failure.
 */
static int wmi_brightness_notify(struct wmi_device *w, enum wmi_brightness_method id, enum wmi_brightness_mode mode, u32 *val)
{
	struct wmi_brightness_args args = {
		.mode = mode,
		.val = 0,
		.ret = 0,
	};
	struct acpi_buffer buf = { (acpi_size)sizeof(args), &args };
	acpi_status status;

	if (id < WMI_BRIGHTNESS_METHOD_LEVEL ||
	    id >= WMI_BRIGHTNESS_METHOD_MAX ||
	    mode < WMI_BRIGHTNESS_MODE_GET || mode >= WMI_BRIGHTNESS_MODE_MAX)
		return -EINVAL;

	if (mode == WMI_BRIGHTNESS_MODE_SET)
		args.val = *val;

	status = wmidev_evaluate_method(w, 0, id, &buf, &buf);
	if (ACPI_FAILURE(status)) {
		dev_err(&w->dev, "EC backlight control failed: %s\n",
			acpi_format_exception(status));
		return -EIO;
	}

	if (mode != WMI_BRIGHTNESS_MODE_SET)
		*val = args.ret;

	return 0;
}

/* Scale the current brightness level of 'from' to the range of 'to'. */
static int scale_backlight_level(const struct backlight_device *from,
				 const struct backlight_device *to)
{
	int from_max = from->props.max_brightness;
	int from_level = from->props.brightness;
	int to_max = to->props.max_brightness;

	return fixp_linear_interpolate(0, 0, from_max, to_max, from_level);
}

static int nvidia_wmi_ec_backlight_update_status(struct backlight_device *bd)
{
	struct wmi_device *wdev = bl_get_data(bd);
	struct nvidia_wmi_ec_backlight_priv *priv = dev_get_drvdata(&wdev->dev);
	struct backlight_device *proxy_target = priv->proxy_target;

	if (proxy_target) {
		int level = scale_backlight_level(bd, proxy_target);

		if (backlight_device_set_brightness(proxy_target, level))
			pr_warn("Failed to relay backlight update to \"%s\"",
				backlight_proxy_target);
	}

	return wmi_brightness_notify(wdev, WMI_BRIGHTNESS_METHOD_LEVEL,
	                             WMI_BRIGHTNESS_MODE_SET,
			             &bd->props.brightness);
}

static int nvidia_wmi_ec_backlight_get_brightness(struct backlight_device *bd)
{
	struct wmi_device *wdev = bl_get_data(bd);
	u32 level;
	int ret;

	ret = wmi_brightness_notify(wdev, WMI_BRIGHTNESS_METHOD_LEVEL,
	                            WMI_BRIGHTNESS_MODE_GET, &level);
	if (ret < 0)
		return ret;

	return level;
}

static const struct backlight_ops nvidia_wmi_ec_backlight_ops = {
	.update_status = nvidia_wmi_ec_backlight_update_status,
	.get_brightness = nvidia_wmi_ec_backlight_get_brightness,
};

static int nvidia_wmi_ec_backlight_pm_notifier(struct notifier_block *nb, unsigned long event, void *d)
{

	/*
	 * On some systems, the EC backlight level gets reset to 100% when
	 * resuming from suspend, but the backlight device state still reflects
	 * the pre-suspend value. Refresh the existing state to sync the EC's
	 * state back up with the kernel's.
	 */
	if (event == PM_POST_SUSPEND) {
		struct nvidia_wmi_ec_backlight_priv *p;
		int ret;

		p = container_of(nb, struct nvidia_wmi_ec_backlight_priv, nb);
		ret = backlight_update_status(p->bl_dev);

		if (ret)
			pr_warn("failed to refresh backlight level: %d", ret);

		return NOTIFY_OK;
	}

	return NOTIFY_DONE;
}

static void putdev(void *data)
{
	struct device *dev = data;

	put_device(dev);
}

static int nvidia_wmi_ec_backlight_probe(struct wmi_device *wdev, const void *ctx)
{
	struct backlight_device *bdev, *target = NULL;
	struct nvidia_wmi_ec_backlight_priv *priv;
	struct backlight_properties props = {};
	u32 source;
	int ret;

	/*
	 * Check quirks tables to see if this system needs any of the firmware
	 * bug workarounds.
	 */
	dmi_check_system(quirks_table);

	if (backlight_proxy_target && backlight_proxy_target[0]) {
		static int num_reprobe_attempts;

		target = backlight_device_get_by_name(backlight_proxy_target);

		if (target) {
			ret = devm_add_action_or_reset(&wdev->dev, putdev,
						       &target->dev);
			if (ret)
				return ret;
		} else {
			/*
			 * The target backlight device might not be ready;
			 * try again and disable backlight proxying if it
			 * fails too many times.
			 */
			if (num_reprobe_attempts < max_reprobe_attempts) {
				num_reprobe_attempts++;
				return -EPROBE_DEFER;
			}

			pr_warn("Unable to acquire %s after %d attempts. Disabling backlight proxy.",
				backlight_proxy_target, max_reprobe_attempts);
		}
	}

	ret = wmi_brightness_notify(wdev, WMI_BRIGHTNESS_METHOD_SOURCE,
	                           WMI_BRIGHTNESS_MODE_GET, &source);
	if (ret)
		return ret;

	/*
	 * This driver is only to be used when brightness control is handled
	 * by the EC; otherwise, the GPU driver(s) should control brightness.
	 */
	if (source != WMI_BRIGHTNESS_SOURCE_EC)
		return -ENODEV;

	/*
	 * Identify this backlight device as a firmware device so that it can
	 * be prioritized over any exposed GPU-driven raw device(s).
	 */
	props.type = BACKLIGHT_FIRMWARE;

	ret = wmi_brightness_notify(wdev, WMI_BRIGHTNESS_METHOD_LEVEL,
	                           WMI_BRIGHTNESS_MODE_GET_MAX_LEVEL,
	                           &props.max_brightness);
	if (ret)
		return ret;

	ret = wmi_brightness_notify(wdev, WMI_BRIGHTNESS_METHOD_LEVEL,
	                           WMI_BRIGHTNESS_MODE_GET, &props.brightness);
	if (ret)
		return ret;

	bdev = devm_backlight_device_register(&wdev->dev,
	                                      "nvidia_wmi_ec_backlight",
					      &wdev->dev, wdev,
					      &nvidia_wmi_ec_backlight_ops,
					      &props);

	if (IS_ERR(bdev))
		return PTR_ERR(bdev);

	priv = devm_kzalloc(&wdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->bl_dev = bdev;

	dev_set_drvdata(&wdev->dev, priv);

	if (target) {
		int level = scale_backlight_level(target, bdev);

		if (backlight_device_set_brightness(bdev, level))
			pr_warn("Unable to import initial brightness level from %s.",
				backlight_proxy_target);
		priv->proxy_target = target;
	}

	if (restore_level_on_resume) {
		priv->nb.notifier_call = nvidia_wmi_ec_backlight_pm_notifier;
		register_pm_notifier(&priv->nb);
	}

	return 0;
}

static void nvidia_wmi_ec_backlight_remove(struct wmi_device *wdev)
{
	struct nvidia_wmi_ec_backlight_priv *priv = dev_get_drvdata(&wdev->dev);

	if (priv->nb.notifier_call)
		unregister_pm_notifier(&priv->nb);
}

#define WMI_BRIGHTNESS_GUID "603E9613-EF25-4338-A3D0-C46177516DB7"

static const struct wmi_device_id nvidia_wmi_ec_backlight_id_table[] = {
	{ .guid_string = WMI_BRIGHTNESS_GUID },
	{ }
};
MODULE_DEVICE_TABLE(wmi, nvidia_wmi_ec_backlight_id_table);

static struct wmi_driver nvidia_wmi_ec_backlight_driver = {
	.driver = {
		.name = "nvidia-wmi-ec-backlight",
	},
	.probe = nvidia_wmi_ec_backlight_probe,
	.remove = nvidia_wmi_ec_backlight_remove,
	.id_table = nvidia_wmi_ec_backlight_id_table,
};
module_wmi_driver(nvidia_wmi_ec_backlight_driver);

MODULE_AUTHOR("Daniel Dadap <ddadap@nvidia.com>");
MODULE_DESCRIPTION("NVIDIA WMI EC Backlight driver");
MODULE_LICENSE("GPL");
