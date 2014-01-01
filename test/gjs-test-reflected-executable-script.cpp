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
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <glib.h>
#include <gio/gio.h>
#include <gjs/gjs.h>
#include <gjs/reflected-script.h>
#include <gjs/reflected-executable-script.h>

typedef struct _TestDataBase {
    const char *name;
} TestDataBase;

typedef struct _GjsReflectedExecutableScriptTestFixture {
    char *temporary_js_script_filename;
    int  temporary_js_script_open_handle;
} GjsReflectedExecutableScriptTestFixture;

static void
gjs_reflected_executable_test_fixture_set_up(gpointer      fixture_data,
                                             gconstpointer test_data)
{
    GjsReflectedExecutableScriptTestFixture *fixture = (GjsReflectedExecutableScriptTestFixture *) fixture_data;
    char                                    *current_dir = g_get_current_dir();
    fixture->temporary_js_script_open_handle = g_file_open_tmp("mock-js-XXXXXXX.js",
                                                               &fixture->temporary_js_script_filename,
                                                               NULL);

    g_free(current_dir);
}

static GArray *
generate_sequential_array_of_program_counter_lines(unsigned int n_lines)
{
    GArray *array = g_array_sized_new(FALSE, FALSE, sizeof(unsigned int), n_lines);
    unsigned int i;

    for (i = 0; i < n_lines; ++i)
    {
        unsigned int line_number = i + 1;
        g_array_append_val(array, line_number);
    }

    return array;
}

static void
gjs_reflected_executable_test_fixture_tear_down(gpointer      fixture_data,
                                                gconstpointer test_data)
{
    GjsReflectedExecutableScriptTestFixture *fixture = (GjsReflectedExecutableScriptTestFixture *) fixture_data;
    unlink(fixture->temporary_js_script_filename);
    g_free(fixture->temporary_js_script_filename);
    close(fixture->temporary_js_script_open_handle);
}

static void
test_reflect_creation_and_destruction(gpointer      fixture_data,
                                      gconstpointer user_data)
{
    GjsReflectedExecutableScriptTestFixture *fixture = (GjsReflectedExecutableScriptTestFixture *) fixture_data;

    const char mock_script[] = "var a = 1;\n";

    if (write(fixture->temporary_js_script_open_handle, mock_script, strlen(mock_script) * sizeof(char)) == -1)
        g_error("Failed to write to test script");

    GjsReflectedExecutableScript *script = gjs_reflected_executable_script_new(fixture->temporary_js_script_filename);
    g_object_unref(script);
}

static int
count_lines_in_string(const char *string)
{
    unsigned int counter = 0;
    const char *iter = string;

    do {
        ++counter;
        iter = strstr(iter, "\n");

        if (iter)
            iter += 1;
    } while (iter);

    return counter;
}

static gboolean
integer_arrays_equal(const unsigned int *actual,
                     unsigned int        actual_n,
                     const unsigned int *expected,
                     unsigned int        expected_n)
{
    if (actual_n != expected_n)
        return FALSE;

    unsigned int i;

    for (i = 0; i < actual_n; ++i)
        if (actual[i] != expected[i])
            return FALSE;

    return TRUE;
}

static void
test_reflect_get_all_executable_expression_lines(gpointer      fixture_data,
                                                 gconstpointer user_data)
{
    GjsReflectedExecutableScriptTestFixture *fixture = (GjsReflectedExecutableScriptTestFixture *) fixture_data;

    const char mock_script[] =
            "var a = 1.0;\n"
            "var b = 2.0;\n"
            "var c = 3.0;\n";

    if (write(fixture->temporary_js_script_open_handle, mock_script, strlen(mock_script) * sizeof(char)) == -1)
        g_error("Failed to write to test script");

    GjsReflectedExecutableScript *script =
        gjs_reflected_executable_script_new(fixture->temporary_js_script_filename);

    unsigned int       n_executable_lines = 0;
    const unsigned int *executable_lines = gjs_reflected_script_executable_lines(GJS_REFLECTED_SCRIPT_INTERFACE(script),
                                                                                 &n_executable_lines);

    const unsigned int expected_executable_lines[] = {
        1, 2, 3
    };
    const unsigned int n_expected_executable_lines = G_N_ELEMENTS(expected_executable_lines);

    g_assert(integer_arrays_equal(executable_lines,
                                  n_executable_lines,
                                  expected_executable_lines,
                                  n_expected_executable_lines));

    g_object_unref(script);
}

typedef struct _GjsReflectedExecutableScriptTableTestFixture
{
    GjsReflectedExecutableScriptTestFixture base;
    GjsReflectedExecutableScript            *script;
} GjsReflectedExecutableScriptTableTestFixture;

typedef struct _GjsReflectedExecutableMockScriptTestData
{
    TestDataBase base;
    const char *mock_script;
} GjsReflectedExecutableMockScriptTestData;

static void
gjs_reflected_executable_mock_script_test_fixture_set_up(gpointer      fixture_data,
                                                         gconstpointer user_data)
{
    GjsReflectedExecutableMockScriptTestData     *data = (GjsReflectedExecutableMockScriptTestData *) user_data;
    GjsReflectedExecutableScriptTableTestFixture *fixture = (GjsReflectedExecutableScriptTableTestFixture *) fixture_data;
    gjs_reflected_executable_test_fixture_set_up(fixture_data, user_data);

    if (write(fixture->base.temporary_js_script_open_handle,
              data->mock_script,
              strlen(data->mock_script) * sizeof(char)) == -1)
        g_error("Failed to write to test script");

    fixture->script =
        gjs_reflected_executable_script_new(fixture->base.temporary_js_script_filename);
}

static void
gjs_reflected_executable_mock_script_test_fixture_tear_down(gpointer      fixture_data,
                                                            gconstpointer user_data)
{
    GjsReflectedExecutableScriptTableTestFixture *fixture = (GjsReflectedExecutableScriptTableTestFixture *) fixture_data;

    g_object_unref(fixture->script);
    gjs_reflected_executable_test_fixture_tear_down(fixture_data, user_data);
}

typedef struct _GjsReflectedExecutableScriptLinesTestData
{
    GjsReflectedExecutableMockScriptTestData base;
    /* We can't have more than 256 expected executable lines,
     * because we can't dynamically assign a static array,
     * but it doesn't matter too much */
    const unsigned int expected_executable_lines[256];
    const unsigned int n_expected_executable_lines;
} GjsReflectedExecutableScriptLinesTestData;

static void
test_reflected_script_has_expected_executable_lines_for_script(gpointer      fixture_data,
                                                               gconstpointer user_data)
{
    GjsReflectedExecutableScriptLinesTestData    *data = (GjsReflectedExecutableScriptLinesTestData *) user_data;
    GjsReflectedExecutableScriptTableTestFixture *fixture = (GjsReflectedExecutableScriptTableTestFixture *) fixture_data;

    unsigned int       n_executable_lines = 0;
    const unsigned int *executable_lines = gjs_reflected_script_executable_lines(GJS_REFLECTED_SCRIPT_INTERFACE(fixture->script),
                                                                                 &n_executable_lines);

    const unsigned int *expected_executable_lines = data->expected_executable_lines;
    const unsigned int n_expected_executable_lines = data->n_expected_executable_lines;

    g_assert(integer_arrays_equal(executable_lines,
                                  n_executable_lines,
                                  expected_executable_lines,
                                  n_expected_executable_lines));
}

typedef struct _GjsReflectedExecutableScriptFunctionsTestData
{
    GjsReflectedExecutableMockScriptTestData base;
    /* We can't have more than 256 function names, but
     * it doesn't matter */
    const char                               *expected_functions[256];
} GjsReflectedExecutableScriptFunctionsTestData;

static gboolean
has_elements_in_strv_in_order (const char **actual,
                               const char **expected)
{
    const char **expected_iterator = expected;
    const char **actual_iterator = actual;

    while (*expected_iterator) {
        /* Size mismatch */
        if (!*actual_iterator)
            return FALSE;

        if (strcmp(*actual_iterator, *expected_iterator) != 0)
            return FALSE;

        ++actual_iterator;
        ++expected_iterator;
    }

    return TRUE;
}

static void
test_reflected_script_has_expected_function_names(gpointer      fixture_data,
                                                  gconstpointer user_data)
{
    GjsReflectedExecutableScriptTableTestFixture  *fixture = (GjsReflectedExecutableScriptTableTestFixture *) fixture_data;
    GjsReflectedExecutableScriptFunctionsTestData *data = (GjsReflectedExecutableScriptFunctionsTestData *) user_data;
    const char **functions = gjs_reflected_script_functions(GJS_REFLECTED_SCRIPT_INTERFACE(fixture->script));
    const char **expected_functions = data->expected_functions;

    g_assert(has_elements_in_strv_in_order(functions, expected_functions));
}

typedef struct _GjsReflectedExecutableScriptExpectedBranch {
    unsigned int point;
    unsigned int alternatives[256];
    unsigned int n_alternatives;
} GjsReflectedExecutableScriptExpectedBranch;

typedef struct _GjsReflectedExecutableScriptBranchesTestData {
    GjsReflectedExecutableMockScriptTestData   base;
    GjsReflectedExecutableScriptExpectedBranch expected_branches[256];
    unsigned int                               n_expected_branches;
} GjsReflectedExecutableScriptBranchesTestData;

static gboolean
branch_info_equal(GjsReflectedExecutableScriptExpectedBranch *expected,
                  const GjsReflectedScriptBranchInfo         *branch)
{
    if (gjs_reflected_script_branch_info_get_branch_point(branch) != expected->point)
        return FALSE;

    unsigned int       n_actual_alternatives = 0;
    const unsigned int *actual_alternatives = gjs_reflected_script_branch_info_get_branch_alternatives(branch,
                                                                                                       &n_actual_alternatives);
    unsigned int       n_expected_alternatives = expected->n_alternatives;
    const unsigned int *expected_alternatives = expected->alternatives;

    return integer_arrays_equal(actual_alternatives,
                                n_actual_alternatives,
                                expected_alternatives,
                                n_expected_alternatives);
}

static gboolean
has_elements_in_branch_array_in_order (GjsReflectedExecutableScriptExpectedBranch *expected,
                                       const GjsReflectedScriptBranchInfo         **branches,
                                       unsigned int                                n_expected)
{
    const GjsReflectedScriptBranchInfo **branch_iterator = branches;
    unsigned int i;

    for (i = 0; i < n_expected; ++i, ++branch_iterator) {
        /* Size mismatch */
        if (!(*branch_iterator))
            return FALSE;

        if (!branch_info_equal(&expected[i],
                               *branch_iterator))
            return FALSE;
    }

    /* Size mismatch - there are still remaining branches */
    if (*branch_iterator)
        return FALSE;

    return TRUE;
}

static void
test_reflected_script_has_expected_branches(gpointer      fixture_data,
                                            gconstpointer user_data)
{
    GjsReflectedExecutableScriptTableTestFixture  *fixture = (GjsReflectedExecutableScriptTableTestFixture *) fixture_data;
    GjsReflectedExecutableScriptBranchesTestData *data = (GjsReflectedExecutableScriptBranchesTestData *) user_data;

    GjsReflectedExecutableScriptExpectedBranch *expected = data->expected_branches;
    const GjsReflectedScriptBranchInfo         **branches = gjs_reflected_script_branches(GJS_REFLECTED_SCRIPT_INTERFACE(fixture->script));

    g_assert(has_elements_in_branch_array_in_order(expected, branches, data->n_expected_branches));
}

typedef struct _TestFixture
{
    GTestFixtureFunc set_up;
    GTestFixtureFunc tear_down;
    size_t           test_fixture_size;
} TestFixture;

typedef struct _TableData
{
    TestDataBase  *all_data;
    gsize         element_size;
    gsize         n_data_elements;
} TableData;

static void
add_test_for_fixture_with_data(const char       *name,
                               TestFixture      *fixture,
                               TableData        *data,
                               GTestFixtureFunc  test_func)
{
    char  *test_data_pointer;
    gsize test_data_iterator;

    /* We use a fairly simple trick here - all table driven
     * test data must have TestDataBase as the first member
     * and then we advance the array by the provided element
     * size (as opposed to sizeof(TestDataBase)).  That way,
     * we can get test names whilst still having only
     * one level of indirection */
    for (test_data_iterator = 0,
         test_data_pointer = (char *) data->all_data;
         test_data_iterator < data->n_data_elements;
         test_data_pointer += data->element_size,
         ++test_data_iterator) {
        const TestDataBase *base = (const TestDataBase *) test_data_pointer;
        char *full_test_path = g_strconcat(name, "/", base->name, NULL);

        g_test_add_vtable(full_test_path,
                          fixture->test_fixture_size,
                          (gconstpointer) test_data_pointer,
                          fixture->set_up,
                          test_func,
                          fixture->tear_down);

        g_free(full_test_path);
    }
}

static void
add_test_for_fixture(const char       *name,
                     TestFixture      *fixture,
                     GTestFixtureFunc  test_func)
{
    g_test_add_vtable(name,
                      fixture->test_fixture_size,
                      NULL,
                      fixture->set_up,
                      test_func,
                      fixture->tear_down);
}

void
gjs_test_add_tests_for_reflected_executable_script(void)
{
    TestFixture reflected_script_default_fixture = {
        gjs_reflected_executable_test_fixture_set_up,
        gjs_reflected_executable_test_fixture_tear_down,
        sizeof(GjsReflectedExecutableScriptTestFixture)
    };

    add_test_for_fixture("/gjs/debug/reflected_script/construction",
                         &reflected_script_default_fixture,
                         test_reflect_creation_and_destruction);
    add_test_for_fixture("/gjs/debug/reflected_script/all_lines_executable_for_expressions",
                         &reflected_script_default_fixture,
                         test_reflect_get_all_executable_expression_lines);

    TestFixture reflected_script_mock_script_fixture = {
        gjs_reflected_executable_mock_script_test_fixture_set_up,
        gjs_reflected_executable_mock_script_test_fixture_tear_down,
        sizeof(GjsReflectedExecutableScriptTableTestFixture)
    };

    /* This needs to be static so that it sticks around after this function goes out of scope */
    static GjsReflectedExecutableScriptLinesTestData reflected_script_executable_lines_table[] = {
        {
            {
                { "lines_inside_functions" },
                "function f(a, b) {\n"
                "    let x = a;\n"
                "    let y = b;\n"
                "    return x + y;\n"
                "}\n"
                "\n"
                "var z = f(1, 2);\n"
            },
            { 2, 3, 4, 7 },
            4
        },
        {
            {
                { "lines_inside_anonymous_functions" },
                "var z = (function f(a, b) {\n"
                "     let x = a;\n"
                "     let y = b;\n"
                "     return x + y;\n"
                " })();\n"
            },
            { 1, 2, 3, 4 },
            4
        },
        {
            {
                { "lines_inside_functions_as_properties" },
                "var o = {\n"
                "    foo: function () {\n"
                "        let x = a;\n"
                "    }\n"
                "};\n"
            },
            { 1, 2, 3 },
            3
        },
        {
            {
                { "lines_inside_calls_as_properties_of_object_to_call" },
                "function f(a) {\n"
                "}\n"
                "f({\n"
                "    foo: function() {\n"
                "        let x = a;\n"
                "    }\n"
                "});\n"
            },
            { 3, 4, 5 },
            3
        },
        {
            {
                { "function_argument_lines" },
                "function f(a, b, c) {\n"
                "}\n"
                "f(1,\n"
                "  2,\n"
                "  3);\n"
            },
            { 3, 4, 5 },
            3
        },
        {
            {
                { "new_call_argument_lines" },
                "function f(o) {\n"
                "}\n"
                "new f({ a: 1,\n"
                "        b: 2,\n"
                "        c: 3});\n"
            },
            { 3, 4, 5 },
            3
        },
        {
            {
                { "object_property_from_new_call" },
                "function f(o) {\n"
                "}\n"
                "let obj = {\n"
                "    Name: new f({ a: 1,\n"
                "                  b: 2,\n"
                "                  c: 3\n"
                "                })\n"
                "}\n"
            },
            { 3, 4, 5, 6 },
            4
        },
        {
            {
                { "lines_inside_while_loop" },
                "var a = 0;\n"
                "while (a < 1) {\n"
                "    let x = 0;\n"
                "    let y = 1;\n"
                "    a++;"
                "\n"
                "}\n"
            },
            { 1, 2, 3, 4, 5 },
            5
        },
        {
            {
                { "try_catch_finally" },
                "var a = 0;\n"
                "try {\n"
                "    a++;\n"
                "} catch (e) {\n"
                "    a++;\n"
                "} finally {\n"
                "    a++;\n"
                "}\n"
            },
            { 1, 2, 3, 4, 5, 7 },
            6
        },
        {
            /* "case" labels are never executable */
            {
                { "lines_inside_of_case_statements" },
                "var a = 0;\n"
                "switch (a) {\n"
                "case 1:\n"
                "    a++;\n"
                "    break;\n"
                "case 2:\n"
                "    a++;\n"
                "    break;\n"
                "}\n"
            },
            { 1, 2, 4, 5, 7, 8 },
            6
        },
        {
            {
                { "lines_inside_for_loop" },
                "for (let i = 0; i < 1; i++) {\n"
                "    let x = 0;\n"
                "    let y = 1;\n"
                "\n"
                "}\n"
            },
            { 1, 2, 3 },
            3
        },
        {
            {
                { "lines_inside_if_blocks" },
                "if (1 > 0) {\n"
                "    let i = 0;\n"
                "} else {\n"
                "    let j = 1;\n"
                "}\n"
            },
            { 1, 2, 4 },
            3
        },
        {
            {
                { "lines_inside_if_tests" },
                "if (1 > 0 &&\n"
                "    2 > 0 &&\n"
                "    3 > 0){\n"
                "    let a = 3;\n"
                "}\n"
            },
            { 1, 4 },
            2
        },
        {
            {
                { "object_property_expressions" },
                "var b = 1;\n"
                "var a = {\n"
                "    Name: b,\n"
                "    Ex: b\n"
                "};\n"
            },
            { 1, 2, 3, 4 },
            4
        },
        {
            {
                { "object_property_literals" },
                "var a = {\n"
                "    Name: 'foo',\n"
                "    Ex: 'bar'\n"
                "};\n"
            },
            { 1, 2, 3 },
            3
        },
        {
            {
                { "object_property_function_expression" },
                "var a = {\n"
                "    Name: function() {},\n"
                "};\n"
            },
            { 1, 2 },
            2
        },
        {
            {
                { "object_property_object_expression" },
                "var a = {\n"
                "    Name: {},\n"
                "};\n"
            },
            { 1, 2 },
            2
        },
        {
            {
                { "object_property_array_expression" },
                "var a = {\n"
                "    Name: {},\n"
                "};\n"
            },
            { 1, 2 },
            2
        },
        {
            {
                { "object_args_to_return" },
                "function f() {\n"
                "    return {\n"
                "        a: 1,\n"
                "        b: 2\n"
                "    }\n"
                "}\n"
            },
            { 2, 3, 4 },
            3
        },
        {
            {
                { "object_args_to_throw" },
                "function f() {\n"
                "    throw {\n"
                "        a: 1,\n"
                "        b: 2\n"
                "    }\n"
                "}\n"
            },
            { 2, 3, 4 },
            3
        }
    };

    TableData reflected_script_executable_lines_data = {
        (TestDataBase *) reflected_script_executable_lines_table,
        sizeof(GjsReflectedExecutableScriptLinesTestData),
        G_N_ELEMENTS(reflected_script_executable_lines_table)
    };

    add_test_for_fixture_with_data("/gjs/debug/reflected_script/executable_lines",
                                   &reflected_script_mock_script_fixture,
                                   &reflected_script_executable_lines_data,
                                   test_reflected_script_has_expected_executable_lines_for_script);

    static GjsReflectedExecutableScriptFunctionsTestData reflected_executable_script_functions_table[] = {
        {
            {
                { "list_of_functions" },
                "function f1() {}\n"
                "function f2() {}\n"
                "function f3() {}\n"
            },
            {
                "f1",
                "f2",
                "f3",
                NULL
            }
        },
        {
            {
                { "nested_functions" },
                "function f1() {\n"
                "    let f2 = function() {\n"
                "        let f3 = function() {\n"
                "        }\n"
                "    }\n"
                "}\n"
            },
            {
                "f1",
                "function:2",
                "function:3"
            }
        }
    };

    TableData reflected_script_functions_data = {
        (TestDataBase *) reflected_executable_script_functions_table,
        sizeof(GjsReflectedExecutableScriptFunctionsTestData),
        G_N_ELEMENTS(reflected_executable_script_functions_table)
    };

    add_test_for_fixture_with_data("/gjs/debug/reflected_script/functions",
                                   &reflected_script_mock_script_fixture,
                                   &reflected_script_functions_data,
                                   test_reflected_script_has_expected_function_names);

    static GjsReflectedExecutableScriptBranchesTestData reflected_executable_script_branches_table[] = {
        {
            {
                { "simple_if_else_branch" },
                "if (1) {\n"
                "    let a = 1;\n"
                "} else {\n"
                "    let b = 2;\n"
                "}\n"
            },
            {
                /* One expected branch */
                {
                    1,
                    { 2, 4 },
                    2
                }
            },
            1
        },
        {
            {
                { "if_branch_with_only_one_consequent" },
                "if (1) {\n"
                "    let a = 1.0;\n"
                "}\n"
            },
            {
                {
                    1,
                    { 2 },
                    1
                }
            },
            1
        },
        {
            {
                { "nested_if_else_branches" },
                "if (1) {\n"
                "    let a = 1.0;\n"
                "} else if (2) {\n"
                "    let b = 2.0;\n"
                "} else if (3) {\n"
                "    let c = 3.0;\n"
                "} else {\n"
                "    let d = 4.0;\n"
                "}\n"
            },
            {
                /* the 'else if' is executable since
                 * it is an if condition */
                {
                    1,
                    { 2, 3 },
                    2
                },
                {
                    3,
                    { 4, 5 },
                    2
                },
                /* The 'else' by itself is not executable,
                 * that part is the contents of the next block
                 */
                {
                    5,
                    { 6, 8 },
                    2
                }
            },
            3
        },
        {
            {
                { "if_else_branch_without_blocks" },
                "let a, b;\n"
                "if (1)\n"
                "    a = 1.0\n"
                "else\n"
                "    b = 2.0\n"
                "\n"
            },
            {
                {
                    2,
                    { 3, 5 },
                    2
                }
            },
            1
        },
        {
            {
                { "no_branch_if_consequent_empty" },
                "let a, b;\n"
                "if (1);\n"
            },
            {
            },
            0
        },
        {
            {
                { "branch_if_consequent_empty_but_alternate_defined" },
                "let a, b;\n"
                "if (1);\n"
                "else\n"
                "    a++;\n"
            },
            {
                {
                    2,
                    { 4 },
                    1
                }
            },
            1
        },
        {
            {
                { "while_statement_implicit_branch" },
                "while (1) {\n"
                "    let a = 1;\n"
                "}\n"
                "let b = 2;"
            },
            {
                {
                    1,
                    { 2 },
                    1
                }
            },
            1
        },
        {
            {
                { "do_while_statement_implicit_branch" },
                "do {\n"
                "    let a = 1;\n"
                "} while (1)\n"
                "let b = 2;"
            },
            {
                /* For do-while loops the branch-point is
                 * at the 'do' condition and not the
                 * 'while' */
                {
                    1,
                    { 2 },
                    1
                }
            },
            1
        },
        {
            {
                { "case_statements" },
                "let a = 1;\n"
                "switch (1) {\n"
                "case '1':\n"
                "    a++;\n"
                "    break;\n"
                "case '2':\n"
                "    a++\n"
                "    break;\n"
                "default:\n"
                "    a++\n"
                "    break;\n"
                "}\n"
            },
            {
                /* There are three potential branches here */
                {
                    2,
                    { 4, 7, 10 },
                    3
                }
            },
            1
        },
        {
            {
                { "case_statements_with_noop_labels" },
                "let a = 1;\n"
                "switch (1) {\n"
                "case '1':\n"
                "case '2':\n"
                "default:\n"
                "}\n"
            },
            {
            },
            0
        }
    };

    TableData reflected_script_branches_data = {
        (TestDataBase *) reflected_executable_script_branches_table,
        sizeof(GjsReflectedExecutableScriptBranchesTestData),
        G_N_ELEMENTS(reflected_executable_script_branches_table)
    };

    add_test_for_fixture_with_data("/gjs/debug/reflected_script/branches",
                                   &reflected_script_mock_script_fixture,
                                   &reflected_script_branches_data,
                                   test_reflected_script_has_expected_branches);
}
