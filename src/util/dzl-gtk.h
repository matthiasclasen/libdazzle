/* dzl-gtk.h
 *
 * Copyright (C) 2015-2017 Christian Hergert <chergert@redhat.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef DZL_GTK_H
#define DZL_GTK_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

gboolean      dzl_gtk_widget_action              (GtkWidget               *widget,
                                                  const gchar             *group,
                                                  const gchar             *name,
                                                  GVariant                *param);
gboolean      dzl_gtk_widget_action_with_string  (GtkWidget               *widget,
                                                  const gchar             *group,
                                                  const gchar             *name,
                                                  const gchar             *param);
void          dzl_gtk_widget_hide_with_fade      (GtkWidget               *widget);
void          dzl_gtk_widget_show_with_fade      (GtkWidget               *widget);
void          dzl_gtk_widget_add_style_class     (GtkWidget               *widget,
                                                  const gchar             *class_name);
gpointer      dzl_gtk_widget_find_child_typed    (GtkWidget               *widget,
                                                  GType                    type);
void          dzl_gtk_text_buffer_remove_tag     (GtkTextBuffer           *buffer,
                                                  GtkTextTag              *tag,
                                                  const GtkTextIter       *start,
                                                  const GtkTextIter       *end,
                                                  gboolean                 minimal_damage);

G_END_DECLS

#endif /* DZL_GTK_H */
