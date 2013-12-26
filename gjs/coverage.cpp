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
#include <stdio.h>
#include <string.h>
#include <gio/gio.h>
#include <gjs/gjs.h>
#include <gjs/debug-hooks.h>
#include <gjs/debug-connection.h>
#include <gjs/reflected-script.h>
#include <gjs/reflected-executable-script.h>
#include <gjs/coverage.h>

typedef struct _GjsCoverageBranchData GjsCoverageBranchData;

struct _GjsCoveragePrivate {
    GHashTable           *file_statistics;
    GjsDebugHooks        *debug_hooks;
    GjsContext           *context;
    gchar                **covered_paths;
    GjsDebugConnection   *new_scripts_connection;
    GjsDebugConnection   *single_step_connection;
    GjsDebugConnection   *function_calls_and_execution_connection;

    /* If we hit a branch and the next single-step line will
     * activate one of the branch alternatives then this will
     * be set to that branch
     *
     * XXX: This isn't necessarily safe in the presence of
     * multiple execution contexts which are connected
     * to the same GjsCoveragePrivate's single step hook */
    GjsCoverageBranchData *active_branch;
};

G_DEFINE_TYPE_WITH_PRIVATE(GjsCoverage,
                           gjs_coverage,
                           G_TYPE_OBJECT)

enum {
    PROP_0,
    PROP_DEBUG_HOOKS,
    PROP_CONTEXT,
    PROP_COVERAGE_PATHS,
    PROP_N
};

static GParamSpec *properties[PROP_N] = { NULL, };

struct _GjsCoverageBranchData {
    const GjsReflectedScriptBranchInfo *info;
    GArray                             *branch_alternatives_taken;
    gboolean                           branch_hit;
};

static void
gjs_coverage_branch_info_init(GjsCoverageBranchData              *data,
                              const GjsReflectedScriptBranchInfo *info)
{
    g_assert(data->info == NULL);
    g_assert(data->branch_alternatives_taken == NULL);

    unsigned int n_branches;
    gjs_reflected_script_branch_info_get_branch_alternatives(info, &n_branches);

    data->info = info;
    data->branch_alternatives_taken =
        g_array_new(FALSE, TRUE, sizeof(unsigned int));
    g_array_set_size(data->branch_alternatives_taken, n_branches);
    data->branch_hit = FALSE;
}

static void
gjs_coverage_branch_info_clear(gpointer data_ptr)
{
    GjsCoverageBranchData *data = (GjsCoverageBranchData *) data_ptr;

    if (data->branch_alternatives_taken) {
        g_array_unref(data->branch_alternatives_taken);
        data->branch_alternatives_taken = NULL;
    }
}

typedef struct _GjsCoverageFileStatistics {
    /* 1-1 with line numbers for O(N) lookup */
    GArray     *lines;
    GArray     *branches;

    /* Hash buckets for O(logn) lookup */
    GHashTable *functions;
} GjsCoverageFileStatistics;

GjsCoverageFileStatistics *
gjs_coverage_file_statistics_new(GArray *all_lines,
                                 GArray *all_branches,
                                 GHashTable *all_functions)
{
    GjsCoverageFileStatistics *file_stats = g_new0(GjsCoverageFileStatistics, 1);
    file_stats->lines = all_lines;
    file_stats->branches = all_branches;
    file_stats->functions = all_functions;
    return file_stats;
}

void
gjs_coverage_file_statistics_destroy(gpointer data)
{
    GjsCoverageFileStatistics *file_stats = (GjsCoverageFileStatistics *) data;
    g_array_unref(file_stats->lines);
    g_array_unref(file_stats->branches);
    g_hash_table_unref(file_stats->functions);
    g_free(file_stats);
}

static void
increment_line_hits(GArray       *line_counts,
                    unsigned int  line_no)
{
    g_assert(line_no <= line_counts->len);

    /* If this happens it is not a huge problem - we only try to
     * filter out lines which we think are not executable so
     * that they don't cause execess noise in coverage reports */
    int *line_hit_count = &(g_array_index(line_counts, int, line_no));

    if (*line_hit_count == -1)
        *line_hit_count = 0;

    ++(*line_hit_count);
}

static void
increment_hits_on_branch(GjsCoverageBranchData *branch,
                         unsigned int           line)
{
    if (!branch)
        return;

    unsigned int n_branch_alternatives;
    const unsigned int *branch_alternatives =
        gjs_reflected_script_branch_info_get_branch_alternatives(branch->info,
                                                                 &n_branch_alternatives);

    g_assert (n_branch_alternatives == branch->branch_alternatives_taken->len);

    unsigned int i;
    for (i = 0; i < n_branch_alternatives; ++i) {
        if (branch_alternatives[i] == line) {
            unsigned int *hit_count = &(g_array_index(branch->branch_alternatives_taken,
                                                      unsigned int,
                                                      i));
            ++(*hit_count);
        }
    }
}

/* Return a valid GjsCoverageBranchData if this line actually
 * contains a valid branch (eg GjsReflectedScriptBranchInfo is set) */
static GjsCoverageBranchData *
find_active_branch(GArray       *branches,
                   unsigned int  line)
{
    g_assert(line <= branches->len);

    GjsCoverageBranchData *branch = &(g_array_index(branches, GjsCoverageBranchData, line));
    if (branch->info) {
        branch->branch_hit = TRUE;
        return branch;
    }

    return NULL;
}

static void
gjs_coverage_single_step_interrupt_hook(GjsDebugHooks    *hooks,
                                        GjsContext       *context,
                                        GjsInterruptInfo *info,
                                        gpointer          user_data)
{
    GjsCoverage *coverage = (GjsCoverage *) user_data;
    GjsCoveragePrivate *priv = (GjsCoveragePrivate *) gjs_coverage_get_instance_private(coverage);

    const char *filename = gjs_interrupt_info_get_filename(info);
    unsigned int line_no = gjs_interrupt_info_get_line(info);
    GHashTable  *statistics_table = priv->file_statistics;
    GjsCoverageFileStatistics *statistics =
        (GjsCoverageFileStatistics *) g_hash_table_lookup(statistics_table,
                                                          filename);
    /* We don't care about this file, even if we're single-stepping it */
    if (!statistics)
        return;

    /* Line counters */
    increment_line_hits(statistics->lines, line_no);

    /* Branch counters. First increment branch hits for the active
     * branch and then find a new potentially active branch */
    increment_hits_on_branch(priv->active_branch, line_no);
    priv->active_branch = find_active_branch(statistics->branches, line_no);
}

static void
gjs_coverage_function_calls_and_execution_hook(GjsDebugHooks *hooks,
                                               GjsContext    *context,
                                               GjsFrameInfo  *info,
                                               gpointer       user_data)
{
    /* We don't care about after-hits */
    if (gjs_frame_info_get_state(info) != GJS_INTERRUPT_FRAME_BEFORE)
        return;

    GHashTable *all_statistics = (GHashTable *) user_data;
    GjsCoverageFileStatistics *file_statistics =
        (GjsCoverageFileStatistics *) g_hash_table_lookup(all_statistics,
                                                          gjs_interrupt_info_get_filename((GjsInterruptInfo *) info));

    /* We don't care about this script */
    if (!file_statistics)
        return;

    const char *function_name =
        gjs_interrupt_info_get_function_name((GjsInterruptInfo *) info);

    /* It is not a critical error if we hit this condition. We just won't
     * log calls for that function (which we couldn't find via reflection).
     *
     * The reason is that there may be cases on the execution hook where we
     * can't determine a function name and need to assign one based on the
     * script line-number. We do that for anonymous functions but also
     * on general toplevel script execution */
    if (!g_hash_table_contains(file_statistics->functions, function_name)) {
        return;
    }

    unsigned int hit_count = GPOINTER_TO_INT(g_hash_table_lookup(file_statistics->functions, function_name));
    ++hit_count;

    /* The GHashTable API requires that we copy the key again, in both the
     * insert and replace case */
    g_hash_table_replace(file_statistics->functions,
                         g_strdup(function_name),
                         GINT_TO_POINTER(hit_count));
}

/*
 * The created array is a 1-1 representation of the hitcount in the filename. Each
 * element refers to an individual line. In order to avoid confusion, our array
 * is zero indexed, but the zero'th line is always ignored and the first element
 * refers to the first line of the file.
 *
 * A value of -1 for an element means that the line is non-executable and never actually
 * reached. A value of 0 means that it was executable but never reached. A positive value
 * indicates the hit count.
 *
 * The reason for using a 1-1 mapping as opposed to an array of key-value pairs for executable
 * lines is:
 *   1. Lookup speed is O(1) instead of O(log(n))
 *   2. There's a possibility we might hit a line which we thought was non-executable, in which
 *      case we can neatly handle the error by marking that line executable. A hit on a line
 *      we thought was non-executable is not as much of a problem as noise generated by
 *      ostensible "misses" which could in fact never be executed.
 *
 */
static GArray *
create_line_coverage_statistics_from_reflection(GjsReflectedScript *reflected_script)
{
    unsigned int line_count = gjs_reflected_script_n_lines(reflected_script);
    GArray *line_statistics = g_array_new(TRUE, FALSE, sizeof(int));
    g_array_set_size(line_statistics, line_count);

    if (line_count)
        memset(line_statistics->data, -1, sizeof(int) * line_statistics->len);

    unsigned int       n_executable_lines;
    const unsigned int *executable_lines =
        gjs_reflected_script_executable_lines(reflected_script,
                                              &n_executable_lines);

    /* In order to determine which lines are executable to start off with, we should take
     * the array of executable lines provided to us with gjs_debug_script_info_get_executable_lines
     * and change the array value of each line to zero. If these lines are never executed then
     * they will be considered a coverage miss */
    if (executable_lines) {
        unsigned int i;
        for (i = 0; i < n_executable_lines; ++i)
            g_array_index(line_statistics, int, executable_lines[i]) = 0;
    }

    return line_statistics;
}

/* Again we are creating a 1-1 representation of script lines to potential branches
 * where each element refers to a 1-index line (with the zero'th ignored).
 *
 * Each element is a GjsCoverageBranchData which, if the line at the element
 * position describes a branch, will be populated with a GjsReflectedScriptBranchInfo
 * and an array of unsigned each specifying the hit-count for each potential branch
 * in the branch info */
static GArray *
create_branch_coverage_statistics_from_reflection(GjsReflectedScript *reflected_script)
{
    unsigned int line_count = gjs_reflected_script_n_lines(reflected_script);
    GArray *branch_statistics = g_array_new(FALSE, TRUE, sizeof(GjsCoverageBranchData));
    g_array_set_size(branch_statistics, line_count);
    g_array_set_clear_func(branch_statistics, gjs_coverage_branch_info_clear);

    const GjsReflectedScriptBranchInfo **branch_info_iterator =
        gjs_reflected_script_branches(reflected_script);

    if (*branch_info_iterator) {
        do {
            unsigned int branch_point =
                gjs_reflected_script_branch_info_get_branch_point(*branch_info_iterator);

            g_assert(branch_point <= branch_statistics->len);

            GjsCoverageBranchData *branch_data (&(g_array_index(branch_statistics,
                                                                GjsCoverageBranchData,
                                                                branch_point)));
            gjs_coverage_branch_info_init(branch_data, *branch_info_iterator);
        } while (*(++branch_info_iterator));
    }

    return branch_statistics;
}

static GHashTable *
create_function_coverage_statistics_from_reflection(GjsReflectedScript *reflected_script)
{
    GHashTable *functions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    const char **function_names_iterator =
        gjs_reflected_script_functions(reflected_script);

    if (*function_names_iterator) {
        do {
            g_hash_table_insert(functions, g_strdup(*function_names_iterator), GINT_TO_POINTER(0));
        } while (*(++function_names_iterator));
    }

    return functions;
}

static GjsCoverageFileStatistics *
create_statistics_from_reflection(GjsReflectedScript *reflected_script)
{
    GArray *line_coverage_statistics =
        create_line_coverage_statistics_from_reflection(reflected_script);
    GArray *branch_coverage_statistics =
        create_branch_coverage_statistics_from_reflection(reflected_script);
    GHashTable *function_coverage_statistics =
        create_function_coverage_statistics_from_reflection(reflected_script);

    g_assert(line_coverage_statistics);
    g_assert(branch_coverage_statistics);
    g_assert(function_coverage_statistics);

    return gjs_coverage_file_statistics_new(line_coverage_statistics,
                                            branch_coverage_statistics,
                                            function_coverage_statistics);

}

static void
gjs_coverage_new_script_available_hook(GjsDebugHooks      *reg,
                                       GjsContext         *context,
                                       GjsDebugScriptInfo *info,
                                       gpointer            user_data)
{
    const gchar *filename = gjs_debug_script_info_get_filename(info);
    GHashTable  *file_statistics = (GHashTable *) user_data;
    if (g_hash_table_contains(file_statistics, filename)) {
        GjsReflectedScript *reflected_script = gjs_debug_script_info_get_reflection(info);
        GjsCoverageFileStatistics *statistics =
            (GjsCoverageFileStatistics *) g_hash_table_lookup(file_statistics,
                                                              filename);

        /* No current value exists, open the file and create statistics for
         * it now that we have the number of executable lines for this file */
        if (!statistics) {
            statistics = create_statistics_from_reflection(reflected_script);

            /* If create_statistics_for_filename returns NULL then we can
             * just bail out here, the stats print function will handle
             * the NULL case */
            if (!statistics)
                return;

            g_hash_table_insert(file_statistics,
                                g_strdup(filename),
                                statistics);
        }
    }
}

static void
write_string_into_stream(GOutputStream *stream,
                         const gchar   *string)
{
    g_output_stream_write(stream, (gconstpointer) string, strlen(string) * sizeof(gchar), NULL, NULL);
}

static void
write_source_file_header(GOutputStream *stream,
                         const gchar   *source_file_path)
{
    write_string_into_stream(stream, "SF:");
    write_string_into_stream(stream, source_file_path);
    write_string_into_stream(stream, "\n");
}

static void
write_function_foreach_func(gpointer key,
                            gpointer value,
                            gpointer user_data)
{
    GOutputStream *stream = (GOutputStream *) user_data;
    const char    *function_name = (const char *) key;

    write_string_into_stream(stream, "FN:");
    write_string_into_stream(stream, function_name);
    write_string_into_stream(stream, "\n");
}

typedef struct _FunctionHitCountData {
    GOutputStream *stream;
    unsigned int  *n_functions_found;
    unsigned int  *n_functions_hit;
} FunctionHitCountData;

static void
write_function_hit_count_foreach_func(gpointer key,
                                      gpointer value,
                                      gpointer user_data)
{
    FunctionHitCountData *data = (FunctionHitCountData *) user_data;
    GOutputStream *stream = data->stream;
    const char    *function_name = (const char *) key;
    unsigned int  hit_count = GPOINTER_TO_INT(value);

    char *line = g_strdup_printf("FNDA:%i,%s\n",
                                 hit_count,
                                 function_name);

    (*data->n_functions_found)++;

    if (hit_count > 0)
        (*data->n_functions_hit)++;

    write_string_into_stream(stream, line);
    g_free(line);
}

static void
write_functions_hit_counts(GOutputStream *stream,
                           GHashTable    *functions,
                           unsigned int  *n_functions_found,
                           unsigned int  *n_functions_hit)
{
    FunctionHitCountData data = {
        stream,
        n_functions_found,
        n_functions_hit
    };

    g_hash_table_foreach(functions,
                         write_function_hit_count_foreach_func,
                         &data);
}

static void
write_functions(GOutputStream *data_stream,
                GHashTable    *functions)
{
    g_hash_table_foreach(functions, write_function_foreach_func, data_stream);
}

static void
write_uint32_into_stream(GOutputStream *stream,
                         unsigned int   integer)
{
    char buf[32];
    g_snprintf(buf, 32, "%u", integer);
    g_output_stream_write(stream, (gconstpointer) buf, strlen(buf) * sizeof(char), NULL, NULL);
}

static void
write_int32_into_stream(GOutputStream *stream,
                        int            integer)
{
    char buf[32];
    g_snprintf(buf, 32, "%i", integer);
    g_output_stream_write(stream, (gconstpointer) buf, strlen(buf) * sizeof(char), NULL, NULL);
}

static void
write_function_coverage(GOutputStream *data_stream,
                        unsigned int  n_found_functions,
                        unsigned int  n_hit_functions)
{
    write_string_into_stream(data_stream, "FNF:");
    write_uint32_into_stream(data_stream, n_found_functions);
    write_string_into_stream(data_stream, "\n");

    write_string_into_stream(data_stream, "FNH:");
    write_uint32_into_stream(data_stream, n_hit_functions);
    write_string_into_stream(data_stream, "\n");
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

typedef struct _WriteAlternativeData {
    unsigned int  *n_branch_alternatives_found;
    unsigned int  *n_branch_alternatives_hit;
    GOutputStream *output_stream;
    gpointer      *all_alternatives;
    gboolean      branch_point_was_hit;
} WriteAlternativeData;

typedef struct _WriteBranchInfoData {
    unsigned int *n_branch_alternatives_found;
    unsigned int *n_branch_alternatives_hit;
    GOutputStream *output_stream;
} WriteBranchInfoData;

static void
write_individual_branch(gpointer branch_ptr,
                        gpointer user_data)
{
    GjsCoverageBranchData *branch = (GjsCoverageBranchData *) branch_ptr;
    WriteBranchInfoData   *data = (WriteBranchInfoData *) user_data;

    /* This line is not a branch, don't write anything */
    if (!branch->info)
        return;

    unsigned int          n_alternatives;
    const unsigned int    *branch_alternative_lines =
        gjs_reflected_script_branch_info_get_branch_alternatives(branch->info,
                                                                 &n_alternatives);

    unsigned int i = 0;
    for (; i < branch->branch_alternatives_taken->len; ++i) {
        unsigned int alternative_counter = g_array_index(branch->branch_alternatives_taken,
                                                         unsigned int,
                                                         i);
        unsigned int branch_point =
            gjs_reflected_script_branch_info_get_branch_point(branch->info);
        char         *hit_count_string = NULL;

        if (!branch->branch_hit)
            hit_count_string = g_strdup_printf("-");
        else
            hit_count_string = g_strdup_printf("%i", alternative_counter);

        char *branch_alternative_line = g_strdup_printf("BRDA:%i,0,%i,%s\n",
                                                        branch_point,
                                                        i,
                                                        hit_count_string);

        write_string_into_stream(data->output_stream, branch_alternative_line);
        g_free(hit_count_string);
        g_free(branch_alternative_line);

        ++(*data->n_branch_alternatives_found);

        if (alternative_counter > 0)
            ++(*data->n_branch_alternatives_hit);
    }
}

static void
write_branch_coverage(GOutputStream *stream,
                      GArray        *branches,
                      unsigned int  *n_branch_alternatives_found,
                      unsigned int  *n_branch_alternatives_hit)

{
    /* Write individual branches and pass-out the totals */
    WriteBranchInfoData data = {
        n_branch_alternatives_found,
        n_branch_alternatives_hit,
        stream
    };

    for_each_element_in_array(branches,
                              write_individual_branch,
                              &data);
}

static void
write_branch_totals(GOutputStream *stream,
                    unsigned int   n_branch_alternatives_found,
                    unsigned int   n_branch_alternatives_hit)
{
    write_string_into_stream(stream, "BRF:");
    write_uint32_into_stream(stream, n_branch_alternatives_found);
    write_string_into_stream(stream, "\n");

    write_string_into_stream(stream, "BRH:");
    write_uint32_into_stream(stream, n_branch_alternatives_hit);
    write_string_into_stream(stream, "\n");
}

static void
write_line_coverage(GOutputStream *stream,
                    GArray        *stats,
                    unsigned int  *lines_hit_count,
                    unsigned int  *executable_lines_count)
{
    unsigned int i = 0;
    for (i = 0; i < stats->len; ++i) {
        int hit_count_for_line = g_array_index(stats, int, i);

        if (hit_count_for_line == -1)
            continue;

        write_string_into_stream(stream, "DA:");
        write_uint32_into_stream(stream, i);
        write_string_into_stream(stream, ",");
        write_int32_into_stream(stream, hit_count_for_line);
        write_string_into_stream(stream, "\n");

        if (hit_count_for_line > 0)
          ++(*lines_hit_count);

        ++(*executable_lines_count);
    }
}

static void
write_line_totals(GOutputStream *stream,
                  unsigned int   lines_hit_count,
                  unsigned int   executable_lines_count)
{
    write_string_into_stream(stream, "LH:");
    write_uint32_into_stream(stream, lines_hit_count);
    write_string_into_stream(stream, "\n");

    write_string_into_stream(stream, "LF:");
    write_uint32_into_stream(stream, executable_lines_count);
    write_string_into_stream(stream, "\n");
}

static void
write_end_of_record(GOutputStream *stream)
{
    write_string_into_stream(stream, "end_of_record\n");
}

static GFileOutputStream *
delete_file_and_open_anew(GFile *file, GError **error)
{
    return g_file_replace(file,
                          NULL,
                          FALSE,
                          G_FILE_CREATE_REPLACE_DESTINATION,
                          NULL,
                          error);
}

static GFileOutputStream *
open_file_for_appending(GFile *file, GError **error)
{
    g_return_val_if_fail(file, NULL);
    g_return_val_if_fail(error, NULL);

    /* First try to create the file if it does not already exist,
     * if it does then we should open it readwrite */
    GFileOutputStream *ostream = g_file_create(file,
                                               G_FILE_CREATE_NONE,
                                               NULL,
                                               error);
    if (*error) {
        if ((*error)->code == G_IO_ERROR_EXISTS) {
            g_clear_error(error);
            ostream = g_file_append_to(file,
                                       G_FILE_CREATE_NONE,
                                       NULL,
                                       error);
            if (*error)
                return NULL;
        }
    }

    return ostream;
}

static GFile *
create_tracefile_for_script_name(const char *script_name)
{
    gsize tracefile_name_buffer_size = strlen(script_name) + 8;
    char  tracefile_name_buffer[tracefile_name_buffer_size];
    snprintf(tracefile_name_buffer,
             tracefile_name_buffer_size,
             "%s.info",
             (const char *) script_name);

    return g_file_new_for_path(tracefile_name_buffer);
}

static void
seek_to_end(GFileOutputStream *ostream)
{
    GError    *error = NULL;
    GSeekable *seekable = G_SEEKABLE(ostream);
    g_return_if_fail(g_seekable_can_seek(seekable));

    if (!g_seekable_seek(seekable, 0, (GSeekType) G_SEEK_END, NULL, &error))
        g_print("Failed to seek %s to end position : %s\n",
                "unknown",
                error->message);
}

static GOutputStream *
get_appropriate_tracefile_ref(GFileOutputStream *specified_tracefile,
                              const char        *script_name,
                              gboolean          accumulate_coverage)
{
    /* If we provided a tracefile, then just increase the refcount. It will
     * be unreferenced later. We want to make sure we're at the end
     * of the stream too */
    if (specified_tracefile) {
        GFileOutputStream *ostream = (GFileOutputStream *) g_object_ref(specified_tracefile);
        seek_to_end(ostream);
        return G_OUTPUT_STREAM(ostream);
    }

    /* We need to create a tracefile for the script with a reference count
     * of one */
    GError *error = NULL;
    GFile *script_tracefile = create_tracefile_for_script_name(script_name);
    GFileOutputStream *ostream = NULL;

    if (accumulate_coverage) {
        ostream = open_file_for_appending(script_tracefile, &error);

        if (ostream)
            seek_to_end(ostream);
    } else {
        ostream = delete_file_and_open_anew(script_tracefile, &error);
    }

    if (!ostream) {
        char *path = g_file_get_path(script_tracefile);
        g_critical("Failed to open %s for writing: %s",
                   path,
                   error->message);
        g_free(path);
        g_error_free(error);
    }

    /* Unreferencing the tracefile is safe here as the
     * underlying GFileOutputStream does not have
     * an observing reference to it */
    g_object_unref(script_tracefile);

    return G_OUTPUT_STREAM(ostream);
}

static GjsCoverageFileStatistics *
insert_zeroed_statistics_for_unexecuted_file(GjsContext *context,
                                             const char *filename)
{
    GjsReflectedExecutableScript *reflected_script =
        gjs_reflected_executable_script_new(filename);
    GjsCoverageFileStatistics *stats =
        create_statistics_from_reflection(GJS_REFLECTED_SCRIPT_INTERFACE(reflected_script));
    g_object_unref(reflected_script);

    return stats;
}

typedef struct _StatisticsPrintUserData {
    GjsContext        *context;
    GFileOutputStream *specified_ostream;
    gboolean          accumulate_coverage;
} StatisticsPrintUserData;

static void
print_statistics_for_files(gpointer key,
                           gpointer value,
                           gpointer user_data)
{
    StatisticsPrintUserData   *statistics_print_data = (StatisticsPrintUserData *) user_data;
    const char                *filename = (const char *) key;
    GjsCoverageFileStatistics *stats = (GjsCoverageFileStatistics *) value;

    /* If there is no statistics for this file, then we should
     * compile the script and print statistics for it now */
    if (!stats)
        stats = insert_zeroed_statistics_for_unexecuted_file(statistics_print_data->context,
                                                             filename);

    /* Still couldn't create statistics, bail out */
    if (!stats)
        return;

    /* get_appropriate_tracefile_ref will automatically set the write
     * pointer to the correct place in the file */
    GOutputStream *ostream = get_appropriate_tracefile_ref(statistics_print_data->specified_ostream,
                                                           filename,
                                                           statistics_print_data->accumulate_coverage);

    /* get_appropriate_tracefile_ref already prints a critical here if it
     * fails, just early-return if it does */
    if (!ostream)
        return;

    write_source_file_header(ostream, (const char *) key);
    write_functions(ostream,
                    stats->functions);

    unsigned int functions_hit_count = 0;
    unsigned int functions_found_count = 0;

    write_functions_hit_counts(ostream,
                               stats->functions,
                               &functions_found_count,
                               &functions_hit_count);
    write_function_coverage(ostream,
                            functions_found_count,
                            functions_hit_count);

    unsigned int branches_hit_count = 0;
    unsigned int branches_found_count = 0;

    write_branch_coverage(ostream,
                          stats->branches,
                          &branches_found_count,
                          &branches_hit_count);
    write_branch_totals(ostream,
                        branches_found_count,
                        branches_hit_count);

    unsigned int lines_hit_count = 0;
    unsigned int executable_lines_count = 0;

    write_line_coverage(ostream,
                        stats->lines,
                        &lines_hit_count,
                        &executable_lines_count);
    write_line_totals(ostream,
                      lines_hit_count,
                      executable_lines_count);
    write_end_of_record(ostream);

    /* If value was initially NULL, then we should unref stats here */
    if (!value)
        gjs_coverage_file_statistics_destroy(stats);

    g_object_unref(ostream);
}

void
gjs_coverage_write_statistics(GjsCoverage *coverage,
                              GFile       *output_file,
                              gboolean     accumulate_coverage)
{
    GjsCoveragePrivate *priv = (GjsCoveragePrivate *) gjs_coverage_get_instance_private(coverage);
    GError *error = NULL;
    GFileOutputStream *ostream = NULL;

    /* Remove our new script hook so that we don't get spurious calls
     * to it whilst compiling new scripts */
    gjs_debug_connection_unregister(priv->new_scripts_connection);
    priv->new_scripts_connection = NULL;

    if (output_file) {
        if (accumulate_coverage)
            ostream = open_file_for_appending(output_file, &error);
        else
            ostream = delete_file_and_open_anew(output_file, &error);

        if (!ostream) {
            char *output_file_path = g_file_get_path(output_file);
            g_warning("Unable to open output file %s: %s",
                      output_file_path,
                      error->message);
            g_free(output_file_path);
            g_error_free(error);
        }
    }

    /* print_statistics_for_files can handle the NULL
     * case just fine, so there's no need to return if
     * output_file is NULL */
    StatisticsPrintUserData data = {
        priv->context,
        ostream,
        accumulate_coverage
    };

    g_hash_table_foreach(priv->file_statistics,
                         print_statistics_for_files,
                         &data);

    g_object_unref(ostream);

    /* Re-insert our new script hook in case we need it again */
    priv->new_scripts_connection =
        gjs_debug_hooks_connect_to_script_load(priv->debug_hooks,
                                               gjs_coverage_new_script_available_hook,
                                               priv->file_statistics);
}

static void
destroy_coverage_statistics_if_if_nonnull(gpointer statistics)
{
    if (statistics)
        gjs_coverage_file_statistics_destroy(statistics);
}

static void
gjs_coverage_init(GjsCoverage *self)
{
    GjsCoveragePrivate *priv = (GjsCoveragePrivate *) gjs_coverage_get_instance_private(self);
    priv->file_statistics = g_hash_table_new_full(g_str_hash,
                                                  g_str_equal,
                                                  g_free,
                                                  destroy_coverage_statistics_if_if_nonnull);
    priv->active_branch = NULL;
}

/* This function just adds a key with no value to the
 * filename statistics. We'll create a proper source file
 * map once we get a new script callback (to avoid lots
 * of recompiling) and also create a source map on
 * coverage data generation if we didn't already have one */
static void
add_filename_key_to_statistics(GFile      *file,
                               GHashTable *statistics)
{
    char *path = g_file_get_path(file);
    g_hash_table_insert(statistics, path, NULL);
}

static void
recursive_scan_for_potential_js_files(GFile    *node,
                                      gpointer  user_data)
{
    GHashTable      *statistics = (GHashTable *) user_data;
    GFileEnumerator *enumerator =
        g_file_enumerate_children(node,
                                  "standard::type,standard::name",
                                  G_FILE_QUERY_INFO_NONE,
                                  NULL,
                                  NULL);

    /* This isn't a directory and doesn't have children */
    if (!enumerator)
        return;

    GFileInfo *current_file = g_file_enumerator_next_file(enumerator, NULL, NULL);

    while (current_file) {
        GFile *child = g_file_enumerator_get_child(enumerator, current_file);
        if (g_file_info_get_file_type(current_file) == G_FILE_TYPE_DIRECTORY)
            recursive_scan_for_potential_js_files(child, user_data);
        else if (g_file_info_get_file_type(current_file) == G_FILE_TYPE_REGULAR) {
            const char *filename = g_file_info_get_name(current_file);

            if (g_str_has_suffix(filename, ".js"))
                add_filename_key_to_statistics(child, statistics);
        }

        g_object_unref(child);
        g_object_unref(current_file);
        current_file = g_file_enumerator_next_file(enumerator, NULL, NULL);
    }

    g_object_unref(enumerator);
}

static void
begin_recursive_scan_for_potential_js_files(const char *toplevel_path,
                                            gpointer    user_data)
{
    GFile *toplevel_file = g_file_new_for_path(toplevel_path);
    recursive_scan_for_potential_js_files(toplevel_file, user_data);
    g_object_unref(toplevel_file);
}

static void
gjs_coverage_constructed(GObject *object)
{
    G_OBJECT_CLASS(gjs_coverage_parent_class)->constructed(object);

    GjsCoverage *coverage = GJS_DEBUG_COVERAGE(object);
    GjsCoveragePrivate *priv = (GjsCoveragePrivate *) gjs_coverage_get_instance_private(coverage);

    /* Recursively scan the directories provided to us for files ending
    * with .js and add them to the coverage data hashtable */
    if (priv->covered_paths) {
        const char **iterator = (const char **) priv->covered_paths;

        do {
            begin_recursive_scan_for_potential_js_files(*iterator,
                                                        priv->file_statistics);
        } while (*(++iterator));
    }

    /* Add hook for new scripts and singlestep execution */
    priv->new_scripts_connection =
        gjs_debug_hooks_connect_to_script_load(priv->debug_hooks,
                                               gjs_coverage_new_script_available_hook,
                                               priv->file_statistics);

    priv->single_step_connection =
        gjs_debug_hooks_start_singlestep(priv->debug_hooks,
                                         gjs_coverage_single_step_interrupt_hook,
                                         coverage);

    priv->function_calls_and_execution_connection =
        gjs_debug_hooks_connect_to_function_calls_and_execution(priv->debug_hooks,
                                                                gjs_coverage_function_calls_and_execution_hook,
                                                                priv->file_statistics);
}

static void
gjs_coverage_set_property(GObject      *object,
                          unsigned int  prop_id,
                          const GValue *value,
                          GParamSpec   *pspec)
{
    GjsCoverage *coverage = GJS_DEBUG_COVERAGE(object);
    GjsCoveragePrivate *priv = (GjsCoveragePrivate *) gjs_coverage_get_instance_private(coverage);
    switch (prop_id) {
    case PROP_DEBUG_HOOKS:
        priv->debug_hooks = GJS_DEBUG_HOOKS_INTERFACE(g_value_dup_object(value));
        break;
    case PROP_CONTEXT:
        priv->context = GJS_CONTEXT (g_value_get_object (value));
        break;
    case PROP_COVERAGE_PATHS:
        g_assert(priv->covered_paths == NULL);
        priv->covered_paths = (char **) g_value_dup_boxed (value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void
gjs_coverage_dispose(GObject *object)
{
    GjsCoverage *coverage = GJS_DEBUG_COVERAGE (object);
    GjsCoveragePrivate *priv = (GjsCoveragePrivate *) gjs_coverage_get_instance_private(coverage);

    g_clear_object(&priv->debug_hooks);

    G_OBJECT_CLASS(gjs_coverage_parent_class)->dispose(object);
}

static void
gjs_coverage_finalize (GObject *object)
{
    GjsCoverage *coverage = GJS_DEBUG_COVERAGE(object);
    GjsCoveragePrivate *priv = (GjsCoveragePrivate *) gjs_coverage_get_instance_private(coverage);

    gjs_debug_connection_unregister(priv->new_scripts_connection);
    gjs_debug_connection_unregister(priv->single_step_connection);
    gjs_debug_connection_unregister(priv->function_calls_and_execution_connection);
    g_hash_table_unref(priv->file_statistics);
    g_strfreev(priv->covered_paths);

    G_OBJECT_CLASS(gjs_coverage_parent_class)->finalize(object);
}

static void
gjs_coverage_class_init (GjsCoverageClass *klass)
{
    GObjectClass *object_class = (GObjectClass *) klass;

    object_class->constructed = gjs_coverage_constructed;
    object_class->dispose = gjs_coverage_dispose;
    object_class->finalize = gjs_coverage_finalize;
    object_class->set_property = gjs_coverage_set_property;

    properties[PROP_DEBUG_HOOKS] = g_param_spec_object("debug-hooks",
                                                              "Debug Hooks",
                                                              "Debug Hooks",
                                                              GJS_TYPE_DEBUG_HOOKS_INTERFACE,
                                                              (GParamFlags) (G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE));
    properties[PROP_CONTEXT] = g_param_spec_object("context",
                                                   "Context",
                                                   "Running Context",
                                                   GJS_TYPE_CONTEXT,
                                                   (GParamFlags) (G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE));
    properties[PROP_COVERAGE_PATHS] = g_param_spec_boxed("coverage-paths",
                                                         "Coverage Paths",
                                                         "Paths (and included subdirectories) of which to perform coverage analysis",
                                                         G_TYPE_STRV,
                                                         (GParamFlags) (G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE));

    g_object_class_install_properties(object_class,
                                      PROP_N,
                                      properties);
}

/**
 * gjs_coverage_new:
 * @debug_hooks: (transfer full): A #GjsDebugHooks to register callbacks on
 * @context: (transfer full): A #GjsContext
 * @coverage_paths: (transfer none): A null-terminated strv of directories to generate
 * coverage_data for
 *
 * Returns: A #GjsDebugCoverage
 */
GjsCoverage *
gjs_coverage_new (GjsDebugHooks *debug_hooks,
                  GjsContext    *context,
                  const char    **coverage_paths)
{
    GjsCoverage *coverage =
        GJS_DEBUG_COVERAGE(g_object_new(GJS_TYPE_DEBUG_COVERAGE,
                                        "debug-hooks", debug_hooks,
                                        "context", context,
                                        "coverage-paths", coverage_paths,
                                        NULL));

    return coverage;
}
