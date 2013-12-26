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
#include <glib-object.h>

#include <gjs/gjs.h>
#include <gjs/compat.h>
#include <gjs/gjs-module.h>

#include <gjs/debug-connection.h>
#include <gjs/debug-hooks.h>
#include <gjs/multiplexed-debug-hooks.h>
#include <gjs/reflected-script.h>
#include <gjs/reflected-executable-script.h>

#include "debug-hooks-private.h"
#include "debug-connection-private.h"

static void gjs_debug_hooks_interface_init(GjsDebugHooksInterface *);

struct _GjsMultiplexedDebugHooksPrivate {
    /* Hook lock count */
    unsigned int debug_mode_lock_count;
    unsigned int single_step_mode_lock_count;
    unsigned int interrupt_function_lock_count;
    unsigned int call_and_execute_hook_lock_count;
    unsigned int new_script_hook_lock_count;

    /* These are data structures which contain callback points
     * whenever our internal JS debugger hooks get called */
    GHashTable *breakpoints;
    GHashTable *pending_breakpoints;
    GArray     *single_step_hooks;
    GArray     *call_and_execute_hooks;
    GArray     *new_script_hooks;

    /* These are data structures which we can use to
     * look up the keys for the above structures on
     * destruction. */
    GHashTable *breakpoints_connections;
    GHashTable *single_step_connections;
    GHashTable *call_and_execute_connections;
    GHashTable *new_script_connections;

    /* This is a hashtable of GjsDebugScriptLookupInfo to
     * GjsDebugScript */
    GHashTable *scripts_loaded;

    /* This is a hashtable of script filenames to
     * GjsReflectedScript */
    GHashTable *reflected_scripts;

    /* Observing Reference */
    GjsContext *context;
};

G_DEFINE_TYPE_WITH_CODE(GjsMultiplexedDebugHooks,
                        gjs_multiplexed_debug_hooks,
                        G_TYPE_OBJECT,
                        G_ADD_PRIVATE(GjsMultiplexedDebugHooks)
                        G_IMPLEMENT_INTERFACE(GJS_TYPE_DEBUG_HOOKS_INTERFACE,
                                              gjs_debug_hooks_interface_init))

enum {
    PROP_0,
    PROP_CONTEXT,
    PROP_N
};

static GParamSpec *properties[PROP_N];

typedef struct _GjsDebugUserCallback {
    GCallback callback;
    gpointer  user_data;
} GjsDebugUserCallback;

static void gjs_multiplexed_debug_hooks_unlock_new_script_callback(GjsMultiplexedDebugHooks *hooks);

static void
gjs_debug_user_callback_assign(GjsDebugUserCallback *user_callback,
                               GCallback             callback,
                               gpointer              user_data)
{
    user_callback->callback = callback;
    user_callback->user_data = user_data;
}

static GjsDebugUserCallback *
gjs_debug_user_callback_new(GCallback callback,
                            gpointer  user_data)
{
    GjsDebugUserCallback *user_callback = g_new0(GjsDebugUserCallback, 1);
    gjs_debug_user_callback_assign(user_callback, callback, user_data);
    return user_callback;
}

static void
gjs_debug_user_callback_free(GjsDebugUserCallback *user_callback)
{
    g_free(user_callback);
}

typedef struct _GjsDebugScriptLookupInfo
{
    char         *name;
    unsigned int lineno;
} GjsDebugScriptLookupInfo;

static GjsDebugScriptLookupInfo *
gjs_debug_script_lookup_info_new(const char   *name,
                                 unsigned int  lineno)
{
    GjsDebugScriptLookupInfo *info = g_new0(GjsDebugScriptLookupInfo, 1);
    info->name = g_strdup(name);
    info->lineno = lineno;
    return info;
}

static unsigned int
gjs_debug_script_lookup_info_hash (gconstpointer key)
{
    GjsDebugScriptLookupInfo *info = (GjsDebugScriptLookupInfo *) key;
    return g_int_hash(&info->lineno) ^ g_str_hash(info->name);
}

static gboolean
gjs_debug_script_lookup_info_equal(gconstpointer first,
                                   gconstpointer second)
{
    GjsDebugScriptLookupInfo *first_info = (GjsDebugScriptLookupInfo *) first;
    GjsDebugScriptLookupInfo *second_info = (GjsDebugScriptLookupInfo *) second;

    return first_info->lineno == second_info->lineno &&
           g_strcmp0(first_info->name, second_info->name) == 0;
}

static void
gjs_debug_script_lookup_info_destroy (gpointer info)
{
    GjsDebugScriptLookupInfo *lookup_info = (GjsDebugScriptLookupInfo *) info;
    g_free(lookup_info->name);
    g_free(lookup_info);
}

typedef struct _GjsDebugScript {
    GjsReflectedExecutableScript *reflected_script;
    JSScript                     *native_script;
} GjsDebugScript;

static GjsDebugScript *
gjs_debug_script_new(GjsReflectedExecutableScript *reflected_script,
                     JSScript                     *native_script)
{
    GjsDebugScript *script = g_new0(GjsDebugScript, 1);
    script->native_script = native_script;
    script->reflected_script = reflected_script;
    return script;
}

static void
gjs_debug_script_destroy(gpointer data)
{
    GjsDebugScript *script = (GjsDebugScript *) data;
    g_free(script);
}

typedef struct _InterruptCallbackDispatchData {
    GjsMultiplexedDebugHooks *hooks;
    GjsInterruptInfo         *info;
} InterruptCallbackDispatchData;

static void
dispatch_interrupt_callbacks (gpointer element,
                              gpointer user_data)
{
    GjsDebugUserCallback            *user_callback = (GjsDebugUserCallback *) element;
    InterruptCallbackDispatchData   *dispatch_data = (InterruptCallbackDispatchData *) user_data;
    GjsMultiplexedDebugHooksPrivate *priv = (GjsMultiplexedDebugHooksPrivate *) gjs_multiplexed_debug_hooks_get_instance_private(dispatch_data->hooks);
    GjsContext                      *context = priv->context;
    GjsInterruptCallback            callback = (GjsInterruptCallback) user_callback->callback;

    callback(GJS_DEBUG_HOOKS_INTERFACE(dispatch_data->hooks),
             context,
             dispatch_data->info,
             user_callback->user_data);
}

typedef struct _InfoCallbackDispatchData {
    GjsMultiplexedDebugHooks *hooks;
    GjsDebugScriptInfo       *info;
} InfoCallbackDispatchData;

static void
dispatch_info_callbacks (gpointer element,
                         gpointer user_data)

{
    GjsDebugUserCallback            *user_callback = (GjsDebugUserCallback *) element;
    InfoCallbackDispatchData        *dispatch_data = (InfoCallbackDispatchData *) user_data;
    GjsMultiplexedDebugHooksPrivate *priv = (GjsMultiplexedDebugHooksPrivate *) gjs_multiplexed_debug_hooks_get_instance_private(dispatch_data->hooks);
    GjsContext                      *context = priv->context;
    GjsInfoCallback                 callback = (GjsInfoCallback) user_callback->callback;

    callback(GJS_DEBUG_HOOKS_INTERFACE(dispatch_data->hooks),
             context,
             dispatch_data->info,
             user_callback->user_data);
}

typedef struct _FrameCallbackDispatchData {
  GjsMultiplexedDebugHooks *hooks;
  GjsFrameInfo             *info;
} FrameCallbackDispatchData;

static void
dispatch_frame_callbacks (gpointer element,
                          gpointer user_data)

{
    GjsDebugUserCallback            *user_callback = (GjsDebugUserCallback *) element;
    FrameCallbackDispatchData       *dispatch_data = (FrameCallbackDispatchData *) user_data;
    GjsMultiplexedDebugHooksPrivate *priv = (GjsMultiplexedDebugHooksPrivate *) gjs_multiplexed_debug_hooks_get_instance_private(dispatch_data->hooks);
    GjsContext                      *context = priv->context;
    GjsFrameCallback                callback = (GjsFrameCallback) user_callback->callback;

    callback(GJS_DEBUG_HOOKS_INTERFACE(dispatch_data->hooks),
             context,
             dispatch_data->info,
             user_callback->user_data);
}

static void
for_each_element_in_array(GArray   *array,
                          GFunc     func,
                          gpointer  user_data)
{
    const gsize element_size = g_array_get_element_size(array);
    unsigned int i;
    char         *current_array_pointer = (char *) array->data;

    for (i = 0; i < array->len; ++i, current_array_pointer += element_size)
        (*func)(current_array_pointer, user_data);
}

static char *
get_fully_qualified_path(const char *filename)
{
    char *fully_qualified_path = NULL;
    /* Sometimes we might get just a basename if the script is in the current
     * working directly. If that's the case, then we need to add the fully
     * qualified pathname */
    if (!g_path_is_absolute(filename)) {
        char *current_dir = g_get_current_dir();
        fully_qualified_path = g_strconcat(current_dir,
                                           "/",
                                           filename,
                                           NULL);
        g_free(current_dir);
    } else {
        fully_qualified_path = g_strdup(filename);
    }

    return fully_qualified_path;
}

static void
gjs_multiplexed_debug_hooks_populate_interrupt_info_from_js_function(GjsInterruptInfo *info,
                                                                     JSContext        *js_context,
                                                                     JSScript         *script,
                                                                     JSFunction       *js_function)
{
    JSString *js_function_name = NULL;

    JSAutoCompartment ac(js_context,
                         JS_GetGlobalObject(js_context));

    if (js_function)
        js_function_name = JS_GetFunctionId(js_function);

    info->filename = get_fully_qualified_path(JS_GetScriptFilename(js_context, script));
    info->line = JS_GetScriptBaseLineNumber(js_context, script);

    char *function_name = NULL;

    if (!js_function_name ||
        !gjs_string_to_utf8(js_context,
                            STRING_TO_JSVAL(js_function_name),
                            &function_name))
        function_name = g_strdup_printf("function:%i", info->line);

    info->function_name = function_name;
}

static void
gjs_multiplexed_debug_hooks_populate_interrupt_info(GjsInterruptInfo *info,
                                                    JSContext        *js_context,
                                                    JSScript         *script,
                                                    jsbytecode       *pc)
{
    JSAutoCompartment ac(js_context,
                         JS_GetGlobalObject(js_context));

    JSFunction *js_function  = JS_GetScriptFunction(js_context, script);
    gjs_multiplexed_debug_hooks_populate_interrupt_info_from_js_function(info,
                                                                          js_context,
                                                                          script,
                                                                          js_function);
    info->line = JS_PCToLineNumber(js_context, script, pc);
}

static void
gjs_multiplexed_debug_hooks_clear_interrupt_info(GjsInterruptInfo *info)
{
    g_free(info->function_name);
    g_free(info->filename);
}

static void
gjs_multiplexed_debug_hooks_populate_script_info(GjsDebugScriptInfo *info,
                                                 JSContext          *js_context,
                                                 GjsDebugScript     *script,
                                                 const char         *filename)
{
    info->reflected_script = GJS_REFLECTED_SCRIPT_INTERFACE (script->reflected_script);
    info->filename = filename;
    info->begin_line = JS_GetScriptBaseLineNumber(js_context, script->native_script);
}

typedef struct _GjsBreakpoint {
    JSScript *script;
    jsbytecode *pc;
} GjsBreakpoint;

static void
gjs_breakpoint_destroy(gpointer data)
{
    GjsBreakpoint *breakpoint = (GjsBreakpoint *) data;

    g_free(breakpoint);
}

static GjsBreakpoint *
gjs_breakpoint_new(JSScript   *script,
                   jsbytecode *pc)
{
    GjsBreakpoint *breakpoint = g_new0(GjsBreakpoint, 1);
    breakpoint->script = script;
    breakpoint->pc = pc;
    return breakpoint;
}

typedef struct _GjsPendingBreakpoint {
    char         *filename;
    unsigned int lineno;
} GjsPendingBreakpoint;

static void
gjs_pending_breakpoint_destroy(gpointer data)
{
    GjsPendingBreakpoint *pending = (GjsPendingBreakpoint *) data;
    g_free(pending->filename);
    g_free(pending);
}

static GjsPendingBreakpoint *
gjs_pending_breakpoint_new(const char   *filename,
                           unsigned int  lineno)
{
    GjsPendingBreakpoint *pending = g_new0(GjsPendingBreakpoint, 1);
    pending->filename = g_strdup(filename);
    pending->lineno = lineno;
    return pending;
}

typedef struct _BreakpointActivationData {
    GjsMultiplexedDebugHooks *debug_hooks;
    JSContext                *js_context;
    JSScript                 *js_script;
    const char               *filename;
    unsigned int             begin_lineno;
    GHashTable               *breakpoints;
    GList                    *breakpoints_changed;
} BreakpointActivationData;

static void
remove_breakpoint_from_hashtable_by_user_callback(gpointer list_item,
                                                  gpointer hashtable_pointer)
{
    GHashTable           *breakpoints = (GHashTable *) hashtable_pointer;
    GjsPendingBreakpoint *pending_breakpoint = (GjsPendingBreakpoint *) g_hash_table_lookup(breakpoints, list_item);

    g_return_if_fail(pending_breakpoint);

    gjs_pending_breakpoint_destroy(pending_breakpoint);
    g_hash_table_remove(breakpoints, list_item);
}

static void
remove_activated_breakpoints_from_pending(BreakpointActivationData *data,
                                          GHashTable               *pending)
{
    g_list_foreach(data->breakpoints_changed,
                   remove_breakpoint_from_hashtable_by_user_callback,
                   pending);
    g_list_free(data->breakpoints_changed);
}

static unsigned int
get_script_end_lineno(JSContext *js_context,
                      JSScript  *js_script)
{
    JSAutoCompartment ac(js_context,
                         JS_GetGlobalObject(js_context));

    jsbytecode *pc = JS_EndPC(js_context, js_script);
    return JS_PCToLineNumber(js_context,
                             js_script,
                             pc);
}

typedef struct _GjsMultiplexedDebugHooksTrapPrivateData {
    GjsMultiplexedDebugHooks *hooks;
    GjsDebugUserCallback     *user_callback;
} GjsMultiplexedDebugHooksTrapPrivateData;

GjsMultiplexedDebugHooksTrapPrivateData *
gjs_multiplexed_debug_hooks_trap_private_data_new(GjsMultiplexedDebugHooks *hooks,
                                                  GjsDebugUserCallback     *user_callback)
{
    GjsMultiplexedDebugHooksTrapPrivateData *data =
        g_new0(GjsMultiplexedDebugHooksTrapPrivateData, 1);

    data->hooks = hooks;
    data->user_callback = user_callback;

    return data;
}

static void
gjs_multiplexed_debug_hooks_trap_private_data_destroy(GjsMultiplexedDebugHooksTrapPrivateData *data)
{
    g_free (data);
}

/* Callbacks */
static JSTrapStatus
gjs_multiplexed_debug_hooks_trap_handler(JSContext  *context,
                                         JSScript   *script,
                                         jsbytecode *pc,
                                         jsval      *rval,
                                         jsval       closure)
{
    GjsMultiplexedDebugHooksTrapPrivateData *data =
        (GjsMultiplexedDebugHooksTrapPrivateData *) JSVAL_TO_PRIVATE(closure);

    /* And there goes the law of demeter */
    GjsMultiplexedDebugHooks *hooks = data->hooks;
    GjsMultiplexedDebugHooksPrivate *priv = (GjsMultiplexedDebugHooksPrivate *) gjs_multiplexed_debug_hooks_get_instance_private(hooks);
    GjsInterruptInfo interrupt_info;
    GjsInterruptCallback callback = (GjsInterruptCallback) data->user_callback->callback;
    gjs_multiplexed_debug_hooks_populate_interrupt_info(&interrupt_info, context, script, pc);

    callback(GJS_DEBUG_HOOKS_INTERFACE(hooks),
             priv->context,
             &interrupt_info,
             data->user_callback->user_data);

    gjs_multiplexed_debug_hooks_clear_interrupt_info(&interrupt_info);

    return JSTRAP_CONTINUE;
}

static GjsBreakpoint *
gjs_debug_create_native_breakpoint_for_script(GjsMultiplexedDebugHooks *hooks,
                                              JSContext                *js_context,
                                              JSScript                 *script,
                                              unsigned int              line,
                                              GjsDebugUserCallback     *user_callback)
{
    GjsMultiplexedDebugHooksTrapPrivateData *data =
        gjs_multiplexed_debug_hooks_trap_private_data_new(hooks, user_callback);

    JSAutoCompartment ac(js_context,
                         JS_GetGlobalObject(js_context));

    /* This always succeeds, although it might only return the very-end
     * or very-beginning program counter if the line is out of range */
    jsbytecode *pc =
        JS_LineNumberToPC(js_context, script, line);

    /* Set the breakpoint on the JS side now that we're tracking it */
    JS_SetTrap(js_context,
               script,
               pc,
               gjs_multiplexed_debug_hooks_trap_handler,
               PRIVATE_TO_JSVAL(data));

    return gjs_breakpoint_new(script, pc);
}

static void
activate_breakpoint_if_within_script(gpointer key,
                                     gpointer value,
                                     gpointer user_data) {
    GjsPendingBreakpoint     *pending = (GjsPendingBreakpoint *) value;
    BreakpointActivationData *activation_data = (BreakpointActivationData *) user_data;

    /* Interrogate the script for its last program counter and thus its
     * last line. If the desired breakpoint line falls within this script's
     * line range then activate it. */
    if (strcmp(activation_data->filename, pending->filename) == 0) {
        unsigned int end_lineno = get_script_end_lineno(activation_data->js_context,
                                                        activation_data->js_script);

        if (activation_data->begin_lineno <= pending->lineno &&
            end_lineno >= pending->lineno) {
            GjsBreakpoint *breakpoint =
                gjs_debug_create_native_breakpoint_for_script(activation_data->debug_hooks,
                                                              activation_data->js_context,
                                                              activation_data->js_script,
                                                              pending->lineno,
                                                              (GjsDebugUserCallback *) key);
            g_hash_table_insert(activation_data->breakpoints,
                                key,
                                breakpoint);

            /* We append "key" here as that is what we need to remove-by later */
            activation_data->breakpoints_changed =
                g_list_append(activation_data->breakpoints_changed,
                              key);

            /* Decrement new script callback, we might not need to know about
             * new scripts anymore as the breakpoint is no longer pending */
            gjs_multiplexed_debug_hooks_unlock_new_script_callback(activation_data->debug_hooks);
        }
    }
}

static GjsReflectedExecutableScript *
lookup_or_create_script_reflection(GHashTable *reflected_scripts,
                                   const char *filename)
{
    gpointer result = g_hash_table_lookup(reflected_scripts,
                                          (gconstpointer) filename);
    if (result) {
        return (GjsReflectedExecutableScript *) result;
    } else {
        GjsReflectedExecutableScript *reflected_script = gjs_reflected_executable_script_new(filename);
        g_hash_table_insert(reflected_scripts,
                            g_strdup(filename),
                            reflected_script);
        return reflected_script;
    }
}

static void
gjs_multiplexed_debug_hooks_new_script_callback(JSContext    *context,
                                                const char  *filename,
                                                unsigned int  lineno,
                                                JSScript     *native_script,
                                                JSFunction   *function,
                                                gpointer      caller_data)
{
    /* We don't care about NULL-filename scripts, they are probably just initialization
     * scripts */
    if (!filename)
        return;

    GjsMultiplexedDebugHooks *hooks = GJS_MULTIPLEXED_DEBUG_HOOKS(caller_data);
    GjsMultiplexedDebugHooksPrivate *priv = (GjsMultiplexedDebugHooksPrivate *) gjs_multiplexed_debug_hooks_get_instance_private(hooks);
    GjsDebugScriptLookupInfo *info =
        gjs_debug_script_lookup_info_new(filename, lineno);
    GjsReflectedExecutableScript *reflected = lookup_or_create_script_reflection(priv->reflected_scripts, filename);
    GjsDebugScript *script = gjs_debug_script_new(reflected, native_script);
    JSContext *js_context = (JSContext *) gjs_context_get_native_context(priv->context);
    char *fully_qualified_path = get_fully_qualified_path(filename);

    JSAutoCompartment ac(js_context,
                         JS_GetGlobalObject(js_context));

    g_hash_table_insert(priv->scripts_loaded,
                        info,
                        script);

    /* Special case - if single-step mode is enabled then we should enable it
     * here */
    if (priv->single_step_mode_lock_count)
        JS_SetSingleStepMode(js_context, native_script, TRUE);

    /* Special case - search pending breakpoints for the current script filename
     * and convert them to real breakpoints if need be */
    BreakpointActivationData activation_data = {
        hooks,
        js_context,
        native_script,
        fully_qualified_path,
        lineno,
        priv->breakpoints,
        NULL
    };

    g_hash_table_foreach(priv->pending_breakpoints,
                         activate_breakpoint_if_within_script,
                         &activation_data);
    remove_activated_breakpoints_from_pending(&activation_data,
                                              priv->pending_breakpoints);

    GjsDebugScriptInfo debug_script_info;
    gjs_multiplexed_debug_hooks_populate_script_info(&debug_script_info,
                                                     context,
                                                     script,
                                                     fully_qualified_path);


    InfoCallbackDispatchData data = {
        hooks,
        &debug_script_info
    };

    /* Finally, call the callback function */
    for_each_element_in_array(priv->new_script_hooks,
                              dispatch_info_callbacks,
                              &data);

    g_free(fully_qualified_path);
}

static void
gjs_multiplexed_debug_hooks_script_destroyed_callback(JSFreeOp     *fo,
                                                      JSScript     *script,
                                                      gpointer      caller_data)
{
    GjsMultiplexedDebugHooks        *hooks = GJS_MULTIPLEXED_DEBUG_HOOKS(caller_data);
    GjsMultiplexedDebugHooksPrivate *priv = (GjsMultiplexedDebugHooksPrivate *) gjs_multiplexed_debug_hooks_get_instance_private(hooks);
    JSContext *js_context = (JSContext *) gjs_context_get_native_context(priv->context);

    JSAutoCompartment ac(js_context,
                         JS_GetGlobalObject(js_context));

    GjsDebugScriptLookupInfo info = {
        (char *) JS_GetScriptFilename(js_context, script),
        JS_GetScriptBaseLineNumber(js_context, script)
    };

    g_hash_table_remove(priv->scripts_loaded, &info);
}

static JSTrapStatus
gjs_multiplexed_debug_hooks_interrupt_callback(JSContext  *context,
                                               JSScript   *script,
                                               jsbytecode *pc,
                                               jsval      *rval,
                                               gpointer    closure)
{
    GjsMultiplexedDebugHooks        *hooks = GJS_MULTIPLEXED_DEBUG_HOOKS(closure);
    GjsMultiplexedDebugHooksPrivate *priv = (GjsMultiplexedDebugHooksPrivate *) gjs_multiplexed_debug_hooks_get_instance_private(hooks);

    GjsInterruptInfo interrupt_info;
    gjs_multiplexed_debug_hooks_populate_interrupt_info(&interrupt_info,
                                                         context,
                                                         script,
                                                         pc);

    InterruptCallbackDispatchData data = {
        hooks,
        &interrupt_info,
    };

    for_each_element_in_array(priv->single_step_hooks,
                              dispatch_interrupt_callbacks,
                              &data);

    gjs_multiplexed_debug_hooks_clear_interrupt_info(&interrupt_info);

    return JSTRAP_CONTINUE;
}

static void *
gjs_multiplexed_debug_hooks_function_call_or_execution_callback(JSContext          *context,
                                                                JSAbstractFramePtr  frame,
                                                                bool                is_constructing,
                                                                JSBool              before,
                                                                JSBool             *ok,
                                                                gpointer            closure)
{
    JSFunction                      *function = frame.maybeFun();
    JSScript                        *script = frame.script();
    GjsMultiplexedDebugHooks        *hooks = GJS_MULTIPLEXED_DEBUG_HOOKS(closure);
    GjsMultiplexedDebugHooksPrivate *priv = (GjsMultiplexedDebugHooksPrivate *) gjs_multiplexed_debug_hooks_get_instance_private(hooks);

    GjsFrameInfo frame_info;
    gjs_multiplexed_debug_hooks_populate_interrupt_info_from_js_function(&frame_info.interrupt,
                                                                          context,
                                                                          script,
                                                                          function);

    frame_info.frame_state = before ? GJS_INTERRUPT_FRAME_BEFORE : GJS_INTERRUPT_FRAME_AFTER;

    FrameCallbackDispatchData data = {
        hooks,
        &frame_info
    };

    for_each_element_in_array(priv->call_and_execute_hooks,
                              dispatch_frame_callbacks,
                              &data);

    gjs_multiplexed_debug_hooks_clear_interrupt_info(&frame_info.interrupt);

    return closure;
}

typedef struct _ChangeDebugModeData {
    unsigned int flags;
    gboolean     enabled;
} ChangeDebugModeData;

typedef void (*LockAction)(JSContext *context,
                           gpointer   user_data);

static void
lock_and_perform_if_unlocked(GjsContext   *context,
                             unsigned int *lock_count,
                             LockAction    action,
                             gpointer      user_data)
{
    if ((*lock_count)++ == 0) {
        JSContext *js_context = (JSContext *) gjs_context_get_native_context(context);
        (*action)(js_context, user_data);
    }
}

static void
unlock_and_perform_if_locked(GjsContext   *context,
                             unsigned int *lock_count,
                             LockAction    action,
                             gpointer      user_data)
{
    if (--(*lock_count) == 0) {
        JSContext *js_context = (JSContext *) gjs_context_get_native_context(context);
        (*action)(js_context, user_data);
    }
}

static void
change_debug_mode(JSContext *context,
                  gpointer  user_data)
{
  ChangeDebugModeData *data = (ChangeDebugModeData *) user_data;

  JSAutoCompartment ac(context,
                       JS_GetGlobalObject(context));

  JS_BeginRequest(context);
  JS_SetOptions(context, data->flags);
  JS_SetDebugMode(context, data->enabled);
  JS_EndRequest(context);
}

static void
gjs_multiplexed_debug_hooks_lock_debug_mode(GjsMultiplexedDebugHooks *hooks)
{
    GjsMultiplexedDebugHooksPrivate *priv = (GjsMultiplexedDebugHooksPrivate *) gjs_multiplexed_debug_hooks_get_instance_private(hooks);
    ChangeDebugModeData data = {
        JSOPTION_BASELINE | JSOPTION_TYPE_INFERENCE,
        TRUE
    };

    lock_and_perform_if_unlocked(priv->context,
                                 &priv->debug_mode_lock_count,
                                 change_debug_mode,
                                 &data);
}

static void
gjs_multiplexed_debug_hooks_unlock_debug_mode(GjsMultiplexedDebugHooks *hooks)
{
    GjsMultiplexedDebugHooksPrivate *priv = (GjsMultiplexedDebugHooksPrivate *) gjs_multiplexed_debug_hooks_get_instance_private(hooks);
    ChangeDebugModeData data = {
        0,
        FALSE
    };

    unlock_and_perform_if_locked(priv->context,
                                 &priv->debug_mode_lock_count,
                                 change_debug_mode,
                                 &data);
}

typedef struct _ChangeInterruptFunctionData
{
  JSInterruptHook callback;
  gpointer        user_data;
} ChangeInterruptFunctionData;

static void
set_interrupt_function_hook(JSContext *context,
                            gpointer   user_data)
{
    ChangeInterruptFunctionData *data = (ChangeInterruptFunctionData *) user_data;

    JSAutoCompartment ac(context,
                         JS_GetGlobalObject(context));

    JS_SetInterrupt(JS_GetRuntime(context),
                    data->callback,
                    data->user_data);
}

static void
gjs_multiplexed_debug_hooks_lock_interrupt_function(GjsMultiplexedDebugHooks *hooks)
{
    GjsMultiplexedDebugHooksPrivate *priv = (GjsMultiplexedDebugHooksPrivate *) gjs_multiplexed_debug_hooks_get_instance_private(hooks);
    ChangeInterruptFunctionData data = {
        gjs_multiplexed_debug_hooks_interrupt_callback,
        hooks
    };

    lock_and_perform_if_unlocked(priv->context,
                                &priv->interrupt_function_lock_count,
                                set_interrupt_function_hook,
                                &data);
}

static void
gjs_multiplexed_debug_hooks_unlock_interrupt_function(GjsMultiplexedDebugHooks *hooks)
{
    GjsMultiplexedDebugHooksPrivate *priv = (GjsMultiplexedDebugHooksPrivate *) gjs_multiplexed_debug_hooks_get_instance_private(hooks);
    ChangeInterruptFunctionData data = {
        NULL,
        NULL
    };

    unlock_and_perform_if_locked(priv->context,
                                 &priv->interrupt_function_lock_count,
                                 set_interrupt_function_hook,
                                 &data);
}

typedef struct _NewScriptHookData {
    JSNewScriptHook      new_callback;
    JSDestroyScriptHook  destroy_callback;
    gpointer             user_data;
} NewScriptHookData;

static void
set_new_script_hook(JSContext *context,
                    gpointer   user_data)
{
    NewScriptHookData *data = (NewScriptHookData *) user_data;

    JSAutoCompartment ac(context,
                        JS_GetGlobalObject(context));

    JS_SetNewScriptHook(JS_GetRuntime(context), data->new_callback, data->user_data);
    JS_SetDestroyScriptHook(JS_GetRuntime(context), data->destroy_callback, data->user_data);
}

static void
gjs_multiplexed_debug_hooks_lock_new_script_callback (GjsMultiplexedDebugHooks *hooks)
{
    GjsMultiplexedDebugHooksPrivate *priv = (GjsMultiplexedDebugHooksPrivate *) gjs_multiplexed_debug_hooks_get_instance_private(hooks);
    NewScriptHookData data = {
        gjs_multiplexed_debug_hooks_new_script_callback,
        gjs_multiplexed_debug_hooks_script_destroyed_callback,
        hooks
    };

    lock_and_perform_if_unlocked(priv->context,
                                 &priv->new_script_hook_lock_count,
                                 set_new_script_hook,
                                 &data);
}

static void
gjs_multiplexed_debug_hooks_unlock_new_script_callback (GjsMultiplexedDebugHooks *hooks)
{
    GjsMultiplexedDebugHooksPrivate *priv = (GjsMultiplexedDebugHooksPrivate *) gjs_multiplexed_debug_hooks_get_instance_private(hooks);
    NewScriptHookData data = {
        NULL,
        NULL,
        NULL
    };

  unlock_and_perform_if_locked(priv->context,
                               &priv->new_script_hook_lock_count,
                               set_new_script_hook,
                               &data);
}

typedef struct _SingleStepModeData {
    JSContext  *context;
    GHashTable *scripts;
    gboolean   enabled;
} SingleStepModeData;

static void
set_single_step_mode_on_registered_script(gpointer key,
                                          gpointer value,
                                          gpointer user_data)
{
    GjsDebugScript     *script = (GjsDebugScript *) value;
    SingleStepModeData *data = (SingleStepModeData *) user_data;

    JSAutoCompartment ac(data->context,
                         JS_GetGlobalObject(data->context));

    JS_SetSingleStepMode(data->context,
                         script->native_script,
                         data->enabled);
}

typedef struct _SingleStepModeForeachData {
    GHashTable *scripts;
    gboolean   enabled;
} SingleStepModeForeachData;

static void
set_single_step_mode(JSContext *context,
                     gpointer  user_data)
{
    SingleStepModeForeachData *foreach_data = (SingleStepModeForeachData *) user_data;

    SingleStepModeData data = {
        context,
        foreach_data->scripts,
        foreach_data->enabled
    };

    g_hash_table_foreach(foreach_data->scripts,
                         set_single_step_mode_on_registered_script,
                         &data);
}

static void
gjs_multiplexed_debug_hooks_lock_single_step_mode(GjsMultiplexedDebugHooks *hooks)
{
    GjsMultiplexedDebugHooksPrivate *priv = (GjsMultiplexedDebugHooksPrivate *) gjs_multiplexed_debug_hooks_get_instance_private(hooks);
    SingleStepModeForeachData data = {
        priv->scripts_loaded,
        TRUE
    };

    lock_and_perform_if_unlocked(priv->context,
                                 &priv->single_step_mode_lock_count,
                                 set_single_step_mode,
                                 &data);
}

static void
gjs_multiplexed_debug_hooks_unlock_single_step_mode(GjsMultiplexedDebugHooks *hooks)
{
    GjsMultiplexedDebugHooksPrivate *priv = (GjsMultiplexedDebugHooksPrivate *) gjs_multiplexed_debug_hooks_get_instance_private(hooks);
    SingleStepModeForeachData data = {
        priv->scripts_loaded,
        FALSE
    };

    unlock_and_perform_if_locked(priv->context,
                                 &priv->single_step_mode_lock_count,
                                 set_single_step_mode,
                                 &data);
}

typedef struct _FunctionCallsAndExecutionHooksData
{
    JSInterpreterHook hook;
    gpointer          user_data;
} FunctionCallsAndExecutionHooksData;

static void
set_function_calls_and_execution_hooks(JSContext *context,
                                       gpointer  user_data)
{
    JSRuntime                          *js_runtime = JS_GetRuntime(context);
    FunctionCallsAndExecutionHooksData *data = (FunctionCallsAndExecutionHooksData *) user_data;

    JSAutoCompartment ac(context,
                         JS_GetGlobalObject(context));

    JS_SetExecuteHook(js_runtime, data->hook, data->user_data);
    JS_SetCallHook(js_runtime, data->hook, data->user_data);
}

static void
gjs_multiplexed_debug_hooks_lock_function_calls_and_execution(GjsMultiplexedDebugHooks *hooks)
{
    GjsMultiplexedDebugHooksPrivate *priv = (GjsMultiplexedDebugHooksPrivate *) gjs_multiplexed_debug_hooks_get_instance_private(hooks);
    FunctionCallsAndExecutionHooksData data = {
        gjs_multiplexed_debug_hooks_function_call_or_execution_callback,
        hooks
    };

    lock_and_perform_if_unlocked(priv->context,
                                 &priv->call_and_execute_hook_lock_count,
                                 set_function_calls_and_execution_hooks,
                                 &data);
}

static void
gjs_multiplexed_debug_hooks_unlock_function_calls_and_execution(GjsMultiplexedDebugHooks *hooks)
{
    GjsMultiplexedDebugHooksPrivate *priv = (GjsMultiplexedDebugHooksPrivate *) gjs_multiplexed_debug_hooks_get_instance_private(hooks);
    FunctionCallsAndExecutionHooksData data = {
        NULL,
        NULL
    };

    unlock_and_perform_if_locked(priv->context,
                                 &priv->call_and_execute_hook_lock_count,
                                 set_function_calls_and_execution_hooks,
                                 &data);
}

static void
gjs_multiplexed_debug_hooks_remove_breakpoint(GjsDebugConnection *connection,
                                               gpointer            user_data)
{
    GjsMultiplexedDebugHooks *hooks = GJS_MULTIPLEXED_DEBUG_HOOKS(user_data);
    GjsMultiplexedDebugHooksPrivate *priv = (GjsMultiplexedDebugHooksPrivate *) gjs_multiplexed_debug_hooks_get_instance_private(hooks);
    JSContext *js_context = (JSContext *) gjs_context_get_native_context(priv->context);
    GjsDebugUserCallback *callback =
        (GjsDebugUserCallback *) g_hash_table_lookup(priv->breakpoints_connections,
                                                     connection);
    GjsBreakpoint *breakpoint =
        (GjsBreakpoint *) g_hash_table_lookup(priv->breakpoints, callback);

    gboolean item_was_removed = FALSE;

    /* Remove breakpoint if there was one */
    if (breakpoint) {
        g_hash_table_remove(priv->breakpoints, callback);

        JSAutoCompartment ac(js_context,
                           JS_GetGlobalObject(js_context));

        jsval previous_closure;

        JS_ClearTrap(js_context,
                     breakpoint->script,
                     breakpoint->pc,
                     NULL,
                     &previous_closure);

        GjsMultiplexedDebugHooksTrapPrivateData *private_data =
            (GjsMultiplexedDebugHooksTrapPrivateData *) JSVAL_TO_PRIVATE(previous_closure);
        gjs_multiplexed_debug_hooks_trap_private_data_destroy(private_data);

        gjs_breakpoint_destroy(breakpoint);
        item_was_removed = TRUE;
    } else {
        /* Try to find pending breakpoints we never got to insert */
        GjsPendingBreakpoint *pending_breakpoint =
            (GjsPendingBreakpoint *) g_hash_table_lookup(priv->pending_breakpoints, callback);

        if (pending_breakpoint) {
            g_hash_table_remove(priv->pending_breakpoints, callback);
            gjs_pending_breakpoint_destroy(pending_breakpoint);

            /* When removing a pending breakpoint, we must also unlock the new
             * script hook as we might not care about new scripts anymore if pending
             * breakpoints are empty */
            gjs_multiplexed_debug_hooks_unlock_new_script_callback(hooks);

            item_was_removed = TRUE;
        }
    }

    g_assert(item_was_removed);

    g_hash_table_remove(priv->breakpoints_connections, connection);
    gjs_debug_user_callback_free(callback);

    gjs_multiplexed_debug_hooks_unlock_debug_mode(hooks);
}

typedef struct _GjsScriptHashTableSearchData
{
    JSContext *js_context;
    JSScript *return_result;
    const char *filename;
    unsigned int line;
} GjsScriptHashTableSearchData;

static JSScript *
lookup_script_for_filename_with_closest_baseline_floor(GjsMultiplexedDebugHooks *hooks,
                                                       const char               *filename,
                                                       unsigned int              line)
{
    GjsMultiplexedDebugHooksPrivate *priv = (GjsMultiplexedDebugHooksPrivate *) gjs_multiplexed_debug_hooks_get_instance_private(hooks);
    JSContext      *js_context = (JSContext *) gjs_context_get_native_context(priv->context);
    GHashTableIter hash_table_iterator;
    gpointer       key = NULL;
    gpointer       value = NULL;

    g_hash_table_iter_init(&hash_table_iterator, priv->scripts_loaded);

    while (g_hash_table_iter_next(&hash_table_iterator, &key, &value)) {
        GjsDebugScriptLookupInfo *info = (GjsDebugScriptLookupInfo *) key;

        if (g_strcmp0(info->name, filename) == 0) {
            GjsDebugScript *script = (GjsDebugScript *) value;
            unsigned int    script_end_line = get_script_end_lineno(js_context,
                                                                    script->native_script);

            if (info->lineno <= line &&
                script_end_line >= line)
                return script->native_script;
        }
    }

    return NULL;
}

static GjsBreakpoint *
lookup_line_and_create_native_breakpoint(JSContext                *js_context,
                                         GjsMultiplexedDebugHooks *debug_hooks,
                                         const char               *filename,
                                         unsigned int              line,
                                         GjsDebugUserCallback     *user_callback)
{
    JSScript *script =
        lookup_script_for_filename_with_closest_baseline_floor(debug_hooks,
                                                               filename,
                                                               line);

    if (!script)
        return NULL;

    return gjs_debug_create_native_breakpoint_for_script(debug_hooks,
                                                         js_context,
                                                         script,
                                                         line,
                                                         user_callback);
}

static GjsDebugConnection *
gjs_multiplexed_debug_hooks_add_breakpoint(GjsDebugHooks        *hooks,
                                           const char           *filename,
                                           unsigned int          line,
                                           GjsInterruptCallback  callback,
                                           gpointer              user_data)
{
    GjsMultiplexedDebugHooks        *debug_hooks = GJS_MULTIPLEXED_DEBUG_HOOKS(hooks);
    GjsMultiplexedDebugHooksPrivate *priv = (GjsMultiplexedDebugHooksPrivate *) gjs_multiplexed_debug_hooks_get_instance_private(debug_hooks);

    JSContext *js_context =
        (JSContext *) gjs_context_get_native_context(priv->context);

    /* We always have a user callback even if we couldn't successfully create a native
     * breakpoint as we can always fall back to creating a pending one */
    GjsDebugUserCallback *user_callback = gjs_debug_user_callback_new(G_CALLBACK (callback),
                                                                      user_data);
    GjsDebugConnection *connection =
        gjs_debug_connection_new(gjs_multiplexed_debug_hooks_remove_breakpoint,
                                 debug_hooks);

    /* Try to create a native breakpoint. If it succeeds, add it to the breakpoints
     * table, otherwise create a pending breakpoint */
    GjsBreakpoint *breakpoint = lookup_line_and_create_native_breakpoint(js_context,
                                                                         debug_hooks,
                                                                         filename,
                                                                         line,
                                                                         user_callback);

    if (breakpoint) {
        g_hash_table_insert(priv->breakpoints,
                            user_callback,
                            breakpoint);
    } else {
        GjsPendingBreakpoint *pending = gjs_pending_breakpoint_new(filename, line);
        g_hash_table_insert(priv->pending_breakpoints,
                            user_callback,
                            pending);

        /* We'll need to know about new scripts being loaded too */
        gjs_multiplexed_debug_hooks_lock_new_script_callback(debug_hooks);
    }

    g_hash_table_insert(priv->breakpoints_connections,
                        connection,
                        user_callback);

    /* We need debug mode for now */
    gjs_multiplexed_debug_hooks_lock_debug_mode(debug_hooks);

    return connection;
}

static int
lookup_index_by_data_in_array(GArray   *array,
                              gpointer  data)
{
    unsigned int i;
    gsize element_size = g_array_get_element_size(array);
    char *underlying_array_pointer = (char *) array->data;

    for (i = 0, underlying_array_pointer = (char *) array->data;
         i < array->len;
         ++i, underlying_array_pointer += element_size)
    {
        if (data == (gpointer) underlying_array_pointer)
            return (int) i;
    }

    return -1;
}

static GjsDebugConnection *
insert_hook_callback(GArray                            *hooks_array,
                     GHashTable                        *hooks_connections_table,
                     GCallback                          callback,
                     gpointer                           user_data,
                     GjsDebugConnectionDisposeCallback  dispose_callback,
                     GjsMultiplexedDebugHooks          *debug_hooks)
{
    unsigned int last_size = hooks_array->len;
    g_array_set_size(hooks_array,
                     last_size + 1);

    GjsDebugUserCallback *user_callback =
        &(g_array_index(hooks_array,
                        GjsDebugUserCallback,
                        last_size));

    gjs_debug_user_callback_assign(user_callback,
                                    callback,
                                    user_data);
    GjsDebugConnection *connection =
        gjs_debug_connection_new(dispose_callback,
                                 debug_hooks);

    g_hash_table_insert(hooks_connections_table,
                        connection,
                        user_callback);

    return connection;
}

static void
remove_hook_callback (GjsDebugConnection *connection,
                      GHashTable         *hooks_connection_table,
                      GArray             *hooks_array)
{
    GjsDebugUserCallback *user_callback =
        (GjsDebugUserCallback *) g_hash_table_lookup(hooks_connection_table,
                                                     connection);
    int array_index = lookup_index_by_data_in_array(hooks_array,
                                                    user_callback);

    g_hash_table_remove(hooks_connection_table,
                        connection);

    if (array_index > -1)
        g_array_remove_index(hooks_array, array_index);
    else
        g_error("Unable to find user callback %p in array index!", user_callback);
}

static void
gjs_multiplexed_debug_hooks_remove_singlestep (GjsDebugConnection *connection,
                                               gpointer            user_data)
{
    GjsMultiplexedDebugHooks        *hooks = GJS_MULTIPLEXED_DEBUG_HOOKS(user_data);
    GjsMultiplexedDebugHooksPrivate *priv = (GjsMultiplexedDebugHooksPrivate *) gjs_multiplexed_debug_hooks_get_instance_private(hooks);
    remove_hook_callback(connection,
                         priv->single_step_connections,
                         priv->single_step_hooks);

    gjs_multiplexed_debug_hooks_unlock_interrupt_function(hooks);
    gjs_multiplexed_debug_hooks_unlock_single_step_mode(hooks);
    gjs_multiplexed_debug_hooks_unlock_debug_mode(hooks);
}

static GjsDebugConnection *
gjs_multiplexed_debug_hooks_add_singlestep(GjsDebugHooks        *hooks,
                                            GjsInterruptCallback  callback,
                                            gpointer              user_data)
{
    GjsMultiplexedDebugHooks        *debug_hooks = GJS_MULTIPLEXED_DEBUG_HOOKS(hooks);
    GjsMultiplexedDebugHooksPrivate *priv = (GjsMultiplexedDebugHooksPrivate *) gjs_multiplexed_debug_hooks_get_instance_private(debug_hooks);
    gjs_multiplexed_debug_hooks_lock_debug_mode(debug_hooks);
    gjs_multiplexed_debug_hooks_lock_interrupt_function(debug_hooks);
    gjs_multiplexed_debug_hooks_lock_single_step_mode(debug_hooks);
    return insert_hook_callback(priv->single_step_hooks,
                                priv->single_step_connections,
                                G_CALLBACK(callback),
                                user_data,
                                gjs_multiplexed_debug_hooks_remove_singlestep,
                                debug_hooks);
}

static void
gjs_multiplexed_debug_hooks_remove_connection_to_script_load(GjsDebugConnection *connection,
                                                             gpointer            user_data)
{
    GjsMultiplexedDebugHooks *hooks = GJS_MULTIPLEXED_DEBUG_HOOKS(user_data);
    GjsMultiplexedDebugHooksPrivate *priv = (GjsMultiplexedDebugHooksPrivate *) gjs_multiplexed_debug_hooks_get_instance_private(hooks);
    remove_hook_callback(connection,
                         priv->new_script_connections,
                         priv->new_script_hooks);
    gjs_multiplexed_debug_hooks_unlock_new_script_callback(hooks);
    gjs_multiplexed_debug_hooks_unlock_debug_mode(hooks);
}

static GjsDebugConnection *
gjs_multiplexed_debug_hooks_connect_to_script_load(GjsDebugHooks   *hooks,
                                                   GjsInfoCallback  callback,
                                                   gpointer         user_data)
{
  GjsMultiplexedDebugHooks        *debug_hooks = GJS_MULTIPLEXED_DEBUG_HOOKS(hooks);
  GjsMultiplexedDebugHooksPrivate *priv = (GjsMultiplexedDebugHooksPrivate *) gjs_multiplexed_debug_hooks_get_instance_private(debug_hooks);
  gjs_multiplexed_debug_hooks_lock_debug_mode(debug_hooks);
  gjs_multiplexed_debug_hooks_lock_new_script_callback(debug_hooks);
  return insert_hook_callback(priv->new_script_hooks,
                              priv->new_script_connections,
                              G_CALLBACK(callback),
                              user_data,
                              gjs_multiplexed_debug_hooks_remove_connection_to_script_load,
                              debug_hooks);
}

static void
gjs_multiplexed_debug_hooks_remove_connection_to_function_calls_and_execution(GjsDebugConnection *connection,
                                                                              gpointer            user_data)
{
    GjsMultiplexedDebugHooks        *hooks = GJS_MULTIPLEXED_DEBUG_HOOKS(user_data);
    GjsMultiplexedDebugHooksPrivate *priv = (GjsMultiplexedDebugHooksPrivate *) gjs_multiplexed_debug_hooks_get_instance_private(hooks);
    remove_hook_callback(connection,
                         priv->call_and_execute_connections,
                         priv->call_and_execute_hooks);
    gjs_multiplexed_debug_hooks_unlock_function_calls_and_execution(hooks);
    gjs_multiplexed_debug_hooks_unlock_debug_mode(hooks);
}

static GjsDebugConnection *
gjs_multiplexed_debug_hooks_connect_to_function_calls_and_execution(GjsDebugHooks    *hooks,
                                                                    GjsFrameCallback  callback,
                                                                    gpointer          user_data)
{
    GjsMultiplexedDebugHooks        *debug_hooks = GJS_MULTIPLEXED_DEBUG_HOOKS(hooks);
    GjsMultiplexedDebugHooksPrivate *priv = (GjsMultiplexedDebugHooksPrivate *) gjs_multiplexed_debug_hooks_get_instance_private(debug_hooks);
    gjs_multiplexed_debug_hooks_lock_debug_mode(debug_hooks);
    gjs_multiplexed_debug_hooks_lock_function_calls_and_execution(debug_hooks);
    return insert_hook_callback(priv->call_and_execute_hooks,
                                priv->call_and_execute_connections,
                                G_CALLBACK(callback),
                                user_data,
                                gjs_multiplexed_debug_hooks_remove_connection_to_function_calls_and_execution,
                                debug_hooks);
}

static void
gjs_debug_hooks_interface_init (GjsDebugHooksInterface *interface)
{
    interface->add_breakpoint = gjs_multiplexed_debug_hooks_add_breakpoint;
    interface->start_singlestep = gjs_multiplexed_debug_hooks_add_singlestep;
    interface->connect_to_script_load = gjs_multiplexed_debug_hooks_connect_to_script_load;
    interface->connect_to_function_calls_and_execution = gjs_multiplexed_debug_hooks_connect_to_function_calls_and_execution;
}

static void
gjs_multiplexed_debug_hooks_init (GjsMultiplexedDebugHooks *hooks)
{
    GjsMultiplexedDebugHooksPrivate *priv = (GjsMultiplexedDebugHooksPrivate *) gjs_multiplexed_debug_hooks_get_instance_private(hooks);
    priv->scripts_loaded = g_hash_table_new_full(gjs_debug_script_lookup_info_hash,
                                                 gjs_debug_script_lookup_info_equal,
                                                 gjs_debug_script_lookup_info_destroy,
                                                 gjs_debug_script_destroy);
    priv->reflected_scripts = g_hash_table_new_full(g_str_hash,
                                                    g_str_equal,
                                                    g_free,
                                                    g_object_unref);

    priv->breakpoints_connections = g_hash_table_new(g_direct_hash, g_direct_equal);
    priv->new_script_connections = g_hash_table_new(g_direct_hash, g_direct_equal);
    priv->call_and_execute_connections = g_hash_table_new(g_direct_hash, g_direct_equal);
    priv->single_step_connections = g_hash_table_new(g_direct_hash, g_direct_equal);

    priv->breakpoints = g_hash_table_new(g_direct_hash, g_direct_equal);
    priv->pending_breakpoints = g_hash_table_new(g_direct_hash, g_direct_equal);
    priv->single_step_hooks = g_array_new(TRUE, TRUE, sizeof(GjsDebugUserCallback));
    priv->call_and_execute_hooks = g_array_new(TRUE, TRUE, sizeof(GjsDebugUserCallback));
    priv->new_script_hooks = g_array_new(TRUE, TRUE, sizeof(GjsDebugUserCallback));
}

static void
unref_all_hashtables (GHashTable **hashtable_array)
{
    GHashTable **hashtable_iterator = hashtable_array;

    do {
        g_assert (g_hash_table_size(*hashtable_iterator) == 0);
        g_hash_table_unref(*hashtable_iterator);
    } while (*(++hashtable_iterator));
}

static void
destroy_all_arrays (GArray **array_array)
{
    GArray **array_iterator = array_array;

    do {
        g_assert((*array_iterator)->len == 0);
        g_array_free(*array_iterator, TRUE);
    } while (*(++array_iterator));
}

static void
gjs_multiplexed_debug_hooks_finalize (GObject *object)
{
    GjsMultiplexedDebugHooks        *hooks = GJS_MULTIPLEXED_DEBUG_HOOKS(object);
    GjsMultiplexedDebugHooksPrivate *priv = (GjsMultiplexedDebugHooksPrivate *) gjs_multiplexed_debug_hooks_get_instance_private(hooks);

    /* Unref scripts_loaded here as there's no guaruntee it will be empty
     * since the garbage-collect phase might happen after we're unreffed */
    g_hash_table_unref(priv->scripts_loaded);
    g_hash_table_unref(priv->reflected_scripts);

    GHashTable *hashtables_to_unref[] = {
        priv->breakpoints_connections,
        priv->new_script_connections,
        priv->single_step_connections,
        priv->call_and_execute_connections,
        priv->breakpoints,
        priv->pending_breakpoints,
        NULL
    };

    GArray *arrays_to_destroy[] = {
        priv->new_script_hooks,
        priv->call_and_execute_hooks,
        priv->single_step_hooks,
        NULL
    };

    unref_all_hashtables(hashtables_to_unref);
    destroy_all_arrays(arrays_to_destroy);

    /* If we've still got locks on the context debug hooks then that's
     * an error and we should assert here */
    g_assert(priv->call_and_execute_hook_lock_count == 0);
    g_assert(priv->debug_mode_lock_count == 0);
    g_assert(priv->interrupt_function_lock_count == 0);
    g_assert(priv->new_script_hook_lock_count == 0);
    g_assert(priv->single_step_mode_lock_count == 0);
    
    G_OBJECT_CLASS(gjs_multiplexed_debug_hooks_parent_class)->finalize(object);
}

static void
gjs_multiplexed_debug_hooks_set_property(GObject      *object,
                                         unsigned int  prop_id,
                                         const GValue *value,
                                         GParamSpec   *pspec)
{
    GjsMultiplexedDebugHooks        *debug_hooks = GJS_MULTIPLEXED_DEBUG_HOOKS(object);
    GjsMultiplexedDebugHooksPrivate *priv = (GjsMultiplexedDebugHooksPrivate *) gjs_multiplexed_debug_hooks_get_instance_private(debug_hooks);

    switch (prop_id) {
    case PROP_CONTEXT:
        priv->context = GJS_CONTEXT(g_value_get_object(value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
gjs_multiplexed_debug_hooks_class_init(GjsMultiplexedDebugHooksClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->set_property = gjs_multiplexed_debug_hooks_set_property;
    object_class->finalize = gjs_multiplexed_debug_hooks_finalize;

    properties[PROP_0] = NULL;
    properties[PROP_CONTEXT] = g_param_spec_object("context",
                                                   "Context",
                                                   "GjsContext",
                                                   GJS_TYPE_CONTEXT,
                                                   (GParamFlags) (G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE));

    g_object_class_install_properties(object_class,
                                      PROP_N,
                                      properties);
}

GjsMultiplexedDebugHooks *
gjs_multiplexed_debug_hooks_new(GjsContext *context)
{
    GjsMultiplexedDebugHooks *hooks =
        GJS_MULTIPLEXED_DEBUG_HOOKS(g_object_new(GJS_TYPE_MULTIPLEXED_DEBUG_HOOKS,
                                                  "context", context,
                                                  NULL));
    return hooks;
}
