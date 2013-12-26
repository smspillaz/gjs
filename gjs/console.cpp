/* -*- mode: C; c-basic-offset: 4; indent-tabs-mode: nil; -*- */
/*
 * Copyright (c) 2008  litl, LLC
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
 */

#include <config.h>
#include <string.h>
#include <stdlib.h>
#include <locale.h>

#include <gjs/gjs.h>
#include <gjs/coverage.h>

static char **include_path = NULL;
static char **coverage_paths = NULL;
static char *coverage_output_path = NULL;
static char *command = NULL;

static GOptionEntry entries[] = {
    { "command", 'c', 0, G_OPTION_ARG_STRING, &command, "Program passed in as a string", "COMMAND" },
    { "coverage-path", 'C', 0, G_OPTION_ARG_STRING_ARRAY, &coverage_paths, "Add the directory DIR to the list of directories to generate coverage info for", "DIR" },
    { "coverage-output", 0, 0, G_OPTION_ARG_STRING, &coverage_output_path, "Write coverage output to a directory DIR. This option is mandatory when using --coverage-path", "DIR", },
    { "include-path", 'I', 0, G_OPTION_ARG_STRING_ARRAY, &include_path, "Add the directory DIR to the list of directories to search for js files.", "DIR" },
    { NULL }
};

G_GNUC_NORETURN
static void
print_help (GOptionContext *context,
            gboolean        main_help)
{
  gchar *help;

  help = g_option_context_get_help (context, main_help, NULL);
  g_print ("%s", help);
  g_free (help);

  exit (0);
}

static GValue *
init_array_parameter(GArray      *array,
                     guint       index,
                     const gchar *name,
                     GType       type)
{
    if (index >= array->len)
        g_array_set_size(array, index + 1);

    GParameter *param = &(g_array_index(array, GParameter, index));
    param->name = name;
    g_value_init(&param->value, type);
    return &param->value;
}

static void
clear_array_parameter_value(gpointer value)
{
    GParameter *parameter = (GParameter *) value;
    g_value_unset(&parameter->value);
}

int
main(int argc, char **argv)
{
    GOptionContext *context;
    GError *error = NULL;
    GjsContext *js_context;
    GjsCoverage *coverage = NULL;
    char *script;
    const char *filename;
    const char *program_name;
    gsize len;
    int code;

    context = g_option_context_new(NULL);

    /* pass unknown through to the JS script */
    g_option_context_set_ignore_unknown_options(context, TRUE);
    g_option_context_set_help_enabled(context, FALSE);

    g_option_context_add_main_entries(context, entries, NULL);
    if (!g_option_context_parse(context, &argc, &argv, &error))
        g_error("option parsing failed: %s", error->message);

    if (argc >= 2) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)
            print_help(context, TRUE);
        else if (strcmp(argv[1], "--help-all") == 0)
            print_help(context, FALSE);
    }

    g_option_context_free (context);

    setlocale(LC_ALL, "");

    if (command != NULL) {
        script = command;
        len = strlen(script);
        filename = "<command line>";
        program_name = argv[0];
    } else if (argc <= 1) {
        script = g_strdup("const Console = imports.console; Console.interact();");
        len = strlen(script);
        filename = "<stdin>";
        program_name = argv[0];
    } else /*if (argc >= 2)*/ {
        error = NULL;
        if (!g_file_get_contents(argv[1], &script, &len, &error)) {
            g_printerr("%s\n", error->message);
            exit(1);
        }
        filename = argv[1];
        program_name = argv[1];
        argc--;
        argv++;
    }

    js_context = (GjsContext*) g_object_new(GJS_TYPE_CONTEXT,
                                            "search-path", include_path,
                                            "program-name", program_name,
                                            NULL);

    if (coverage_paths) {
        if (!coverage_output_path)
            g_error("--coverage-output-path is required when taking coverage statistics");

        coverage = gjs_coverage_new(gjs_context_get_debug_hooks(js_context),
                                    (const gchar **) coverage_paths);
    }

    /* prepare command line arguments */
    if (!gjs_context_define_string_array(js_context, "ARGV",
                                         argc - 1, (const char**)argv + 1,
                                         &error)) {
        g_printerr("Failed to defined ARGV: %s", error->message);
        g_object_unref(js_context);
        exit(1);
    }

    /* evaluate the script */
    error = NULL;
    if (!gjs_context_eval(js_context, script, len,
                          filename, &code, &error)) {
        g_free(script);
        g_printerr("%s\n", error->message);
        g_object_unref(js_context);
        exit(1);
    }

    if (coverage) {
        gjs_coverage_write_statistics(coverage,
                                      coverage_output_path);

        g_clear_object(&coverage);
    }

    g_object_unref(js_context);

    g_free(script);
    exit(code);
}
