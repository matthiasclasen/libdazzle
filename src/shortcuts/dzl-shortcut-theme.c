/* dzl-shortcut-theme.c
 *
 * Copyright (C) 2016 Christian Hergert <chergert@redhat.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
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

#define G_LOG_DOMAIN "dzl-shortcut-theme"

#include "shortcuts/dzl-shortcut-private.h"
#include "shortcuts/dzl-shortcut-chord.h"
#include "shortcuts/dzl-shortcut-theme.h"

typedef struct
{
  gchar *name;
  gchar *title;
  gchar *subtitle;

  /*
   * The parent_name property can be used to inherit from another
   * shortcut theme when dispatching operations. The controllers
   * will use this to locate the parent theme/context pair and
   * try after the active theme fails to dispatch.
   */
  gchar *parent_name;

  /*
   * A hashtable of context names to the context pointer. The hashtable
   * owns the reference to the context.
   */
  GHashTable *contexts;

  /*
   * Commands and actions can be mapped from a context or directly from the
   * theme for convenience (to avoid having to define them from every context).
   */
  DzlShortcutChordTable *actions_table;
  DzlShortcutChordTable *commands_table;

  /*
   * Weak back-pointer to the DzlShorcutMangaer that owns this theme. A theme
   * can only be in one manager at a time. This will be cleared when the theme
   * is removed from the manager.
   */
  DzlShortcutManager *manager;

  /*
   * The theme also maintains a list of chains overridden by the theme. These
   * are indexed by the interned string pointer (for direct pointer copmarison)
   * of the command_id or action_id.
   */
  GHashTable *chains;
} DzlShortcutThemePrivate;

enum {
  PROP_0,
  PROP_NAME,
  PROP_PARENT_NAME,
  PROP_SUBTITLE,
  PROP_TITLE,
  N_PROPS
};

G_DEFINE_TYPE_WITH_PRIVATE (DzlShortcutTheme, dzl_shortcut_theme, G_TYPE_OBJECT)

static GParamSpec *properties [N_PROPS];

static void
dzl_shortcut_theme_finalize (GObject *object)
{
  DzlShortcutTheme *self = (DzlShortcutTheme *)object;
  DzlShortcutThemePrivate *priv = dzl_shortcut_theme_get_instance_private (self);

  g_clear_pointer (&priv->name, g_free);
  g_clear_pointer (&priv->parent_name, g_free);
  g_clear_pointer (&priv->title, g_free);
  g_clear_pointer (&priv->subtitle, g_free);
  g_clear_pointer (&priv->contexts, g_hash_table_unref);
  g_clear_pointer (&priv->chains, g_hash_table_unref);
  g_clear_pointer (&priv->actions_table, dzl_shortcut_chord_table_free);
  g_clear_pointer (&priv->commands_table, dzl_shortcut_chord_table_free);

  G_OBJECT_CLASS (dzl_shortcut_theme_parent_class)->finalize (object);
}

static void
dzl_shortcut_theme_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  DzlShortcutTheme *self = (DzlShortcutTheme *)object;

  switch (prop_id)
    {
    case PROP_NAME:
      g_value_set_string (value, dzl_shortcut_theme_get_name (self));
      break;

    case PROP_PARENT_NAME:
      g_value_set_string (value, dzl_shortcut_theme_get_parent_name (self));
      break;

    case PROP_TITLE:
      g_value_set_string (value, dzl_shortcut_theme_get_title (self));
      break;

    case PROP_SUBTITLE:
      g_value_set_string (value, dzl_shortcut_theme_get_subtitle (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
dzl_shortcut_theme_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  DzlShortcutTheme *self = (DzlShortcutTheme *)object;
  DzlShortcutThemePrivate *priv = dzl_shortcut_theme_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_NAME:
      priv->name = g_value_dup_string (value);
      break;

    case PROP_PARENT_NAME:
      dzl_shortcut_theme_set_parent_name (self, g_value_get_string (value));
      break;

    case PROP_TITLE:
      g_free (priv->title);
      priv->title = g_value_dup_string (value);
      break;

    case PROP_SUBTITLE:
      g_free (priv->subtitle);
      priv->subtitle = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
dzl_shortcut_theme_class_init (DzlShortcutThemeClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = dzl_shortcut_theme_finalize;
  object_class->get_property = dzl_shortcut_theme_get_property;
  object_class->set_property = dzl_shortcut_theme_set_property;

  properties [PROP_NAME] =
    g_param_spec_string ("name",
                         "Name",
                         "The name of the theme",
                         NULL,
                         (G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

  properties [PROP_PARENT_NAME] =
    g_param_spec_string ("parent-name",
                         "Parent Name",
                         "The name of the parent shortcut theme",
                         NULL,
                         (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  properties [PROP_TITLE] =
    g_param_spec_string ("title",
                         "Title",
                         "The title of the theme as used for UI elements",
                         NULL,
                         (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  properties [PROP_SUBTITLE] =
    g_param_spec_string ("subtitle",
                         "Subtitle",
                         "The subtitle of the theme as used for UI elements",
                         NULL,
                         (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_properties (object_class, N_PROPS, properties);
}

static void
dzl_shortcut_theme_init (DzlShortcutTheme *self)
{
  DzlShortcutThemePrivate *priv = dzl_shortcut_theme_get_instance_private (self);

  priv->commands_table = dzl_shortcut_chord_table_new ();
  priv->actions_table = dzl_shortcut_chord_table_new ();
  priv->contexts = g_hash_table_new_full (NULL, NULL, NULL, g_object_unref);
  priv->chains = g_hash_table_new_full (NULL, NULL, NULL,
                                        (GDestroyNotify)dzl_shortcut_closure_chain_free);
}

const gchar *
dzl_shortcut_theme_get_name (DzlShortcutTheme *self)
{
  DzlShortcutThemePrivate *priv = dzl_shortcut_theme_get_instance_private (self);

  g_return_val_if_fail (DZL_IS_SHORTCUT_THEME (self), NULL);

  return priv->name;
}

/**
 * dzl_shortcut_theme_find_context_by_name:
 * @self: An #DzlShortcutContext
 * @name: The name of the context
 *
 * Gets the context named @name. If the context does not exist, it will
 * be created.
 *
 * Returns: (not nullable) (transfer none): An #DzlShortcutContext
 */
DzlShortcutContext *
dzl_shortcut_theme_find_context_by_name (DzlShortcutTheme *self,
                                         const gchar      *name)
{
  DzlShortcutThemePrivate *priv = dzl_shortcut_theme_get_instance_private (self);
  DzlShortcutContext *ret;

  g_return_val_if_fail (DZL_IS_SHORTCUT_THEME (self), NULL);
  g_return_val_if_fail (name != NULL, NULL);

  name = g_intern_string (name);

  if (NULL == (ret = g_hash_table_lookup (priv->contexts, name)))
    {
      ret = dzl_shortcut_context_new (name);
      g_hash_table_insert (priv->contexts, (gchar *)name, ret);
    }

  return ret;
}

static DzlShortcutContext *
dzl_shortcut_theme_find_default_context_by_type (DzlShortcutTheme *self,
                                                 GType             type)
{
  g_return_val_if_fail (DZL_IS_SHORTCUT_THEME (self), NULL);
  g_return_val_if_fail (g_type_is_a (type, GTK_TYPE_WIDGET), NULL);

  return dzl_shortcut_theme_find_context_by_name (self, g_type_name (type));
}

/**
 * dzl_shortcut_theme_find_default_context:
 *
 * Finds the default context in the theme for @widget.
 *
 * Returns: (nullable) (transfer none): An #DzlShortcutContext or %NULL.
 */
DzlShortcutContext *
dzl_shortcut_theme_find_default_context (DzlShortcutTheme *self,
                                         GtkWidget        *widget)
{
  g_return_val_if_fail (DZL_IS_SHORTCUT_THEME (self), NULL);
  g_return_val_if_fail (GTK_IS_WIDGET (widget), NULL);

  return dzl_shortcut_theme_find_default_context_by_type (self, G_OBJECT_TYPE (widget));
}

void
dzl_shortcut_theme_add_context (DzlShortcutTheme   *self,
                                DzlShortcutContext *context)
{
  DzlShortcutThemePrivate *priv = dzl_shortcut_theme_get_instance_private (self);
  const gchar *name;

  g_return_if_fail (DZL_IS_SHORTCUT_THEME (self));
  g_return_if_fail (DZL_IS_SHORTCUT_CONTEXT (context));

  name = dzl_shortcut_context_get_name (context);

  g_return_if_fail (name != NULL);

  g_hash_table_insert (priv->contexts, g_strdup (name), g_object_ref (context));
}

DzlShortcutTheme *
dzl_shortcut_theme_new (const gchar *name)
{
  return g_object_new (DZL_TYPE_SHORTCUT_THEME,
                       "name", name,
                       NULL);
}

const gchar *
dzl_shortcut_theme_get_title (DzlShortcutTheme *self)
{
  DzlShortcutThemePrivate *priv = dzl_shortcut_theme_get_instance_private (self);

  g_return_val_if_fail (DZL_IS_SHORTCUT_THEME (self), NULL);

  return priv->title;
}

const gchar *
dzl_shortcut_theme_get_subtitle (DzlShortcutTheme *self)
{
  DzlShortcutThemePrivate *priv = dzl_shortcut_theme_get_instance_private (self);

  g_return_val_if_fail (DZL_IS_SHORTCUT_THEME (self), NULL);

  return priv->subtitle;
}

GHashTable *
_dzl_shortcut_theme_get_contexts (DzlShortcutTheme *self)
{
  DzlShortcutThemePrivate *priv = dzl_shortcut_theme_get_instance_private (self);

  g_return_val_if_fail (DZL_IS_SHORTCUT_THEME (self), NULL);

  return priv->contexts;
}

void
_dzl_shortcut_theme_set_name (DzlShortcutTheme *self,
                              const gchar      *name)
{
  DzlShortcutThemePrivate *priv = dzl_shortcut_theme_get_instance_private (self);

  g_return_if_fail (DZL_IS_SHORTCUT_THEME (self));

  if (g_strcmp0 (name, priv->name) != 0)
    {
      g_free (priv->name);
      priv->name = g_strdup (name);
      g_object_notify_by_pspec (G_OBJECT (self), properties [PROP_NAME]);
    }
}

/**
 * dzl_shortcut_theme_get_parent_name:
 * @self: a #DzlShortcutTheme
 *
 * Gets the name of the parent shortcut theme.
 *
 * This is used to resolve shortcuts from the parent theme without having to
 * copy them directly into this shortcut theme. It allows for some level of
 * copy-on-write (CoW).
 *
 * Returns: (nullable): The name of the parent theme, or %NULL if none is set.
 */
const gchar *
dzl_shortcut_theme_get_parent_name (DzlShortcutTheme *self)
{
  DzlShortcutThemePrivate *priv = dzl_shortcut_theme_get_instance_private (self);

  g_return_val_if_fail (DZL_IS_SHORTCUT_THEME (self), NULL);

  return priv->parent_name;
}

void
dzl_shortcut_theme_set_parent_name (DzlShortcutTheme *self,
                                    const gchar      *parent_name)
{
  DzlShortcutThemePrivate *priv = dzl_shortcut_theme_get_instance_private (self);

  g_return_if_fail (DZL_IS_SHORTCUT_THEME (self));

  if (g_strcmp0 (parent_name, priv->parent_name) != 0)
    {
      g_free (priv->parent_name);
      priv->parent_name = g_strdup (parent_name);
      g_object_notify_by_pspec (G_OBJECT (self), properties [PROP_PARENT_NAME]);
    }
}

void
dzl_shortcut_theme_set_chord_for_action (DzlShortcutTheme       *self,
                                         const gchar            *detailed_action_name,
                                         const DzlShortcutChord *chord)
{
  DzlShortcutThemePrivate *priv = dzl_shortcut_theme_get_instance_private (self);

  g_return_if_fail (DZL_IS_SHORTCUT_THEME (self));

  detailed_action_name = g_intern_string (detailed_action_name);

  if (detailed_action_name == NULL)
    {
      dzl_shortcut_chord_table_remove (priv->actions_table, chord);
      return;
    }

  dzl_shortcut_chord_table_remove_data (priv->actions_table,
                                        (gpointer)detailed_action_name);

  if (chord != NULL)
    dzl_shortcut_chord_table_add (priv->actions_table, chord,
                                  (gpointer)detailed_action_name);

  if (!g_hash_table_contains (priv->chains, detailed_action_name))
    g_hash_table_insert (priv->chains,
                         (gchar *)detailed_action_name,
                         dzl_shortcut_closure_chain_append_action_string (NULL, detailed_action_name));
}

const DzlShortcutChord *
dzl_shortcut_theme_get_chord_for_action (DzlShortcutTheme *self,
                                         const gchar      *detailed_action_name)
{
  DzlShortcutThemePrivate *priv = dzl_shortcut_theme_get_instance_private (self);

  g_return_val_if_fail (DZL_IS_SHORTCUT_THEME (self), NULL);

  if (priv->actions_table == NULL)
    return NULL;

  return dzl_shortcut_chord_table_lookup_data (priv->actions_table,
                                               (gpointer)g_intern_string (detailed_action_name));
}

void
dzl_shortcut_theme_set_accel_for_action (DzlShortcutTheme *self,
                                         const gchar      *detailed_action_name,
                                         const gchar      *accel)
{
  g_autoptr(DzlShortcutChord) chord = NULL;

  g_return_if_fail (DZL_IS_SHORTCUT_THEME (self));

  if (accel != NULL)
    chord = dzl_shortcut_chord_new_from_string (accel);

  dzl_shortcut_theme_set_chord_for_action (self, detailed_action_name, chord);
}

/**
 * dzl_shortcut_theme_set_chord_for_command:
 * @self: a #DzlShortcutTheme
 * @chord: (nullable): the chord for the command
 * @command: (nullable): the command to be executed
 *
 * This will set the command to execute when @chord is pressed.  If command is
 * %NULL, the accelerator will be cleared.  If @chord is %NULL, all
 * accelerators for @command will be cleared.
 */
void
dzl_shortcut_theme_set_chord_for_command (DzlShortcutTheme       *self,
                                          const gchar            *command,
                                          const DzlShortcutChord *chord)
{
  DzlShortcutThemePrivate *priv = dzl_shortcut_theme_get_instance_private (self);

  g_return_if_fail (DZL_IS_SHORTCUT_THEME (self));

  command = g_intern_string (command);

  if (command == NULL)
    {
      dzl_shortcut_chord_table_remove (priv->commands_table, chord);
      return;
    }

  dzl_shortcut_chord_table_remove_data (priv->commands_table, (gpointer)command);

  if (chord != NULL)
    dzl_shortcut_chord_table_add (priv->commands_table, chord, (gpointer)command);

  if (!g_hash_table_contains (priv->chains, command))
    g_hash_table_insert (priv->chains,
                         (gchar *)command,
                         dzl_shortcut_closure_chain_append_command (NULL, command));
}

const DzlShortcutChord *
dzl_shortcut_theme_get_chord_for_command (DzlShortcutTheme *self,
                                          const gchar      *command)
{
  DzlShortcutThemePrivate *priv = dzl_shortcut_theme_get_instance_private (self);

  g_return_val_if_fail (DZL_IS_SHORTCUT_THEME (self), NULL);

  if (priv->commands_table == NULL)
    return NULL;

  return dzl_shortcut_chord_table_lookup_data (priv->commands_table,
                                               (gpointer)g_intern_string (command));
}

/**
 * dzl_shortcut_theme_set_accel_for_command:
 * @self: a #DzlShortcutTheme
 * @command: (nullable): the command to be executed
 * @accel: (nullable): the shortcut accelerator
 *
 * This will set the command to execute when @accel is pressed.  If command is
 * %NULL, the accelerator will be cleared.  If accelerator is %NULL, all
 * accelerators for @command will be cleared.
 */
void
dzl_shortcut_theme_set_accel_for_command (DzlShortcutTheme *self,
                                          const gchar      *command,
                                          const gchar      *accel)
{
  g_autoptr(DzlShortcutChord) chord = NULL;

  g_return_if_fail (DZL_IS_SHORTCUT_THEME (self));

  if (accel != NULL)
    chord = dzl_shortcut_chord_new_from_string (accel);

  dzl_shortcut_theme_set_chord_for_command (self, command, chord);
}

/**
 * dzl_shortcut_theme_get_parent:
 * @self: a #DzlShortcutTheme
 *
 * If the #DzlShortcutTheme:parent-name property has been set, this will fetch
 * the parent #DzlShortcutTheme.
 *
 * Returns: (transfer none) (nullable): A #DzlShortcutTheme or %NULL.
 */
DzlShortcutTheme *
dzl_shortcut_theme_get_parent (DzlShortcutTheme *self)
{
  DzlShortcutThemePrivate *priv = dzl_shortcut_theme_get_instance_private (self);

  g_assert (DZL_IS_SHORTCUT_THEME (self));

  if (priv->parent_name == NULL)
    return NULL;

  if (priv->manager == NULL)
    return NULL;

  return dzl_shortcut_manager_get_theme_by_name (priv->manager, priv->parent_name);
}

DzlShortcutMatch
_dzl_shortcut_theme_match (DzlShortcutTheme         *self,
                           const DzlShortcutChord   *chord,
                           DzlShortcutClosureChain **chain)
{
  DzlShortcutThemePrivate *priv = dzl_shortcut_theme_get_instance_private (self);
  DzlShortcutTheme *parent;
  DzlShortcutMatch match1;
  DzlShortcutMatch match2;
  DzlShortcutMatch match3 = DZL_SHORTCUT_MATCH_NONE;
  const gchar *action_id = NULL;
  const gchar *command_id = NULL;

  g_return_val_if_fail (DZL_IS_SHORTCUT_THEME (self), FALSE);
  g_return_val_if_fail (chord != NULL, FALSE);
  g_return_val_if_fail (chain != NULL, FALSE);

  /* TODO: We probably need the ability to have a "block" or "unbind" type
   *       disabling when we implement chaining up to the parent theme.
   */

  match1 = dzl_shortcut_chord_table_lookup (priv->actions_table, chord, (gpointer *)&action_id);

  if (match1 == DZL_SHORTCUT_MATCH_EQUAL)
    {
      *chain = g_hash_table_lookup (priv->chains, action_id);
      return match1;
    }

  match2 = dzl_shortcut_chord_table_lookup (priv->commands_table, chord, (gpointer *)&command_id);

  if (match2 == DZL_SHORTCUT_MATCH_EQUAL)
    {
      *chain = g_hash_table_lookup (priv->chains, command_id);
      return match2;
    }

  /*
   * We didn't find anything in this theme, try our parent theme.
   */

  parent = dzl_shortcut_theme_get_parent (self);

  if (parent != NULL)
    {
      match3 = _dzl_shortcut_theme_match (parent, chord, chain);

      if (match3 == DZL_SHORTCUT_MATCH_EQUAL)
        return match3;
    }

  /*
   * Nothing found, let the caller know if we found a partial match
   * and ensure we zero out chain just to be safe.
   */

  *chain = NULL;

  return (match1 || match2 || match3) ? DZL_SHORTCUT_MATCH_PARTIAL : DZL_SHORTCUT_MATCH_NONE;
}

void
_dzl_shortcut_theme_set_manager (DzlShortcutTheme   *self,
                                 DzlShortcutManager *manager)
{
  DzlShortcutThemePrivate *priv = dzl_shortcut_theme_get_instance_private (self);

  g_return_if_fail (DZL_IS_SHORTCUT_THEME (self));
  g_return_if_fail (!manager || DZL_IS_SHORTCUT_MANAGER (manager));
  g_return_if_fail (priv->manager == NULL);

  priv->manager = manager;
}
