/*
 * Copyright (c) 2014 Benjamin Otte <otte@gnome.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicntnse,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright noticnt and this permission noticnt shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "config.h"
#include <glib/gi18n-lib.h>

#include "css-node-tree.h"
#include "prop-editor.h"

#include "gtktreemodelcssnode.h"
#include "gtktreeview.h"
#include "gtklabel.h"
#include "gtkpopover.h"
#include "gtk/gtkwidgetprivate.h"
#include "gtkcssproviderprivate.h"
#include "gtkcssstylepropertyprivate.h"
#include "gtkcsssectionprivate.h"
#include "gtkcssstyleprivate.h"
#include "gtkcssvalueprivate.h"
#include "gtkliststore.h"
#include "gtksettings.h"
#include "gtktreeview.h"
#include "gtktreeselection.h"
#include "gtktypebuiltins.h"
#include "gtkmodelbutton.h"
#include "gtkstack.h"

enum {
  COLUMN_NODE_NAME,
  COLUMN_NODE_TYPE,
  COLUMN_NODE_VISIBLE,
  COLUMN_NODE_CLASSES,
  COLUMN_NODE_ID,
  COLUMN_NODE_STATE,
  /* add more */
  N_NODE_COLUMNS
};

enum
{
  COLUMN_PROP_NAME,
  COLUMN_PROP_VALUE,
  COLUMN_PROP_LOCATION
};

struct _GtkInspectorCssNodeTreePrivate
{
  GtkWidget *node_tree;
  GtkWidget *stack;
  GtkWidget *node_list_button;
  GtkWidget *prop_list_button;
  GtkWidget *css_node_info;
  GtkTreeModel *node_model;
  GtkTreeViewColumn *node_name_column;
  GtkTreeViewColumn *node_id_column;
  GtkTreeViewColumn *node_classes_column;
  GtkWidget *object_title;
  GtkListStore *prop_model;
  GtkWidget *prop_tree;
  GtkTreeViewColumn *prop_name_column;
  GHashTable *prop_iters;
  GtkCssNode *node;
};

G_DEFINE_TYPE_WITH_PRIVATE (GtkInspectorCssNodeTree, gtk_inspector_css_node_tree, GTK_TYPE_BOX)

typedef struct {
  GtkCssNode *node;
  const gchar *prop_name;
  GdkRectangle rect;
  GtkInspectorCssNodeTree *cnt;
} NodePropEditor;

static void
show_node_prop_editor (NodePropEditor *npe)
{
  GtkWidget *popover;
  GtkWidget *editor;

  popover = gtk_popover_new (GTK_WIDGET (npe->cnt->priv->node_tree));
  gtk_popover_set_pointing_to (GTK_POPOVER (popover), &npe->rect);

  editor = gtk_inspector_prop_editor_new (G_OBJECT (npe->node), npe->prop_name, FALSE);
  gtk_widget_show (editor);

  gtk_container_add (GTK_CONTAINER (popover), editor);

  if (gtk_inspector_prop_editor_should_expand (GTK_INSPECTOR_PROP_EDITOR (editor)))
    gtk_widget_set_vexpand (popover, TRUE);

  gtk_widget_show (popover);

  g_signal_connect (popover, "unmap", G_CALLBACK (gtk_widget_destroy), NULL);
}

static void
row_activated (GtkTreeView             *tv,
               GtkTreePath             *path,
               GtkTreeViewColumn       *col,
               GtkInspectorCssNodeTree *cnt)
{
  GtkTreeIter iter;
  NodePropEditor npe;

  npe.cnt = cnt;

  if (col == cnt->priv->node_name_column)
    npe.prop_name = "name";
  else if (col == cnt->priv->node_id_column)
    npe.prop_name = "id";
  else if (col == cnt->priv->node_classes_column)
    npe.prop_name = "classes";
  else
    return;

  gtk_tree_model_get_iter (cnt->priv->node_model, &iter, path);
  npe.node = gtk_tree_model_css_node_get_node_from_iter (GTK_TREE_MODEL_CSS_NODE (cnt->priv->node_model), &iter);
  gtk_tree_view_get_cell_area (tv, path, col, &npe.rect);
  gtk_tree_view_convert_bin_window_to_widget_coords (tv, npe.rect.x, npe.rect.y, &npe.rect.x, &npe.rect.y);

  show_node_prop_editor (&npe);
}

static void
switch_to_node_list (GtkInspectorCssNodeTree *cnt)
{
  gtk_stack_set_visible_child_name (GTK_STACK (cnt->priv->stack), "node-list");
  gtk_widget_show (cnt->priv->prop_list_button);
  gtk_widget_hide (cnt->priv->node_list_button);
}

static void
switch_to_prop_list (GtkInspectorCssNodeTree *cnt)
{
  gtk_stack_set_visible_child_name (GTK_STACK (cnt->priv->stack), "prop-list");
  gtk_widget_show (cnt->priv->node_list_button);
  gtk_widget_hide (cnt->priv->prop_list_button);
}

static void
show_node_popover (GtkInspectorCssNodeTree *cnt,
                   gdouble                  x,
                   gdouble                  y)
{
  GtkTreeSelection *selection;
  GdkRectangle rect;
  GtkWidget *popover;
  GtkWidget *box;
  GtkWidget *button;
  GtkTreeIter iter;
  GtkTreePath *path;
  NodePropEditor *npe;
  GtkCssNode *node;

  selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (cnt->priv->node_tree));
  if (!gtk_tree_selection_get_selected (selection, NULL, &iter))
    return;

  path = gtk_tree_model_get_path (cnt->priv->node_model, &iter);
  gtk_tree_view_get_cell_area (GTK_TREE_VIEW (cnt->priv->node_tree), path, NULL, &rect);
  gtk_tree_view_convert_bin_window_to_widget_coords (GTK_TREE_VIEW (cnt->priv->node_tree),
                                                     rect.x, rect.y, &rect.x, &rect.y);

  rect.x = CLAMP (x - 20, 0, gtk_widget_get_allocated_width (cnt->priv->node_tree) - 40);
  rect.width = 40;

  node = gtk_tree_model_css_node_get_node_from_iter (GTK_TREE_MODEL_CSS_NODE (cnt->priv->node_model), &iter);

  npe = g_new0 (NodePropEditor, 1);
  npe->node = node;
  npe->prop_name = "name";
  npe->rect = rect;
  npe->cnt = cnt;

  popover = gtk_popover_new (GTK_WIDGET (cnt->priv->node_tree));

  gtk_popover_set_pointing_to (GTK_POPOVER (popover), &rect);
  box = g_object_new (GTK_TYPE_BOX,
                      "orientation", GTK_ORIENTATION_VERTICAL,
                      "visible", TRUE,
                      "margin", 10,
                      NULL);
  gtk_container_add (GTK_CONTAINER (popover), box);
  button = g_object_new (GTK_TYPE_MODEL_BUTTON,
                         "visible", TRUE,
                         "text", _("Change name"),
                         NULL);
  g_signal_connect_swapped (button, "clicked", G_CALLBACK (show_node_prop_editor), npe);
  g_object_set_data_full (G_OBJECT (popover), "prop-name", npe, g_free);

  gtk_container_add (GTK_CONTAINER (box), button);

  npe = g_new0 (NodePropEditor, 1);
  npe->node = node;
  npe->prop_name = "classes";
  npe->rect = rect;
  npe->cnt = cnt;

  button = g_object_new (GTK_TYPE_MODEL_BUTTON,
                         "visible", TRUE,
                         "text", _("Change classes"),
                         NULL);
  g_signal_connect_swapped (button, "clicked", G_CALLBACK (show_node_prop_editor), npe);
  g_object_set_data_full (G_OBJECT (popover), "prop-classes", npe, g_free);

  gtk_container_add (GTK_CONTAINER (box), button);

  button = g_object_new (GTK_TYPE_MODEL_BUTTON,
                         "visible", TRUE,
                         "text", _("CSS properties"),
                         NULL);
  g_signal_connect_swapped (button, "clicked", G_CALLBACK (switch_to_prop_list), cnt);
  gtk_container_add (GTK_CONTAINER (box), button);

  gtk_tree_path_free (path);

  gtk_widget_show (popover);

  g_signal_connect (popover, "unmap", G_CALLBACK (gtk_widget_destroy), NULL);
}

static gboolean
button_pressed (GtkWidget               *widget,
                GdkEventButton          *event,
                GtkInspectorCssNodeTree *cnt)
{
  static gboolean in_press = FALSE;

  if (in_press)
    return FALSE;

  if (!gdk_event_triggers_context_menu ((GdkEvent *)event))
    return FALSE;

  in_press = TRUE;
  gtk_widget_event (cnt->priv->node_tree, (GdkEvent *) event);
  in_press = FALSE;

 show_node_popover (cnt, event->x, event->y);

  return TRUE;
}

static void
gtk_inspector_css_node_tree_set_node (GtkInspectorCssNodeTree *cnt,
                                      GtkCssNode              *node);

static void
selection_changed (GtkTreeSelection *selection, GtkInspectorCssNodeTree *cnt)
{
  GtkTreeIter iter;
  GtkCssNode *node;

  if (!gtk_tree_selection_get_selected (selection, NULL, &iter))
    return;

  node = gtk_tree_model_css_node_get_node_from_iter (GTK_TREE_MODEL_CSS_NODE (cnt->priv->node_model), &iter);
  gtk_inspector_css_node_tree_set_node (cnt, node);
}

static void
gtk_inspector_css_node_tree_unset_node (GtkInspectorCssNodeTree *cnt)
{
  GtkInspectorCssNodeTreePrivate *priv = cnt->priv;

  if (priv->node)
    {
      g_signal_handlers_disconnect_matched (priv->node,
                                            G_SIGNAL_MATCH_DATA,
                                            0, 0, NULL, NULL,
                                            cnt);
      g_object_unref (priv->node);
      priv->node = NULL;
    }
}

static void
gtk_inspector_css_node_tree_finalize (GObject *object)
{
  GtkInspectorCssNodeTree *cnt = GTK_INSPECTOR_CSS_NODE_TREE (object);

  gtk_inspector_css_node_tree_unset_node (cnt);

  g_hash_table_unref (cnt->priv->prop_iters);

  G_OBJECT_CLASS (gtk_inspector_css_node_tree_parent_class)->finalize (object);
}

static void
ensure_css_sections (void)
{
  GtkSettings *settings;
  gchar *theme_name;

  gtk_css_provider_set_keep_css_sections ();

  settings = gtk_settings_get_default ();
  g_object_get (settings, "gtk-theme-name", &theme_name, NULL);
  g_object_set (settings, "gtk-theme-name", theme_name, NULL);
  g_free (theme_name);
}

static void
gtk_inspector_css_node_tree_class_init (GtkInspectorCssNodeTreeClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  ensure_css_sections ();

  object_class->finalize = gtk_inspector_css_node_tree_finalize;

  gtk_widget_class_set_template_from_resource (widget_class, "/org/gtk/libgtk/inspector/css-node-tree.ui");
  gtk_widget_class_bind_template_child_private (widget_class, GtkInspectorCssNodeTree, node_tree);
  gtk_widget_class_bind_template_child_private (widget_class, GtkInspectorCssNodeTree, object_title);
  gtk_widget_class_bind_template_child_private (widget_class, GtkInspectorCssNodeTree, node_name_column);
  gtk_widget_class_bind_template_child_private (widget_class, GtkInspectorCssNodeTree, node_id_column);
  gtk_widget_class_bind_template_child_private (widget_class, GtkInspectorCssNodeTree, node_classes_column);
  gtk_widget_class_bind_template_child_private (widget_class, GtkInspectorCssNodeTree, prop_name_column);
  gtk_widget_class_bind_template_child_private (widget_class, GtkInspectorCssNodeTree, prop_model);
  gtk_widget_class_bind_template_child_private (widget_class, GtkInspectorCssNodeTree, prop_name_column);
  gtk_widget_class_bind_template_child_private (widget_class, GtkInspectorCssNodeTree, stack);
  gtk_widget_class_bind_template_child_private (widget_class, GtkInspectorCssNodeTree, prop_list_button);
  gtk_widget_class_bind_template_child_private (widget_class, GtkInspectorCssNodeTree, node_list_button);
  gtk_widget_class_bind_template_child_private (widget_class, GtkInspectorCssNodeTree, css_node_info);

  gtk_widget_class_bind_template_callback (widget_class, row_activated);
  gtk_widget_class_bind_template_callback (widget_class, selection_changed);
  gtk_widget_class_bind_template_callback (widget_class, button_pressed);
  gtk_widget_class_bind_template_callback (widget_class, switch_to_node_list);
  gtk_widget_class_bind_template_callback (widget_class, switch_to_prop_list);
}

static int
sort_strv (gconstpointer a,
           gconstpointer b,
           gpointer      data)
{
  char **ap = (char **) a;
  char **bp = (char **) b;

  return g_ascii_strcasecmp (*ap, *bp);
}

static void
strv_sort (char **strv)
{
  g_qsort_with_data (strv,
		     g_strv_length (strv),
                     sizeof (char *),
                     sort_strv,
                     NULL);
}

static gchar *
format_state_flags (GtkStateFlags state)
{
  GFlagsClass *fclass;
  GString *str;
  gint i;

  str = g_string_new ("");

  if (state)
    {
      fclass = g_type_class_ref (GTK_TYPE_STATE_FLAGS);
      for (i = 0; i < fclass->n_values; i++)
        {
          if (state & fclass->values[i].value)
            {
              if (str->len)
                g_string_append (str, " | ");
              g_string_append (str, fclass->values[i].value_nick);
            }
        }
      g_type_class_unref (fclass);
    }
  else
    g_string_append (str, "normal");

  return g_string_free (str, FALSE);
}

static void
gtk_inspector_css_node_tree_get_node_value (GtkTreeModelCssNode *model,
                                            GtkCssNode          *node,
                                            int                  column,
                                            GValue              *value)
{
  char **strv;
  char *s;

  switch (column)
    {
    case COLUMN_NODE_NAME:
      g_value_set_string (value, gtk_css_node_get_name (node));
      break;

    case COLUMN_NODE_TYPE:
      g_value_set_string (value, g_type_name (gtk_css_node_get_widget_type (node)));
      break;

    case COLUMN_NODE_VISIBLE:
      g_value_set_boolean (value, gtk_css_node_get_visible (node));
      break;

    case COLUMN_NODE_CLASSES:
      strv = gtk_css_node_get_classes (node);
      strv_sort (strv);
      s = g_strjoinv (" ", strv);
      g_value_take_string (value, s);
      g_strfreev (strv);
      break;

    case COLUMN_NODE_ID:
      g_value_set_string (value, gtk_css_node_get_id (node));
      break;

    case COLUMN_NODE_STATE:
      g_value_take_string (value, format_state_flags (gtk_css_node_get_state (node)));
      break;

    default:
      g_assert_not_reached ();
      break;
    }
}

static void
gtk_inspector_css_node_tree_init (GtkInspectorCssNodeTree *cnt)
{
  GtkInspectorCssNodeTreePrivate *priv;
  gint i;

  cnt->priv = gtk_inspector_css_node_tree_get_instance_private (cnt);
  gtk_widget_init_template (GTK_WIDGET (cnt));
  priv = cnt->priv;

  priv->node_model = gtk_tree_model_css_node_new (gtk_inspector_css_node_tree_get_node_value,
                                                  N_NODE_COLUMNS,
                                                  G_TYPE_STRING,
                                                  G_TYPE_STRING,
                                                  G_TYPE_BOOLEAN,
                                                  G_TYPE_STRING,
                                                  G_TYPE_STRING,
                                                  G_TYPE_STRING);
  gtk_tree_view_set_model (GTK_TREE_VIEW (priv->node_tree), priv->node_model);
  g_object_unref (priv->node_model);

  gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (cnt->priv->prop_model),
                                        COLUMN_PROP_NAME,
                                        GTK_SORT_ASCENDING);

  priv->prop_iters = g_hash_table_new_full (g_str_hash, g_str_equal,
                                            NULL, (GDestroyNotify) gtk_tree_iter_free);

  for (i = 0; i < _gtk_css_style_property_get_n_properties (); i++)
    {
      GtkCssStyleProperty *prop;
      GtkTreeIter iter;
      const gchar *name;

      prop = _gtk_css_style_property_lookup_by_id (i);
      name = _gtk_style_property_get_name (GTK_STYLE_PROPERTY (prop));

      gtk_list_store_append (cnt->priv->prop_model, &iter);
      gtk_list_store_set (cnt->priv->prop_model, &iter, COLUMN_PROP_NAME, name, -1);
      g_hash_table_insert (cnt->priv->prop_iters, (gpointer)name, gtk_tree_iter_copy (&iter));
    }
}

void
gtk_inspector_css_node_tree_set_object (GtkInspectorCssNodeTree *cnt,
                                        GObject                 *object)
{
  GtkInspectorCssNodeTreePrivate *priv;
  const gchar *title;
  GtkCssNode *node, *root;
  GtkTreePath *path;
  GtkTreeIter iter;

  g_return_if_fail (GTK_INSPECTOR_IS_CSS_NODE_TREE (cnt));

  priv = cnt->priv;

  title = (const gchar *)g_object_get_data (object, "gtk-inspector-object-title");
  gtk_label_set_label (GTK_LABEL (priv->object_title), title);

  if (!GTK_IS_WIDGET (object))
    {
      gtk_widget_hide (GTK_WIDGET (cnt));
      return;
    }

  gtk_widget_show (GTK_WIDGET (cnt));

  switch_to_node_list (cnt);

  root = node = gtk_widget_get_css_node (GTK_WIDGET (object));
  while (gtk_css_node_get_parent (root))
    root = gtk_css_node_get_parent (root);

  gtk_tree_model_css_node_set_root_node (GTK_TREE_MODEL_CSS_NODE (priv->node_model), root);

  gtk_tree_model_css_node_get_iter_from_node (GTK_TREE_MODEL_CSS_NODE (priv->node_model), &iter, node);
  path = gtk_tree_model_get_path (priv->node_model, &iter);

  gtk_tree_view_expand_to_path (GTK_TREE_VIEW (priv->node_tree), path);
  gtk_tree_view_set_cursor (GTK_TREE_VIEW (priv->node_tree), path, NULL, FALSE);
  gtk_tree_view_scroll_to_cell (GTK_TREE_VIEW (priv->node_tree), path, NULL, TRUE, 0.5, 0.0);

  gtk_tree_path_free (path);
}

static void
gtk_inspector_css_node_tree_update_style (GtkInspectorCssNodeTree *cnt,
                                          GtkCssStyle             *new_style)
{
  GtkInspectorCssNodeTreePrivate *priv = cnt->priv;
  gint i;

  for (i = 0; i < _gtk_css_style_property_get_n_properties (); i++)
    {
      GtkCssStyleProperty *prop;
      const gchar *name;
      GtkTreeIter *iter;
      GtkCssSection *section;
      gchar *location;
      gchar *value;

      prop = _gtk_css_style_property_lookup_by_id (i);
      name = _gtk_style_property_get_name (GTK_STYLE_PROPERTY (prop));

      iter = (GtkTreeIter *)g_hash_table_lookup (priv->prop_iters, name);

      if (new_style)
        {
          value = _gtk_css_value_to_string (gtk_css_style_get_value (new_style, i));

          section = gtk_css_style_get_section (new_style, i);
          if (section)
            location = _gtk_css_section_to_string (section);
          else
            location = NULL;
        }
      else
        {
          value = NULL;
          location = NULL;
        }

      gtk_list_store_set (priv->prop_model,
                          iter,
                          COLUMN_PROP_VALUE, value,
                          COLUMN_PROP_LOCATION, location,
                          -1);

      g_free (location);
      g_free (value);
    }
}

static void
gtk_inspector_css_node_tree_update_style_cb (GtkCssNode              *node,
                                             GtkCssStyleChange       *change,
                                             GtkInspectorCssNodeTree *cnt)
{
  gtk_inspector_css_node_tree_update_style (cnt, gtk_css_style_change_get_new_style (change));
}

static void
gtk_inspector_css_node_tree_set_node (GtkInspectorCssNodeTree *cnt,
                                      GtkCssNode              *node)
{
  GtkInspectorCssNodeTreePrivate *priv = cnt->priv;
  GString *s;
  GType type;
  const gchar *name;
  gchar **strv;
  gint i;

  if (priv->node == node)
    return;

  if (node)
    g_object_ref (node);

  gtk_inspector_css_node_tree_update_style (cnt, node ? gtk_css_node_get_style (node) : NULL);

  gtk_inspector_css_node_tree_unset_node (cnt);

  priv->node = node;

  g_signal_connect (node, "style-changed", G_CALLBACK (gtk_inspector_css_node_tree_update_style_cb), cnt);

  s = g_string_new ("");
  type = gtk_css_node_get_widget_type (node);
  if (type != G_TYPE_NONE && type != G_TYPE_INVALID)
    g_string_append (s, g_type_name (type));

  name = gtk_css_node_get_name (node);
  if (name)
    {
      if (s->len > 0)
        g_string_append (s, " — ");
      g_string_append (s, name);
    }

  strv = gtk_css_node_get_classes (node);
  if (strv[0] != NULL)
    {
      strv_sort (strv);
      if (s->len > 0)
        g_string_append (s, " — ");
      for (i = 0; strv[i]; i++)
        {
          if (i > 0)
            g_string_append (s, " ");
          g_string_append (s, strv[i]);
        }
      g_strfreev (strv);
    }

  gtk_label_set_label (GTK_LABEL (cnt->priv->css_node_info), s->str);
  g_string_free (s, TRUE);
}

// vim: set et sw=2 ts=2:
