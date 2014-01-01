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
#ifndef GJS_REFLECTED_EXECUTABLE_SCRIPT_H
#define GJS_REFLECTED_EXECUTABLE_SCRIPT_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GJS_TYPE_REFLECTED_EXECUTABLE_SCRIPT gjs_reflected_executable_script_get_type()

#define GJS_REFLECTED_EXECUTABLE_SCRIPT(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST((obj), \
     GJS_TYPE_REFLECTED_EXECUTABLE_SCRIPT, GjsReflectedExecutableScript))

#define GJS_REFLECTED_EXECUTABLE_SCRIPT_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_CAST((klass), \
     GJS_TYPE_REFLECTED_EXECUTABLE_SCRIPT, GjsReflectedExecutableScriptClass))

#define GJS_IS_REFLECTED_EXECUTABLE_SCRIPT(obj) \
    (G_TYPE_CHECK_INSTANCE_TYPE ((obj), \
     GJS_TYPE_REFLECTED_EXECUTABLE_SCRIPT))

#define GJS_IS_REFLECTED_EXECUTABLE_SCRIPT_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_TYPE ((klass), \
     GJS_TYPE_REFLECTED_EXECUTABLE_SCRIPT))

#define GJS_REFLECTED_EXECUTABLE_SCRIPT_GET_CLASS(obj) \
    (G_TYPE_INSTANCE_GET_CLASS ((obj), \
     GJS_TYPE_REFLECTED_EXECUTABLE_SCRIPT, GjsReflectedExecutableScriptClass))

typedef struct _GjsReflectedExecutableScript GjsReflectedExecutableScript;
typedef struct _GjsReflectedExecutableScriptClass GjsReflectedExecutableScriptClass;
typedef struct _GjsReflectedExecutableScriptPrivate GjsReflectedExecutableScriptPrivate;

typedef struct _GjsContext GjsContext;

struct _GjsReflectedExecutableScriptClass {
    GObjectClass parent_class;
};

struct _GjsReflectedExecutableScript {
    GObject parent;
};

GType gjs_reflected_executable_script_get_type(void);

GjsReflectedExecutableScript * gjs_reflected_executable_script_new(const char *filename);

G_END_DECLS

#endif
