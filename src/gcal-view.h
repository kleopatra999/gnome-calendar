/*
 * gcal-view.h
 *
 * Copyright (C) 2012 - Erick Pérez Castellanos
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
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __GCAL_VIEW_H__
#define __GCAL_VIEW_H__

#include "gcal-event-widget.h"

G_BEGIN_DECLS

#define GCAL_TYPE_VIEW (gcal_view_get_type ())

G_DECLARE_INTERFACE (GcalView, gcal_view, GCAL, VIEW, GtkWidget)

struct _GcalViewInterface
{
  GTypeInterface parent;

  /* signals */
  void           (*create_event)                       (GcalView     *view,
                                                        icaltimetype *start_span,
                                                        icaltimetype *end_span,
                                                        gdouble       x,
                                                        gdouble       y);
  void           (*create_event_detailed)              (GcalView     *view,
                                                        icaltimetype *start_span,
                                                        icaltimetype *end_span);

  /* Marks related API */
  void           (*clear_marks)                        (GcalView    *view);

  GList*         (*get_children_by_uuid)               (GcalView    *view,
                                                        const gchar *uuid);
};

void          gcal_view_set_date                      (GcalView     *view,
                                                       icaltimetype *date);

icaltimetype* gcal_view_get_date                      (GcalView     *view);

void          gcal_view_clear_marks                   (GcalView     *view);

GList*        gcal_view_get_children_by_uuid          (GcalView     *view,
                                                       const gchar  *uuid);

G_END_DECLS

#endif /* __GCAL_MONTH_VIEW_H__ */
