/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-

   Copyright (C) 2004-2006 Bastien Nocera <hadess@hadess.net>

   The Gnome Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   The Gnome Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with the Gnome Library; see the file COPYING.LIB.  If not,
   write to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
   Boston, MA 02110-1301  USA.

   Author: Bastien Nocera <hadess@hadess.net>
 */


#include "config.h"

#include <glib/gi18n.h>

#include <gdk/gdk.h>

#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#include <X11/keysym.h>

#ifdef HAVE_XTEST
#include <X11/extensions/XTest.h>
#endif /* HAVE_XTEST */
#endif /* GDK_WINDOWING_X11 */

#include "totem-scrsaver.h"

#define GS_SERVICE   "org.gnome.ScreenSaver"
#define GS_PATH      "/org/gnome/ScreenSaver"
#define GS_INTERFACE "org.gnome.ScreenSaver"

#define XSCREENSAVER_MIN_TIMEOUT 60

static GObjectClass *parent_class = NULL;
static void totem_scrsaver_finalize   (GObject *object);

struct TotemScrsaverPrivate {
	/* Whether the screensaver is disabled */
	gboolean disabled;

        GDBusConnection *connection;
        gboolean have_screensaver_dbus;
        guint watch_id;
	guint32 cookie;

	/* To save the screensaver info */
	int timeout;
	int interval;
	int prefer_blanking;
	int allow_exposures;

	/* For use with XTest */
	int keycode1, keycode2;
	int *keycode;
	gboolean have_xtest;
};

enum {
        PROP_0,
        PROP_CONNECTION
};

G_DEFINE_TYPE(TotemScrsaver, totem_scrsaver, G_TYPE_OBJECT)

static gboolean
screensaver_is_running_dbus (TotemScrsaver *scr)
{
        return scr->priv->have_screensaver_dbus;
}

static void
screensaver_inhibit_dbus (TotemScrsaver *scr,
			  gboolean	 inhibit)
{
        TotemScrsaverPrivate *priv = scr->priv;
	GError *error = NULL;
        GVariant *value;

        if (!priv->have_screensaver_dbus)
                return;

	if (inhibit) {
                value = g_dbus_connection_call_sync (priv->connection,
                                                     GS_SERVICE,
                                                     GS_PATH,
                                                     GS_INTERFACE,
                                                     "Inhibit",
                                                     g_variant_new ("(ss)",
                                                                    "Evince",
                                                                    _("Running in presentation mode")),
                                                     G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                                     -1,
                                                     NULL,
                                                     &error);
		if (error && g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD)) {
			/* try the old API */
                        g_clear_error (&error);
                        value = g_dbus_connection_call_sync (priv->connection,
                                                             GS_SERVICE,
                                                             GS_PATH,
                                                             GS_INTERFACE,
                                                             "InhibitActivation",
                                                             g_variant_new ("(s)",
                                                                            _("Running in presentation mode")),
                                                             G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                                             -1,
                                                             NULL,
                                                             &error);
                }
                if (value != NULL) {
			/* save the cookie */
                        if (g_variant_is_of_type (value, G_VARIANT_TYPE ("(u)")))
			       g_variant_get (value, "(u)", &priv->cookie);
                        else
                                priv->cookie = 0;
                        g_variant_unref (value);
		} else {
			g_warning ("Problem inhibiting the screensaver: %s", error->message);
                        g_error_free (error);
		}

	} else {
                value = g_dbus_connection_call_sync (priv->connection,
                                                     GS_SERVICE,
                                                     GS_PATH,
                                                     GS_INTERFACE,
                                                     "UnInhibit",
                                                     g_variant_new ("(u)", priv->cookie),
                                                     G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                                     -1,
                                                     NULL,
                                                     &error);
		if (error && g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD)) {
			/* try the old API */
                        g_clear_error (&error);
                        value = g_dbus_connection_call_sync (priv->connection,
                                                             GS_SERVICE,
                                                             GS_PATH,
                                                             GS_INTERFACE,
                                                             "AllowActivation",
                                                             g_variant_new ("()"),
                                                             G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                                             -1,
                                                             NULL,
                                                             &error);
                }
                if (value != NULL) {
			/* clear the cookie */
			priv->cookie = 0;
                        g_variant_unref (value);
		} else {
			g_warning ("Problem uninhibiting the screensaver: %s", error->message);
			g_error_free (error);
		}
	}
}

static void
screensaver_enable_dbus (TotemScrsaver *scr)
{
	screensaver_inhibit_dbus (scr, FALSE);
}

static void
screensaver_disable_dbus (TotemScrsaver *scr)
{
	screensaver_inhibit_dbus (scr, TRUE);
}

static void
screensaver_dbus_appeared_cb (GDBusConnection *connection,
                              const char      *name,
                              const char      *name_owner,
                              gpointer         user_data)
{
        TotemScrsaver *scr = TOTEM_SCRSAVER (user_data);
        TotemScrsaverPrivate *priv = scr->priv;

        g_assert (connection == priv->connection);

        priv->have_screensaver_dbus = TRUE;
}

static void
screensaver_dbus_disappeared_cb (GDBusConnection *connection,
                                 const char      *name,
                                 gpointer         user_data)
{
        TotemScrsaver *scr = TOTEM_SCRSAVER (user_data);
        TotemScrsaverPrivate *priv = scr->priv;

        g_assert (connection == priv->connection);

        priv->have_screensaver_dbus = FALSE;
}

static void
screensaver_finalize_dbus (TotemScrsaver *scr)
{
        TotemScrsaverPrivate *priv = scr->priv;

        if (priv->connection == NULL)
                return;

        g_bus_unwatch_name (priv->watch_id);

        g_object_unref (priv->connection);
}

#ifdef GDK_WINDOWING_X11
static void
screensaver_enable_x11 (TotemScrsaver *scr)
{

#ifdef HAVE_XTEST
	if (scr->priv->have_xtest != FALSE)
	{
		g_source_remove_by_user_data (scr);
		return;
	}
#endif /* HAVE_XTEST */

	XLockDisplay (GDK_DISPLAY());
	XSetScreenSaver (GDK_DISPLAY(),
			scr->priv->timeout,
			scr->priv->interval,
			scr->priv->prefer_blanking,
			scr->priv->allow_exposures);
	XUnlockDisplay (GDK_DISPLAY());
}

#ifdef HAVE_XTEST
static gboolean
fake_event (TotemScrsaver *scr)
{
	if (scr->priv->disabled)
	{
		XLockDisplay (GDK_DISPLAY());
		XTestFakeKeyEvent (GDK_DISPLAY(), *scr->priv->keycode,
				True, CurrentTime);
		XTestFakeKeyEvent (GDK_DISPLAY(), *scr->priv->keycode,
				False, CurrentTime);
		XUnlockDisplay (GDK_DISPLAY());
		/* Swap the keycode */
		if (scr->priv->keycode == &scr->priv->keycode1)
			scr->priv->keycode = &scr->priv->keycode2;
		else
			scr->priv->keycode = &scr->priv->keycode1;
	}

	return TRUE;
}
#endif /* HAVE_XTEST */

static void
screensaver_disable_x11 (TotemScrsaver *scr)
{

#ifdef HAVE_XTEST
	if (scr->priv->have_xtest != FALSE)
	{
		XLockDisplay (GDK_DISPLAY());
		XGetScreenSaver(GDK_DISPLAY(), &scr->priv->timeout,
				&scr->priv->interval,
				&scr->priv->prefer_blanking,
				&scr->priv->allow_exposures);
		XUnlockDisplay (GDK_DISPLAY());

		if (scr->priv->timeout != 0) {
			g_timeout_add_seconds (scr->priv->timeout / 2,
					       (GSourceFunc) fake_event, scr);
		} else {
			g_timeout_add_seconds (XSCREENSAVER_MIN_TIMEOUT / 2,
					       (GSourceFunc) fake_event, scr);
		}

		return;
	}
#endif /* HAVE_XTEST */

	XLockDisplay (GDK_DISPLAY());
	XGetScreenSaver(GDK_DISPLAY(), &scr->priv->timeout,
			&scr->priv->interval,
			&scr->priv->prefer_blanking,
			&scr->priv->allow_exposures);
	XSetScreenSaver(GDK_DISPLAY(), 0, 0,
			DontPreferBlanking, DontAllowExposures);
	XUnlockDisplay (GDK_DISPLAY());
}

static void
screensaver_init_x11 (TotemScrsaver *scr)
{
#ifdef HAVE_XTEST
	int a, b, c, d;

	XLockDisplay (GDK_DISPLAY());
	scr->priv->have_xtest = (XTestQueryExtension (GDK_DISPLAY(), &a, &b, &c, &d) == True);
	if (scr->priv->have_xtest != FALSE)
	{
		scr->priv->keycode1 = XKeysymToKeycode (GDK_DISPLAY(), XK_Alt_L);
		if (scr->priv->keycode1 == 0) {
			g_warning ("scr->priv->keycode1 not existant");
		}
		scr->priv->keycode2 = XKeysymToKeycode (GDK_DISPLAY(), XK_Alt_R);
		if (scr->priv->keycode2 == 0) {
			scr->priv->keycode2 = XKeysymToKeycode (GDK_DISPLAY(), XK_Alt_L);
			if (scr->priv->keycode2 == 0) {
				g_warning ("scr->priv->keycode2 not existant");
			}
		}
		scr->priv->keycode = &scr->priv->keycode1;
	}
	XUnlockDisplay (GDK_DISPLAY());
#endif /* HAVE_XTEST */
}

static void
screensaver_finalize_x11 (TotemScrsaver *scr)
{
	g_source_remove_by_user_data (scr);
}
#endif

static void
totem_scrsaver_constructed (GObject *object)
{
        TotemScrsaver *scr = TOTEM_SCRSAVER (object);
        TotemScrsaverPrivate *priv = scr->priv;

        if (priv->connection == NULL)
                return;

        priv->watch_id = g_bus_watch_name_on_connection (priv->connection,
                                                         GS_SERVICE,
                                                         G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                         screensaver_dbus_appeared_cb,
                                                         screensaver_dbus_disappeared_cb,
                                                         scr, NULL);
}

static void
totem_scrsaver_set_property (GObject      *object,
                             guint         prop_id,
                             const GValue *value,
                             GParamSpec   *pspec)
{
        TotemScrsaver *scr = TOTEM_SCRSAVER (object);
        TotemScrsaverPrivate *priv = scr->priv;

	switch (prop_id) {
        case PROP_CONNECTION:
                priv->connection = g_value_dup_object (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
totem_scrsaver_class_init (TotemScrsaverClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

        object_class->set_property = totem_scrsaver_set_property;
        object_class->constructed = totem_scrsaver_constructed;
	object_class->finalize = totem_scrsaver_finalize;

	g_object_class_install_property (object_class,
					 PROP_CONNECTION,
					 g_param_spec_object ("connection", NULL, NULL,
                                                              G_TYPE_DBUS_CONNECTION,
							      G_PARAM_WRITABLE |
                                                              G_PARAM_CONSTRUCT_ONLY |
                                                              G_PARAM_STATIC_STRINGS));
}

/**
 * totem_scrsaver_new:
 * @connection: (allow-none): a #GDBusConnection, or %NULL
 *
 * Creates a #TotemScrsaver object. If @connection is non-%NULL,
 * and the GNOME screen saver is running, it uses its DBUS interface to
 * inhibit the screensaver; otherwise it falls back to using the X
 * screensaver functionality for this.
 *
 * Returns: a newly created #TotemScrsaver
 */
TotemScrsaver *
totem_scrsaver_new (GDBusConnection *connection)
{
	return g_object_new (TOTEM_TYPE_SCRSAVER,
                             "connection", connection,
                             NULL);
}

static void
totem_scrsaver_init (TotemScrsaver *scr)
{
	scr->priv = G_TYPE_INSTANCE_GET_PRIVATE (scr, TOTEM_TYPE_SCRSAVER, TotemScrsaverPrivate);

#ifdef GDK_WINDOWING_X11
	screensaver_init_x11 (scr);
#else
#warning Unimplemented
#endif
}

void
totem_scrsaver_disable (TotemScrsaver *scr)
{
	g_return_if_fail (TOTEM_SCRSAVER (scr));

	if (scr->priv->disabled != FALSE)
		return;

	scr->priv->disabled = TRUE;

	if (screensaver_is_running_dbus (scr) != FALSE)
		screensaver_disable_dbus (scr);
	else 
#ifdef GDK_WINDOWING_X11
		screensaver_disable_x11 (scr);
#else
#warning Unimplemented
	{}
#endif
}

void
totem_scrsaver_enable (TotemScrsaver *scr)
{
	g_return_if_fail (TOTEM_SCRSAVER (scr));

	if (scr->priv->disabled == FALSE)
		return;

	scr->priv->disabled = FALSE;

	if (screensaver_is_running_dbus (scr) != FALSE)
		screensaver_enable_dbus (scr);
	else 
#ifdef GDK_WINDOWING_X11
		screensaver_enable_x11 (scr);
#else
#warning Unimplemented
	{}
#endif
}

void
totem_scrsaver_set_state (TotemScrsaver *scr, gboolean enable)
{
	g_return_if_fail (TOTEM_SCRSAVER (scr));

	if (scr->priv->disabled == !enable)
		return;

	if (enable == FALSE)
		totem_scrsaver_disable (scr);
	else
		totem_scrsaver_enable (scr);
}

static void
totem_scrsaver_finalize (GObject *object)
{
	TotemScrsaver *scr = TOTEM_SCRSAVER (object);

	screensaver_finalize_dbus (scr);
#ifdef GDK_WINDOWING_X11
	screensaver_finalize_x11 (scr);
#else
#warning Unimplemented
	{}
#endif

        G_OBJECT_CLASS (parent_class)->finalize (object);
}
