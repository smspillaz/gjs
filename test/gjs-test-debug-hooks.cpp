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

#include <errno.h>
#include <unistd.h>
#include <sys/types.h>

#include <jsapi.h>
#include <jsdbgapi.h>
#include <gjs/gjs.h>
#include <gjs/jsapi-util.h>
#include <gjs/debug-hooks.h>
#include <gjs/multiplexed-debug-hooks.h>
#include <gjs/debug-connection.h>
#include <gjs/reflected-script.h>
#include <gjs/reflected-executable-script.h>

typedef struct _GjsMultiplexedDebugHooksFixture {
    GjsContext                *context;
    GjsMultiplexedDebugHooks  *debug_hooks;
    char                      *temporary_js_script_filename;
    int                       temporary_js_script_open_handle;
} GjsMultiplexedDebugHooksFixture;

static void
gjs_multiplexed_debug_hooks_fixture_set_up (gpointer      fixture_data,
                                            gconstpointer user_data)
{
    GjsMultiplexedDebugHooksFixture *fixture = (GjsMultiplexedDebugHooksFixture *) fixture_data;
    const char                       *js_script = "function f () { return 1; }\n";
    fixture->context = gjs_context_new();
    fixture->debug_hooks = gjs_multiplexed_debug_hooks_new(fixture->context);
    fixture->temporary_js_script_open_handle = g_file_open_tmp("mock-js-XXXXXXX.js",
                                                               &fixture->temporary_js_script_filename,
                                                               NULL);

    if (write(fixture->temporary_js_script_open_handle, js_script, strlen(js_script) * sizeof (char)) == -1)
        g_print("Error writing to temporary file: %s", strerror(errno));
}

static void
gjs_multiplexed_debug_hooks_fixture_tear_down(gpointer      fixture_data,
                                              gconstpointer user_data)
{
    GjsMultiplexedDebugHooksFixture *fixture = (GjsMultiplexedDebugHooksFixture *) fixture_data;
    unlink(fixture->temporary_js_script_filename);
    g_free(fixture->temporary_js_script_filename);
    close(fixture->temporary_js_script_open_handle);

    g_object_unref(fixture->debug_hooks);
    g_object_unref(fixture->context);
}

typedef GjsDebugConnection *
(*GjsMultiplexedDebugHooksConnectionFunction)(GjsMultiplexedDebugHooks *,
                                               const char              *,
                                               unsigned int             ,
                                               GCallback                ,
                                               gpointer                 ,
                                               GError                  *);

static void
dummy_callback_for_connector(GjsDebugHooks *hooks,
                             GjsContext    *context,
                             gpointer       data,
                             gpointer       user_data)
{
}

static GjsDebugConnection *
add_dummy_connection_from_function(GjsMultiplexedDebugHooksFixture            *fixture,
                                   GjsMultiplexedDebugHooksConnectionFunction connector)
{
    return (*connector)(fixture->debug_hooks,
                        fixture->temporary_js_script_filename,
                        0,
                        (GCallback) dummy_callback_for_connector,
                        NULL,
                        NULL);
}

static void
test_debug_mode_on_while_there_are_active_connections(gpointer      fixture_data,
                                                      gconstpointer user_data)
{
    GjsMultiplexedDebugHooksFixture *fixture = (GjsMultiplexedDebugHooksFixture *) fixture_data;
    GjsMultiplexedDebugHooksConnectionFunction connector = (GjsMultiplexedDebugHooksConnectionFunction) user_data;
    GjsDebugConnection *connection =
        add_dummy_connection_from_function(fixture, connector);
    JSContext *js_context = (JSContext *) gjs_context_get_native_context(fixture->context);
    JSAutoCompartment ac(js_context,
                         JS_GetGlobalObject(js_context));

    g_assert(JS_GetDebugMode(js_context) == JS_TRUE);
    gjs_debug_connection_unregister(connection);
}

static void
test_debug_mode_off_when_active_connections_are_released(gpointer      fixture_data,
                                                         gconstpointer user_data)
{
    GjsMultiplexedDebugHooksFixture *fixture = (GjsMultiplexedDebugHooksFixture *) fixture_data;
    GjsMultiplexedDebugHooksConnectionFunction connector = (GjsMultiplexedDebugHooksConnectionFunction) user_data;
    GjsDebugConnection *connection =
        add_dummy_connection_from_function(fixture, connector);
    gjs_debug_connection_unregister(connection);
    JSContext *js_context = (JSContext *) gjs_context_get_native_context(fixture->context);
    JSAutoCompartment ac(js_context,
                         JS_GetGlobalObject (js_context));

    g_assert (JS_GetDebugMode (js_context) == JS_FALSE);
}

static void
single_step_mock_interrupt_callback(GjsDebugHooks    *hooks,
                                    GjsContext       *context,
                                    GjsInterruptInfo *info,
                                    gpointer          user_data)
{
    unsigned int *hit_count = (unsigned int *) user_data;
    ++(*hit_count);
}

static void
test_interrupts_are_recieved_in_single_step_mode (gpointer      fixture_data,
                                                  gconstpointer user_data)
{
    GjsMultiplexedDebugHooksFixture *fixture = (GjsMultiplexedDebugHooksFixture *) fixture_data;
    unsigned int hit_count = 0;
    GjsDebugConnection *connection =
        gjs_debug_hooks_start_singlestep(GJS_DEBUG_HOOKS_INTERFACE (fixture->debug_hooks),
                                         single_step_mock_interrupt_callback,
                                         &hit_count);
    gjs_context_eval_file(fixture->context,
                          fixture->temporary_js_script_filename,
                          NULL,
                          NULL);
    gjs_debug_connection_unregister(connection);
    g_assert(hit_count > 0);
}

static void
test_interrupts_are_not_recieved_after_single_step_mode_unlocked (gpointer      fixture_data,
                                                                  gconstpointer user_data)
{
    GjsMultiplexedDebugHooksFixture *fixture = (GjsMultiplexedDebugHooksFixture *) fixture_data;
    unsigned int hit_count = 0;
    GjsDebugConnection *connection =
        gjs_debug_hooks_start_singlestep(GJS_DEBUG_HOOKS_INTERFACE (fixture->debug_hooks),
                                         single_step_mock_interrupt_callback,
                                         &hit_count);
    gjs_debug_connection_unregister(connection);
    gjs_context_eval_file(fixture->context,
                          fixture->temporary_js_script_filename,
                          NULL,
                          NULL);
    g_assert(hit_count == 0);
}

static gboolean
uint_in_uint_array(unsigned int *array,
                   unsigned int array_len,
                   unsigned int n)
{
    unsigned int i;
    for (i = 0; i < array_len; ++i)
        if (array[i] == n)
            return TRUE;

    return FALSE;
}

static void
single_step_mock_interrupt_callback_tracking_lines (GjsDebugHooks    *hooks,
                                                    GjsContext       *context,
                                                    GjsInterruptInfo *info,
                                                    gpointer          user_data)
{
    GArray       *line_tracker = (GArray *) user_data;
    unsigned int adjusted_line = gjs_interrupt_info_get_line(info);

    if (!uint_in_uint_array((unsigned int *) line_tracker->data,
                            line_tracker->len,
                            adjusted_line))
        g_array_append_val(line_tracker, adjusted_line);
}

static gboolean
known_executable_lines_are_subset_of_executed_lines(const GArray       *executed_lines,
                                                    const unsigned int *executable_lines,
                                                    gsize               executable_lines_len)
{
    unsigned int i, j;
    for (i = 0; i < executable_lines_len; ++i) {
        gboolean found_executable_line_in_executed_lines = FALSE;
        for (j = 0; j < executed_lines->len; ++j) {
            if (g_array_index (executed_lines, unsigned int, j) == executable_lines[i])
                found_executable_line_in_executed_lines = TRUE;
        }

        if (!found_executable_line_in_executed_lines)
            return FALSE;
    }

    return TRUE;
}

static void
write_content_to_file_at_beginning(int         handle,
                                   const char *content)
{
    if (ftruncate(handle, 0) == -1)
        g_error ("Failed to erase mock file: %s", strerror(errno));
    lseek (handle, 0, SEEK_SET);
    if (write(handle, (gconstpointer) content, strlen(content) * sizeof (char)) == -1)
        g_error ("Failed to write to mock file: %s", strerror(errno));
}

static void
test_interrupts_are_received_on_all_executable_lines_in_single_step_mode (gpointer      fixture_data,
                                                                          gconstpointer user_data)
{
    GArray *line_tracker = g_array_new (FALSE, TRUE, sizeof(unsigned int));
    GjsMultiplexedDebugHooksFixture *fixture = (GjsMultiplexedDebugHooksFixture *) fixture_data;
    GjsDebugConnection *connection =
        gjs_debug_hooks_start_singlestep(GJS_DEBUG_HOOKS_INTERFACE(fixture->debug_hooks),
                                         single_step_mock_interrupt_callback_tracking_lines,
                                         line_tracker);
    const char mock_script[] =
        "let a = 1;\n" \
        "let b = 2;\n" \
        "\n" \
        "function func (a, b) {\n" \
        "    let result = a + b;\n" \
        "    return result;\n" \
        "}\n" \
        "\n" \
        "let c = func (a, b);\n"
        "\n";

    write_content_to_file_at_beginning(fixture->temporary_js_script_open_handle,
                                       mock_script);

    GjsReflectedExecutableScript *reflected = gjs_reflected_executable_script_new(fixture->temporary_js_script_filename);
    unsigned int       n_executable_lines = 0;
    const unsigned int *executable_lines =
        gjs_reflected_script_executable_lines(GJS_REFLECTED_SCRIPT_INTERFACE(reflected), &n_executable_lines);

    gjs_context_eval_file(fixture->context,
                          fixture->temporary_js_script_filename,
                          NULL,
                          NULL);

    g_assert(known_executable_lines_are_subset_of_executed_lines(line_tracker,
                                                                 executable_lines,
                                                                 n_executable_lines) == TRUE);

    g_array_free(line_tracker, TRUE);
    gjs_debug_connection_unregister(connection);
    g_object_unref(reflected);
}

static void
mock_breakpoint_callback(GjsDebugHooks    *hooks,
                         GjsContext       *context,
                         GjsInterruptInfo *info,
                         gpointer          user_data)
{
    unsigned int *line_hit = (unsigned int *) user_data;
    *line_hit = gjs_interrupt_info_get_line(info);
}

static void
test_breakpoint_is_hit_when_adding_before_script_run(gpointer      fixture_data,
                                                     gconstpointer user_data)
{
    GjsMultiplexedDebugHooksFixture *fixture = (GjsMultiplexedDebugHooksFixture *) fixture_data;
    const char mock_script[] =
        "let a = 1;\n"
        "let expected_breakpoint_line = 1;\n"
        "\n";

    write_content_to_file_at_beginning(fixture->temporary_js_script_open_handle,
                                       mock_script);

    unsigned int line_hit = 0;
    GjsDebugConnection *connection =
        gjs_debug_hooks_add_breakpoint(GJS_DEBUG_HOOKS_INTERFACE (fixture->debug_hooks),
                                       fixture->temporary_js_script_filename,
                                       1,
                                       mock_breakpoint_callback,
                                       &line_hit);

    gjs_context_eval_file(fixture->context,
                          fixture->temporary_js_script_filename,
                          NULL,
                          NULL);

    g_assert(line_hit == 1);

    gjs_debug_connection_unregister(connection);
}

static void
test_breakpoint_is_not_hit_when_later_removed (gpointer      fixture_data,
                                               gconstpointer user_data)
{
    GjsMultiplexedDebugHooksFixture *fixture = (GjsMultiplexedDebugHooksFixture *) fixture_data;
    const char mock_script[] =
        "let a = 1;\n"
        "let expected_breakpoint_line = 1;\n"
        "\n";

    write_content_to_file_at_beginning(fixture->temporary_js_script_open_handle,
                                       mock_script);

    unsigned int line_hit = 0;
    GjsDebugConnection *connection =
        gjs_debug_hooks_add_breakpoint(GJS_DEBUG_HOOKS_INTERFACE (fixture->debug_hooks),
                                       fixture->temporary_js_script_filename,
                                       1,
                                       mock_breakpoint_callback,
                                       &line_hit);
    gjs_debug_connection_unregister(connection);

    gjs_context_eval_file(fixture->context,
                          fixture->temporary_js_script_filename,
                          NULL,
                          NULL);

    g_assert(line_hit == 0);
}

static void
mock_function_calls_and_execution_interrupt_handler(GjsDebugHooks *hooks,
                                                    GjsContext    *context,
                                                    GjsFrameInfo  *info,
                                                    gpointer       user_data)
{
    gboolean *interrupts_received = (gboolean *) user_data;
    *interrupts_received = TRUE;
}

static void
test_interrupts_received_when_connected_to_function_calls_and_execution(gpointer      fixture_data,
                                                                        gconstpointer user_data)
{
    GjsMultiplexedDebugHooksFixture *fixture = (GjsMultiplexedDebugHooksFixture *) fixture_data;
    const char mock_script[] =
        "let a = 1;\n"
        "\n";

    write_content_to_file_at_beginning(fixture->temporary_js_script_open_handle,
                                       mock_script);

    gboolean interrupts_received = FALSE;

    GjsDebugConnection *connection =
        gjs_debug_hooks_connect_to_function_calls_and_execution(GJS_DEBUG_HOOKS_INTERFACE (fixture->debug_hooks),
                                                                mock_function_calls_and_execution_interrupt_handler,
                                                                &interrupts_received);

    gjs_context_eval_file(fixture->context,
                          fixture->temporary_js_script_filename,
                          NULL,
                          NULL);

    g_assert(interrupts_received == TRUE);

    gjs_debug_connection_unregister(connection);
}

static void
mock_function_calls_and_execution_interrupt_handler_recording_functions (GjsDebugHooks *hooks,
                                                                         GjsContext    *context,
                                                                         GjsFrameInfo  *info,
                                                                         gpointer       user_data)
{
    GList **function_names_hit = (GList **) user_data;

    *function_names_hit = g_list_append (*function_names_hit,
                                         g_strdup(gjs_interrupt_info_get_function_name((GjsInterruptInfo *) info)));
}

static gboolean
check_if_string_elements_are_in_list (GList       *list,
                                      const char  **elements,
                                      gsize        n_elements)
{
    if (elements && !list)
        return FALSE;

    unsigned int i;
    for (i = 0; i < n_elements; ++i) {
        GList *iter = list;
        gboolean found = FALSE;

        while (iter) {
            if (g_strcmp0 ((const char *) iter->data, elements[i]) == 0) {
                found = TRUE;
                break;
            }

            iter = g_list_next  (iter);
        }

        if (!found)
            return FALSE;
    }

    return TRUE;
}

static void
test_expected_function_names_hit_when_connected_to_calls_and_execution_handler (gpointer      fixture_data,
                                                                                gconstpointer user_data)
{
    GjsMultiplexedDebugHooksFixture *fixture = (GjsMultiplexedDebugHooksFixture *) fixture_data;
    const char mock_script[] =
        "let a = 1;\n"
        "function foo (a) {\n"
        "    return a;\n"
        "}\n"
        "let b = foo (a);\n"
        "\n";

    write_content_to_file_at_beginning(fixture->temporary_js_script_open_handle,
                                       mock_script);

    GList *function_names_hit = NULL;
    GjsDebugConnection *connection =
        gjs_debug_hooks_connect_to_function_calls_and_execution(GJS_DEBUG_HOOKS_INTERFACE (fixture->debug_hooks),
                                                                mock_function_calls_and_execution_interrupt_handler_recording_functions,
                                                                &function_names_hit);

    gjs_context_eval_file(fixture->context,
                          fixture->temporary_js_script_filename,
                          NULL,
                          NULL);

    const char *expected_function_names_hit[] = {
      "foo"
    };
    const gsize expected_function_names_hit_len =
        G_N_ELEMENTS(expected_function_names_hit);

    g_assert(check_if_string_elements_are_in_list(function_names_hit,
                                                  expected_function_names_hit,
                                                  expected_function_names_hit_len));

    if (function_names_hit)
        g_list_free_full(function_names_hit, g_free);

    gjs_debug_connection_unregister(connection);
}

static void
test_nothing_hit_when_function_calls_and_toplevel_execution_handler_removed (gpointer      fixture_data,
                                                                             gconstpointer user_data)
{
    GjsMultiplexedDebugHooksFixture *fixture = (GjsMultiplexedDebugHooksFixture *) fixture_data;
    const char mock_script[] =
        "let a = 1;\n"
        "function foo (a) {\n"
        "    return a;\n"
        "}\n"
        "let b = foo (a);\n"
        "\n";

    write_content_to_file_at_beginning(fixture->temporary_js_script_open_handle,
                                       mock_script);

    GList *function_names_hit = NULL;
    GjsDebugConnection *connection =
        gjs_debug_hooks_connect_to_function_calls_and_execution(GJS_DEBUG_HOOKS_INTERFACE(fixture->debug_hooks),
                                                                mock_function_calls_and_execution_interrupt_handler_recording_functions,
                                                                &function_names_hit);
    gjs_debug_connection_unregister(connection);

    gjs_context_eval_file(fixture->context,
                          fixture->temporary_js_script_filename,
                          NULL,
                          NULL);

    g_assert(function_names_hit == NULL);
}

static void
replace_string(char       **string_pointer,
               const char *new_string)
{
    if (*string_pointer)
        g_free (*string_pointer);

    *string_pointer = g_strdup (new_string);
}

static void
mock_new_script_hook(GjsDebugHooks      *hooks,
                     GjsContext         *context,
                     GjsDebugScriptInfo *info,
                     gpointer            user_data)
{
    char **last_loaded_script = (char **) user_data;

    replace_string(last_loaded_script,
                   gjs_debug_script_info_get_filename(info));
}

static void
test_script_load_notification_sent_on_new_script(gpointer      fixture_data,
                                                 gconstpointer user_data)
{
    GjsMultiplexedDebugHooksFixture *fixture = (GjsMultiplexedDebugHooksFixture *) fixture_data;
    const char loadable_script[] = "let a = 1;\n\n";

    write_content_to_file_at_beginning(fixture->temporary_js_script_open_handle,
                                       loadable_script);

    char *last_loaded_script = NULL;
    GjsDebugConnection *connection =
        gjs_debug_hooks_connect_to_script_load(GJS_DEBUG_HOOKS_INTERFACE (fixture->debug_hooks),
                                                      mock_new_script_hook,
                                                      &last_loaded_script);

    gjs_context_eval_file(fixture->context,
                          fixture->temporary_js_script_filename,
                          NULL,
                          NULL);

    g_assert(last_loaded_script != NULL &&
             g_strcmp0(last_loaded_script,
                       fixture->temporary_js_script_filename) == 0);

    g_free(last_loaded_script);
    gjs_debug_connection_unregister(connection);
}

static void
test_script_load_notification_not_sent_on_connection_removed(gpointer      fixture_data,
                                                             gconstpointer user_data)
{
    GjsMultiplexedDebugHooksFixture *fixture = (GjsMultiplexedDebugHooksFixture *) fixture_data;
    const char loadable_script[] = "let a = 1;\n\n";

    write_content_to_file_at_beginning(fixture->temporary_js_script_open_handle,
                                       loadable_script);

    char *last_loaded_script = NULL;
    GjsDebugConnection *connection =
        gjs_debug_hooks_connect_to_script_load(GJS_DEBUG_HOOKS_INTERFACE (fixture->debug_hooks),
                                               mock_new_script_hook,
                                               &last_loaded_script);

    gjs_debug_connection_unregister(connection);

    gjs_context_eval_file(fixture->context,
                          fixture->temporary_js_script_filename,
                          NULL,
                          NULL);

    g_assert(last_loaded_script == NULL);
}

typedef void (*TestDataFunc)(gpointer      data,
                             gconstpointer user_data);

static void
for_each_in_table_driven_test_data(gconstpointer test_data,
                                   gsize         element_size,
                                   gsize         n_elements,
                                   TestDataFunc  func,
                                   gconstpointer user_data)
{
    const char *test_data_iterator = (const char *) test_data;
    gsize i;
    for (i = 0; i < n_elements; ++i, test_data_iterator += element_size)
        (*func)((char *) (test_data_iterator), user_data);
}

typedef struct _FixturedTest {
    gsize            fixture_size;
    GTestFixtureFunc set_up;
    GTestFixtureFunc tear_down;
} FixturedTest;

static void
add_test_for_fixture(const char       *name,
                     FixturedTest     *fixture,
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

typedef struct _FixturedTableDrivenTestData {
    const char       *test_name;
    gsize            fixture_size;
    GTestFixtureFunc set_up;
    GTestFixtureFunc test_func;
    GTestFixtureFunc tear_down;
} FixturedTableDrivenTestData;

static void
add_test_for_fixture_size_and_funcs(gpointer      data,
                                    gconstpointer user_data)
{
    const FixturedTableDrivenTestData *fixtured_table_driven_test = (const FixturedTableDrivenTestData *) data;
    FixturedTest fixtured_test = {
        fixtured_table_driven_test->fixture_size,
        fixtured_table_driven_test->set_up,
        fixtured_table_driven_test->tear_down
    };
    add_test_for_fixture(fixtured_table_driven_test->test_name,
                         &fixtured_test,
                         fixtured_table_driven_test->test_func,
                         user_data);

}

typedef struct _GjsMultiplexedDebugHooksContextStateData {
    const char                                  *test_name;
    GjsMultiplexedDebugHooksConnectionFunction connector;
} GjsMultiplexedDebugHooksContextStateData;

typedef struct _GjsMultiplexedDebugHooksTableDrivenTest {
    const char      *prefix;
    GTestFixtureFunc test_function;
} GjsMultiplexedDebugHooksTableDrivenTest;

static void
add_gjs_multiplexed_debug_hooks_context_state_data_test(gpointer      data,
                                                        gconstpointer user_data)
{
    const GjsMultiplexedDebugHooksTableDrivenTest  *test = (const GjsMultiplexedDebugHooksTableDrivenTest *) user_data;
    GjsMultiplexedDebugHooksContextStateData       *table_data = (GjsMultiplexedDebugHooksContextStateData *) data;

    char *test_name = g_strconcat(test->prefix, "/", table_data->test_name, NULL);

    FixturedTableDrivenTestData fixtured_data = {
        test_name,
        sizeof(GjsMultiplexedDebugHooksFixture),
        gjs_multiplexed_debug_hooks_fixture_set_up,
        test->test_function,
        gjs_multiplexed_debug_hooks_fixture_tear_down
    };

    add_test_for_fixture_size_and_funcs(&fixtured_data,
                                        (gconstpointer) table_data->connector);

    g_free (test_name);
}

void gjs_test_add_tests_for_debug_hooks()
{
    const GjsMultiplexedDebugHooksContextStateData context_state_data[] = {
        { "add_breakpoint", (GjsMultiplexedDebugHooksConnectionFunction) gjs_debug_hooks_add_breakpoint },
        { "start_singlestep", (GjsMultiplexedDebugHooksConnectionFunction) gjs_debug_hooks_start_singlestep },
        { "connect_to_script_load", (GjsMultiplexedDebugHooksConnectionFunction) gjs_debug_hooks_connect_to_script_load },
        { "connect_to_function_calls_and_execution", (GjsMultiplexedDebugHooksConnectionFunction) gjs_debug_hooks_connect_to_function_calls_and_execution }
    };
    const gsize context_state_data_len =
        G_N_ELEMENTS(context_state_data);

    const GjsMultiplexedDebugHooksTableDrivenTest debug_hooks_tests_info[] = {
        {
            "/gjs/debug/debug_hooks/debug_mode_is_on_when_connection_from",
            test_debug_mode_on_while_there_are_active_connections
        },
        {
            "/gjs/debug/debug_hooks/debug_mode_off_when_connection_released",
            test_debug_mode_off_when_active_connections_are_released
        }
    };
    const gsize debug_hooks_tests_info_size =
        G_N_ELEMENTS(debug_hooks_tests_info);

    gsize i;
    for (i = 0; i < debug_hooks_tests_info_size; ++i)
        for_each_in_table_driven_test_data(&context_state_data,
                                           sizeof(GjsMultiplexedDebugHooksContextStateData),
                                           context_state_data_len,
                                           add_gjs_multiplexed_debug_hooks_context_state_data_test,
                                           (gconstpointer) &debug_hooks_tests_info[i]);

    FixturedTest multiplexed_debug_hooks_fixture = {
        sizeof (GjsMultiplexedDebugHooksFixture),
        gjs_multiplexed_debug_hooks_fixture_set_up,
        gjs_multiplexed_debug_hooks_fixture_tear_down
    };

    add_test_for_fixture("/gjs/debug/debug_hooks/interrupts_recieved_when_in_single_step_mode",
                         &multiplexed_debug_hooks_fixture,
                         test_interrupts_are_recieved_in_single_step_mode,
                         NULL);
    add_test_for_fixture("/gjs/debug/debug_hooks/interrupts_not_received_after_single_step_mode_unlocked",
                         &multiplexed_debug_hooks_fixture,
                         test_interrupts_are_not_recieved_after_single_step_mode_unlocked,
                         NULL);
    add_test_for_fixture("/gjs/debug/debug_hooks/interrupts_received_on_expected_lines_of_script",
                         &multiplexed_debug_hooks_fixture,
                         test_interrupts_are_received_on_all_executable_lines_in_single_step_mode,
                         NULL);
    add_test_for_fixture("/gjs/debug/debug_hooks/breakpoint_hit_when_added_before_script_run",
                         &multiplexed_debug_hooks_fixture,
                         test_breakpoint_is_hit_when_adding_before_script_run,
                         NULL);
    add_test_for_fixture("/gjs/debug/debug_hooks/interrupts_received_when_connected_to_function_calls_and_execution",
                         &multiplexed_debug_hooks_fixture,
                         test_interrupts_received_when_connected_to_function_calls_and_execution,
                         NULL);
    add_test_for_fixture("/gjs/debug/debug_hooks/interrupts_received_for_expected_functions_when_connected_to_function_calls_and_execution",
                         &multiplexed_debug_hooks_fixture,
                         test_expected_function_names_hit_when_connected_to_calls_and_execution_handler,
                         NULL);
    add_test_for_fixture("/gjs/debug/debug_hooks/interrupts_not_received_when_function_calls_and_execution_hook_is_removed",
                         &multiplexed_debug_hooks_fixture,
                         test_nothing_hit_when_function_calls_and_toplevel_execution_handler_removed,
                         NULL);
    add_test_for_fixture("/gjs/debug/debug_hooks/new_script_notification_sent_when_listener_installed",
                         &multiplexed_debug_hooks_fixture,
                         test_script_load_notification_sent_on_new_script,
                         NULL);
    add_test_for_fixture("/gjs/debug/debug_hooks/new_script_notification_not_sent_when_listener_uninstalled",
                         &multiplexed_debug_hooks_fixture,
                         test_script_load_notification_not_sent_on_connection_removed,
                         NULL);
}
