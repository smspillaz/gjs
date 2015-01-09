/*
 * Copyright (c) 2015 Endless Mobile, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authored By: Sam Spilsbury <sam@endlessm.com>
 */

#include <sys/stat.h>
#include <gio/gio.h>

#include "gjs-module.h"
#include "importer.h"
#include "debugger.h"

static JSClass debugger_global_class = {
    "GjsDebuggerCompartment",
    JSCLASS_GLOBAL_FLAGS_WITH_SLOTS(GJS_GLOBAL_SLOT_LAST),
    JS_PropertyStub,
    JS_DeletePropertyStub,
    JS_PropertyStub,
    JS_StrictPropertyStub,
    JS_EnumerateStub,
    JS_ResolveStub,
    JS_ConvertStub,
    NULL,
    NULL /* checkAccess */,
    NULL /* call */,
    NULL /* hasInstance */,
    NULL /* construct */,
    NULL,
    { NULL }
};

static JSBool
debugger_warning(JSContext *context,
                 unsigned   argc,
                 jsval     *vp)
{
    JS::CallArgs  args(JS::CallArgsFromVp(argc, vp));
    JSAutoRequest ar(context);
    char          *output = NULL;

    if (argc != 1) {
        gjs_throw(context, "Must pass a single argument to warning()");
        return JS_FALSE;
    }

    if (!gjs_parse_call_args(context, "output", "s", args, "contents", &output)) {
        gjs_throw(context, "Failed to parse call args");
        return JS_FALSE;
    }

    g_warning("JS DEBUGGER: %s", output);
    g_free(output);

    args.rval().setUndefined();

    return JS_TRUE;
}

static JSFunctionSpec debugger_funcs[] = {
    { "warning", JSOP_WRAPPER (debugger_warning), 1, GJS_MODULE_PROP_FLAGS },
    { NULL },
};

static void
debugger_multiplexer_tracer(JSTracer *trc, void *data)
{
    JSObject *object = (JSObject *)data;
    JS_CallObjectTracer(trc, &object, "debugger_multiplexer");
} 

/* XXX: Once debugger is stabilized, this should be merged with
 * similar bootstrap code in coverage.cpp */
gpointer
gjs_get_debugger_compartment(GjsContext *gjs_context)
{
    static const char *debugger_multiplexer_script = "resource:///org/gnome/gjs/modules/debuggerMultiplexer.js";
    GError    *error = NULL;
    JSContext *context = (JSContext *) gjs_context_get_native_context(gjs_context);
    JSAutoRequest ar(context);

    JS::CompartmentOptions options;
    options.setVersion(JSVERSION_LATEST);

    JS::RootedObject debuggee(context, JS_GetGlobalObject(context));
    JS::RootedObject debugger_compartment(context,
                                          JS_NewGlobalObject(context,
                                                             &debugger_global_class,
                                                             NULL,
                                                             options));

    /* Enter compartment of the debugger and initialize it with the debuggee */
    JSAutoCompartment compartment(context, debugger_compartment);
    JS::RootedObject debuggeeWrapper(context, debuggee);
    if (!JS_WrapObject(context, debuggeeWrapper.address())) {
        gjs_throw(context, "Failed to wrap debuggee");
        return NULL;
    }

    JS::RootedValue debuggeeWrapperValue(context, JS::ObjectValue(*debuggeeWrapper));
    if (!JS_SetProperty(context, debugger_compartment, "debuggee", debuggeeWrapperValue.address())) {
        gjs_throw(context, "Failed to set debuggee property");
        return NULL;
    }

    if (!JS_InitStandardClasses(context, debugger_compartment)) {
        gjs_throw(context, "Failed to init standard classes");
        return NULL;
    }

    JS::RootedObject wrapped_importer(JS_GetRuntime(context),
                                      gjs_wrap_root_importer_in_compartment(context,
                                                                            debugger_compartment));;
    
    if (!wrapped_importer) {
        gjs_throw(context, "Failed to wrap root importer in debugger compartment");
        return NULL;
    }

    /* Now copy the global root importer (which we just created,
     * if it didn't exist) to our global object
     */
    if (!gjs_define_root_importer_object(context, debugger_compartment, wrapped_importer)) {
        gjs_throw(context, "Failed to set 'imports' on debugger compartment");
        return NULL;
    }

    if (!JS_DefineDebuggerObject(context, debugger_compartment)) {
        gjs_throw(context, "Failed to init Debugger");
        return NULL;
    }

    if (!JS_DefineFunctions(context, debugger_compartment, &debugger_funcs[0]))
        g_error("Failed to init debugger helper functions");

    if (!gjs_eval_file_with_scope(context,
                                  debugger_multiplexer_script,
                                  debugger_compartment,
                                  &error)) {
        gjs_log_exception(context);
        g_error("Failed to evaluate debugger script %s", error->message);
    }
    
    return (gpointer) debugger_compartment;
}
