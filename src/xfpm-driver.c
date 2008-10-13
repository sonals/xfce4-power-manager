/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*-
 *
 * * Copyright (C) 2008 Ali <ali.slackware@gmail.com>
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
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>

#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <gtk/gtk.h>
#include <glib.h>
#include <glib/gi18n.h>

#include <dbus/dbus.h>
#include <dbus/dbus-glib-lowlevel.h>

#include <hal/libhal.h>

#include <libxfcegui4/libxfcegui4.h>
#include <xfconf/xfconf.h>

#include "xfpm-driver.h"
#include "xfpm-debug.h"
#include "xfpm-battery.h"
#include "xfpm-cpu.h"
#include "xfpm-ac-adapter.h"
#include "xfpm-button.h"
#include "xfpm-lcd-brightness.h"
#include "xfpm-notify.h"
#include "xfpm-enum-types.h"
#include "xfpm-common.h"
#include "xfpm-settings.h"

#ifdef HAVE_LIBNOTIFY
#include "xfpm-notify.h"
#endif

#ifdef HAVE_DPMS
#include "xfpm-dpms.h"
#include "xfpm-dpms-spins.h"
#endif

/*init*/
static void xfpm_driver_init        (XfpmDriver *drv);
static void xfpm_driver_class_init  (XfpmDriverClass *klass);
static void xfpm_driver_finalize    (GObject *object);

static void xfpm_driver_ac_adapter_state_changed_cb(XfpmAcAdapter *adapter,
                                                    gboolean present,
                                                    XfpmDriver *drv);
                                                    
static void xfpm_driver_property_changed_cb(XfconfChannel *channel,
                                            gchar *property,
                                            GValue *value,
                                            XfpmDriver *drv);
                                            
static gboolean xfpm_driver_show_options_dialog(XfpmDriver *drv);

/* DBus message Filter and replies */
static void xfpm_driver_send_reply(DBusConnection *conn,DBusMessage *mess);
static DBusHandlerResult xfpm_driver_signal_filter
    (DBusConnection *connection,DBusMessage *message,void *user_data);

#define XFPM_DRIVER_GET_PRIVATE(o) \
(G_TYPE_INSTANCE_GET_PRIVATE((o),XFPM_TYPE_DRIVER,XfpmDriverPrivate))

struct XfpmDriverPrivate 
{
    DBusConnection *conn;
    GMainLoop *loop;
    
    gboolean dialog_opened;
    
    SystemFormFactor formfactor;
    gboolean can_suspend;
    gboolean can_hibernate;
    
    XfpmHal     *hal;
    XfpmCpu     *cpu;
    XfpmBattery *batt;
    XfpmButton  *bt;
    XfpmLcdBrightness *lcd;
    
#ifdef HAVE_DPMS    
    XfpmDpms *dpms;
#endif    
    GtkStatusIcon *adapter;
    gboolean ac_adapter_present;
};

G_DEFINE_TYPE(XfpmDriver,xfpm_driver,G_TYPE_OBJECT)

static void
xfpm_driver_class_init(XfpmDriverClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    
    object_class->finalize = xfpm_driver_finalize;
    
    g_type_class_add_private(klass,sizeof(XfpmDriverPrivate));
}

static void
xfpm_driver_init(XfpmDriver *drv)
{
    XfpmDriverPrivate *priv;
    priv = XFPM_DRIVER_GET_PRIVATE(drv);
    priv->dialog_opened = FALSE;
    priv->loop    = NULL;
    priv->cpu     = NULL;
    priv->adapter = NULL;
    priv->batt    = NULL;
    priv->bt      = NULL;
    priv->lcd     = NULL;
    priv->hal     = NULL;
#ifdef HAVE_DPMS
    priv->dpms    = NULL;
#endif     

#ifdef HAVE_LIBNOTIFY
    notify_init("Basics");
#endif    
}

static void
xfpm_driver_finalize(GObject *object)
{
    XfpmDriver *drv;
    drv = XFPM_DRIVER(object);
    drv->priv = XFPM_DRIVER_GET_PRIVATE(drv);

#ifdef HAVE_DPMS
    if ( drv->priv->dpms )
    {
        g_object_unref(drv->priv->dpms);
    }
#endif
    if ( drv->priv->adapter )
    {
        g_object_unref(drv->priv->adapter);
    }
    if ( drv->priv->batt )
    {
        g_object_unref(drv->priv->batt);
    }
    if ( drv->priv->cpu )
    {
        g_object_unref(drv->priv->cpu);
    }
    if ( drv->priv->bt )
    {
        g_object_unref(drv->priv->bt);
    }
    if ( drv->priv->lcd )
    {
        g_object_unref(drv->priv->lcd);
    }
    if ( drv->priv->conn )
    {
        dbus_connection_unref(drv->priv->conn);
    }
    if ( drv->priv->loop )
    {
        g_main_loop_unref(drv->priv->loop);
    }
    G_OBJECT_CLASS(xfpm_driver_parent_class)->finalize(object);
}

static void xfpm_driver_ac_adapter_state_changed_cb(XfpmAcAdapter *adapter,
                                                    gboolean present,
                                                    XfpmDriver *drv)
{
    XfpmDriverPrivate *priv;
    priv = XFPM_DRIVER_GET_PRIVATE(drv);
    
    XFPM_DEBUG("start \n");
    priv->ac_adapter_present = present;
    
#ifdef HAVE_DPMS    
    XFPM_DEBUG("Setting DPMS ac-adapter property\n");
    g_object_set(G_OBJECT(priv->dpms),"on-ac-adapter",priv->ac_adapter_present,NULL);
#endif    
    g_object_set(G_OBJECT(priv->cpu),"on-ac-adapter",priv->ac_adapter_present,NULL);

    g_object_set(G_OBJECT(priv->batt),"on-ac-adapter",priv->ac_adapter_present,NULL);
    g_object_set(G_OBJECT(priv->lcd),"on-ac-adapter",priv->ac_adapter_present,NULL);
}                                                    

static void
_dialog_response_cb(GtkDialog *dialog, gint response, XfpmDriver *drv)
{
    XfpmDriverPrivate *priv;
    priv = XFPM_DRIVER_GET_PRIVATE(drv);
    
    priv->dialog_opened = FALSE;
    
    gpointer data = 
    g_object_get_data(G_OBJECT(drv),"conf-channel");
    
    if ( data )
    {
        XfconfChannel *channel = (XfconfChannel *)data;
        g_object_unref(channel);
        xfconf_shutdown();
    }
    
    switch(response) 
    {
            case GTK_RESPONSE_HELP:
                /*FIXME : Show help */
                gtk_widget_destroy(GTK_WIDGET(dialog));
                break;
            default:
                gtk_widget_destroy(GTK_WIDGET(dialog));
                break;
    }
}              

static void
xfpm_driver_property_changed_cb(XfconfChannel *channel,gchar *property,
                                GValue *value,XfpmDriver *drv)
{
    XFPM_DEBUG("%s \n",property);
    if ( G_VALUE_TYPE(value) == G_TYPE_INVALID )
    {
        XFPM_DEBUG(" invalid value type\n");
        return;
    }
    
    XfpmDriverPrivate *priv;
    priv = XFPM_DRIVER_GET_PRIVATE(drv);
    
    
    if ( !strcmp(property,CRITICAL_BATT_CFG) ) 
    {
        guint val = g_value_get_uint(value);
        g_object_set(G_OBJECT(priv->batt),"critical-charge",val,NULL);
        return;
    }
    
    if ( !strcmp(property,CRITICAL_BATT_ACTION_CFG) ) 
    {
        guint val = g_value_get_uint(value);
        g_object_set(G_OBJECT(priv->batt),"critical-action",val,NULL);
        return;
    }
    
    if ( !strcmp(property,SHOW_TRAY_ICON_CFG) ) 
    {
        gboolean val = g_value_get_uint(value);
        g_object_set(G_OBJECT(priv->batt),"show-tray-icon",val,NULL);
        return;
    }
    
    if ( !strcmp(property,CPU_FREQ_SCALING_CFG) ) 
    {
        gboolean val = g_value_get_boolean(value);
        g_object_set(G_OBJECT(priv->cpu),"cpu-freq",val,NULL);
        return;
    }
    
    if ( !strcmp(property,ON_AC_CPU_GOV_CFG) ) 
    {
        guint val = g_value_get_uint(value);
        g_object_set(G_OBJECT(priv->cpu),"on-ac-cpu-gov",val,NULL);
        return;
    }
    if ( !strcmp(property,ON_BATT_CPU_GOV_CFG) ) 
    {
        guint val = g_value_get_uint(value);
        g_object_set(G_OBJECT(priv->cpu),"on-batt-cpu-gov",val,NULL);
        return;
    }
    
#ifdef HAVE_LIBNOTIFY    
    if ( !strcmp(property,BATT_STATE_NOTIFICATION_CFG) ) 
    {
        gboolean val = g_value_get_boolean(value);
        g_object_set(G_OBJECT(priv->batt),"enable-notification",val,NULL);
        return;
    }
#endif    

    if ( !strcmp(property,LCD_BRIGHTNESS_CFG) )
    {
        gboolean val = g_value_get_boolean(value);
        g_object_set(G_OBJECT(priv->lcd),"brightness-enabled",val,NULL);
        return;
    }
#ifdef HAVE_DPMS
    if ( !strcmp(property,DPMS_ENABLE_CFG) ) 
    {
        gboolean val = g_value_get_boolean(value);
        priv->dpms->dpms_enabled = val;
        g_object_notify(G_OBJECT(priv->dpms),"dpms");
        return;
    }

    if ( !strcmp(property,ON_BATT_DPMS_TIMEOUTS_CFG) ) 
    {
        GPtrArray *arr;
        gpointer data;
        GValue *val;    
        guint value1 = 180,value2 = 240,value3 = 300;
        data = g_value_get_boxed(value);
        arr = (GPtrArray *) data;
        
        val = g_ptr_array_index(arr,0);
        value1 = g_value_get_uint(val);
        
        val = g_ptr_array_index(arr,1);
        value2 = g_value_get_uint(val);
        
        val = g_ptr_array_index(arr,2);
        value3 = g_value_get_uint(val);
        priv->dpms->on_batt_standby_timeout = value1 * 60;
        priv->dpms->on_batt_suspend_timeout = value2 * 60;
        priv->dpms->on_batt_off_timeout     = value3 * 60; 
        g_object_notify(G_OBJECT(priv->dpms),"dpms");
        return;
    }
    
    if ( !strcmp(property,ON_AC_DPMS_TIMEOUTS_CFG) ) 
    {
        GPtrArray *arr;
        gpointer data;
        GValue *val;
        guint value1 = 1800,value2 = 2700,value3 = 3600;
        data = g_value_get_boxed(value);
        arr = (GPtrArray *) data;
        
        val = g_ptr_array_index(arr,0);
        value1 = g_value_get_uint(val);
        
        val = g_ptr_array_index(arr,1);
        value2 = g_value_get_uint(val);
        
        val = g_ptr_array_index(arr,2);
        value3 = g_value_get_uint(val);
        
        priv->dpms->on_ac_standby_timeout = value1 * 60;
        priv->dpms->on_ac_suspend_timeout = value2 * 60;
        priv->dpms->on_ac_off_timeout     = value3 * 60;
        
        g_object_notify(G_OBJECT(priv->dpms),"dpms");
        return;
    }
#endif

    if ( !strcmp(property,LID_SWITCH_CFG) ) 
    {
        guint val = g_value_get_uint(value);
        g_object_set(G_OBJECT(priv->bt),"lid-switch-action",val,NULL);
        return;
    }
    
    if ( !strcmp(property,SLEEP_SWITCH_CFG) ) 
    {
        guint val = g_value_get_uint(value);
        g_object_set(G_OBJECT(priv->bt),"sleep-switch-action",val,NULL);
        return;
    }
    
    if ( !strcmp(property,POWER_SWITCH_CFG) ) 
    {
        guint val = g_value_get_uint(value);
        g_object_set(G_OBJECT(priv->bt),"power-switch-action",val,NULL);
        return;
    }
    
} 

static gboolean
xfpm_driver_show_options_dialog(XfpmDriver *drv)
{
    XfpmDriverPrivate *priv;
    priv = XFPM_DRIVER_GET_PRIVATE(drv);
    
    if ( priv->dialog_opened )
    {
        return FALSE;
    }
    
    XfconfChannel *channel;
    GtkWidget *dialog;

    GError *g_error = NULL;
    if ( !xfconf_init(&g_error) )
    {
        g_critical("Unable to xfconf init failed: %s\n",g_error->message);
        g_error_free(g_error);
    }
    
    channel = xfconf_channel_new(XFPM_CHANNEL_CFG);
    g_object_set_data(G_OBJECT(drv),"conf-channel",channel);
    g_signal_connect(channel,"property-changed",
                     G_CALLBACK(xfpm_driver_property_changed_cb),drv);
    
    gchar **govs;
    govs = xfpm_hal_get_available_cpu_governors(priv->hal);
    int i = 0;
    gint8 gov[4] = { 0, };
    
    if ( govs ) 
    {
        for ( i = 0 ; govs[i] ; i++ )
        {
            if ( !strcmp(govs[i],"powersave") )    gov[0] = 1;
            if ( !strcmp(govs[i],"ondemand") )     gov[1] = 1;
            if ( !strcmp(govs[i],"performance") )  gov[2] = 1;
            if ( !strcmp(govs[i],"conservative") ) gov[3] = 1;
            if ( !strcmp(govs[i],"userspace") )    gov[4] = 1;
        }   
        libhal_free_string_array(govs);
    }
    gboolean with_dpms;
#ifdef HAVE_DPMS
    with_dpms = xfpm_dpms_capable(priv->dpms);
#else
    with_dpms = FALSE;
#endif

    dialog = xfpm_settings_new(channel,
                               priv->formfactor == SYSTEM_LAPTOP ? TRUE : FALSE,
                               priv->can_hibernate,
                               priv->can_suspend,
                               with_dpms,
                               gov);
    
    xfce_gtk_window_center_on_monitor_with_pointer(GTK_WINDOW(dialog));
    gtk_window_set_modal(GTK_WINDOW(dialog), FALSE);
    
    gdk_x11_window_set_user_time(dialog->window,gdk_x11_get_server_time (dialog->window));
    
    g_signal_connect(dialog,"response",G_CALLBACK(_dialog_response_cb),drv);        
    gtk_widget_show(dialog);
    priv->dialog_opened = TRUE;
    
    return FALSE;
}

static void
xfpm_driver_send_reply(DBusConnection *conn,DBusMessage *mess)
{
    DBusMessage *reply;
    reply = dbus_message_new_method_return(mess);
    dbus_connection_send (conn, reply, NULL);
    dbus_connection_flush (conn);
    dbus_message_unref(reply);
    
}

static DBusHandlerResult xfpm_driver_signal_filter
    (DBusConnection *connection,DBusMessage *message,void *user_data) 
{
    XfpmDriver *drv = XFPM_DRIVER(user_data);
    XfpmDriverPrivate *priv;
    priv = XFPM_DRIVER_GET_PRIVATE(drv);
    
    if ( dbus_message_is_signal(message,"xfpm.power.manager","Customize" ) )
    {
        XFPM_DEBUG("message customize received\n");
        /* don't block the signal filter so show the configuration dialog in a
         * timeout function otherwise xfce4-power-manager -q will have no effect*/
        g_timeout_add(100,(GSourceFunc)xfpm_driver_show_options_dialog,drv);
        return DBUS_HANDLER_RESULT_HANDLED;
    }    
    
    if ( dbus_message_is_signal(message,"xfpm.power.manager","Quit" ) )
    {
        XFPM_DEBUG("message quit received\n");
        xfpm_driver_send_reply(connection,message);      
        g_main_loop_quit(priv->loop);
        g_object_unref(drv);
        return DBUS_HANDLER_RESULT_HANDLED;
    }    
    
    if ( dbus_message_is_signal(message,"xfpm.power.manager","Running" ) )
    {
        XFPM_DEBUG("message running received\n");
        xfpm_driver_send_reply(connection,message);
        return DBUS_HANDLER_RESULT_HANDLED;
    }    
    
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

XfpmDriver *
xfpm_driver_new(void)
{
    XfpmDriver *driver = NULL;
    driver =  g_object_new(XFPM_TYPE_DRIVER,NULL);
    return driver;
}

static void
_show_adapter_icon(XfpmBattery *batt,gboolean show,XfpmDriver *drv)
{
    XFPM_DEBUG("start\n");
    XfpmDriverPrivate *priv;
    priv = XFPM_DRIVER_GET_PRIVATE(drv);
    
    g_object_set(G_OBJECT(priv->adapter),"visible",show,NULL);
    
}

static void
_get_system_form_factor(XfpmDriverPrivate *priv)
{
    priv->formfactor = SYSTEM_UNKNOWN;
    
    gchar *factor = xfpm_hal_get_string_info(priv->hal,
                                            HAL_ROOT_COMPUTER,
                                            "system.formfactor",
                                            NULL);
    if (!factor ) return;
    
    if ( !strcmp(factor,"laptop") )
    {
        priv->formfactor = SYSTEM_LAPTOP;
    }
    else if ( !strcmp(factor,"desktop") )
    {
        priv->formfactor = SYSTEM_DESKTOP;
    }
    else if ( !strcmp(factor,"server") )
    {
        priv->formfactor = SYSTEM_SERVER;
    }
    else if ( !strcmp(factor,"unknown") )
    {
        priv->formfactor = SYSTEM_UNKNOWN;
    }
    
}

gboolean
xfpm_driver_monitor (XfpmDriver *drv)
{
    g_return_val_if_fail (XFPM_IS_DRIVER(drv),FALSE);
    XFPM_DEBUG("starting xfpm manager\n");
    XfpmDriverPrivate *priv;
    priv = XFPM_DRIVER_GET_PRIVATE(drv);
    
    priv->loop = g_main_loop_new(NULL,FALSE);
    
    DBusError error;

    dbus_error_init(&error);
    priv->conn = dbus_bus_get(DBUS_BUS_SESSION,&error);
    if (!priv->conn) 
    {
       g_critical("Failed to connect to the DBus daemon: %s\n",error.message);
       dbus_error_free(&error);
       return FALSE;
    }
        
    dbus_connection_setup_with_g_main(priv->conn,NULL);
    dbus_bus_add_match(priv->conn, "type='signal',interface='xfpm.power.manager'",NULL);
    dbus_connection_add_filter(priv->conn,xfpm_driver_signal_filter,drv,NULL);
    
    priv->hal = xfpm_hal_new();
    _get_system_form_factor(priv);
    
    GError *g_error = NULL;
    priv->can_suspend = xfpm_hal_get_bool_info(priv->hal,
                                               HAL_ROOT_COMPUTER,
                                               "power_management.can_suspend",
                                               &g_error);
                                               
    if ( g_error )
    {
        XFPM_DEBUG("%s: \n",g_error->message);
        g_error_free(g_error);
    }            
    
    priv->can_hibernate = xfpm_hal_get_bool_info(priv->hal,
                                               HAL_ROOT_COMPUTER,
                                               "power_management.can_hibernate",
                                               &g_error);
                                               
    if ( g_error )
    {
        XFPM_DEBUG("%s: \n",g_error->message);
        g_error_free(g_error);
    }            
    
#ifdef HAVE_DPMS    
    priv->dpms = xfpm_dpms_new();
#endif  
    priv->cpu = xfpm_cpu_new();
    
    priv->bt  = xfpm_button_new();
    priv->lcd = xfpm_lcd_brightness_new();
    
    priv->batt = xfpm_battery_new();
    g_signal_connect(priv->batt,"xfpm-show-adapter-icon",G_CALLBACK(_show_adapter_icon),drv);
    
    priv->adapter = xfpm_ac_adapter_new(FALSE);
    g_signal_connect(priv->adapter,"xfpm-ac-adapter-changed",
                    G_CALLBACK(xfpm_driver_ac_adapter_state_changed_cb),drv);

    /* This will give a signal concerning the AC adapter presence,
     * so we get the callback and then we set up the ac adapter
     * status for dpms and cpu lcd,...
     */                    
    xfpm_ac_adapter_monitor(XFPM_AC_ADAPTER(priv->adapter),priv->formfactor);
    
    xfpm_battery_monitor(priv->batt);
    g_main_loop_run(priv->loop);
    
    dbus_connection_remove_filter(priv->conn,xfpm_driver_signal_filter,NULL);
    xfconf_shutdown();
    return TRUE;
}
