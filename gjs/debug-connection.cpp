/*
 * Copyright © 2013 Endless Mobile, Inc.
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
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Authored By: Sam Spilsbury <sam@endlessm.com>
 */
#include <gjs/debug-connection.h>
#include "debug-connection-private.h"

struct _GjsDebugConnection {
    GjsDebugConnectionDisposeCallback callback;
    gpointer                          user_data;
};

void
gjs_debug_connection_unregister(GjsDebugConnection *connection)
{
    (*connection->callback)(connection, connection->user_data);
    g_free(connection);
}

GjsDebugConnection *
gjs_debug_connection_new(GjsDebugConnectionDisposeCallback callback,
                         gpointer                          user_data)
{
    GjsDebugConnection *connection = g_new0(GjsDebugConnection, 1);
    connection->callback = callback;
    connection->user_data = user_data;

    return connection;
}
