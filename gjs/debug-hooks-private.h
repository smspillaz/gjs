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
#ifndef GJS_INTERRUPT_REGISTER_PRIVATE_H
#define GJS_INTERRUPT_REGISTER_PRIVATE_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct _GjsInterruptInfo GjsInterruptInfo;
typedef struct _GjsReflectedScript GjsReflectedScript;
typedef enum _GjsFrameState GjsFrameState;

struct _GjsInterruptInfo {
    char         *filename;
    unsigned int line;
    char         *function_name;
};

struct _GjsFrameInfo {
    GjsInterruptInfo interrupt;
    GjsFrameState    frame_state;
};

struct _GjsDebugScriptInfo {
    const char         *filename;
    GjsReflectedScript *reflected_script;
    unsigned int       begin_line;
};

G_END_DECLS

#endif
