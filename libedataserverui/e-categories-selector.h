/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef E_CATEGORIES_SELECTOR_H
#define E_CATEGORIES_SELECTOR_H

#include <gtk/gtk.h>

/* Standard GObject macros */
#define E_TYPE_CATEGORIES_SELECTOR \
	(e_categories_selector_get_type ())
#define E_CATEGORIES_SELECTOR(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST \
	((obj), E_TYPE_CATEGORIES_SELECTOR, ECategoriesSelector))
#define E_CATEGORIES_SELECTOR_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_CAST \
	((cls), E_TYPE_CATEGORIES_SELECTOR, ECategoriesSelectorClass))
#define E_IS_CATEGORIES_SELECTOR(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE \
	((obj), E_TYPE_CATEGORIES_SELECTOR))
#define E_IS_CATEGORIES_SELECTOR_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_TYPE \
	((cls), E_TYPE_CATEGORIES_SELECTOR))
#define E_CATEGORIES_SELECTOR_GET_CLASS(obj) \
	(G_TYPE_INSTANCE_GET_CLASS \
	((obj), E_TYPE_CATEGORIES_SELECTOR, ECategoriesSelectorClass))

G_BEGIN_DECLS

typedef struct _ECategoriesSelector ECategoriesSelector;
typedef struct _ECategoriesSelectorClass ECategoriesSelectorClass;
typedef struct _ECategoriesSelectorPrivate ECategoriesSelectorPrivate;

/**
 * ECategoriesSelector:
 *
 * Contains only private data that should be read and manipulated using the
 * functions below.
 *
 * Since: 3.2
 **/
struct _ECategoriesSelector {
	GtkTreeView parent;
	ECategoriesSelectorPrivate *priv;
};

struct _ECategoriesSelectorClass {
	GtkTreeViewClass parent_class;

	void		(*category_checked)	(ECategoriesSelector *selector,
						 const gchar *category,
						 gboolean checked);

	void		(*selection_changed)	(ECategoriesSelector *selector,
						 GtkTreeSelection *selection);
};

GType		e_categories_selector_get_type	(void);
GtkWidget *	e_categories_selector_new	(void);
const gchar *	e_categories_selector_get_checked
						(ECategoriesSelector *selector);
void		e_categories_selector_set_checked
						(ECategoriesSelector *selector,
						 const gchar *categories);
gboolean	e_categories_selector_get_items_checkable
						(ECategoriesSelector *selector);
void		e_categories_selector_set_items_checkable
						(ECategoriesSelector *selectr,
						 gboolean checkable);
void		e_categories_selector_delete_selection
						(ECategoriesSelector *selector);
const gchar *	e_categories_selector_get_selected
						(ECategoriesSelector *selector);

G_END_DECLS

#endif /* E_CATEGORIES_SELECTOR_H */