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
#ifndef _GJS_REFLECTED_SCRIPT_H
#define _GJS_REFLECTED_SCRIPT_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GJS_REFLECTED_SCRIPT_INTERFACE(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), \
                                             GJS_TYPE_REFLECTED_SCRIPT_INTERFACE, \
                                             GjsReflectedScript))
#define GJS_REFLECTED_SCRIPT_GET_INTERFACE(obj) (G_TYPE_INSTANCE_GET_INTERFACE(obj, \
                                                                               GJS_TYPE_REFLECTED_SCRIPT_INTERFACE, \
                                                                               GjsReflectedScriptInterface))
#define GJS_TYPE_REFLECTED_SCRIPT_INTERFACE (gjs_reflected_script_interface_get_type())

typedef struct _GjsReflectedScriptInterface GjsReflectedScriptInterface;
typedef struct _GjsReflectedScript GjsReflectedScript;

typedef struct _GjsReflectedScriptBranchInfo GjsReflectedScriptBranchInfo;

unsigned int gjs_reflected_script_branch_info_get_branch_point(const GjsReflectedScriptBranchInfo *info);
const unsigned int * gjs_reflected_script_branch_info_get_branch_alternatives(const GjsReflectedScriptBranchInfo *info,
                                                                              unsigned int                       *n);

struct _GjsReflectedScriptInterface {
    GTypeInterface parent;

    const char **                         (*functions)        (GjsReflectedScript *script);
    const GjsReflectedScriptBranchInfo ** (*branches)         (GjsReflectedScript *script);
    const unsigned int *                  (*executable_lines) (GjsReflectedScript *script,
                                                               unsigned int       *n_executable_lines);
    unsigned int                          (*n_lines)          (GjsReflectedScript *script);
};

const char ** gjs_reflected_script_functions(GjsReflectedScript *script);
const unsigned int * gjs_reflected_script_executable_lines(GjsReflectedScript *script,
                                                           unsigned int       *n_executable_lines);
const GjsReflectedScriptBranchInfo ** gjs_reflected_script_branches(GjsReflectedScript *script);
unsigned int gjs_reflected_script_n_lines(GjsReflectedScript *script);

GType gjs_reflected_script_interface_get_type(void);

G_END_DECLS

#endif
