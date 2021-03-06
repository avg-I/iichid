/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2019 Vladimir Kondratyev <wulf@FreeBSD.org>
 * Copyright (c) 2019 Greg V <greg@unrelenting.technology>
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

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

/*
 * Generic / MS Windows compatible HID pen tablet driver:
 * https://docs.microsoft.com/en-us/windows-hardware/design/component-guidelines/required-hid-top-level-collections
 *
 * Tested on: Wacom WCOM50C1 (Google Pixelbook "eve")
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/sysctl.h>

#include <dev/evdev/input.h>
#include <dev/evdev/evdev.h>

#include <dev/usb/input/usb_rdesc.h>

#include "hid.h"
#include "hidbus.h"
#include "hid_quirk.h"
#include "hmap.h"

#define	HID_DEBUG_VAR	hpen_debug
#include "hid_debug.h"

#ifdef HID_DEBUG
static int hpen_debug = 1;

static SYSCTL_NODE(_hw_hid, OID_AUTO, hpen, CTLFLAG_RW, 0,
		"Generic HID tablet");
SYSCTL_INT(_hw_hid_hpen, OID_AUTO, debug, CTLFLAG_RWTUN,
		&hpen_debug, 0, "Debug level");
#endif

static const uint8_t	hpen_graphire_report_descr[] =
			   { UHID_GRAPHIRE_REPORT_DESCR() };
static const uint8_t	hpen_graphire3_4x5_report_descr[] =
			   { UHID_GRAPHIRE3_4X5_REPORT_DESCR() };

static hmap_cb_t	hpen_battery_strenght_cb;
static hmap_cb_t	hpen_compl_digi_cb;
static hmap_cb_t	hpen_compl_pen_cb;

#define HPEN_MAP_BUT(usage, code)	\
	HMAP_KEY(HUP_DIGITIZERS, HUD_##usage, code)
#define HPEN_MAP_ABS(usage, code)	\
	HMAP_ABS(HUP_DIGITIZERS, HUD_##usage, code)
#define HPEN_MAP_ABS_GD(usage, code)	\
	HMAP_ABS(HUP_GENERIC_DESKTOP, HUG_##usage, code)
#define HPEN_MAP_ABS_CB(usage, cb)	\
	HMAP_ABS_CB(HUP_DIGITIZERS, HUD_##usage, &cb)

/* Generic map digitizer page map according to hut1_12v2.pdf */
static const struct hmap_item hpen_map_digi[] = {
    { HPEN_MAP_ABS_GD(X,		ABS_X),		  .required = true },
    { HPEN_MAP_ABS_GD(Y,		ABS_Y),		  .required = true },
    { HPEN_MAP_ABS(   TIP_PRESSURE,	ABS_PRESSURE) },
    { HPEN_MAP_ABS(   X_TILT,		ABS_TILT_X) },
    { HPEN_MAP_ABS(   Y_TILT,		ABS_TILT_Y) },
    { HPEN_MAP_ABS_CB(BATTERY_STRENGTH,	hpen_battery_strenght_cb) },
    { HPEN_MAP_BUT(   TOUCH,		BTN_TOUCH) },
    { HPEN_MAP_BUT(   TIP_SWITCH,	BTN_TOUCH) },
    { HPEN_MAP_BUT(   SEC_TIP_SWITCH,	BTN_TOUCH) },
    { HPEN_MAP_BUT(   IN_RANGE,		BTN_TOOL_PEN) },
    { HPEN_MAP_BUT(   BARREL_SWITCH,	BTN_STYLUS) },
    { HPEN_MAP_BUT(   INVERT,		BTN_TOOL_RUBBER) },
    { HPEN_MAP_BUT(   ERASER,		BTN_TOUCH) },
    { HPEN_MAP_BUT(   TABLET_PICK,	BTN_STYLUS2) },
    { HPEN_MAP_BUT(   SEC_BARREL_SWITCH,BTN_STYLUS2) },
    { HMAP_COMPL_CB(			&hpen_compl_digi_cb) },
};

/* Microsoft-standardized pen support */
static const struct hmap_item hpen_map_pen[] = {
    { HPEN_MAP_ABS_GD(X,		ABS_X),		  .required = true },
    { HPEN_MAP_ABS_GD(Y,		ABS_Y),		  .required = true },
    { HPEN_MAP_ABS(   TIP_PRESSURE,	ABS_PRESSURE),	  .required = true },
    { HPEN_MAP_ABS(   X_TILT,		ABS_TILT_X) },
    { HPEN_MAP_ABS(   Y_TILT,		ABS_TILT_Y) },
    { HPEN_MAP_ABS_CB(BATTERY_STRENGTH,	hpen_battery_strenght_cb) },
    { HPEN_MAP_BUT(   TIP_SWITCH,	BTN_TOUCH),	  .required = true },
    { HPEN_MAP_BUT(   IN_RANGE,		BTN_TOOL_PEN),	  .required = true },
    { HPEN_MAP_BUT(   BARREL_SWITCH,	BTN_STYLUS) },
    { HPEN_MAP_BUT(   INVERT,		BTN_TOOL_RUBBER), .required = true },
    { HPEN_MAP_BUT(   ERASER,		BTN_TOUCH),	  .required = true },
    { HMAP_COMPL_CB(			&hpen_compl_pen_cb) },
};

static const struct hid_device_id hpen_devs[] = {
	{ HID_TLC(HUP_DIGITIZERS, HUD_DIGITIZER) },
	{ HID_TLC(HUP_DIGITIZERS, HUD_PEN) },
};

static int
hpen_battery_strenght_cb(HMAP_CB_ARGS)
{
	struct evdev_dev *evdev = HMAP_CB_GET_EVDEV();
	int32_t data;

	switch (HMAP_CB_GET_STATE()) {
	case HMAP_CB_IS_ATTACHING:
		evdev_support_event(evdev, EV_PWR);
		/* TODO */
		break;
	case HMAP_CB_IS_RUNNING:
		data = ctx;
		/* TODO */
	}

	return (0);
}

static int
hpen_compl_digi_cb(HMAP_CB_ARGS)
{
	struct evdev_dev *evdev = HMAP_CB_GET_EVDEV();

	if (HMAP_CB_GET_STATE() == HMAP_CB_IS_ATTACHING)
		evdev_support_prop(evdev, INPUT_PROP_POINTER);

	/* Do not execute callback at interrupt handler and detach */
	return (ENOSYS);
}

static int
hpen_compl_pen_cb(HMAP_CB_ARGS)
{
	struct evdev_dev *evdev = HMAP_CB_GET_EVDEV();

	if (HMAP_CB_GET_STATE() == HMAP_CB_IS_ATTACHING)
		evdev_support_prop(evdev, INPUT_PROP_DIRECT);

	/* Do not execute callback at interrupt handler and detach */
	return (ENOSYS);
}

static void
hpen_identify(driver_t *driver, device_t parent)
{
	const struct hid_device_info *hw = hid_get_device_info(parent);

	/* the report descriptor for the Wacom Graphire is broken */
	if (hid_test_quirk(hw, HQ_GRAPHIRE))
		hid_set_report_descr(parent, hpen_graphire_report_descr,
		    sizeof(hpen_graphire_report_descr));
	else if (hid_test_quirk(hw, HQ_GRAPHIRE3_4X5))
		hid_set_report_descr(parent, hpen_graphire3_4x5_report_descr,
		    sizeof(hpen_graphire3_4x5_report_descr));
}

static int
hpen_probe(device_t dev)
{
	int error;
	bool is_pen;

	error = hidbus_lookup_driver_info(dev, hpen_devs, sizeof(hpen_devs));
	if (error != 0)
		return (error);

	hmap_set_debug_var(dev, &HID_DEBUG_VAR);

	/* Check if report descriptor belongs to a HID tablet device */
	is_pen = hidbus_get_usage(dev) == HID_USAGE2(HUP_DIGITIZERS, HUD_PEN);
	error = is_pen
	    ? hmap_add_map(dev, hpen_map_pen, nitems(hpen_map_pen), NULL)
	    : hmap_add_map(dev, hpen_map_digi, nitems(hpen_map_digi), NULL);
	if (error != 0)
		return (error);

	hidbus_set_desc(dev, is_pen ? "Pen" : "Digitizer");

	return (BUS_PROBE_DEFAULT);
}

static int
hpen_attach(device_t dev)
{
	const struct hid_device_info *hw = hid_get_device_info(dev);
	int error;

	if (hid_test_quirk(hw, HQ_GRAPHIRE3_4X5)) {
		/*
		 * The Graphire3 needs 0x0202 to be written to
		 * feature report ID 2 before it'll start
		 * returning digitizer data.
		 */
		static const uint8_t reportbuf[3] = {2, 2, 2};
		error = hid_set_report(dev, reportbuf, sizeof(reportbuf),
		    HID_FEATURE_REPORT, reportbuf[0]);
		if (error)
			DPRINTF("set feature report failed, error=%d "
			    "(ignored)\n", error);
	}

	return (hmap_attach(dev));
}

static devclass_t hpen_devclass;

static device_method_t hpen_methods[] = {
	DEVMETHOD(device_identify,	hpen_identify),
	DEVMETHOD(device_probe,		hpen_probe),
	DEVMETHOD(device_attach,	hpen_attach),
	DEVMETHOD_END
};

DEFINE_CLASS_1(hpen, hpen_driver, hpen_methods,
    sizeof(struct hmap_softc), hmap_driver);
DRIVER_MODULE(hpen, hidbus, hpen_driver, hpen_devclass, NULL, 0);
MODULE_DEPEND(hpen, hid, 1, 1, 1);
MODULE_DEPEND(hpen, hmap, 1, 1, 1);
MODULE_DEPEND(hpen, evdev, 1, 1, 1);
MODULE_VERSION(hpen, 1);
