/*
 * Copyright (C) 2019 Red Hat Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 */

#include "config.h"

#include "compositor/meta-compositor-x11.h"

#include <X11/extensions/shape.h>
#include <X11/extensions/Xcomposite.h>

#include "backends/x11/meta-backend-x11.h"
#include "clutter/x11/clutter-x11.h"
#include "compositor/meta-sync-ring.h"
#include "core/display-private.h"
#include "x11/meta-x11-display-private.h"

struct _MetaCompositorX11
{
  MetaCompositor parent;

  Window output;

  gboolean frame_has_updated_xsurfaces;
  gboolean have_x11_sync_object;

  MetaWindow *unredirected_window;
};

G_DEFINE_TYPE (MetaCompositorX11, meta_compositor_x11, META_TYPE_COMPOSITOR)

static void
process_damage (MetaCompositorX11  *compositor_x11,
                XDamageNotifyEvent *damage_xevent,
                MetaWindow         *window)
{
  MetaWindowActor *window_actor = meta_window_actor_from_window (window);

  meta_window_actor_process_x11_damage (window_actor, damage_xevent);

  compositor_x11->frame_has_updated_xsurfaces = TRUE;
}

void
meta_compositor_x11_process_xevent (MetaCompositorX11 *compositor_x11,
                                    XEvent            *xevent,
                                    MetaWindow        *window)
{
  MetaCompositor *compositor = META_COMPOSITOR (compositor_x11);
  MetaDisplay *display = meta_compositor_get_display (compositor);
  MetaX11Display *x11_display = display->x11_display;
  int damage_event_base;

  damage_event_base = meta_x11_display_get_damage_event_base (x11_display);
  if (xevent->type == damage_event_base + XDamageNotify)
    {
      /*
       * Core code doesn't handle damage events, so we need to extract the
       * MetaWindow ourselves.
       */
      if (!window)
        {
          Window xwindow;

          xwindow = ((XDamageNotifyEvent *) xevent)->drawable;
          window = meta_x11_display_lookup_x_window (x11_display, xwindow);
        }

      if (window)
        process_damage (compositor_x11, (XDamageNotifyEvent *) xevent, window);
    }

  if (compositor_x11->have_x11_sync_object)
    meta_sync_ring_handle_event (xevent);

  /*
   * Clutter needs to know about MapNotify events otherwise it will think the
   * stage is invisible
   */
  if (xevent->type == MapNotify)
    clutter_x11_handle_event (xevent);
}

static void
meta_compositor_x11_manage (MetaCompositor *compositor)
{
  MetaCompositorX11 *compositor_x11 = META_COMPOSITOR_X11 (compositor);
  MetaDisplay *display = meta_compositor_get_display (compositor);
  Display *xdisplay = meta_x11_display_get_xdisplay (display->x11_display);
  MetaBackend *backend = meta_get_backend ();
  Window xwindow;

  compositor_x11->output = display->x11_display->composite_overlay_window;

  xwindow = meta_backend_x11_get_xwindow (META_BACKEND_X11 (backend));

  XReparentWindow (xdisplay, xwindow, compositor_x11->output, 0, 0);

  meta_x11_display_clear_stage_input_region (display->x11_display);

  /*
   * Make sure there isn't any left-over output shape on the overlay window by
   * setting the whole screen to be an output region.
   *
   * Note: there doesn't seem to be any real chance of that because the X
   * server will destroy the overlay window when the last client using it
   * exits.
   */
  XFixesSetWindowShapeRegion (xdisplay, compositor_x11->output,
                              ShapeBounding, 0, 0, None);

  /*
   * Map overlay window before redirecting windows offscreen so we catch their
   * contents until we show the stage.
   */
  XMapWindow (xdisplay, compositor_x11->output);

  compositor_x11->have_x11_sync_object = meta_sync_ring_init (xdisplay);
}

static void
meta_compositor_x11_unmanage (MetaCompositor *compositor)
{
  MetaDisplay *display = meta_compositor_get_display (compositor);
  MetaX11Display *x11_display = display->x11_display;
  Display *xdisplay = x11_display->xdisplay;
  Window xroot = x11_display->xroot;

  /*
   * This is the most important part of cleanup - we have to do this before
   * giving up the window manager selection or the next window manager won't be
   * able to redirect subwindows
   */
  XCompositeUnredirectSubwindows (xdisplay, xroot, CompositeRedirectManual);
}

/*
 * Sets an bounding shape on the COW so that the given window
 * is exposed. If window is %NULL it clears the shape again.
 *
 * Used so we can unredirect windows, by shaping away the part
 * of the COW, letting the raw window be seen through below.
 */
static void
shape_cow_for_window (MetaCompositorX11 *compositor_x11,
                      MetaWindow        *window)
{
  MetaCompositor *compositor = META_COMPOSITOR (compositor_x11);
  MetaDisplay *display = meta_compositor_get_display (compositor);
  Display *xdisplay = meta_x11_display_get_xdisplay (display->x11_display);

  if (!window)
    {
      XFixesSetWindowShapeRegion (xdisplay, compositor_x11->output,
                                  ShapeBounding, 0, 0, None);
    }
  else
    {
      XserverRegion output_region;
      XRectangle screen_rect, window_bounds;
      int width, height;
      MetaRectangle rect;

      meta_window_get_frame_rect (window, &rect);

      window_bounds.x = rect.x;
      window_bounds.y = rect.y;
      window_bounds.width = rect.width;
      window_bounds.height = rect.height;

      meta_display_get_size (display, &width, &height);
      screen_rect.x = 0;
      screen_rect.y = 0;
      screen_rect.width = width;
      screen_rect.height = height;

      output_region = XFixesCreateRegion (xdisplay, &window_bounds, 1);

      XFixesInvertRegion (xdisplay, output_region, &screen_rect, output_region);
      XFixesSetWindowShapeRegion (xdisplay, compositor_x11->output,
                                  ShapeBounding, 0, 0, output_region);
      XFixesDestroyRegion (xdisplay, output_region);
    }
}

static void
set_unredirected_window (MetaCompositorX11 *compositor_x11,
                         MetaWindow        *window)
{
  MetaWindow *prev_unredirected_window = compositor_x11->unredirected_window;

  if (prev_unredirected_window == window)
    return;

  if (prev_unredirected_window)
    {
      MetaWindowActor *window_actor;

      window_actor = meta_window_actor_from_window (prev_unredirected_window);
      meta_window_actor_set_unredirected (window_actor, FALSE);
    }

  shape_cow_for_window (compositor_x11, window);
  compositor_x11->unredirected_window = window;

  if (window)
    {
      MetaWindowActor *window_actor;

      window_actor = meta_window_actor_from_window (window);
      meta_window_actor_set_unredirected (window_actor, TRUE);
    }
}

static void
meta_compositor_x11_pre_paint (MetaCompositor *compositor)
{
  MetaCompositorX11 *compositor_x11 = META_COMPOSITOR_X11 (compositor);
  MetaWindowActor *top_window_actor;
  MetaCompositorClass *parent_class;

  top_window_actor = meta_compositor_get_top_window_actor (compositor);
  if (!meta_compositor_is_unredirect_inhibited (compositor) &&
      top_window_actor &&
      meta_window_actor_should_unredirect (top_window_actor))
    {
      MetaWindow *top_window;

      top_window = meta_window_actor_get_meta_window (top_window_actor);
      set_unredirected_window (compositor_x11, top_window);
    }
  else
    {
      set_unredirected_window (compositor_x11, NULL);
    }

  parent_class = META_COMPOSITOR_CLASS (meta_compositor_x11_parent_class);
  parent_class->pre_paint (compositor);

  if (compositor_x11->frame_has_updated_xsurfaces)
    {
      MetaDisplay *display = meta_compositor_get_display (compositor);

      /*
       * We need to make sure that any X drawing that happens before the
       * XDamageSubtract() for each window above is visible to subsequent GL
       * rendering; the standardized way to do this is GL_EXT_X11_sync_object.
       * Since this isn't implemented yet in mesa, we also have a path that
       * relies on the implementation of the open source drivers.
       *
       * Anything else, we just hope for the best.
       *
       * Xorg and open source driver specifics:
       *
       * The X server makes sure to flush drawing to the kernel before sending
       * out damage events, but since we use DamageReportBoundingBox there may
       * be drawing between the last damage event and the XDamageSubtract()
       * that needs to be flushed as well.
       *
       * Xorg always makes sure that drawing is flushed to the kernel before
       * writing events or responses to the client, so any round trip request
       * at this point is sufficient to flush the GLX buffers.
       */
      if (compositor_x11->have_x11_sync_object)
        compositor_x11->have_x11_sync_object = meta_sync_ring_insert_wait ();
      else
        XSync (display->x11_display->xdisplay, False);
    }
}

static void
meta_compositor_x11_post_paint (MetaCompositor *compositor)
{
  MetaCompositorX11 *compositor_x11 = META_COMPOSITOR_X11 (compositor);
  MetaCompositorClass *parent_class;

  if (compositor_x11->frame_has_updated_xsurfaces)
    {
      if (compositor_x11->have_x11_sync_object)
        compositor_x11->have_x11_sync_object = meta_sync_ring_after_frame ();

      compositor_x11->frame_has_updated_xsurfaces = FALSE;
    }

  parent_class = META_COMPOSITOR_CLASS (meta_compositor_x11_parent_class);
  parent_class->post_paint (compositor);
}

static void
meta_compositor_x11_remove_window (MetaCompositor *compositor,
                                   MetaWindow     *window)
{
  MetaCompositorX11 *compositor_x11 = META_COMPOSITOR_X11 (compositor);
  MetaCompositorClass *parent_class;

  if (compositor_x11->unredirected_window == window)
    set_unredirected_window (compositor_x11, NULL);

  parent_class = META_COMPOSITOR_CLASS (meta_compositor_x11_parent_class);
  parent_class->remove_window (compositor, window);
}

Window
meta_compositor_x11_get_output_xwindow (MetaCompositorX11 *compositor_x11)
{
  return compositor_x11->output;
}

static void
meta_compositor_x11_dispose (GObject *object)
{
  MetaCompositorX11 *compositor_x11 = META_COMPOSITOR_X11 (object);

  if (compositor_x11->have_x11_sync_object)
    {
      meta_sync_ring_destroy ();
      compositor_x11->have_x11_sync_object = FALSE;
    }

  G_OBJECT_CLASS (meta_compositor_x11_parent_class)->dispose (object);
}

static void
meta_compositor_x11_init (MetaCompositorX11 *compositor_x11)
{
}

static void
meta_compositor_x11_class_init (MetaCompositorX11Class *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  MetaCompositorClass *compositor_class = META_COMPOSITOR_CLASS (klass);

  object_class->dispose = meta_compositor_x11_dispose;

  compositor_class->manage = meta_compositor_x11_manage;
  compositor_class->unmanage = meta_compositor_x11_unmanage;
  compositor_class->pre_paint = meta_compositor_x11_pre_paint;
  compositor_class->post_paint = meta_compositor_x11_post_paint;
  compositor_class->remove_window = meta_compositor_x11_remove_window;
}