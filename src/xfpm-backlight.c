/*
 * * Copyright (C) 2009-2011 Ali <aliov@xfce.org>
 *
 * Licensed under the GNU General Public License Version 2
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <math.h>

#include <gtk/gtk.h>
#include <libxfce4util/libxfce4util.h>

#include "xfpm-backlight.h"
#include "egg-idletime.h"
#include "xfpm-notify.h"
#include "xfpm-xfconf.h"
#include "xfpm-power.h"
#include "xfpm-config.h"
#include "xfpm-button.h"
#include "xfpm-brightness.h"
#include "xfpm-debug.h"
#include "xfpm-icons.h"

static void xfpm_backlight_finalize     (GObject *object);

#define ALARM_DISABLED 9

#define XFPM_BACKLIGHT_GET_PRIVATE(o) \
(G_TYPE_INSTANCE_GET_PRIVATE ((o), XFPM_TYPE_BACKLIGHT, XfpmBacklightPrivate))

struct XfpmBacklightPrivate
{
    XfpmBrightness *brightness;
    XfpmPower      *power;
    EggIdletime    *idle;
    XfpmXfconf     *conf;
    XfpmButton     *button;
    XfpmNotify     *notify;
    
    NotifyNotification *n;
    
    gboolean	    has_hw;
    gboolean	    on_battery;
    
    glong            last_level;
    glong 	    max_level;
    
    gboolean        dimmed;
    gboolean	    block;
};

G_DEFINE_TYPE (XfpmBacklight, xfpm_backlight, G_TYPE_OBJECT)

static void
xfpm_backlight_dim_brightness (XfpmBacklight *backlight)
{
    gboolean ret;
    
    if (xfpm_power_get_mode (backlight->priv->power) == XFPM_POWER_MODE_NORMAL )
    {
	glong dim_level;
	
	g_object_get (G_OBJECT (backlight->priv->conf),
		      backlight->priv->on_battery ? BRIGHTNESS_LEVEL_ON_BATTERY : BRIGHTNESS_LEVEL_ON_AC, &dim_level,
		      NULL);
	
	ret = xfpm_brightness_get_level (backlight->priv->brightness, &backlight->priv->last_level);
	
	if ( !ret )
	{
	    g_warning ("Unable to get current brightness level");
	    return;
	}
	
	dim_level = dim_level * backlight->priv->max_level / 100;
	
	/**
	 * Only reduce if the current level is brighter than
	 * the configured dim_level
	 **/
	if (backlight->priv->last_level > dim_level)
	{
	    XFPM_DEBUG ("Current brightness level before dimming : %li, new %li", backlight->priv->last_level, dim_level);
	    backlight->priv->dimmed = xfpm_brightness_set_level (backlight->priv->brightness, dim_level);
	}
    }
}

static gboolean
xfpm_backlight_destroy_popup (gpointer data)
{
    XfpmBacklight *backlight;
    
    backlight = XFPM_BACKLIGHT (data);
    
    if ( backlight->priv->n )
    {
	g_object_unref (backlight->priv->n);
	backlight->priv->n = NULL;
    }
    
    return FALSE;
}

static void
xfpm_backlight_show_notification (XfpmBacklight *backlight, gfloat value)
{
    gchar *summary;

    /* create the notification on demand */
    if ( backlight->priv->n == NULL )
    {
	backlight->priv->n = xfpm_notify_new_notification (backlight->priv->notify,
							   "",
							   "",
							   "xfpm-brightness-lcd",
							   0,
							   XFPM_NOTIFY_NORMAL,
							   NULL);
    }

    /* generate a human-readable summary for the notification */
    summary = g_strdup_printf (_("Brightness: %.0f percent"), value);
    notify_notification_update (backlight->priv->n, summary, NULL, NULL);
    g_free (summary);
    
    /* add the brightness value to the notification */
    notify_notification_set_hint_int32 (backlight->priv->n, "value", value);
    
    /* show the notification */
    notify_notification_show (backlight->priv->n, NULL);
}

static void
xfpm_backlight_show (XfpmBacklight *backlight, gint level)
{
    gfloat value;
    
    XFPM_DEBUG ("Level %u", level);
    
    value = (gfloat) 100 * level / backlight->priv->max_level;
    xfpm_backlight_show_notification (backlight, value);
}


static void
xfpm_backlight_alarm_timeout_cb (EggIdletime *idle, guint id, XfpmBacklight *backlight)
{
    backlight->priv->block = FALSE;
    
    if ( id == TIMEOUT_BRIGHTNESS_ON_AC && !backlight->priv->on_battery)
	xfpm_backlight_dim_brightness (backlight);
    else if ( id == TIMEOUT_BRIGHTNESS_ON_BATTERY && backlight->priv->on_battery)
	xfpm_backlight_dim_brightness (backlight);
}

static void
xfpm_backlight_reset_cb (EggIdletime *idle, XfpmBacklight *backlight)
{
    if ( backlight->priv->dimmed)
    {
	if ( !backlight->priv->block)
	{
	    XFPM_DEBUG ("Alarm reset, setting level to %li", backlight->priv->last_level);
	    xfpm_brightness_set_level (backlight->priv->brightness, backlight->priv->last_level);
	}
	backlight->priv->dimmed = FALSE;
    }
}

static void
xfpm_backlight_button_pressed_cb (XfpmButton *button, XfpmButtonKey type, XfpmBacklight *backlight)
{
    glong level;
    gboolean ret = TRUE;
    
    gboolean enable_brightness, show_popup;
    
    g_object_get (G_OBJECT (backlight->priv->conf),
                  ENABLE_BRIGHTNESS_CONTROL, &enable_brightness,
		  SHOW_BRIGHTNESS_POPUP, &show_popup,
                  NULL);
    
    if ( type == BUTTON_MON_BRIGHTNESS_UP )
    {
	backlight->priv->block = TRUE;
	if ( enable_brightness )
	    ret = xfpm_brightness_up (backlight->priv->brightness, &level);
	if ( ret && show_popup)
	    xfpm_backlight_show (backlight, level);
    }
    else if ( type == BUTTON_MON_BRIGHTNESS_DOWN )
    {
	backlight->priv->block = TRUE;
	if ( enable_brightness )
	    ret = xfpm_brightness_down (backlight->priv->brightness, &level);
	if ( ret && show_popup)
	    xfpm_backlight_show (backlight, level);
    }
}

static void
xfpm_backlight_brightness_on_ac_settings_changed (XfpmBacklight *backlight)
{
    guint timeout_on_ac;
    
    g_object_get (G_OBJECT (backlight->priv->conf),
		  BRIGHTNESS_ON_AC, &timeout_on_ac,
		  NULL);
		  
    XFPM_DEBUG ("Alarm on ac timeout changed %u", timeout_on_ac);
    
    if ( timeout_on_ac == ALARM_DISABLED )
    {
	egg_idletime_alarm_remove (backlight->priv->idle, TIMEOUT_BRIGHTNESS_ON_AC );
    }
    else
    {
	egg_idletime_alarm_set (backlight->priv->idle, TIMEOUT_BRIGHTNESS_ON_AC, timeout_on_ac * 1000);
    }
}

static void
xfpm_backlight_brightness_on_battery_settings_changed (XfpmBacklight *backlight)
{
    guint timeout_on_battery ;
    
    g_object_get (G_OBJECT (backlight->priv->conf),
		  BRIGHTNESS_ON_BATTERY, &timeout_on_battery,
		  NULL);
    
    XFPM_DEBUG ("Alarm on battery timeout changed %u", timeout_on_battery);
    
    if ( timeout_on_battery == ALARM_DISABLED )
    {
	egg_idletime_alarm_remove (backlight->priv->idle, TIMEOUT_BRIGHTNESS_ON_BATTERY );
    }
    else
    {
	egg_idletime_alarm_set (backlight->priv->idle, TIMEOUT_BRIGHTNESS_ON_BATTERY, timeout_on_battery * 1000);
    } 
}


static void
xfpm_backlight_set_timeouts (XfpmBacklight *backlight)
{
    xfpm_backlight_brightness_on_ac_settings_changed (backlight);
    xfpm_backlight_brightness_on_battery_settings_changed (backlight);
}

static void
xfpm_backlight_on_battery_changed_cb (XfpmPower *power, gboolean on_battery, XfpmBacklight *backlight)
{
    backlight->priv->on_battery = on_battery;
}

static void
xfpm_backlight_class_init (XfpmBacklightClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->finalize = xfpm_backlight_finalize;

    g_type_class_add_private (klass, sizeof (XfpmBacklightPrivate));
}

static void
xfpm_backlight_init (XfpmBacklight *backlight)
{
    backlight->priv = XFPM_BACKLIGHT_GET_PRIVATE (backlight);
    
    backlight->priv->brightness = xfpm_brightness_new ();
    backlight->priv->has_hw     = xfpm_brightness_setup (backlight->priv->brightness);
    
    backlight->priv->notify = NULL;
    backlight->priv->idle   = NULL;
    backlight->priv->conf   = NULL;
    backlight->priv->button = NULL;
    backlight->priv->power    = NULL;
    backlight->priv->dimmed = FALSE;
    backlight->priv->block = FALSE;
    
    if ( !backlight->priv->has_hw )
    {
	g_object_unref (backlight->priv->brightness);
	backlight->priv->brightness = NULL;
    }
    else
    {
	backlight->priv->idle   = egg_idletime_new ();
	backlight->priv->conf   = xfpm_xfconf_new ();
	backlight->priv->button = xfpm_button_new ();
	backlight->priv->power    = xfpm_power_get ();
	backlight->priv->notify = xfpm_notify_new ();
	backlight->priv->max_level = xfpm_brightness_get_max_level (backlight->priv->brightness);
	g_signal_connect (backlight->priv->idle, "alarm-expired",
                          G_CALLBACK (xfpm_backlight_alarm_timeout_cb), backlight);
        
        g_signal_connect (backlight->priv->idle, "reset",
                          G_CALLBACK(xfpm_backlight_reset_cb), backlight);
			  
	g_signal_connect (backlight->priv->button, "button-pressed",
		          G_CALLBACK (xfpm_backlight_button_pressed_cb), backlight);
			  
	g_signal_connect_swapped (backlight->priv->conf, "notify::" BRIGHTNESS_ON_AC,
				  G_CALLBACK (xfpm_backlight_brightness_on_ac_settings_changed), backlight);
	
	g_signal_connect_swapped (backlight->priv->conf, "notify::" BRIGHTNESS_ON_BATTERY,
				  G_CALLBACK (xfpm_backlight_brightness_on_battery_settings_changed), backlight);
				
	g_signal_connect (backlight->priv->power, "on-battery-changed",
			  G_CALLBACK (xfpm_backlight_on_battery_changed_cb), backlight);
	g_object_get (G_OBJECT (backlight->priv->power),
		      "on-battery", &backlight->priv->on_battery,
		      NULL);
	xfpm_brightness_get_level (backlight->priv->brightness, &backlight->priv->last_level);
	xfpm_backlight_set_timeouts (backlight);
    }
}

static void
xfpm_backlight_finalize (GObject *object)
{
    XfpmBacklight *backlight;

    backlight = XFPM_BACKLIGHT (object);

    xfpm_backlight_destroy_popup (backlight);

    if ( backlight->priv->brightness )
	g_object_unref (backlight->priv->brightness);

    if ( backlight->priv->idle )
	g_object_unref (backlight->priv->idle);

    if ( backlight->priv->conf )
	g_object_unref (backlight->priv->conf);

    if ( backlight->priv->button )
	g_object_unref (backlight->priv->button);

    if ( backlight->priv->power )
	g_object_unref (backlight->priv->power);

    if ( backlight->priv->notify)
	g_object_unref (backlight->priv->notify);

    G_OBJECT_CLASS (xfpm_backlight_parent_class)->finalize (object);
}

XfpmBacklight *
xfpm_backlight_new (void)
{
    XfpmBacklight *backlight = NULL;
    backlight = g_object_new (XFPM_TYPE_BACKLIGHT, NULL);
    return backlight;
}

gboolean xfpm_backlight_has_hw (XfpmBacklight *backlight)
{
    return backlight->priv->has_hw;
}
