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
#ifndef _GJS_DEBUG_HOOKS_H
#define _GJS_DEBUG_HOOKS_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GJS_DEBUG_HOOKS_INTERFACE(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), \
                                        GJS_TYPE_DEBUG_HOOKS_INTERFACE, \
                                        GjsDebugHooks))
#define GJS_DEBUG_HOOKS_GET_INTERFACE(obj) (G_TYPE_INSTANCE_GET_INTERFACE(obj, \
                                            GJS_TYPE_DEBUG_HOOKS_INTERFACE, \
                                            GjsDebugHooksInterface))
#define GJS_TYPE_DEBUG_HOOKS_INTERFACE (gjs_debug_hooks_interface_get_type())

typedef struct _GjsDebugHooksInterface GjsDebugHooksInterface;
typedef struct _GjsDebugHooks GjsDebugHooks;

typedef struct _GjsDebugConnection GjsDebugConnection;
typedef struct _GjsContext GjsContext;
typedef struct _GjsReflectedScript GjsReflectedScript;

typedef struct _GjsDebugScriptInfo GjsDebugScriptInfo;
typedef struct _GjsInterruptInfo GjsInterruptInfo;
typedef struct _GjsFrameInfo GjsFrameInfo;

typedef enum _GjsFrameState {
    GJS_INTERRUPT_FRAME_BEFORE = 0,
    GJS_INTERRUPT_FRAME_AFTER = 1
} GjsFrameState;

const char * gjs_interrupt_info_get_filename(const GjsInterruptInfo *info);
unsigned int gjs_interrupt_info_get_line(const GjsInterruptInfo *info);
const char * gjs_interrupt_info_get_function_name(const GjsInterruptInfo *info);
GjsFrameState gjs_frame_info_get_state(const GjsFrameInfo *info);

const char * gjs_debug_script_info_get_filename(const GjsDebugScriptInfo *info);
unsigned int gjs_debug_script_info_get_begin_line(const GjsDebugScriptInfo *info);
GjsReflectedScript * gjs_debug_script_info_get_reflection(const GjsDebugScriptInfo *info);



typedef void (*GjsFrameCallback)(GjsDebugHooks *hooks,
                                 GjsContext    *context,
                                 GjsFrameInfo  *info,
                                 gpointer       user_data);

typedef void (*GjsInterruptCallback)(GjsDebugHooks    *hooks,
                                     GjsContext       *context,
                                     GjsInterruptInfo *info,
                                     gpointer          user_data);

typedef void (*GjsInfoCallback) (GjsDebugHooks      *hooks,
                                 GjsContext         *context,
                                 GjsDebugScriptInfo *info,
                                 gpointer            user_data);

struct _GjsDebugHooksInterface {
    GTypeInterface parent;

    GjsDebugConnection * (*add_breakpoint) (GjsDebugHooks        *hooks,
                                            const char           *filename,
                                            unsigned int          line,
                                            GjsInterruptCallback  callback,
                                            gpointer              user_data);
    GjsDebugConnection * (*start_singlestep) (GjsDebugHooks       *hooks,
                                              GjsInterruptCallback callback,
                                              gpointer             user_data);
    GjsDebugConnection * (*connect_to_script_load) (GjsDebugHooks   *hooks,
                                                    GjsInfoCallback  callback,
                                                    gpointer         user_data);
    GjsDebugConnection * (*connect_to_function_calls_and_execution) (GjsDebugHooks    *hooks,
                                                                     GjsFrameCallback  callback,
                                                                     gpointer          user_data);
};

GjsDebugConnection *
gjs_debug_hooks_add_breakpoint(GjsDebugHooks        *hooks,
                               const char           *filename,
                               unsigned int          line,
                               GjsInterruptCallback  callback,
                               gpointer              user_data);

GjsDebugConnection *
gjs_debug_hooks_start_singlestep(GjsDebugHooks        *hooks,
                                 GjsInterruptCallback  callback,
                                 gpointer              user_data);

GjsDebugConnection *
gjs_debug_hooks_connect_to_script_load(GjsDebugHooks   *hooks,
                                       GjsInfoCallback  callback,
                                       gpointer         user_data);

GjsDebugConnection *
gjs_debug_hooks_connect_to_function_calls_and_execution(GjsDebugHooks    *hooks,
                                                        GjsFrameCallback  callback,
                                                        gpointer          user_data);

GType gjs_debug_hooks_interface_get_type(void);

G_END_DECLS

#endif
