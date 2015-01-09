/*
 * Copyright © 2014 Endless Mobile, Inc.
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

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include <sys/types.h>
#include <fcntl.h>
#include <ftw.h>

#include <glib.h>
#include <gio/gio.h>
#include <gjs/gjs.h>
#include <gjs/coverage.h>
#include <gjs/debugger.h>
#include <gjs/gjs-module.h>
#include <util/error.h>

#include <jsapi.h>

/* XXX: Once debugger is stabilized, these helper functions
 * should be merged with gjs-test-coverage.cpp */
static void
write_to_file(int        handle,
              const char *contents)
{
    if (write(handle,
              (gconstpointer) contents,
              sizeof(char) * strlen(contents)) == -1)
        g_error("Failed to write %s to file", contents);
}

static void
write_to_file_at_beginning(int        handle,
                           const char *content)
{
    if (ftruncate(handle, 0) == -1)
        g_print("Error deleting contents of test temporary file: %s\n", strerror(errno));
    lseek(handle, 0, SEEK_SET);
    write_to_file(handle, content);
}

static int
unlink_if_node_is_a_file(const char *path, const struct stat *sb, int typeflag)
{
    if (typeflag == FTW_F)
        unlink(path);
    return 0;
}

static int
rmdir_if_node_is_a_dir(const char *path, const struct stat *sb, int typeflag)
{
    if (typeflag == FTW_D)
        rmdir(path);
    return 0;
}

static void
recursive_delete_dir_at_path(const char *path)
{
    /* We have to recurse twice - once to delete files, and once
     * to delete directories (because ftw uses preorder traversal) */
    ftw(path, unlink_if_node_is_a_file, 100);
    ftw(path, rmdir_if_node_is_a_dir, 100);
}

static void
run_script_in_debugger_compartment(GjsContext       *context,
                                   JS::HandleObject  debugger_compartment,
                                   const char       *debugger_script)
{
    JSContext *js_context = (JSContext *) gjs_context_get_native_context(context);
    JSAutoRequest ar(js_context);
    JSAutoCompartment ac(js_context, debugger_compartment);

    GError *error = NULL;
    jsval value;
    if (!gjs_eval_with_scope(js_context,
                             debugger_compartment,
                             debugger_script,
                             strlen(debugger_script),
                             "<prelude>",
                             &value)) {
        gjs_log_exception(js_context);
        g_set_error(&error, GJS_ERROR, GJS_ERROR_FAILED, "Failed to eval debugger script");
    }

    g_assert_no_error(error);
}

static void
run_script_file_in_main_compartment(GjsContext       *context,
                                    const char       *filename)
{
    JSContext *js_context = (JSContext *) gjs_context_get_native_context(context);
    JSAutoRequest ar(js_context);

    GError *error = NULL;
    gjs_context_eval_file(context,
                          filename,
                          NULL,
                          &error);
    g_assert_no_error(error);
}

typedef struct _GjsDebuggerFixture {
    GjsContext           *context;
    JS::Heap<JSObject *>  debugger_compartment;
    char                 *temporary_js_script_directory_name;
    char                 *temporary_js_script_filename;
    int                   temporary_js_script_open_handle;
} GjsDebuggerFixture;

static void
gjs_debugger_fixture_set_up(gpointer      fixture_data,
                            gconstpointer user_data)
{
    GjsDebuggerFixture *fixture = (GjsDebuggerFixture *) fixture_data;
    const char         *js_script = "function f () { return 1; }\n";

    fixture->temporary_js_script_directory_name = g_strdup("/tmp/gjs_debugger_tmp.XXXXXX");
    fixture->temporary_js_script_directory_name =
        mkdtemp (fixture->temporary_js_script_directory_name);

    if (!fixture->temporary_js_script_directory_name)
        g_error ("Failed to create temporary directory for test files: %s\n", strerror (errno));

    fixture->temporary_js_script_filename = g_strconcat(fixture->temporary_js_script_directory_name,
                                                        "/",
                                                        "gjs_debugger_script.XXXXXX.js",
                                                        NULL);
    fixture->temporary_js_script_open_handle =
        mkstemps(fixture->temporary_js_script_filename, 3);

    /* Allocate a strv that we can pass over to gjs_coverage_new */
    const char *coverage_paths[] = {
        fixture->temporary_js_script_filename,
        NULL
    };

    const char *search_paths[] = {
        fixture->temporary_js_script_directory_name,
        NULL
    };

    fixture->context = gjs_context_new_with_search_path((char **) search_paths);
    fixture->debugger_compartment = (JSObject *) gjs_get_debugger_compartment(fixture->context);

    write_to_file(fixture->temporary_js_script_open_handle, js_script);

    char *prologue = g_strdup_printf("const JSUnit = imports.jsUnit;\n"
                                     "let __script_name = '%s'\n"
                                     "function assertArrayContains(array, contains) {\n"
                                     "    if (array.indexOf(contains) === -1)\n"
                                     "        JSUnit.fail('Array ' + array + ' does not contain ' + contains);\n"
                                     "}\n"
                                     "function assertArrayDoesNotContain(array, contains) {\n"
                                     "    if (array.indexOf(contains) !== -1)\n"
                                     "        JSUnit.fail('Array ' + array + ' contains ' + contains);\n"
                                     "}\n",
                                     fixture->temporary_js_script_filename);

    run_script_in_debugger_compartment(fixture->context,
                                       fixture->debugger_compartment,
                                       prologue);

    g_free (prologue);
}

static void
gjs_debugger_fixture_tear_down(gpointer      fixture_data,
                               gconstpointer user_data)
{
    GjsDebuggerFixture *fixture = (GjsDebuggerFixture *) fixture_data;
    unlink(fixture->temporary_js_script_filename);
    g_free(fixture->temporary_js_script_filename);
    close(fixture->temporary_js_script_open_handle);
    recursive_delete_dir_at_path(fixture->temporary_js_script_directory_name);
    g_free(fixture->temporary_js_script_directory_name);

    fixture->debugger_compartment = NULL;
    g_object_unref(fixture->context);
    gjs_clear_thread_runtime();
}

static void
test_debugger_eval_script_for_success(gpointer fixture_data,
                                      gconstpointer user_data)
{
    GjsDebuggerFixture *fixture = (GjsDebuggerFixture *) fixture_data;

    /* Just evaluate a script (the debugger is enabled) and check that
     * it succeeds */
    run_script_file_in_main_compartment(fixture->context,
                                        fixture->temporary_js_script_filename);
}

typedef struct _GjsDebuggerSingleHandlerFixture {
    GjsDebuggerFixture base_fixture;
} GjsDebuggerSingleHandlerFixture;

static void
gjs_debugger_single_handler_fixture_set_up(gpointer      fixture_data,
                                           gconstpointer user_data)
{
    GjsDebuggerFixture *fixture = (GjsDebuggerFixture *) fixture_data;
    gjs_debugger_fixture_set_up(fixture, user_data);

    run_script_in_debugger_compartment(fixture->context,
                                       fixture->debugger_compartment,
                                       "let __events = [];\n"
                                       "let __controller = new DebuggerCommandController(function(info) {\n"
                                       "                       if (info.url === __script_name)\n"
                                       "                           __events.push(info.type);\n"
                                       "                       return true;\n"
                                       "                   });\n");
}

static void
gjs_debugger_single_handler_fixture_tear_down(gpointer      fixture_data,
                                              gconstpointer user_data)
{
    gjs_debugger_fixture_tear_down(fixture_data, user_data);
}

static void
run_debugger_command_list(GjsContext       *context,
                          JS::HandleObject  debugger_compartment,
                          const char       *debugger_command_array)
{
    char *subscript = g_strdup_printf("__controller.handleInput(%s);\n",
                                      debugger_command_array);
    run_script_in_debugger_compartment(context,
                                       debugger_compartment,
                                       subscript);
    g_free(subscript);
}

static void
assert_debugger_got_event(GjsContext       *context,
                          JS::HandleObject  debugger_compartment,
                          const char       *event_type)
{
    char *assertion = g_strdup_printf("assertArrayContains(__events, DebuggerEventTypes.%s);\n",
                                      event_type);
    run_script_in_debugger_compartment(context,
                                       debugger_compartment,
                                       assertion);
    g_free(assertion);
}

static void
assert_debugger_did_not_get_event(GjsContext       *context,
                                  JS::HandleObject  debugger_compartment,
                                  const char       *event_type)
{
    char *assertion = g_strdup_printf("assertArrayDoesNotContain(__events, DebuggerEventTypes.%s);\n",
                                      event_type);
    run_script_in_debugger_compartment(context,
                                       debugger_compartment,
                                       assertion);
    g_free(assertion);
}

static void
test_debugger_got_enter_frame_notify(gpointer fixture_data,
                                     gconstpointer user_data)
{
    GjsDebuggerFixture *fixture = (GjsDebuggerFixture *) fixture_data;

    run_debugger_command_list(fixture->context,
                              fixture->debugger_compartment,
                              "['step', 'frame']");
    run_script_file_in_main_compartment(fixture->context,
                                        fixture->temporary_js_script_filename);
    assert_debugger_got_event(fixture->context,
                              fixture->debugger_compartment,
                              "FRAME_ENTERED");
}

static void
test_debugger_disable_frame_entry_notification(gpointer fixture_data,
                                               gconstpointer user_data)
{
    GjsDebuggerFixture *fixture = (GjsDebuggerFixture *) fixture_data;

    run_debugger_command_list(fixture->context,
                              fixture->debugger_compartment,
                              "['disable', 'step', 'frame']");
    run_script_file_in_main_compartment(fixture->context,
                                        fixture->temporary_js_script_filename);
    assert_debugger_did_not_get_event(fixture->context,
                                      fixture->debugger_compartment,
                                      "FRAME_ENTERED");
}

/* These tests require the debugger to be bootstrapped and then
 * we feed some custom commands to it every time it stops for
 * certain events */
typedef struct _GjsDebuggerInteractiveFixture {
    GjsDebuggerFixture base_fixture;
} GjsDebuggerInteractiveFixture;

static void
append_command_object_to_command_list(GjsContext       *context,
                                      JS::HandleObject debugger_compartment,
                                      const char       *event_to_respond_to,
                                      const char       *script_name_to_expect,
                                      unsigned int     script_line_to_expect,
                                      const char       *next_command_to_give_debugger)
{
    char *script = g_strdup_printf("__cmds.push({ event: DebuggerEventTypes.%s, expectName: '%s', expectLine: %d, cmd: '%s' });\n",
                                   event_to_respond_to,
                                   script_name_to_expect,
                                   script_line_to_expect,
                                   next_command_to_give_debugger);
    run_script_in_debugger_compartment(context,
                                       debugger_compartment,
                                       script);

    g_free(script);
}

static void
gjs_debugger_interactive_fixture_set_up(gpointer      fixture_data,
                                        gconstpointer user_data)
{
    GjsDebuggerFixture *fixture = (GjsDebuggerFixture *) fixture_data;
    gjs_debugger_fixture_set_up(fixture, user_data);

    run_script_in_debugger_compartment(fixture->context,
                                       fixture->debugger_compartment,
                                       "let __cmds = [];\n"
                                       "let __controller = new DebuggerCommandController(function(info) {\n"
                                       "    if (__cmds.length === 0)\n"
                                       "        return true;\n"
                                       "    if (__cmds[0].event === info.type &&\n"
                                       "        __cmds[0].expectName === info.url &&\n"
                                       "        __cmds[0].expectLine === info.line) {\n"
                                       "        let command = __cmds.shift();\n"
                                       "        if (__controller.handleInput(command.cmd.split(' ')) == DebuggerCommandState.RETURN_CONTROL)\n"
                                       "            return true;\n"
                                       "        else\n"
                                       "            return false;\n"
                                       "    }\n"
                                       "    return true;\n"
                                       "});\n");
}

static void
gjs_debugger_interactive_fixture_tear_down(gpointer      fixture_data,
                                           gconstpointer user_data)
{
    GjsDebuggerFixture *fixture = (GjsDebuggerFixture *) fixture_data;

    run_script_in_debugger_compartment(fixture->context,
                                       fixture->debugger_compartment,
                                       "JSUnit.assertEquals(__cmds.length, 0);\n");

    gjs_debugger_fixture_tear_down(fixture_data, user_data);
}


static void
test_debugger_got_single_step(gpointer fixture_data,
                              gconstpointer user_data)
{
    GjsDebuggerFixture *fixture = (GjsDebuggerFixture *) fixture_data;

    append_command_object_to_command_list(fixture->context,
                                          fixture->debugger_compartment,
                                          "SINGLE_STEP",
                                          fixture->temporary_js_script_filename,
                                          1,
                                          "cont");

    run_debugger_command_list(fixture->context,
                              fixture->debugger_compartment,
                              "['step']");
    run_script_file_in_main_compartment(fixture->context,
                                        fixture->temporary_js_script_filename);
}

static void
test_debugger_got_many_single_steps(gpointer fixture_data,
                                    gconstpointer user_data)
{
    GjsDebuggerFixture *fixture = (GjsDebuggerFixture *) fixture_data;

    append_command_object_to_command_list(fixture->context,
                                          fixture->debugger_compartment,
                                          "SINGLE_STEP",
                                          fixture->temporary_js_script_filename,
                                          1,
                                          "step");
    append_command_object_to_command_list(fixture->context,
                                          fixture->debugger_compartment,
                                          "SINGLE_STEP",
                                          fixture->temporary_js_script_filename,
                                          1,
                                          "cont");

    run_debugger_command_list(fixture->context,
                              fixture->debugger_compartment,
                              "['step']");
    run_script_file_in_main_compartment(fixture->context,
                                        fixture->temporary_js_script_filename);
}

typedef struct _FixturedTest {
    gsize            fixture_size;
    GTestFixtureFunc set_up;
    GTestFixtureFunc tear_down;
} FixturedTest;

static void
add_test_for_fixture(const char      *name,
                     FixturedTest    *fixture,
                     GTestFixtureFunc test_func,
                     gconstpointer    user_data)
{
    g_test_add_vtable(name,
                      fixture->fixture_size,
                      user_data,
                      fixture->set_up,
                      test_func,
                      fixture->tear_down);
}

void gjs_test_add_tests_for_debugger()
{
    FixturedTest debugger_fixture = {
        sizeof(GjsDebuggerFixture),
        gjs_debugger_fixture_set_up,
        gjs_debugger_fixture_tear_down
    };

    add_test_for_fixture("/gjs/debugger/evaluate_script_for_success",
                         &debugger_fixture,
                         test_debugger_eval_script_for_success,
                         NULL);

    FixturedTest debugger_single_command_fixture = {
        sizeof(GjsDebuggerSingleHandlerFixture),
        gjs_debugger_single_handler_fixture_set_up,
        gjs_debugger_single_handler_fixture_tear_down
    };

    add_test_for_fixture("/gjs/debugger/got_enter_frame_notify",
                         &debugger_single_command_fixture,
                         test_debugger_got_enter_frame_notify,
                         NULL);
    add_test_for_fixture("/gjs/debugger/disable_frame_entry_notification",
                         &debugger_single_command_fixture,
                         test_debugger_disable_frame_entry_notification,
                         NULL);

    FixturedTest debugger_interactive_command_fixture = {
        sizeof(GjsDebuggerInteractiveFixture),
        gjs_debugger_interactive_fixture_set_up,
        gjs_debugger_interactive_fixture_tear_down
    };

    add_test_for_fixture("/gjs/debugger/got_single_step_notify",
                         &debugger_interactive_command_fixture,
                         test_debugger_got_single_step,
                         NULL);
    add_test_for_fixture("/gjs/debugger/got_many_single_steps_notify",
                         &debugger_interactive_command_fixture,
                         test_debugger_got_single_step,
                         NULL);
}
