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
#include <gjs/reflected-script.h>
#include "reflected-script-private.h"

static void gjs_reflected_script_interface_default_init(GjsReflectedScriptInterface *settings_interface);

G_DEFINE_INTERFACE(GjsReflectedScript, gjs_reflected_script_interface, G_TYPE_OBJECT);

static void
gjs_reflected_script_interface_default_init(GjsReflectedScriptInterface *interface)
{
}

const char **
gjs_reflected_script_functions(GjsReflectedScript *script)
{
    g_return_val_if_fail(script, NULL);

    return GJS_REFLECTED_SCRIPT_GET_INTERFACE(script)->functions(script);
}

const GjsReflectedScriptBranchInfo **
gjs_reflected_script_branches(GjsReflectedScript *script)
{
    g_return_val_if_fail(script, NULL);

    return GJS_REFLECTED_SCRIPT_GET_INTERFACE(script)->branches(script);
}

unsigned int
gjs_reflected_script_n_lines(GjsReflectedScript *script)
{
    g_return_val_if_fail(script, 0);

    return GJS_REFLECTED_SCRIPT_GET_INTERFACE(script)->n_lines(script);
}

const unsigned int *
gjs_reflected_script_executable_lines(GjsReflectedScript *script,
                                      unsigned int       *n)
{
    g_return_val_if_fail(script, NULL);
    g_return_val_if_fail(n, NULL);

    return GJS_REFLECTED_SCRIPT_GET_INTERFACE(script)->executable_lines(script,
                                                                        n);
}

GjsReflectedScriptBranchInfo *
gjs_reflected_script_branch_info_new(unsigned int  branch_point,
                                     GArray       *alternatives)
{
    GjsReflectedScriptBranchInfo *info = g_new0(GjsReflectedScriptBranchInfo, 1);
    info->branch_point = branch_point;
    info->branch_alternatives = alternatives;
    return info;
}

void
gjs_reflected_script_branch_info_destroy(gpointer info_data)
{
    GjsReflectedScriptBranchInfo *info = (GjsReflectedScriptBranchInfo *) info_data;
    g_array_free(info->branch_alternatives, TRUE);
    g_free(info);
}

unsigned int
gjs_reflected_script_branch_info_get_branch_point(const GjsReflectedScriptBranchInfo *info)
{
    return info->branch_point;
}

const unsigned int *
gjs_reflected_script_branch_info_get_branch_alternatives(const GjsReflectedScriptBranchInfo *info,
                                                         unsigned int                       *n)
{
    g_return_val_if_fail(n, NULL);

    *n = info->branch_alternatives->len;
    return (unsigned int *) info->branch_alternatives->data;
}
