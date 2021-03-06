/*
 * Copyright (C) 2020 Red Hat Inc.
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

#include "backends/meta-screen-cast-area-stream-src.h"

#include <spa/buffer/meta.h>

#include "backends/meta-backend-private.h"
#include "backends/meta-cursor-tracker-private.h"
#include "backends/meta-screen-cast-area-stream.h"
#include "backends/meta-screen-cast-session.h"
#include "backends/meta-stage-private.h"
#include "clutter/clutter.h"
#include "clutter/clutter-mutter.h"
#include "core/boxes-private.h"

struct _MetaScreenCastAreaStreamSrc
{
  MetaScreenCastStreamSrc parent;

  gboolean cursor_bitmap_invalid;
  gboolean hw_cursor_inhibited;

  GList *watches;

  gulong cursor_moved_handler_id;
  gulong cursor_changed_handler_id;

  guint maybe_record_idle_id;
};

static void
hw_cursor_inhibitor_iface_init (MetaHwCursorInhibitorInterface *iface);

G_DEFINE_TYPE_WITH_CODE (MetaScreenCastAreaStreamSrc,
                         meta_screen_cast_area_stream_src,
                         META_TYPE_SCREEN_CAST_STREAM_SRC,
                         G_IMPLEMENT_INTERFACE (META_TYPE_HW_CURSOR_INHIBITOR,
                                                hw_cursor_inhibitor_iface_init))

static ClutterStage *
get_stage (MetaScreenCastAreaStreamSrc *area_src)
{
  MetaScreenCastStreamSrc *src;
  MetaScreenCastStream *stream;
  MetaScreenCastAreaStream *area_stream;

  src = META_SCREEN_CAST_STREAM_SRC (area_src);
  stream = meta_screen_cast_stream_src_get_stream (src);
  area_stream = META_SCREEN_CAST_AREA_STREAM (stream);

  return meta_screen_cast_area_stream_get_stage (area_stream);
}

static MetaBackend *
get_backend (MetaScreenCastAreaStreamSrc *area_src)
{
  MetaScreenCastStreamSrc *src = META_SCREEN_CAST_STREAM_SRC (area_src);
  MetaScreenCastStream *stream = meta_screen_cast_stream_src_get_stream (src);
  MetaScreenCastSession *session = meta_screen_cast_stream_get_session (stream);
  MetaScreenCast *screen_cast =
    meta_screen_cast_session_get_screen_cast (session);

  return meta_screen_cast_get_backend (screen_cast);
}

static MetaCursorRenderer *
get_cursor_renderer (MetaScreenCastAreaStreamSrc *area_src)
{
  MetaScreenCastStreamSrc *src = META_SCREEN_CAST_STREAM_SRC (area_src);
  MetaScreenCastStream *stream = meta_screen_cast_stream_src_get_stream (src);
  MetaScreenCastSession *session = meta_screen_cast_stream_get_session (stream);
  MetaScreenCast *screen_cast =
    meta_screen_cast_session_get_screen_cast (session);
  MetaBackend *backend = meta_screen_cast_get_backend (screen_cast);

  return meta_backend_get_cursor_renderer (backend);
}

static void
meta_screen_cast_area_stream_src_get_specs (MetaScreenCastStreamSrc *src,
                                            int                     *width,
                                            int                     *height,
                                            float                   *frame_rate)
{
  MetaScreenCastStream *stream = meta_screen_cast_stream_src_get_stream (src);
  MetaScreenCastAreaStream *area_stream = META_SCREEN_CAST_AREA_STREAM (stream);
  MetaRectangle *area;
  float scale;

  area = meta_screen_cast_area_stream_get_area (area_stream);
  scale = meta_screen_cast_area_stream_get_scale (area_stream);

  *width = (int) roundf (area->width * scale);
  *height = (int) roundf (area->height * scale);
  *frame_rate = 60.0;
}

static gboolean
is_cursor_in_stream (MetaScreenCastAreaStreamSrc *area_src)
{
  MetaScreenCastStreamSrc *src = META_SCREEN_CAST_STREAM_SRC (area_src);
  MetaScreenCastStream *stream = meta_screen_cast_stream_src_get_stream (src);
  MetaScreenCastAreaStream *area_stream = META_SCREEN_CAST_AREA_STREAM (stream);
  MetaBackend *backend = get_backend (area_src);
  MetaCursorRenderer *cursor_renderer =
    meta_backend_get_cursor_renderer (backend);
  MetaRectangle *area;
  graphene_rect_t area_rect;
  MetaCursorSprite *cursor_sprite;

  area = meta_screen_cast_area_stream_get_area (area_stream);
  area_rect = meta_rectangle_to_graphene_rect (area);

  cursor_sprite = meta_cursor_renderer_get_cursor (cursor_renderer);
  if (cursor_sprite)
    {
      graphene_rect_t cursor_rect;

      cursor_rect = meta_cursor_renderer_calculate_rect (cursor_renderer,
                                                         cursor_sprite);
      return graphene_rect_intersection (&cursor_rect, &area_rect, NULL);
    }
  else
    {
      graphene_point_t cursor_position;

      cursor_position = meta_cursor_renderer_get_position (cursor_renderer);
      return graphene_rect_contains_point (&area_rect, &cursor_position);
    }
}

static void
sync_cursor_state (MetaScreenCastAreaStreamSrc *area_src)
{
  MetaScreenCastStreamSrc *src = META_SCREEN_CAST_STREAM_SRC (area_src);
  ClutterStage *stage = get_stage (area_src);

  if (!is_cursor_in_stream (area_src))
    return;

  if (clutter_stage_is_redraw_queued (stage))
    return;

  meta_screen_cast_stream_src_maybe_record_frame (src);
}

static void
cursor_moved (MetaCursorTracker           *cursor_tracker,
              float                        x,
              float                        y,
              MetaScreenCastAreaStreamSrc *area_src)
{
  sync_cursor_state (area_src);
}

static void
cursor_changed (MetaCursorTracker           *cursor_tracker,
                MetaScreenCastAreaStreamSrc *area_src)
{
  area_src->cursor_bitmap_invalid = TRUE;
  sync_cursor_state (area_src);
}

static void
inhibit_hw_cursor (MetaScreenCastAreaStreamSrc *area_src)
{
  MetaCursorRenderer *cursor_renderer;
  MetaHwCursorInhibitor *inhibitor;

  g_return_if_fail (!area_src->hw_cursor_inhibited);

  cursor_renderer = get_cursor_renderer (area_src);
  inhibitor = META_HW_CURSOR_INHIBITOR (area_src);
  meta_cursor_renderer_add_hw_cursor_inhibitor (cursor_renderer, inhibitor);

  area_src->hw_cursor_inhibited = TRUE;
}

static void
uninhibit_hw_cursor (MetaScreenCastAreaStreamSrc *area_src)
{
  MetaCursorRenderer *cursor_renderer;
  MetaHwCursorInhibitor *inhibitor;

  g_return_if_fail (area_src->hw_cursor_inhibited);

  cursor_renderer = get_cursor_renderer (area_src);
  inhibitor = META_HW_CURSOR_INHIBITOR (area_src);
  meta_cursor_renderer_remove_hw_cursor_inhibitor (cursor_renderer, inhibitor);

  area_src->hw_cursor_inhibited = FALSE;
}

static gboolean
maybe_record_frame_on_idle (gpointer user_data)
{
  MetaScreenCastAreaStreamSrc *area_src =
    META_SCREEN_CAST_AREA_STREAM_SRC (user_data);
  MetaScreenCastStreamSrc *src = META_SCREEN_CAST_STREAM_SRC (area_src);

  area_src->maybe_record_idle_id = 0;

  meta_screen_cast_stream_src_maybe_record_frame (src);

  return G_SOURCE_REMOVE;
}

static void
stage_painted (MetaStage           *stage,
               ClutterStageView    *view,
               ClutterPaintContext *paint_context,
               gpointer             user_data)
{
  MetaScreenCastAreaStreamSrc *area_src =
    META_SCREEN_CAST_AREA_STREAM_SRC (user_data);
  MetaScreenCastStreamSrc *src = META_SCREEN_CAST_STREAM_SRC (area_src);
  MetaScreenCastStream *stream = meta_screen_cast_stream_src_get_stream (src);
  MetaScreenCastAreaStream *area_stream = META_SCREEN_CAST_AREA_STREAM (stream);
  const cairo_region_t *redraw_clip;
  MetaRectangle *area;

  if (area_src->maybe_record_idle_id)
    return;

  area = meta_screen_cast_area_stream_get_area (area_stream);
  redraw_clip = clutter_paint_context_get_redraw_clip (paint_context);

  if (redraw_clip)
    {
      switch (cairo_region_contains_rectangle (redraw_clip, area))
        {
        case CAIRO_REGION_OVERLAP_IN:
        case CAIRO_REGION_OVERLAP_PART:
          break;
        case CAIRO_REGION_OVERLAP_OUT:
          return;
        }
    }

  area_src->maybe_record_idle_id = g_idle_add (maybe_record_frame_on_idle, src);
}

static void
add_view_painted_watches (MetaScreenCastAreaStreamSrc *area_src,
                          MetaStageWatchPhase          watch_phase)
{
  MetaScreenCastStreamSrc *src = META_SCREEN_CAST_STREAM_SRC (area_src);
  MetaScreenCastStream *stream = meta_screen_cast_stream_src_get_stream (src);
  MetaScreenCastAreaStream *area_stream = META_SCREEN_CAST_AREA_STREAM (stream);
  MetaBackend *backend = get_backend (area_src);
  MetaRenderer *renderer = meta_backend_get_renderer (backend);
  ClutterStage *stage;
  MetaStage *meta_stage;
  MetaRectangle *area;
  GList *l;

  stage = get_stage (area_src);
  meta_stage = META_STAGE (stage);
  area = meta_screen_cast_area_stream_get_area (area_stream);

  for (l = meta_renderer_get_views (renderer); l; l = l->next)
    {
      MetaRendererView *view = l->data;
      MetaRectangle view_layout;

      clutter_stage_view_get_layout (CLUTTER_STAGE_VIEW (view), &view_layout);
      if (meta_rectangle_overlap (area, &view_layout))
        {
          MetaStageWatch *watch;

          watch = meta_stage_watch_view (meta_stage,
                                         CLUTTER_STAGE_VIEW (view),
                                         watch_phase,
                                         stage_painted,
                                         area_src);

          area_src->watches = g_list_prepend (area_src->watches, watch);
        }
    }
}

static void
meta_screen_cast_area_stream_src_enable (MetaScreenCastStreamSrc *src)
{
  MetaScreenCastAreaStreamSrc *area_src =
    META_SCREEN_CAST_AREA_STREAM_SRC (src);
  MetaBackend *backend = get_backend (area_src);
  MetaCursorTracker *cursor_tracker = meta_backend_get_cursor_tracker (backend);
  ClutterStage *stage;
  MetaScreenCastStream *stream;

  stream = meta_screen_cast_stream_src_get_stream (src);
  stage = get_stage (area_src);

  switch (meta_screen_cast_stream_get_cursor_mode (stream))
    {
    case META_SCREEN_CAST_CURSOR_MODE_METADATA:
      area_src->cursor_moved_handler_id =
        g_signal_connect_after (cursor_tracker, "cursor-moved",
                                G_CALLBACK (cursor_moved),
                                area_src);
      area_src->cursor_changed_handler_id =
        g_signal_connect_after (cursor_tracker, "cursor-changed",
                                G_CALLBACK (cursor_changed),
                                area_src);
      G_GNUC_FALLTHROUGH;
    case META_SCREEN_CAST_CURSOR_MODE_HIDDEN:
      add_view_painted_watches (area_src,
                                META_STAGE_WATCH_AFTER_ACTOR_PAINT);
      break;
    case META_SCREEN_CAST_CURSOR_MODE_EMBEDDED:
      inhibit_hw_cursor (area_src);
      add_view_painted_watches (area_src,
                                META_STAGE_WATCH_AFTER_ACTOR_PAINT);
      break;
    }

  clutter_actor_queue_redraw (CLUTTER_ACTOR (stage));
}

static void
meta_screen_cast_area_stream_src_disable (MetaScreenCastStreamSrc *src)
{
  MetaScreenCastAreaStreamSrc *area_src =
    META_SCREEN_CAST_AREA_STREAM_SRC (src);
  MetaBackend *backend = get_backend (area_src);
  MetaCursorTracker *cursor_tracker = meta_backend_get_cursor_tracker (backend);
  ClutterStage *stage;
  MetaStage *meta_stage;
  GList *l;

  stage = get_stage (area_src);
  meta_stage = META_STAGE (stage);

  for (l = area_src->watches; l; l = l->next)
    {
      MetaStageWatch *watch = l->data;

      meta_stage_remove_watch (meta_stage, watch);
    }
  g_clear_pointer (&area_src->watches, g_list_free);

  if (area_src->hw_cursor_inhibited)
    uninhibit_hw_cursor (area_src);

  g_clear_signal_handler (&area_src->cursor_moved_handler_id,
                          cursor_tracker);
  g_clear_signal_handler (&area_src->cursor_changed_handler_id,
                          cursor_tracker);

  g_clear_handle_id (&area_src->maybe_record_idle_id, g_source_remove);
}

static gboolean
meta_screen_cast_area_stream_src_record_frame (MetaScreenCastStreamSrc *src,
                                               uint8_t                 *data)
{
  MetaScreenCastAreaStreamSrc *area_src =
    META_SCREEN_CAST_AREA_STREAM_SRC (src);
  MetaScreenCastStream *stream = meta_screen_cast_stream_src_get_stream (src);
  MetaScreenCastAreaStream *area_stream = META_SCREEN_CAST_AREA_STREAM (stream);
  ClutterStage *stage;
  MetaRectangle *area;
  float scale;
  int stride;
  ClutterPaintFlag paint_flags = CLUTTER_PAINT_FLAG_NONE;
  g_autoptr (GError) error = NULL;

  stage = get_stage (area_src);
  area = meta_screen_cast_area_stream_get_area (area_stream);
  scale = meta_screen_cast_area_stream_get_scale (area_stream);
  stride = meta_screen_cast_stream_src_get_stride (src);

  switch (meta_screen_cast_stream_get_cursor_mode (stream))
    {
    case META_SCREEN_CAST_CURSOR_MODE_METADATA:
    case META_SCREEN_CAST_CURSOR_MODE_HIDDEN:
      paint_flags |= CLUTTER_PAINT_FLAG_NO_CURSORS;
      break;
    case META_SCREEN_CAST_CURSOR_MODE_EMBEDDED:
      break;
    }

  if (!clutter_stage_paint_to_buffer (stage, area, scale,
                                      data,
                                      stride,
                                      CLUTTER_CAIRO_FORMAT_ARGB32,
                                      paint_flags,
                                      &error))
    {
      g_warning ("Failed to record area: %s", error->message);
      return FALSE;
    }

  return TRUE;
}

static gboolean
meta_screen_cast_area_stream_src_blit_to_framebuffer (MetaScreenCastStreamSrc *src,
                                                      CoglFramebuffer         *framebuffer)
{
  MetaScreenCastAreaStreamSrc *area_src =
    META_SCREEN_CAST_AREA_STREAM_SRC (src);
  MetaScreenCastStream *stream = meta_screen_cast_stream_src_get_stream (src);
  MetaScreenCastAreaStream *area_stream = META_SCREEN_CAST_AREA_STREAM (stream);
  MetaBackend *backend = get_backend (area_src);
  ClutterStage *stage;
  MetaRectangle *area;
  float scale;
  ClutterPaintFlag paint_flags = CLUTTER_PAINT_FLAG_NONE;

  stage = CLUTTER_STAGE (meta_backend_get_stage (backend));
  area = meta_screen_cast_area_stream_get_area (area_stream);
  scale = meta_screen_cast_area_stream_get_scale (area_stream);

  switch (meta_screen_cast_stream_get_cursor_mode (stream))
    {
    case META_SCREEN_CAST_CURSOR_MODE_METADATA:
    case META_SCREEN_CAST_CURSOR_MODE_HIDDEN:
      paint_flags |= CLUTTER_PAINT_FLAG_NO_CURSORS;
      break;
    case META_SCREEN_CAST_CURSOR_MODE_EMBEDDED:
      break;
    }
  clutter_stage_paint_to_framebuffer (stage, framebuffer,
                                      area, scale,
                                      paint_flags);

  cogl_framebuffer_finish (framebuffer);

  return TRUE;
}

static void
meta_screen_cast_area_stream_src_set_cursor_metadata (MetaScreenCastStreamSrc *src,
                                                      struct spa_meta_cursor  *spa_meta_cursor)
{
  MetaScreenCastAreaStreamSrc *area_src =
    META_SCREEN_CAST_AREA_STREAM_SRC (src);
  MetaScreenCastStream *stream = meta_screen_cast_stream_src_get_stream (src);
  MetaScreenCastAreaStream *area_stream = META_SCREEN_CAST_AREA_STREAM (stream);
  MetaBackend *backend = get_backend (area_src);
  MetaCursorRenderer *cursor_renderer =
    meta_backend_get_cursor_renderer (backend);
  MetaCursorSprite *cursor_sprite;
  MetaRectangle *area;
  float scale;
  graphene_point_t cursor_position;
  int x, y;

  cursor_sprite = meta_cursor_renderer_get_cursor (cursor_renderer);

  if (!is_cursor_in_stream (area_src))
    {
      meta_screen_cast_stream_src_unset_cursor_metadata (src,
                                                         spa_meta_cursor);
      return;
    }

  area = meta_screen_cast_area_stream_get_area (area_stream);
  scale = meta_screen_cast_area_stream_get_scale (area_stream);

  cursor_position = meta_cursor_renderer_get_position (cursor_renderer);
  cursor_position.x -= area->x;
  cursor_position.y -= area->y;
  cursor_position.x *= scale;
  cursor_position.y *= scale;

  x = (int) roundf (cursor_position.x);
  y = (int) roundf (cursor_position.y);

  if (area_src->cursor_bitmap_invalid)
    {
      if (cursor_sprite)
        {
          float cursor_scale;
          float metadata_scale;

          cursor_scale = meta_cursor_sprite_get_texture_scale (cursor_sprite);
          metadata_scale = scale * cursor_scale;
          meta_screen_cast_stream_src_set_cursor_sprite_metadata (src,
                                                                  spa_meta_cursor,
                                                                  cursor_sprite,
                                                                  x, y,
                                                                  metadata_scale);
        }
      else
        {
          meta_screen_cast_stream_src_set_empty_cursor_sprite_metadata (src,
                                                                        spa_meta_cursor,
                                                                        x, y);
        }

      area_src->cursor_bitmap_invalid = FALSE;
    }
  else
    {
      meta_screen_cast_stream_src_set_cursor_position_metadata (src,
                                                                spa_meta_cursor,
                                                                x, y);
    }
}

static gboolean
meta_screen_cast_area_stream_src_is_cursor_sprite_inhibited (MetaHwCursorInhibitor *inhibitor,
                                                             MetaCursorSprite      *cursor_sprite)
{
  MetaScreenCastAreaStreamSrc *area_src =
    META_SCREEN_CAST_AREA_STREAM_SRC (inhibitor);

  return is_cursor_in_stream (area_src);
}

static void
hw_cursor_inhibitor_iface_init (MetaHwCursorInhibitorInterface *iface)
{
  iface->is_cursor_sprite_inhibited =
    meta_screen_cast_area_stream_src_is_cursor_sprite_inhibited;
}

MetaScreenCastAreaStreamSrc *
meta_screen_cast_area_stream_src_new (MetaScreenCastAreaStream  *area_stream,
                                      GError                   **error)
{
  return g_initable_new (META_TYPE_SCREEN_CAST_AREA_STREAM_SRC, NULL, error,
                         "stream", area_stream,
                         NULL);
}

static void
meta_screen_cast_area_stream_src_init (MetaScreenCastAreaStreamSrc *area_src)
{
  area_src->cursor_bitmap_invalid = TRUE;
}

static void
meta_screen_cast_area_stream_src_class_init (MetaScreenCastAreaStreamSrcClass *klass)
{
  MetaScreenCastStreamSrcClass *src_class =
    META_SCREEN_CAST_STREAM_SRC_CLASS (klass);

  src_class->get_specs = meta_screen_cast_area_stream_src_get_specs;
  src_class->enable = meta_screen_cast_area_stream_src_enable;
  src_class->disable = meta_screen_cast_area_stream_src_disable;
  src_class->record_frame = meta_screen_cast_area_stream_src_record_frame;
  src_class->blit_to_framebuffer =
    meta_screen_cast_area_stream_src_blit_to_framebuffer;
  src_class->set_cursor_metadata =
    meta_screen_cast_area_stream_src_set_cursor_metadata;
}
