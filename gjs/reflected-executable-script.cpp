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
#include <gjs/jsapi-util.h>
#include <gjs/compat.h>
#include <gjs/gjs-module.h>

#include <gjs/reflected-script.h>
#include <gjs/reflected-executable-script.h>
#include "reflected-script-private.h"

static void gjs_reflected_executable_script_interface_init(GjsReflectedScriptInterface *);

struct _GjsReflectedExecutableScriptPrivate {
    /* Internal Context, used to define reflection object */

    char *script_filename;

    /* Array of strings, null-terminated */
    GArray *all_function_names;

    /* Array of GjsReflectedScriptBranchInfo */
    GArray *all_branches;

    /* Sorted array of unsigned int */
    GArray *all_expression_lines;

    /* Number of lines */
    unsigned int n_lines;

    /* A flag which indicates whether or not reflection
     * data has been gathered for this script yet. Reflect.parse
     * can be a super-expensive operation for large scripts so
     * we should perform it on-demand when we actually need to */
    gboolean reflection_performed;
};

G_DEFINE_TYPE_WITH_CODE(GjsReflectedExecutableScript,
                        gjs_reflected_executable_script,
                        G_TYPE_OBJECT,
                        G_ADD_PRIVATE(GjsReflectedExecutableScript)
                        G_IMPLEMENT_INTERFACE(GJS_TYPE_REFLECTED_SCRIPT_INTERFACE,
                                              gjs_reflected_executable_script_interface_init))

enum {
    PROP_0,
    PROP_SCRIPT_FILENAME,
    PROP_N
};

static GParamSpec *properties[PROP_N];

static void perform_reflection_if_necessary(GjsReflectedExecutableScript *script);

static const GjsReflectedScriptBranchInfo **
gjs_reflected_executable_script_branches(GjsReflectedScript *script)
{
    GjsReflectedExecutableScript *executable_script = GJS_REFLECTED_EXECUTABLE_SCRIPT(script);
    GjsReflectedExecutableScriptPrivate *priv = (GjsReflectedExecutableScriptPrivate *) gjs_reflected_executable_script_get_instance_private(executable_script);

    perform_reflection_if_necessary(executable_script);

    if (priv->all_branches)
        return (const GjsReflectedScriptBranchInfo **) priv->all_branches->data;
    else
        return NULL;
}

static const char **
gjs_reflected_executable_script_functions(GjsReflectedScript *script)
{
    GjsReflectedExecutableScript *executable_script = GJS_REFLECTED_EXECUTABLE_SCRIPT(script);
    GjsReflectedExecutableScriptPrivate *priv = (GjsReflectedExecutableScriptPrivate *) gjs_reflected_executable_script_get_instance_private(executable_script);

    perform_reflection_if_necessary(executable_script);

    if (priv->all_function_names)
        return (const char **) priv->all_function_names->data;
    else
        return NULL;
}

static const unsigned int *
gjs_reflected_executable_script_executable_lines(GjsReflectedScript *script,
                                                 unsigned int       *n)
{
    g_return_val_if_fail(n, NULL);

    GjsReflectedExecutableScript *executable_script = GJS_REFLECTED_EXECUTABLE_SCRIPT(script);
    GjsReflectedExecutableScriptPrivate *priv = (GjsReflectedExecutableScriptPrivate *) gjs_reflected_executable_script_get_instance_private(executable_script);

    perform_reflection_if_necessary(executable_script);

    if (priv->all_expression_lines) {
        *n = priv->all_expression_lines->len;
        return (const unsigned int *) priv->all_expression_lines->data;
    } else {
        *n = 0;
        return NULL;
    }
}

static JSObject *
get_object_property_as_object(JSContext  *context,
                              JSObject   *object,
                              const char *name)
{
    JSObject *propertyObject = NULL;
    jsval    propertyObjectValue;

    if (!JS_GetProperty(context, object, name, &propertyObjectValue) ||
        !JS_ValueToObject(context, propertyObjectValue, &propertyObject))
        return NULL;

    return propertyObject;
}

typedef gboolean (*ConvertAndInsertJSVal) (GArray    *array,
                                           JSContext *context,
                                           jsval     *element);

static GArray *
get_array_from_js_value(JSContext             *context,
                        jsval                 *value,
                        size_t                array_element_size,
                        GDestroyNotify        element_clear_func,
                        ConvertAndInsertJSVal inserter)
{
    JSObject *js_array = JSVAL_TO_OBJECT(*value);

    if (!JS_IsArrayObject(context, js_array)) {
        g_critical("Returned object from is not an array");
        return NULL;
    }

    GArray *return_array = g_array_sized_new(TRUE, TRUE, array_element_size, 10);
    u_int32_t script_funtions_array_len;

    if (element_clear_func)
        g_array_set_clear_func(return_array, element_clear_func);

    if (JS_GetArrayLength(context, js_array, &script_funtions_array_len)) {
        u_int32_t i = 0;
        for (; i < script_funtions_array_len; ++i) {
            jsval element;
            if (!JS_GetElement(context, js_array, i, &element)) {
                g_critical("Failed to get function names array element %i", i);
                continue;
            }

            if (!((*inserter)(return_array, context, &element))) {
                g_critical("Failed to convert array element %i", i);
                continue;
            }
        }
    }

    return return_array;
}

static GArray *
call_js_function_for_array_return(JSContext             *context,
                                  JSObject              *object,
                                  size_t                 array_element_size,
                                  GDestroyNotify         element_clear_func,
                                  ConvertAndInsertJSVal  inserter,
                                  const char            *function_name,
                                  jsval                 *ast)
{
    jsval rval;
    if (!JS_CallFunctionName(context, object, function_name, 1, ast, &rval)) {
        gjs_log_exception(context);
        g_critical("Failed to call %s", function_name);
        return NULL;
    }

    return get_array_from_js_value(context,
                                   &rval,
                                   array_element_size,
                                   element_clear_func,
                                   inserter);
}

static void
clear_string(gpointer string_location)
{
    char **string_ptr = (char **) string_location;
    g_free(*string_ptr);
}

static gboolean
convert_and_insert_utf8_string(GArray    *array,
                               JSContext *context,
                               jsval     *element)
{
    if (!JSVAL_IS_STRING(*element)) {
        g_critical("Array element is not a string");
        return FALSE;
    }

    char *utf8_string = NULL;
    if (!gjs_string_to_utf8(context, *element, &utf8_string)) {
        g_critical("Failed to convert array element to string");
        return FALSE;
    }

    char *utf8_string_copy = g_strdup(utf8_string);
    g_array_append_val(array, utf8_string_copy);

    JS_free(context, utf8_string);

    return TRUE;
}

static GArray *
get_script_functions_from_info_reflect(JSContext *context,
                                       JSObject  *info_reflect,
                                       jsval     *ast)
{
    return call_js_function_for_array_return(context,
                                             info_reflect,
                                             sizeof(char *),
                                             clear_string,
                                             convert_and_insert_utf8_string,
                                             "functionNamesForAST",
                                             ast);
}

static gboolean
convert_and_insert_unsigned_int(GArray    *array,
                                JSContext *context,
                                jsval     *element)
{
    if (!JSVAL_IS_INT(*element)) {
        g_critical("Array element is not an integer");
        return FALSE;
    }

    unsigned int element_integer = JSVAL_TO_INT(*element);
    g_array_append_val(array, element_integer);
    return TRUE;
}

static int
uint_compare(gconstpointer left,
             gconstpointer right)
{
    unsigned int *left_int = (unsigned int *) left;
    unsigned int *right_int = (unsigned int *) right;

    return *left_int - *right_int;
}

static GArray *
get_all_lines_with_executable_expressions_from_script(JSContext *context,
                                                      JSObject  *info_reflect,
                                                      jsval     *ast)
{
    GArray *all_expressions = call_js_function_for_array_return(context,
                                                                info_reflect,
                                                                sizeof(unsigned int),
                                                                NULL,
                                                                convert_and_insert_unsigned_int,
                                                                "executableExpressionLinesForAST",
                                                                ast);

    /* Sort, just to be sure */
    g_array_sort(all_expressions, uint_compare);
    return all_expressions;
}

static void
gjs_reflected_script_branch_info_clear(gpointer branch_info_location)
{
    GjsReflectedScriptBranchInfo **info_ptr = (GjsReflectedScriptBranchInfo **) branch_info_location;
    gjs_reflected_script_branch_info_destroy(*info_ptr);
}

static gboolean
convert_and_insert_branch_info(GArray    *array,
                               JSContext *context,
                               jsval     *element)
{
    if (!JSVAL_IS_OBJECT(*element)) {
        g_critical("Array element is not an object");
        return FALSE;
    }

    JSObject *object = JSVAL_TO_OBJECT(*element);

    if (!object) {
        g_critical("Converting element to object failed");
        return FALSE;
    }

    jsval   branch_point_value;
    int32_t branch_point;

    if (!JS_GetProperty(context, object, "point", &branch_point_value) ||
        !JSVAL_IS_INT(branch_point_value)) {
        g_critical("Failed to get point property from element");
        return FALSE;
    }

    branch_point = JSVAL_TO_INT(branch_point_value);

    jsval  branch_alternatives_value;
    GArray *branch_alternatives_array;

    if (!JS_GetProperty(context, object, "alternates", &branch_alternatives_value) ||
        !JSVAL_IS_OBJECT(branch_alternatives_value)) {
        g_critical("Failed to get alternates property from element");
        return FALSE;
    }

    branch_alternatives_array =
        get_array_from_js_value(context,
                                &branch_alternatives_value,
                                sizeof(unsigned int),
                                NULL,
                                convert_and_insert_unsigned_int);

    GjsReflectedScriptBranchInfo *info = gjs_reflected_script_branch_info_new(branch_point,
                                                                              branch_alternatives_array);

    g_array_append_val(array, info);

    return TRUE;
}

static GArray *
get_script_branches_from_info_reflect(JSContext *context,
                                      JSObject  *info_reflect,
                                      jsval     *ast)
{
    return call_js_function_for_array_return(context,
                                             info_reflect,
                                             sizeof(GjsReflectedScriptBranchInfo *),
                                             gjs_reflected_script_branch_info_clear,
                                             convert_and_insert_branch_info,
                                             "branchesForAST",
                                             ast);
}

static GjsContext *
push_new_context()
{
    gjs_context_make_current(NULL);
    return gjs_context_new();
}

static void
restore_old_context_and_destroy_current(GjsContext *restore, GjsContext *destroy)
{
    gjs_context_make_current(NULL);
    g_object_unref(destroy);
    gjs_context_make_current(restore);
}

static unsigned int
count_lines_in_script(const char *data)
{
    int lines = 1;
    for (; *data; ++data)
        if (*data == '\n')
            ++lines;
    return lines;
}

static void
perform_reflection_within_compartment(GjsContext                   *internal_context,
                                      GjsReflectedExecutableScript *script)
{
    GjsReflectedExecutableScriptPrivate *priv = (GjsReflectedExecutableScriptPrivate *) gjs_reflected_executable_script_get_instance_private(script);
    JSContext *js_context = (JSContext *) gjs_context_get_native_context(internal_context);
    JSObject *global = JS_GetGlobalObject(js_context);

    JSAutoCompartment ac(js_context, global);

    JSObject *info_reflect = get_object_property_as_object(js_context, global, "InfoReflect");
    JSObject *reflectOptions = get_object_property_as_object(js_context, global, "ReflectOptions");

    if (!info_reflect || !reflectOptions) {
        g_critical("Failed to get 'InfoReflect' or 'ReflectOptions' "
                  "properties from toplevel script");
        return;
    }

    /* Call a wrapper to Reflect.parse to get the AST as a jsval.
     * This wrapper function handles the corner-case where we
     * have shebangs in scripts (Reflect.parse can't handle that) */
    char         *script_contents;
    unsigned int script_contents_len;
    if (!g_file_get_contents(priv->script_filename,
                             &script_contents,
                             &script_contents_len,
                             NULL)) {
        g_critical("Failed to get script contents for %s", priv->script_filename);
        return;
    }

    unsigned int script_n_lines = count_lines_in_script(script_contents);

    JSString *str = JS_NewStringCopyZ(js_context, script_contents);
    jsval parseArgv[] = {
        STRING_TO_JSVAL(str),
        OBJECT_TO_JSVAL(reflectOptions)
    };
    jsval ast_value;

    g_free(script_contents);

    if (!JS_CallFunctionName(js_context, info_reflect, "removeShebangsAndParse", 2, parseArgv, &ast_value)) {
        g_critical("Failed to call Reflect.parse wrapper");
        return;
    }

    priv->all_function_names = get_script_functions_from_info_reflect(js_context,
                                                                      info_reflect,
                                                                      &ast_value);
    priv->all_branches = get_script_branches_from_info_reflect(js_context,
                                                               info_reflect,
                                                               &ast_value);
    priv->all_expression_lines = get_all_lines_with_executable_expressions_from_script(js_context,
                                                                                       info_reflect,
                                                                                       &ast_value);
    priv->n_lines = script_n_lines;
}


static void
perform_reflection_if_necessary(GjsReflectedExecutableScript *script)
{
    GjsReflectedExecutableScriptPrivate *priv = (GjsReflectedExecutableScriptPrivate *) gjs_reflected_executable_script_get_instance_private(script);

    if (G_LIKELY(priv->reflection_performed))
        return;

    /* It appears that we only want one current context at a time, so we need
     * to explicitly make any current context non-current so that we can briefly
     * use ours. Back up the current context and restore it later */
    GjsContext *current_context = gjs_context_get_current();
    GjsContext *internal_context = push_new_context();

    const char bootstrap_script[] =
            "const InfoReflect = imports.info_reflect;\n"
            "const ReflectOptions = {\n"
            "    loc: true\n"
            "};\n";

    if (!gjs_context_eval(internal_context,
                          bootstrap_script,
                          strlen(bootstrap_script),
                          NULL,
                          NULL,
                          NULL)) {
        restore_old_context_and_destroy_current(current_context, internal_context);
        g_critical("Failed to evaluate bootstrap script.");
        return;
    }

    perform_reflection_within_compartment(internal_context, script);
    restore_old_context_and_destroy_current(current_context, internal_context);

    priv->reflection_performed = TRUE;
}

static unsigned int
gjs_reflected_executable_script_n_lines(GjsReflectedScript *script)
{
    GjsReflectedExecutableScript *executable_script = GJS_REFLECTED_EXECUTABLE_SCRIPT(script);
    GjsReflectedExecutableScriptPrivate *priv = (GjsReflectedExecutableScriptPrivate *) gjs_reflected_executable_script_get_instance_private(executable_script);

    perform_reflection_if_necessary(executable_script);

    return priv->n_lines;
}

static void
gjs_reflected_executable_script_interface_init(GjsReflectedScriptInterface *interface)
{
    interface->branches = gjs_reflected_executable_script_branches;
    interface->functions = gjs_reflected_executable_script_functions;
    interface->executable_lines = gjs_reflected_executable_script_executable_lines;
    interface->n_lines = gjs_reflected_executable_script_n_lines;
}

static void
gjs_reflected_executable_script_init(GjsReflectedExecutableScript *script)
{
    GjsReflectedExecutableScriptPrivate *priv = (GjsReflectedExecutableScriptPrivate *) gjs_reflected_executable_script_get_instance_private(script);

    priv->all_branches = NULL;
    priv->all_function_names = NULL;
    priv->all_expression_lines = NULL;
}

static void
unref_array_if_nonnull(GArray *array)
{
    if (array)
        g_array_unref(array);
}

static void
gjs_reflected_executable_script_finalize (GObject *object)
{
    GjsReflectedExecutableScript *hooks = GJS_REFLECTED_EXECUTABLE_SCRIPT(object);
    GjsReflectedExecutableScriptPrivate *priv = (GjsReflectedExecutableScriptPrivate *) gjs_reflected_executable_script_get_instance_private(hooks);

    unref_array_if_nonnull(priv->all_branches);
    unref_array_if_nonnull(priv->all_function_names);
    unref_array_if_nonnull(priv->all_expression_lines);

    g_free(priv->script_filename);

    G_OBJECT_CLASS(gjs_reflected_executable_script_parent_class)->finalize(object);
}

static void
gjs_reflected_executable_script_set_property(GObject      *object,
                                             unsigned int  prop_id,
                                             const GValue *value,
                                             GParamSpec   *pspec)
{
    GjsReflectedExecutableScript *script = GJS_REFLECTED_EXECUTABLE_SCRIPT(object);
    GjsReflectedExecutableScriptPrivate *priv = (GjsReflectedExecutableScriptPrivate *) gjs_reflected_executable_script_get_instance_private(script);

    switch (prop_id) {
    case PROP_SCRIPT_FILENAME:
        priv->script_filename = g_value_dup_string(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
gjs_reflected_executable_script_class_init(GjsReflectedExecutableScriptClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->set_property = gjs_reflected_executable_script_set_property;
    object_class->finalize = gjs_reflected_executable_script_finalize;

    properties[PROP_0] = NULL;
    properties[PROP_SCRIPT_FILENAME] = g_param_spec_string("filename",
                                                           "Script Filename",
                                                           "Valid path to script",
                                                           NULL,
                                                           (GParamFlags) (G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE));
    g_object_class_install_properties(object_class,
                                      PROP_N,
                                      properties);
}

GjsReflectedExecutableScript *
gjs_reflected_executable_script_new(const char *filename)
{
    GjsReflectedExecutableScript *script =
        GJS_REFLECTED_EXECUTABLE_SCRIPT(g_object_new(GJS_TYPE_REFLECTED_EXECUTABLE_SCRIPT,
                                                     "filename", filename,
                                                     NULL));
    return script;
}
