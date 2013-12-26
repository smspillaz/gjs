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
 * Authored By: Sam Spilsbury <sam.spilsbury@canonical.com>
 */
#include <gjs/debug-hooks.h>
#include "debug-hooks-private.h"

static void gjs_debug_hooks_interface_default_init(GjsDebugHooksInterface *settings_interface);

G_DEFINE_INTERFACE(GjsDebugHooks, gjs_debug_hooks_interface, G_TYPE_OBJECT);

static void
gjs_debug_hooks_interface_default_init(GjsDebugHooksInterface *settings_interface)
{
}

GjsDebugConnection *
gjs_debug_hooks_add_breakpoint(GjsDebugHooks        *hooks,
                               const char           *filename,
                               unsigned int          line,
                               GjsInterruptCallback  callback,
                               gpointer              user_data)
{
    g_return_val_if_fail (hooks, NULL);
    g_return_val_if_fail (filename, NULL);
    g_return_val_if_fail (callback, NULL);

    return GJS_DEBUG_HOOKS_GET_INTERFACE(hooks)->add_breakpoint(hooks,
                                                              filename,
                                                              line,
                                                              callback,
                                                              user_data);
}

GjsDebugConnection *
gjs_debug_hooks_start_singlestep(GjsDebugHooks        *hooks,
                                 GjsInterruptCallback  callback,
                                 gpointer              user_data)
{
    g_return_val_if_fail (hooks, NULL);
    g_return_val_if_fail (callback, NULL);

    return GJS_DEBUG_HOOKS_GET_INTERFACE(hooks)->start_singlestep(hooks,
                                                                callback,
                                                                user_data);
}

GjsDebugConnection *
gjs_debug_hooks_connect_to_script_load(GjsDebugHooks   *hooks,
                                       GjsInfoCallback  callback,
                                       gpointer         user_data)
{
    g_return_val_if_fail (hooks, NULL);
    g_return_val_if_fail (callback, NULL);

    return GJS_DEBUG_HOOKS_GET_INTERFACE(hooks)->connect_to_script_load(hooks,
                                                                      callback,
                                                                      user_data);
}

GjsDebugConnection *
gjs_debug_hooks_connect_to_function_calls_and_execution(GjsDebugHooks    *hooks,
                                                        GjsFrameCallback  callback,
                                                        gpointer          user_data)
{
    g_return_val_if_fail (hooks, NULL);
    g_return_val_if_fail (callback, NULL);

    return GJS_DEBUG_HOOKS_GET_INTERFACE(hooks)->connect_to_function_calls_and_execution(hooks,
                                                                                       callback,
                                                                                       user_data);
}

const char *
gjs_interrupt_info_get_filename(const GjsInterruptInfo *info)
{
    return (const char *) info->filename;
}

unsigned int
gjs_interrupt_info_get_line(const GjsInterruptInfo *info)
{
    return info->line;
}

const char *
gjs_interrupt_info_get_function_name(const GjsInterruptInfo *info)
{
    return (const char *) info->function_name;
}

GjsFrameState
gjs_frame_info_get_state(const GjsFrameInfo *info)
{
    return info->frame_state;
}

const char *
gjs_debug_script_info_get_filename(const GjsDebugScriptInfo *info)
{
    return (const char *) info->filename;
}

GjsReflectedScript *
gjs_debug_script_info_get_reflection(const GjsDebugScriptInfo *info)
{
    return info->reflected_script;
}
