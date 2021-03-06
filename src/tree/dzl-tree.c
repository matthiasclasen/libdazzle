/* dzl-tree.c
 *
 * Copyright (C) 2011-2017 Christian Hergert <christian@hergert.me>
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

#define G_LOG_DOMAIN "dzl-tree"

#include <glib/gi18n.h>

#include "tree/dzl-tree.h"
#include "tree/dzl-tree-node.h"
#include "tree/dzl-tree-private.h"
#include "util/dzl-util-private.h"

typedef struct
{
  GPtrArray          *builders;
  DzlTreeNode        *root;
  DzlTreeNode        *selection;
  GtkTreeViewColumn  *column;
  GtkCellRenderer    *cell_pixbuf;
  GtkCellRenderer    *cell_text;
  GtkTreeStore       *store;
  GMenuModel         *context_menu;
  GdkRGBA             dim_foreground;
  guint               show_icons : 1;
} DzlTreePrivate;

typedef struct
{
  gpointer     key;
  GEqualFunc   equal_func;
  DzlTreeNode *result;
} NodeLookup;

typedef struct
{
  DzlTree           *self;
  DzlTreeFilterFunc  filter_func;
  gpointer           filter_data;
  GDestroyNotify     filter_data_destroy;
} FilterFunc;

static void dzl_tree_buildable_init (GtkBuildableIface *iface);

G_DEFINE_TYPE_WITH_CODE (DzlTree, dzl_tree, GTK_TYPE_TREE_VIEW,
                         G_ADD_PRIVATE (DzlTree)
                         G_IMPLEMENT_INTERFACE (GTK_TYPE_BUILDABLE, dzl_tree_buildable_init))

enum {
  PROP_0,
  PROP_CONTEXT_MENU,
  PROP_ROOT,
  PROP_SELECTION,
  PROP_SHOW_ICONS,
  LAST_PROP
};

enum {
  ACTION,
  POPULATE_POPUP,
  LAST_SIGNAL
};

static GtkBuildableIface *dzl_tree_parent_buildable_iface;
static GParamSpec *properties [LAST_PROP];
static guint signals [LAST_SIGNAL];

/**
 * dzl_tree_get_context_menu:
 *
 * Returns: (transfer none) (nullable): A #GMenuModel or %NULL.
 */
GMenuModel *
dzl_tree_get_context_menu (DzlTree *self)
{
  DzlTreePrivate *priv = dzl_tree_get_instance_private (self);

  g_return_val_if_fail (DZL_IS_TREE (self), NULL);

  return priv->context_menu;
}

void
dzl_tree_set_context_menu (DzlTree    *self,
                           GMenuModel *model)
{
  DzlTreePrivate *priv = dzl_tree_get_instance_private (self);

  g_return_if_fail (DZL_IS_TREE (self));
  g_return_if_fail (!model || G_IS_MENU_MODEL (model));

  if (g_set_object (&priv->context_menu, model))
    g_object_notify_by_pspec (G_OBJECT (self), properties [PROP_CONTEXT_MENU]);
}

void
_dzl_tree_build_node (DzlTree     *self,
                      DzlTreeNode *node)
{
  DzlTreePrivate *priv = dzl_tree_get_instance_private (self);
  gsize i;

  g_assert (DZL_IS_TREE (self));
  g_assert (DZL_IS_TREE_NODE (node));

  _dzl_tree_node_set_needs_build (node, FALSE);
  _dzl_tree_node_remove_dummy_child (node);

  for (i = 0; i < priv->builders->len; i++)
    {
      DzlTreeBuilder *builder;

      builder = g_ptr_array_index (priv->builders, i);
      _dzl_tree_builder_build_node (builder, node);
    }
}

static void
dzl_tree_unselect (DzlTree *self)
{
  GtkTreeSelection *selection;

  g_return_if_fail (DZL_IS_TREE (self));

  selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (self));
  gtk_tree_selection_unselect_all (selection);
}

static void
dzl_tree_select (DzlTree     *self,
                 DzlTreeNode *node)
{
  DzlTreePrivate *priv = dzl_tree_get_instance_private (self);
  GtkTreeSelection *selection;
  GtkTreePath *path;

  g_return_if_fail (DZL_IS_TREE (self));
  g_return_if_fail (DZL_IS_TREE_NODE (node));

  if (priv->selection)
    {
      dzl_tree_unselect (self);
      g_assert (!priv->selection);
    }

  priv->selection = node;

  path = dzl_tree_node_get_path (node);
  selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (self));
  gtk_tree_selection_select_path (selection, path);
  gtk_tree_path_free (path);
}

static void
check_visible_foreach (GtkWidget *widget,
                       gpointer   user_data)
{
  gboolean *at_least_one_visible = user_data;

  if (*at_least_one_visible == FALSE)
    *at_least_one_visible = gtk_widget_get_visible (widget);
}

static void
dzl_tree_popup (DzlTree        *self,
                DzlTreeNode    *node,
                GdkEventButton *event,
                gint            target_x,
                gint            target_y)
{
  DzlTreePrivate *priv = dzl_tree_get_instance_private (self);
  gboolean at_least_one_visible = FALSE;
  GtkWidget *menu_widget;

  g_return_if_fail (DZL_IS_TREE (self));
  g_return_if_fail (DZL_IS_TREE_NODE (node));

  if (priv->context_menu != NULL)
    {
      for (guint i = 0; i < priv->builders->len; i++)
        {
          DzlTreeBuilder *builder = g_ptr_array_index (priv->builders, i);

          _dzl_tree_builder_node_popup (builder, node, G_MENU (priv->context_menu));
        }
    }

  if (priv->context_menu != NULL)
    menu_widget = gtk_menu_new_from_model (G_MENU_MODEL (priv->context_menu));
  else
    menu_widget = gtk_menu_new ();

  g_signal_emit (self, signals [POPULATE_POPUP], 0, menu_widget);

  gtk_container_foreach (GTK_CONTAINER (menu_widget),
                         check_visible_foreach,
                         &at_least_one_visible);

  if (at_least_one_visible)
    {
      gtk_menu_attach_to_widget (GTK_MENU (menu_widget),
                                 GTK_WIDGET (self),
                                 NULL);
      g_signal_connect_after (menu_widget,
                              "selection-done",
                              G_CALLBACK (gtk_widget_destroy),
                              NULL);

      g_object_set (G_OBJECT (menu_widget),
                    "rect-anchor-dx", target_x - 12,
                    "rect-anchor-dy", target_y - 3,
                    NULL);
      gtk_menu_popup_at_widget (GTK_MENU (menu_widget),
                                GTK_WIDGET (self),
                                GDK_GRAVITY_NORTH_WEST,
                                GDK_GRAVITY_NORTH_WEST,
                                (GdkEvent *)event);
    }
  else
    {
      gtk_widget_destroy (menu_widget);
    }
}

static gboolean
dzl_tree_popup_menu (GtkWidget *widget)
{
  DzlTree *self = (DzlTree *)widget;
  DzlTreeNode *node;
  GdkRectangle area;

  g_assert (DZL_IS_TREE (self));

  if (!(node = dzl_tree_get_selected (self)))
    return FALSE;

  dzl_tree_node_get_area (node, &area);
  dzl_tree_popup (self, node, NULL, area.x + area.width, area.y - 1);

  return TRUE;
}

static void
dzl_tree_selection_changed (DzlTree         *self,
                           GtkTreeSelection *selection)
{
  DzlTreePrivate *priv = dzl_tree_get_instance_private (self);
  DzlTreeBuilder *builder;
  GtkTreeModel *model;
  GtkTreeIter iter;
  DzlTreeNode *node;
  DzlTreeNode *unselection;

  g_return_if_fail (DZL_IS_TREE (self));
  g_return_if_fail (GTK_IS_TREE_SELECTION (selection));

  if ((unselection = priv->selection))
    {
      priv->selection = NULL;
      for (guint i = 0; i < priv->builders->len; i++)
        {
          builder = g_ptr_array_index (priv->builders, i);
          _dzl_tree_builder_node_unselected (builder, unselection);
        }
    }

  if (gtk_tree_selection_get_selected (selection, &model, &iter))
    {
      gtk_tree_model_get (model, &iter, 0, &node, -1);
      if (node)
        {
          for (guint i = 0; i < priv->builders->len; i++)
            {
              builder = g_ptr_array_index (priv->builders, i);
              _dzl_tree_builder_node_selected (builder, node);
            }

          g_object_unref (node);
        }
    }

  g_object_notify_by_pspec (G_OBJECT (self), properties [PROP_SELECTION]);
}

static gboolean
dzl_tree_add_builder_foreach_cb (GtkTreeModel *model,
                                 GtkTreePath  *path,
                                 GtkTreeIter  *iter,
                                 gpointer      user_data)
{
  DzlTreeBuilder *builder = user_data;
  DzlTreeNode *node = NULL;

  g_return_val_if_fail (GTK_IS_TREE_MODEL (model), FALSE);
  g_return_val_if_fail (path != NULL, FALSE);
  g_return_val_if_fail (iter != NULL, FALSE);

  gtk_tree_model_get (model, iter, 0, &node, -1);
  if (!_dzl_tree_node_get_needs_build (node))
    _dzl_tree_builder_build_node (builder, node);
  g_clear_object (&node);

  return FALSE;
}

static gboolean
dzl_tree_foreach (DzlTree                 *self,
                  GtkTreeIter             *iter,
                  GtkTreeModelForeachFunc  func,
                  gpointer                 user_data)
{
  DzlTreePrivate *priv = dzl_tree_get_instance_private (self);
  GtkTreeModel *model;
  GtkTreePath *path;
  GtkTreeIter child;
  gboolean ret;

  g_assert (DZL_IS_TREE (self));
  g_assert (iter != NULL);
  g_assert (gtk_tree_store_iter_is_valid (priv->store, iter));
  g_assert (func != NULL);

  model = GTK_TREE_MODEL (priv->store);
  path = gtk_tree_model_get_path (model, iter);
  ret = func (model, path, iter, user_data);
  gtk_tree_path_free (path);

  if (ret)
    return TRUE;

  if (gtk_tree_model_iter_children (model, &child, iter))
    {
      do
        {
          if (dzl_tree_foreach (self, &child, func, user_data))
            return TRUE;
        }
      while (gtk_tree_model_iter_next (model, &child));
    }

  return FALSE;
}

static void
pixbuf_func (GtkCellLayout   *cell_layout,
             GtkCellRenderer *cell,
             GtkTreeModel    *tree_model,
             GtkTreeIter     *iter,
             gpointer         data)
{
  g_autoptr(DzlTreeNode) node = NULL;
  g_autoptr(GIcon) old_icon = NULL;
  GIcon *icon;

  g_assert (GTK_IS_CELL_LAYOUT (cell_layout));
  g_assert (GTK_IS_CELL_RENDERER_PIXBUF (cell));
  g_assert (GTK_IS_TREE_MODEL (tree_model));
  g_assert (iter != NULL);

  gtk_tree_model_get (tree_model, iter, 0, &node, -1);
  icon = dzl_tree_node_get_gicon (node);
  g_object_get (cell, "gicon", &old_icon, NULL);
  if (icon != old_icon)
    g_object_set (cell, "gicon", icon, NULL);
}

static void
text_func (GtkCellLayout   *cell_layout,
           GtkCellRenderer *cell,
           GtkTreeModel    *tree_model,
           GtkTreeIter     *iter,
           gpointer         data)
{
  DzlTree *self = data;
  DzlTreePrivate *priv = dzl_tree_get_instance_private (self);
  DzlTreeNode *node = NULL;

  g_assert (DZL_IS_TREE (self));
  g_assert (GTK_IS_CELL_LAYOUT (cell_layout));
  g_assert (GTK_IS_CELL_RENDERER_TEXT (cell));
  g_assert (GTK_IS_TREE_MODEL (tree_model));
  g_assert (iter != NULL);

  gtk_tree_model_get (tree_model, iter, 0, &node, -1);

  if (node)
    {
      GdkRGBA *rgba = NULL;
      const gchar *text;
      gboolean use_markup;

      text = dzl_tree_node_get_text (node);
      use_markup = dzl_tree_node_get_use_markup (node);

      if (dzl_tree_node_get_use_dim_label (node))
        rgba = &priv->dim_foreground;

      g_object_set (cell,
                    use_markup ? "markup" : "text", text,
                    "foreground-rgba", rgba,
                    NULL);
    }
}

static void
dzl_tree_add (DzlTree     *self,
              DzlTreeNode *node,
              DzlTreeNode *child,
              gboolean     prepend)
{
  DzlTreePrivate *priv = dzl_tree_get_instance_private (self);
  GtkTreePath *path;
  GtkTreeIter *parentptr = NULL;
  GtkTreeIter iter;
  GtkTreeIter parent;

  g_return_if_fail (DZL_IS_TREE (self));
  g_return_if_fail (DZL_IS_TREE_NODE (node));
  g_return_if_fail (DZL_IS_TREE_NODE (child));

  _dzl_tree_node_set_tree (child, self);
  _dzl_tree_node_set_parent (child, node);

  g_object_ref_sink (child);

  if (node != priv->root)
    {
      path = dzl_tree_node_get_path (node);
      gtk_tree_model_get_iter (GTK_TREE_MODEL (priv->store), &parent, path);
      parentptr = &parent;
      g_clear_pointer (&path, gtk_tree_path_free);
    }

  gtk_tree_store_insert_with_values (priv->store, &iter, parentptr,
                                     prepend ? 0 : -1,
                                     0, child,
                                     -1);

  if (dzl_tree_node_get_children_possible (child))
    _dzl_tree_node_add_dummy_child (child);

  if (node == priv->root)
    _dzl_tree_build_node (self, child);

  g_object_unref (child);
}

void
_dzl_tree_insert_sorted (DzlTree                *self,
                         DzlTreeNode            *node,
                         DzlTreeNode            *child,
                         DzlTreeNodeCompareFunc  compare_func,
                         gpointer                user_data)
{
  DzlTreePrivate *priv = dzl_tree_get_instance_private (self);
  GtkTreeModel *model;
  GtkTreeIter *parent = NULL;
  GtkTreeIter node_iter;
  GtkTreeIter children;

  g_return_if_fail (DZL_IS_TREE (self));
  g_return_if_fail (DZL_IS_TREE_NODE (node));
  g_return_if_fail (DZL_IS_TREE_NODE (child));
  g_return_if_fail (compare_func != NULL);

  model = GTK_TREE_MODEL (priv->store);

  _dzl_tree_node_set_tree (child, self);
  _dzl_tree_node_set_parent (child, node);

  g_object_ref_sink (child);

  if (dzl_tree_node_get_iter (node, &node_iter))
    parent = &node_iter;

  if (gtk_tree_model_iter_children (model, &children, parent))
    {
      do
        {
          g_autoptr(DzlTreeNode) sibling = NULL;
          GtkTreeIter that;

          gtk_tree_model_get (model, &children, 0, &sibling, -1);

          if (compare_func (sibling, child, user_data) > 0)
            {
              gtk_tree_store_insert_before (priv->store, &that, parent, &children);
              gtk_tree_store_set (priv->store, &that, 0, child, -1);
              goto inserted;
            }
        }
      while (gtk_tree_model_iter_next (model, &children));
    }

  gtk_tree_store_append (priv->store, &children, parent);
  gtk_tree_store_set (priv->store, &children, 0, child, -1);

inserted:
  if (node == priv->root)
    _dzl_tree_build_node (self, child);

  g_object_unref (child);
}

static void
dzl_tree_row_activated (GtkTreeView       *tree_view,
                        GtkTreePath       *path,
                        GtkTreeViewColumn *column)
{
  DzlTree *self = (DzlTree *)tree_view;
  DzlTreePrivate *priv = dzl_tree_get_instance_private (self);
  DzlTreeBuilder *builder;
  GtkTreeModel *model;
  GtkTreeIter iter;
  DzlTreeNode *node = NULL;
  gboolean handled = FALSE;

  g_return_if_fail (DZL_IS_TREE (self));
  g_return_if_fail (path != NULL);

  model = gtk_tree_view_get_model (tree_view);

  if (gtk_tree_model_get_iter (model, &iter, path))
    {
      gtk_tree_model_get (model, &iter, 0, &node, -1);
      for (guint i = 0; i < priv->builders->len; i++)
        {
          builder = g_ptr_array_index (priv->builders, i);
          if ((handled = _dzl_tree_builder_node_activated (builder, node)))
            break;
        }
      g_clear_object (&node);
    }

  if (!handled)
    {
      if (gtk_tree_view_row_expanded (tree_view, path))
        gtk_tree_view_collapse_row (tree_view, path);
      else
        gtk_tree_view_expand_to_path (tree_view, path);
    }
}

static void
dzl_tree_row_expanded (GtkTreeView *tree_view,
                       GtkTreeIter *iter,
                       GtkTreePath *path)
{
  DzlTree *self = (DzlTree *)tree_view;
  GtkTreeModel *model;
  DzlTreeNode *node;

  g_assert (DZL_IS_TREE (self));
  g_assert (iter != NULL);
  g_assert (path != NULL);

  model = gtk_tree_view_get_model (tree_view);

  gtk_tree_model_get (model, iter, 0, &node, -1);

  /*
   * If we are expanding a row that has a dummy child, we might need to
   * build the node immediately, and re-expand it.
   */
  if (_dzl_tree_node_get_needs_build (node))
    {
      _dzl_tree_build_node (self, node);
      dzl_tree_node_expand (node, FALSE);
      dzl_tree_node_select (node);
    }

  g_clear_object (&node);
}

static gboolean
dzl_tree_button_press_event (GtkWidget      *widget,
                             GdkEventButton *button)
{
  DzlTree *self = (DzlTree *)widget;
  DzlTreePrivate *priv = dzl_tree_get_instance_private (self);
  GtkAllocation alloc;
  GtkTreePath *tree_path = NULL;
  GtkTreeIter iter;
  DzlTreeNode *node = NULL;
  gint cell_y;

  g_assert (DZL_IS_TREE (self));
  g_assert (button != NULL);

  if ((button->type == GDK_BUTTON_PRESS) && (button->button == GDK_BUTTON_SECONDARY))
    {
      if (!gtk_widget_has_focus (GTK_WIDGET (self)))
        gtk_widget_grab_focus (GTK_WIDGET (self));

      gtk_tree_view_get_path_at_pos (GTK_TREE_VIEW (self),
                                     button->x,
                                     button->y,
                                     &tree_path,
                                     NULL,
                                     NULL,
                                     &cell_y);

      if (!tree_path)
        {
          dzl_tree_unselect (self);
        }
      else
        {
          gtk_widget_get_allocation (GTK_WIDGET (self), &alloc);
          gtk_tree_model_get_iter (GTK_TREE_MODEL (priv->store), &iter, tree_path);
          gtk_tree_model_get (GTK_TREE_MODEL (priv->store), &iter, 0, &node, -1);
          dzl_tree_select (self, node);
          dzl_tree_popup (self, node, button,
                          alloc.x + alloc.width,
                          button->y - cell_y);
          g_object_unref (node);
          gtk_tree_path_free (tree_path);
        }

      return GDK_EVENT_STOP;
    }

  return GTK_WIDGET_CLASS (dzl_tree_parent_class)->button_press_event (widget, button);
}

static gboolean
dzl_tree_find_item_foreach_cb (GtkTreeModel *model,
                               GtkTreePath  *path,
                               GtkTreeIter  *iter,
                               gpointer      user_data)
{
  DzlTreeNode *node = NULL;
  NodeLookup *lookup = user_data;
  gboolean ret = FALSE;

  g_assert (GTK_IS_TREE_MODEL (model));
  g_assert (path != NULL);
  g_assert (iter != NULL);
  g_assert (lookup != NULL);

  gtk_tree_model_get (model, iter, 0, &node, -1);

  if (node != NULL)
    {
      GObject *item;

      item = dzl_tree_node_get_item (node);

      if (lookup->equal_func (lookup->key, item))
        {
          lookup->result = node;
          ret = TRUE;
        }
    }

  g_clear_object (&node);

  return ret;
}

static void
dzl_tree_real_action (DzlTree     *self,
                      const gchar *prefix,
                      const gchar *action_name,
                      const gchar *param)
{
  GVariant *variant = NULL;
  g_autofree gchar *name = NULL;

  g_return_if_fail (DZL_IS_TREE (self));
  g_return_if_fail (action_name != NULL);

  if (*param != 0)
    {
      GError *error = NULL;

      variant = g_variant_parse (NULL, param, NULL, NULL, &error);

      if (variant == NULL)
        {
          g_warning ("can't parse keybinding parameters \"%s\": %s",
                     param, error->message);
          g_clear_error (&error);
          return;
        }
    }

  if (prefix)
    name = g_strdup_printf ("%s.%s", prefix, action_name);
  else
    name = g_strdup (action_name);

  dzl_gtk_widget_activate_action (GTK_WIDGET (self), name, variant);
}

static gboolean
dzl_tree_default_search_equal_func (GtkTreeModel *model,
                                    gint          column,
                                    const gchar  *key,
                                    GtkTreeIter  *iter,
                                    gpointer      user_data)
{
  DzlTreeNode *node = NULL;
  gboolean ret = TRUE;

  g_assert (GTK_IS_TREE_MODEL (model));
  g_assert (column == 0);
  g_assert (key != NULL);
  g_assert (iter != NULL);

  gtk_tree_model_get (model, iter, 0, &node, -1);

  if (node != NULL)
    {
      const gchar *text;

      text = dzl_tree_node_get_text (node);
      ret = !(strstr (key, text) != NULL);
      g_object_unref (node);
    }

  return ret;
}

static void
dzl_tree_add_child (GtkBuildable *buildable,
                    GtkBuilder   *builder,
                    GObject      *child,
                    const gchar  *type)
{
  DzlTree *self = (DzlTree *)buildable;

  g_assert (DZL_IS_TREE (self));
  g_assert (GTK_IS_BUILDER (builder));
  g_assert (G_IS_OBJECT (child));

  if (g_strcmp0 (type, "builder") == 0)
    {
      if (!DZL_IS_TREE_BUILDER (child))
        {
          g_warning ("Attempt to add invalid builder of type %s to DzlTree.",
                     g_type_name (G_OBJECT_TYPE (child)));
          return;
        }

      dzl_tree_add_builder (self, DZL_TREE_BUILDER (child));
      return;
    }

  dzl_tree_parent_buildable_iface->add_child (buildable, builder, child, type);
}

static void
dzl_tree_style_updated (GtkWidget *widget)
{
  DzlTree *self = (DzlTree *)widget;
  DzlTreePrivate *priv = dzl_tree_get_instance_private (self);
  GtkStyleContext *style_context;

  g_assert (DZL_IS_TREE (self));

  GTK_WIDGET_CLASS (dzl_tree_parent_class)->style_updated (widget);

  style_context = gtk_widget_get_style_context (widget);
  gtk_style_context_save (style_context);
  gtk_style_context_add_class (style_context, "dim-label");
  gtk_style_context_get_color (style_context,
                               gtk_style_context_get_state (style_context),
                               &priv->dim_foreground);
  gtk_style_context_restore (style_context);
}

static void
dzl_tree_finalize (GObject *object)
{
  DzlTree *self = DZL_TREE (object);
  DzlTreePrivate *priv = dzl_tree_get_instance_private (self);

  g_ptr_array_unref (priv->builders);
  g_clear_object (&priv->store);
  g_clear_object (&priv->root);

  G_OBJECT_CLASS (dzl_tree_parent_class)->finalize (object);
}

static void
dzl_tree_get_property (GObject    *object,
                       guint       prop_id,
                       GValue     *value,
                       GParamSpec *pspec)
{
  DzlTree *self = DZL_TREE (object);
  DzlTreePrivate *priv = dzl_tree_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_CONTEXT_MENU:
      g_value_set_object (value, priv->context_menu);
      break;

    case PROP_ROOT:
      g_value_set_object (value, priv->root);
      break;

    case PROP_SELECTION:
      g_value_set_object (value, priv->selection);
      break;

    case PROP_SHOW_ICONS:
      g_value_set_boolean (value, priv->show_icons);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
dzl_tree_set_property (GObject      *object,
                       guint         prop_id,
                       const GValue *value,
                       GParamSpec   *pspec)
{
  DzlTree *self = DZL_TREE (object);

  switch (prop_id)
    {
    case PROP_CONTEXT_MENU:
      dzl_tree_set_context_menu (self, g_value_get_object (value));
      break;

    case PROP_ROOT:
      dzl_tree_set_root (self, g_value_get_object (value));
      break;

    case PROP_SELECTION:
      dzl_tree_select (self, g_value_get_object (value));
      break;

    case PROP_SHOW_ICONS:
      dzl_tree_set_show_icons (self, g_value_get_boolean (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
dzl_tree_buildable_init (GtkBuildableIface *iface)
{
  dzl_tree_parent_buildable_iface = g_type_interface_peek_parent (iface);

  iface->add_child = dzl_tree_add_child;
}

static void
dzl_tree_class_init (DzlTreeClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GtkTreeViewClass *tree_view_class = GTK_TREE_VIEW_CLASS (klass);

  object_class->finalize = dzl_tree_finalize;
  object_class->get_property = dzl_tree_get_property;
  object_class->set_property = dzl_tree_set_property;

  widget_class->popup_menu = dzl_tree_popup_menu;
  widget_class->button_press_event = dzl_tree_button_press_event;
  widget_class->style_updated = dzl_tree_style_updated;

  tree_view_class->row_activated = dzl_tree_row_activated;
  tree_view_class->row_expanded = dzl_tree_row_expanded;

  klass->action = dzl_tree_real_action;

  properties[PROP_CONTEXT_MENU] =
    g_param_spec_object ("context-menu",
                         "Context Menu",
                         "The context menu to display",
                         G_TYPE_MENU_MODEL,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_ROOT] =
    g_param_spec_object ("root",
                         "Root",
                         "The root object of the tree",
                         DZL_TYPE_TREE_NODE,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_SELECTION] =
    g_param_spec_object ("selection",
                         "Selection",
                         "The node selection",
                         DZL_TYPE_TREE_NODE,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties [PROP_SHOW_ICONS] =
    g_param_spec_boolean ("show-icons",
                          "Show Icons",
                          "Show Icons",
                          FALSE,
                          (G_PARAM_READWRITE |
                           G_PARAM_STATIC_STRINGS));

  g_object_class_install_properties (object_class, LAST_PROP, properties);

  signals [ACTION] =
    g_signal_new ("action",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
                  G_STRUCT_OFFSET (DzlTreeClass, action),
                  NULL, NULL, NULL,
                  G_TYPE_NONE,
                  3,
                  G_TYPE_STRING,
                  G_TYPE_STRING,
                  G_TYPE_STRING);

  signals [POPULATE_POPUP] =
    g_signal_new ("populate-popup",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (DzlTreeClass, populate_popup),
                  NULL, NULL, NULL,
                  G_TYPE_NONE,
                  1,
                  GTK_TYPE_WIDGET);
}

static void
dzl_tree_init (DzlTree *self)
{
  DzlTreePrivate *priv = dzl_tree_get_instance_private (self);
  GtkTreeSelection *selection;
  GtkCellRenderer *cell;
  GtkCellLayout *column;

  priv->builders = g_ptr_array_new ();
  g_ptr_array_set_free_func (priv->builders, g_object_unref);
  priv->store = gtk_tree_store_new (1, DZL_TYPE_TREE_NODE);

  selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (self));
  g_signal_connect_object (selection, "changed",
                           G_CALLBACK (dzl_tree_selection_changed),
                           self,
                           G_CONNECT_SWAPPED);

  column = g_object_new (GTK_TYPE_TREE_VIEW_COLUMN,
                         "title", "Node",
                         NULL);
  priv->column = GTK_TREE_VIEW_COLUMN (column);

  cell = g_object_new (GTK_TYPE_CELL_RENDERER_PIXBUF,
                       "xpad", 3,
                       "visible", priv->show_icons,
                       NULL);
  priv->cell_pixbuf = cell;
  g_object_bind_property (self, "show-icons", cell, "visible", 0);
  gtk_cell_layout_pack_start (column, cell, FALSE);
  gtk_cell_layout_set_cell_data_func (column, cell, pixbuf_func, NULL, NULL);

  cell = g_object_new (GTK_TYPE_CELL_RENDERER_TEXT,
                       "ellipsize", PANGO_ELLIPSIZE_NONE,
                       NULL);
  priv->cell_text = cell;
  gtk_cell_layout_pack_start (column, cell, TRUE);
  gtk_cell_layout_set_cell_data_func (column, cell, text_func, self, NULL);

  gtk_tree_view_append_column (GTK_TREE_VIEW (self),
                               GTK_TREE_VIEW_COLUMN (column));

  gtk_tree_view_set_model (GTK_TREE_VIEW (self),
                           GTK_TREE_MODEL (priv->store));

  gtk_tree_view_set_search_equal_func (GTK_TREE_VIEW (self),
                                       dzl_tree_default_search_equal_func,
                                       NULL, NULL);
  gtk_tree_view_set_search_column (GTK_TREE_VIEW (self), 0);
}

void
dzl_tree_expand_to_node (DzlTree     *self,
                         DzlTreeNode *node)
{
  g_assert (DZL_IS_TREE (self));
  g_assert (DZL_IS_TREE_NODE (node));

  if (dzl_tree_node_get_expanded (node))
    {
      dzl_tree_node_expand (node, TRUE);
    }
  else
    {
      dzl_tree_node_expand (node, TRUE);
      dzl_tree_node_collapse (node);
    }
}

gboolean
dzl_tree_get_show_icons (DzlTree *self)
{
  DzlTreePrivate *priv = dzl_tree_get_instance_private (self);

  g_return_val_if_fail (DZL_IS_TREE (self), FALSE);

  return priv->show_icons;
}

void
dzl_tree_set_show_icons (DzlTree   *self,
                         gboolean   show_icons)
{
  DzlTreePrivate *priv = dzl_tree_get_instance_private (self);

  g_return_if_fail (DZL_IS_TREE (self));

  show_icons = !!show_icons;

  if (show_icons != priv->show_icons)
    {
      priv->show_icons = show_icons;
      g_object_set (priv->cell_pixbuf, "visible", show_icons, NULL);
      /*
       * WORKAROUND:
       *
       * Changing the visibility of the cell does not force a redraw of the
       * tree view. So to force it, we will hide/show our entire pixbuf/text
       * column.
       */
      gtk_tree_view_column_set_visible (priv->column, FALSE);
      gtk_tree_view_column_set_visible (priv->column, TRUE);
      g_object_notify_by_pspec (G_OBJECT (self),
                                properties [PROP_SHOW_ICONS]);
    }
}

/**
 * dzl_tree_get_selected:
 * @self: (in): A #DzlTree.
 *
 * Gets the currently selected node in the tree.
 *
 * Returns: (transfer none): A #DzlTreeNode.
 */
DzlTreeNode *
dzl_tree_get_selected (DzlTree *self)
{
  GtkTreeSelection *selection;
  GtkTreeModel *model;
  GtkTreeIter iter;
  DzlTreeNode *ret = NULL;

  g_return_val_if_fail (DZL_IS_TREE (self), NULL);

  selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (self));
  if (gtk_tree_selection_get_selected (selection, &model, &iter))
    {
      gtk_tree_model_get (model, &iter, 0, &ret, -1);

      /*
       * We incurred an extra reference when extracting the value from
       * the treemodel. Since it owns the reference, we can drop it here
       * so that we don't transfer the ownership to the caller.
       */
      g_object_unref (ret);
    }

  return ret;
}

/**
 * dzl_tree_unselect_all:
 * @self: (in): A #DzlTree.
 *
 * Unselects the currently selected node in the tree.
 */
void
dzl_tree_unselect_all (DzlTree *self)
{
  GtkTreeSelection *selection;

  g_return_if_fail (DZL_IS_TREE (self));

  selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (self));
  gtk_tree_selection_unselect_all (selection);
}

void
dzl_tree_scroll_to_node (DzlTree     *self,
                         DzlTreeNode *node)
{
  GtkTreePath *path;

  g_return_if_fail (DZL_IS_TREE (self));
  g_return_if_fail (DZL_IS_TREE_NODE (node));

  path = dzl_tree_node_get_path (node);
  gtk_tree_view_scroll_to_cell (GTK_TREE_VIEW (self), path, NULL, FALSE, 0, 0);
  gtk_tree_path_free (path);
}

GtkTreePath *
_dzl_tree_get_path (DzlTree *self,
                    GList   *list)
{
  DzlTreePrivate *priv = dzl_tree_get_instance_private (self);
  GtkTreeModel *model;
  GtkTreeIter iter;
  GtkTreeIter *iter_ptr;
  GList *list_iter;

  g_assert (DZL_IS_TREE (self));

  model = GTK_TREE_MODEL (priv->store);

  if ((list == NULL) || (list->data != priv->root) || (list->next == NULL))
    return NULL;

  iter_ptr = NULL;

  for (list_iter = list->next; list_iter; list_iter = list_iter->next)
    {
      GtkTreeIter children;

      if (gtk_tree_model_iter_children (model, &children, iter_ptr))
        {
          gboolean found = FALSE;

          do
            {
              g_autoptr(DzlTreeNode) item = NULL;

              gtk_tree_model_get (model, &children, 0, &item, -1);
              found = (item == (DzlTreeNode *)list_iter->data);
            }
          while (!found && gtk_tree_model_iter_next (model, &children));

          if (found)
            {
              iter = children;
              iter_ptr = &iter;
              continue;
            }
        }

      return NULL;
    }

  return gtk_tree_model_get_path (model, &iter);
}

/**
 * dzl_tree_add_builder:
 * @self: A #DzlTree.
 * @builder: A #DzlTreeBuilder to add.
 *
 * Add a builder to the tree.
 */
void
dzl_tree_add_builder (DzlTree        *self,
                      DzlTreeBuilder *builder)
{
  GtkTreeIter iter;
  DzlTreePrivate *priv = dzl_tree_get_instance_private (self);

  g_return_if_fail (DZL_IS_TREE (self));
  g_return_if_fail (DZL_IS_TREE_BUILDER (builder));

  g_ptr_array_add (priv->builders, g_object_ref_sink (builder));

  _dzl_tree_builder_set_tree (builder, self);
  _dzl_tree_builder_added (builder, self);

  if (gtk_tree_model_get_iter_first (GTK_TREE_MODEL (priv->store), &iter))
    dzl_tree_foreach (self, &iter, dzl_tree_add_builder_foreach_cb, builder);
}

/**
 * dzl_tree_remove_builder:
 * @self: (in): A #DzlTree.
 * @builder: (in): A #DzlTreeBuilder to remove.
 *
 * Removes a builder from the tree.
 */
void
dzl_tree_remove_builder (DzlTree        *self,
                         DzlTreeBuilder *builder)
{
  DzlTreePrivate *priv = dzl_tree_get_instance_private (self);
  gsize i;

  g_return_if_fail (DZL_IS_TREE (self));
  g_return_if_fail (DZL_IS_TREE_BUILDER (builder));

  for (i = 0; i < priv->builders->len; i++)
    {
      if (builder == g_ptr_array_index (priv->builders, i))
        {
          g_object_ref (builder);
          g_ptr_array_remove_index (priv->builders, i);
          _dzl_tree_builder_removed (builder, self);
          g_object_unref (builder);
        }
    }
}

/**
 * dzl_tree_get_root:
 *
 * Retrieves the root node of the tree. The root node is not a visible node
 * in the self, but a placeholder for all other builders to build upon.
 *
 * Returns: (transfer none) (nullable): A #DzlTreeNode or %NULL.
 */
DzlTreeNode *
dzl_tree_get_root (DzlTree *self)
{
  DzlTreePrivate *priv = dzl_tree_get_instance_private (self);

  g_return_val_if_fail (DZL_IS_TREE (self), NULL);

  return priv->root;
}

/**
 * dzl_tree_set_root:
 * @self: A #DzlTree.
 * @node: A #DzlTreeNode.
 *
 * Sets the root node of the #DzlTree widget. This is used to build
 * the items within the treeview. The item itself will not be added
 * to the self, but the direct children will be.
 */
void
dzl_tree_set_root (DzlTree     *self,
                   DzlTreeNode *root)
{
  DzlTreePrivate *priv = dzl_tree_get_instance_private (self);

  g_return_if_fail (DZL_IS_TREE (self));

  if (priv->root != root)
    {
      GtkTreeSelection *selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (self));
      GtkTreeModel *current;

      gtk_tree_selection_unselect_all (selection);

      if (priv->root != NULL)
        {
          _dzl_tree_node_set_parent (priv->root, NULL);
          _dzl_tree_node_set_tree (priv->root, NULL);
          gtk_tree_store_clear (priv->store);
          g_clear_object (&priv->root);
        }

      current = gtk_tree_view_get_model (GTK_TREE_VIEW (self));
      if (GTK_IS_TREE_MODEL_FILTER (current))
        gtk_tree_model_filter_clear_cache (GTK_TREE_MODEL_FILTER (current));

      if (root != NULL)
        {
          priv->root = g_object_ref_sink (root);
          _dzl_tree_node_set_parent (priv->root, NULL);
          _dzl_tree_node_set_tree (priv->root, self);
          _dzl_tree_build_node (self, priv->root);
        }

      g_object_notify_by_pspec (G_OBJECT (self), properties [PROP_ROOT]);
    }
}

void
dzl_tree_rebuild (DzlTree *self)
{
  DzlTreePrivate *priv = dzl_tree_get_instance_private (self);
  GtkTreeSelection *selection;

  g_return_if_fail (DZL_IS_TREE (self));

  /*
   * We don't want notification of selection changes while rebuilding.
   */
  selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (self));
  gtk_tree_selection_unselect_all (selection);

  if (priv->root != NULL)
    {
      gtk_tree_store_clear (priv->store);
      _dzl_tree_build_node (self, priv->root);
    }
}

/**
 * dzl_tree_find_custom:
 * @self: A #DzlTree
 * @equal_func: (scope call): A #GEqualFunc
 * @key: the key for @equal_func
 *
 * Walks the entire tree looking for the first item that matches given
 * @equal_func and @key.
 *
 * The first parameter to @equal_func will always be @key.
 * The second parameter will be the nodes #DzlTreeNode:item property.
 *
 * Returns: (nullable) (transfer none): A #DzlTreeNode or %NULL.
 */
DzlTreeNode *
dzl_tree_find_custom (DzlTree     *self,
                      GEqualFunc   equal_func,
                      gpointer     key)
{
  DzlTreePrivate *priv = dzl_tree_get_instance_private (self);
  NodeLookup lookup;

  g_return_val_if_fail (DZL_IS_TREE (self), NULL);
  g_return_val_if_fail (equal_func != NULL, NULL);

  lookup.key = key;
  lookup.equal_func = equal_func;
  lookup.result = NULL;

  gtk_tree_model_foreach (GTK_TREE_MODEL (priv->store),
                          dzl_tree_find_item_foreach_cb,
                          &lookup);

  return lookup.result;
}

/**
 * dzl_tree_find_item:
 * @self: A #DzlTree.
 * @item: (allow-none): A #GObject or %NULL.
 *
 * Finds a #DzlTreeNode with an item property matching @item.
 *
 * Returns: (transfer none) (nullable): A #DzlTreeNode or %NULL.
 */
DzlTreeNode *
dzl_tree_find_item (DzlTree  *self,
                    GObject  *item)
{
  DzlTreePrivate *priv = dzl_tree_get_instance_private (self);
  NodeLookup lookup;

  g_return_val_if_fail (DZL_IS_TREE (self), NULL);
  g_return_val_if_fail (!item || G_IS_OBJECT (item), NULL);

  lookup.key = item;
  lookup.equal_func = g_direct_equal;
  lookup.result = NULL;

  gtk_tree_model_foreach (GTK_TREE_MODEL (priv->store),
                          dzl_tree_find_item_foreach_cb,
                          &lookup);

  return lookup.result;
}

void
_dzl_tree_append (DzlTree     *self,
                  DzlTreeNode *node,
                  DzlTreeNode *child)
{
  g_return_if_fail (DZL_IS_TREE (self));
  g_return_if_fail (DZL_IS_TREE_NODE (node));
  g_return_if_fail (DZL_IS_TREE_NODE (child));

  dzl_tree_add (self, node, child, FALSE);
}

void
_dzl_tree_prepend (DzlTree     *self,
                   DzlTreeNode *node,
                   DzlTreeNode *child)
{
  g_return_if_fail (DZL_IS_TREE (self));
  g_return_if_fail (DZL_IS_TREE_NODE (node));
  g_return_if_fail (DZL_IS_TREE_NODE (child));

  dzl_tree_add (self, node, child, TRUE);
}

void
_dzl_tree_invalidate (DzlTree     *self,
                      DzlTreeNode *node)
{
  DzlTreePrivate *priv = dzl_tree_get_instance_private (self);
  GtkTreeModel *model;
  GtkTreePath *path;
  DzlTreeNode *parent;
  GtkTreeIter iter;
  GtkTreeIter child;

  g_return_if_fail (DZL_IS_TREE (self));
  g_return_if_fail (DZL_IS_TREE_NODE (node));

  model = GTK_TREE_MODEL (priv->store);
  path = dzl_tree_node_get_path (node);

  if (path != NULL)
    {
      gtk_tree_model_get_iter (model, &iter, path);

      if (gtk_tree_model_iter_children (model, &child, &iter))
        {
          while (gtk_tree_store_remove (priv->store, &child))
            {
              /* Do nothing */
            }
        }

      gtk_tree_path_free (path);
    }

  _dzl_tree_node_set_needs_build (node, TRUE);

  parent = dzl_tree_node_get_parent (node);

  if ((parent == NULL) || dzl_tree_node_get_expanded (parent))
    _dzl_tree_build_node (self, node);
}

/**
 * dzl_tree_find_child_node:
 * @self: A #DzlTree
 * @node: A #DzlTreeNode
 * @find_func: (scope call): A callback to locate the child
 * @user_data: user data for @find_func
 *
 * Searches through the direct children of @node for a matching child.
 * @find_func should return %TRUE if the child matches, otherwise %FALSE.
 *
 * Returns: (transfer none) (nullable): A #DzlTreeNode or %NULL.
 */
DzlTreeNode *
dzl_tree_find_child_node (DzlTree         *self,
                          DzlTreeNode     *node,
                          DzlTreeFindFunc  find_func,
                          gpointer         user_data)
{
  DzlTreePrivate *priv = dzl_tree_get_instance_private (self);
  GtkTreeModel *model;
  GtkTreePath *path;
  GtkTreeIter iter;
  GtkTreeIter children;

  g_return_val_if_fail (DZL_IS_TREE (self), NULL);
  g_return_val_if_fail (!node || DZL_IS_TREE_NODE (node), NULL);
  g_return_val_if_fail (find_func, NULL);

  if (node == NULL)
    node = priv->root;

  if (node == NULL)
    {
      g_warning ("Cannot find node. No root node has been set on %s.",
                 g_type_name (G_OBJECT_TYPE (self)));
      return NULL;
    }

  if (_dzl_tree_node_get_needs_build (node))
    _dzl_tree_build_node (self, node);

  model = GTK_TREE_MODEL (priv->store);
  path = dzl_tree_node_get_path (node);

  if (path != NULL)
    {
      if (!gtk_tree_model_get_iter (model, &iter, path))
        goto failure;

      if (!gtk_tree_model_iter_children (model, &children, &iter))
        goto failure;
    }
  else
    {
      if (!gtk_tree_model_iter_children (model, &children, NULL))
        goto failure;
    }

  do
    {
      DzlTreeNode *child = NULL;

      gtk_tree_model_get (model, &children, 0, &child, -1);

      if (find_func (self, node, child, user_data))
        {
          /*
           * We want to returned a borrowed reference to the child node.
           * It is safe to unref the child here before we return.
           */
          g_object_unref (child);
          return child;
        }

      g_clear_object (&child);
    }
  while (gtk_tree_model_iter_next (model, &children));

failure:
  g_clear_pointer (&path, gtk_tree_path_free);

  return NULL;
}

void
_dzl_tree_remove (DzlTree     *self,
                  DzlTreeNode *node)
{
  DzlTreePrivate *priv = dzl_tree_get_instance_private (self);
  GtkTreePath *path;
  GtkTreeIter iter;

  g_return_if_fail (DZL_IS_TREE (self));
  g_return_if_fail (DZL_IS_TREE_NODE (node));

  path = dzl_tree_node_get_path (node);

  if (gtk_tree_model_get_iter (GTK_TREE_MODEL (priv->store), &iter, path))
    gtk_tree_store_remove (priv->store, &iter);

  gtk_tree_path_free (path);
}

gboolean
_dzl_tree_get_iter (DzlTree      *self,
                    DzlTreeNode  *node,
                    GtkTreeIter  *iter)
{
  DzlTreePrivate *priv = dzl_tree_get_instance_private (self);
  GtkTreePath *path;
  gboolean ret = FALSE;

  g_return_val_if_fail (DZL_IS_TREE (self), FALSE);
  g_return_val_if_fail (DZL_IS_TREE_NODE (node), FALSE);
  g_return_val_if_fail (iter, FALSE);

  if ((path = dzl_tree_node_get_path (node)) != NULL)
    {
      ret = gtk_tree_model_get_iter (GTK_TREE_MODEL (priv->store), iter, path);
      gtk_tree_path_free (path);
    }

  return ret;
}

static void
filter_func_free (gpointer user_data)
{
  FilterFunc *data = user_data;

  if (data->filter_data_destroy)
    data->filter_data_destroy (data->filter_data);

  g_free (data);
}

static gboolean
dzl_tree_model_filter_recursive (GtkTreeModel *model,
                                 GtkTreeIter  *parent,
                                 FilterFunc   *filter)
{
  GtkTreeIter child;

  if (gtk_tree_model_iter_children (model, &child, parent))
    {
      do
        {
          g_autoptr(DzlTreeNode) node = NULL;

          gtk_tree_model_get (model, &child, 0, &node, -1);

          if ((node != NULL) && !_dzl_tree_node_get_needs_build (node))
            {
              if (filter->filter_func (filter->self, node, filter->filter_data))
                return TRUE;

              if (dzl_tree_model_filter_recursive (model, &child, filter))
                return TRUE;
            }
        }
      while (gtk_tree_model_iter_next (model, &child));
    }

  return FALSE;
}

static gboolean
dzl_tree_model_filter_visible_func (GtkTreeModel *model,
                                    GtkTreeIter  *iter,
                                    gpointer      data)
{
  DzlTreeNode *node = NULL;
  FilterFunc *filter = data;
  gboolean ret;

  g_assert (filter != NULL);
  g_assert (DZL_IS_TREE (filter->self));
  g_assert (filter->filter_func != NULL);

  /*
   * This is a rather complex situation.
   *
   * We might not match, but one of our children nodes might match.
   * Furthering the issue, the children might still need to be built.
   * For some cases, this could be really expensive (think file tree)
   * but for other things, it could be cheap. If you are going to use
   * a filter func for your tree, you probably should avoid being
   * too lazy and ensure the nodes are available.
   *
   * Therefore, we will only check available nodes, and ignore the
   * case where the children nodes need to be built.   *
   *
   * TODO: Another option would be to iteratively build the items after
   *       the initial filter.
   */

  gtk_tree_model_get (model, iter, 0, &node, -1);
  ret = filter->filter_func (filter->self, node, filter->filter_data);
  g_clear_object (&node);

  /*
   * Short circuit if we already matched.
   */
  if (ret)
    return TRUE;

  /*
   * If any of our children match, we should match.
   */
  if (dzl_tree_model_filter_recursive (model, iter, filter))
    return TRUE;

  return FALSE;
}

/**
 * dzl_tree_set_filter:
 * @self: A #DzlTree
 * @filter_func: (scope notified): A callback to determien visibility.
 * @filter_data: User data for @filter_func.
 * @filter_data_destroy: Destroy notify for @filter_data.
 *
 * Sets the filter function to be used to determine visability of a tree node.
 */
void
dzl_tree_set_filter (DzlTree           *self,
                     DzlTreeFilterFunc  filter_func,
                     gpointer           filter_data,
                     GDestroyNotify     filter_data_destroy)
{
  DzlTreePrivate *priv = dzl_tree_get_instance_private (self);

  g_return_if_fail (DZL_IS_TREE (self));

  if (filter_func == NULL)
    {
      gtk_tree_view_set_model (GTK_TREE_VIEW (self), GTK_TREE_MODEL (priv->store));
    }
  else
    {
      FilterFunc *data;
      GtkTreeModel *filter;

      data = g_new0 (FilterFunc, 1);
      data->self = self;
      data->filter_func = filter_func;
      data->filter_data = filter_data;
      data->filter_data_destroy = filter_data_destroy;

      filter = gtk_tree_model_filter_new (GTK_TREE_MODEL (priv->store), NULL);
      gtk_tree_model_filter_set_visible_func (GTK_TREE_MODEL_FILTER (filter),
                                              dzl_tree_model_filter_visible_func,
                                              data,
                                              filter_func_free);
      gtk_tree_view_set_model (GTK_TREE_VIEW (self), GTK_TREE_MODEL (filter));
      g_clear_object (&filter);
    }
}

GtkTreeStore *
_dzl_tree_get_store (DzlTree *self)
{
  DzlTreePrivate *priv = dzl_tree_get_instance_private (self);

  g_return_val_if_fail (DZL_IS_TREE (self), NULL);

  return priv->store;
}
