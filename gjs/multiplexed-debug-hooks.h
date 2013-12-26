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
#ifndef GJS_MULTIPLEXED_DEBUG_HOOKS_H
#define GJS_MULTIPLEXED_DEBUG_HOOKS_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GJS_TYPE_MULTIPLEXED_DEBUG_HOOKS gjs_multiplexed_debug_hooks_get_type()

#define GJS_MULTIPLEXED_DEBUG_HOOKS(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST((obj), \
     GJS_TYPE_MULTIPLEXED_DEBUG_HOOKS, GjsMultiplexedDebugHooks))

#define GJS_MULTIPLEXED_DEBUG_HOOKS_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_CAST((klass), \
     GJS_TYPE_MULTIPLEXED_DEBUG_HOOKS, GjsMultiplexedDebugHooksClass))

#define GJS_IS_MULTIPLEXED_DEBUG_HOOKS(obj) \
    (G_TYPE_CHECK_INSTANCE_TYPE ((obj), \
     GJS_TYPE_MULTIPLEXED_DEBUG_HOOKS))

#define GJS_IS_MULTIPLEXED_DEBUG_HOOKS_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_TYPE ((klass), \
     GJS_TYPE_MULTIPLEXED_DEBUG_HOOKS))

#define GJS_MULTIPLEXED_DEBUG_HOOKS_GET_CLASS(obj) \
    (G_TYPE_INSTANCE_GET_CLASS ((obj), \
     GJS_TYPE_MULTIPLEXED_DEBUG_HOOKS, GjsMultiplexedDebugHooksClass))

typedef struct _GjsMultiplexedDebugHooks GjsMultiplexedDebugHooks;
typedef struct _GjsMultiplexedDebugHooksClass GjsMultiplexedDebugHooksClass;
typedef struct _GjsMultiplexedDebugHooksPrivate GjsMultiplexedDebugHooksPrivate;

typedef struct _GjsContext GjsContext;

struct _GjsMultiplexedDebugHooksClass {
    GObjectClass parent_class;
};

struct _GjsMultiplexedDebugHooks {
    GObject parent;
};

GType gjs_multiplexed_debug_hooks_get_type(void);

GjsMultiplexedDebugHooks * gjs_multiplexed_debug_hooks_new(GjsContext *context);

G_END_DECLS

#endif
