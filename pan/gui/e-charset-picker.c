/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/*
 * Copyright (C) 2001 Ximian, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "e-charset-picker.h"

#include <string.h>
#include <iconv.h>

#include <gtk/gtkhbox.h>
#include <gtk/gtkvbox.h>
#include <gtk/gtkentry.h>
#include <gtk/gtkstock.h>
#include <gtk/gtklabel.h>
#include <gtk/gtkmenuitem.h>
#include <gtk/gtkoptionmenu.h>
#include <gtk/gtksignal.h>
#include <gtk/gtkmessagedialog.h>

#include <glib/gi18n.h>

typedef enum {
	E_CHARSET_UNKNOWN,
	E_CHARSET_BALTIC,
	E_CHARSET_CENTRAL_EUROPEAN,
	E_CHARSET_CHINESE,
	E_CHARSET_CYRILLIC,
	E_CHARSET_GREEK,
	E_CHARSET_HEBREW,
	E_CHARSET_JAPANESE,
	E_CHARSET_KOREAN,
	E_CHARSET_THAI,
	E_CHARSET_TURKISH,
	E_CHARSET_UNICODE,
	E_CHARSET_WESTERN_EUROPEAN,
	E_CHARSET_WESTERN_EUROPEAN_NEW,
} ECharsetClass;

static const char *classnames[] = {
	N_("Unknown"),
	N_("Baltic"),
	N_("Central European"),
	N_("Chinese"),
	N_("Cyrillic"),
	N_("Greek"),
	N_("Hebrew"),
	N_("Japanese"),
	N_("Korean"),
	N_("Thai"),
	N_("Turkish"),
	N_("Unicode"),
	N_("Western European"),
	N_("Western European, New"),
};

typedef struct {
	char *name;
	ECharsetClass class;
	char *subclass;
} ECharset;

/* This list is based on what other mailers/browsers support. There's
 * not a lot of point in using, say, ISO-8859-3, if anything that can
 * read that can read UTF8 too.
 */
/* To Translators: Character set "Logical Hebrew" */
static ECharset charsets[] = {
	{ "ISO-8859-13", E_CHARSET_BALTIC, NULL },
	{ "ISO-8859-4", E_CHARSET_BALTIC, NULL },
	{ "ISO-8859-2", E_CHARSET_CENTRAL_EUROPEAN, NULL },
	{ "Big5", E_CHARSET_CHINESE, N_("Traditional") },
	{ "BIG5HKSCS", E_CHARSET_CHINESE, N_("Traditional") },
	{ "EUC-TW", E_CHARSET_CHINESE, N_("Traditional") },
	{ "GB18030", E_CHARSET_CHINESE, N_("Simplified") },
	{ "GB2312", E_CHARSET_CHINESE, N_("Simplified") },
	{ "HZ", E_CHARSET_CHINESE, N_("Simplified") },
	{ "ISO-2022-CN", E_CHARSET_CHINESE, N_("Simplified") },
	{ "KOI8-R", E_CHARSET_CYRILLIC, NULL },
	{ "Windows-1251", E_CHARSET_CYRILLIC, NULL },
	{ "KOI8-U", E_CHARSET_CYRILLIC, N_("Ukrainian") },
	{ "ISO-8859-5", E_CHARSET_CYRILLIC, NULL },
	{ "ISO-8859-7", E_CHARSET_GREEK, NULL },
	{ "ISO-8859-8", E_CHARSET_HEBREW, N_("Visual") },
	{ "ISO-2022-JP", E_CHARSET_JAPANESE, NULL },
	{ "EUC-JP", E_CHARSET_JAPANESE, NULL },
	{ "Shift_JIS", E_CHARSET_JAPANESE, NULL },
	{ "EUC-KR", E_CHARSET_KOREAN, NULL },
	{ "TIS-620", E_CHARSET_THAI, NULL },
	{ "ISO-8859-9", E_CHARSET_TURKISH, NULL },
	{ "UTF-8", E_CHARSET_UNICODE, NULL },
	{ "UTF-7", E_CHARSET_UNICODE, NULL },
	{ "ISO-8859-1", E_CHARSET_WESTERN_EUROPEAN, NULL },
	{ "ISO-8859-15", E_CHARSET_WESTERN_EUROPEAN_NEW, NULL },
};
static const int num_charsets = sizeof (charsets) / sizeof (charsets[0]);

static void
select_item (GtkMenuShell *menu_shell, GtkWidget *item)
{
	gtk_menu_shell_select_item (menu_shell, item);
	gtk_menu_shell_deactivate (menu_shell);
}

static void
activate (GtkWidget *item, gpointer menu)
{
	g_object_set_data ((GObject *) menu, "activated_item", item);
}

static GtkWidget *
add_charset (GtkWidget *menu, ECharset *charset, gboolean free_name)
{
	GtkWidget *item;
	char *label;
	
	if (charset->subclass) {
		label = g_strdup_printf ("%s, %s (%s)",
					 _(classnames[charset->class]),
					 _(charset->subclass),
					 charset->name);
	} else if (charset->class) {
		label = g_strdup_printf ("%s (%s)",
					 _(classnames[charset->class]),
					 charset->name);
	} else {
		label = g_strdup (charset->name);
	}
	
	item = gtk_menu_item_new_with_label (label);
	g_object_set_data_full ((GObject *) item, "charset",
				charset->name, free_name ? g_free : NULL);
	g_free (label);
	
	gtk_widget_show (item);
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
	g_signal_connect (item, "activate", G_CALLBACK (activate), menu);
	
	return item;
}

static gboolean
add_other_charset (GtkWidget *menu, GtkWidget *other, const char *new_charset) 
{
	ECharset charset = { NULL, E_CHARSET_UNKNOWN, NULL };
	GtkWidget *item;
	iconv_t ic;
	
	ic = iconv_open ("UTF-8", new_charset);
	if (ic == (iconv_t)-1) {
		return FALSE;
	}
	iconv_close (ic);
	
	/* Temporarily remove the "Other..." item */
	g_object_ref (other);
	gtk_container_remove (GTK_CONTAINER (menu), other);
	
	/* Create new menu item */
	charset.name = g_strdup (new_charset);
	item = add_charset (menu, &charset, TRUE);
	
	/* And re-add "Other..." */
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), other);
	g_object_unref (other);
	
	g_object_set_data_full ((GObject *) menu, "other_charset",
				g_strdup (new_charset), g_free);
	
	g_object_set_data ((GObject *) menu, "activated_item", item);
	select_item (GTK_MENU_SHELL (menu), item);
	
	return TRUE;
}

static void
activate_entry (GtkWidget *entry, GtkDialog *dialog)
{
	gtk_dialog_response (dialog, GTK_RESPONSE_OK);
}

static void
activate_other (GtkWidget *item, gpointer menu)
{
	GtkWidget *window, *entry, *label, *vbox, *hbox;
	char *old_charset, *new_charset;
	GtkDialog *dialog;

	window = gtk_widget_get_toplevel (menu);
	if (!GTK_WIDGET_TOPLEVEL (window))
		window = gtk_widget_get_ancestor (item, GTK_TYPE_WINDOW);

	old_charset = g_object_get_data(menu, "other_charset");

	dialog = GTK_DIALOG (gtk_dialog_new_with_buttons (_("Character Encoding"),
							  GTK_WINDOW (window),
							  GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
							  GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
							  GTK_STOCK_OK, GTK_RESPONSE_OK,
							  NULL));

	gtk_dialog_set_has_separator (dialog, FALSE);
	gtk_dialog_set_default_response (dialog, GTK_RESPONSE_OK);

	vbox = gtk_vbox_new (FALSE, 6);
	gtk_container_set_border_width (GTK_CONTAINER (vbox), 12);
	gtk_box_pack_start (GTK_BOX (dialog->vbox), vbox, TRUE, TRUE, 0);
	gtk_widget_show (vbox);

	label = gtk_label_new (_("Enter the character set to use"));
	gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
	gtk_misc_set_alignment (GTK_MISC (label), 0.0, 0.5);
	gtk_box_pack_start (GTK_BOX (vbox), label, FALSE, FALSE, 0);
	gtk_widget_show (label);

	hbox = gtk_hbox_new (FALSE, 12);
	gtk_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 0);
	gtk_widget_show (hbox);

	label = gtk_label_new ("");
	gtk_box_pack_start (GTK_BOX (hbox), label, FALSE, FALSE, 0);
	gtk_widget_show (label);

	entry = gtk_entry_new ();
	gtk_box_pack_start (GTK_BOX (hbox), entry, TRUE, TRUE, 0);
	gtk_widget_show (entry);

	if (old_charset)
		gtk_entry_set_text (GTK_ENTRY (entry), old_charset);
	g_signal_connect (entry, "activate",
			  G_CALLBACK (activate_entry), dialog);
	
	gtk_container_set_border_width (GTK_CONTAINER (dialog->vbox), 0);
	gtk_container_set_border_width (GTK_CONTAINER (dialog->action_area), 12);

	gtk_widget_show_all (GTK_WIDGET (dialog));

	g_object_ref (dialog);
	if (gtk_dialog_run (dialog) == GTK_RESPONSE_OK) {
		new_charset = (char *)gtk_entry_get_text (GTK_ENTRY (entry));

	       	if (*new_charset) {
			if (add_other_charset (menu, item, new_charset)) {
				gtk_widget_destroy (GTK_WIDGET (dialog));
				g_object_unref (dialog);
				return;
			}
		}
	}
	gtk_widget_destroy (GTK_WIDGET (dialog));
	g_object_unref (dialog);

	/* Revert to previous selection */
	select_item (GTK_MENU_SHELL (menu), g_object_get_data(G_OBJECT(menu), "activated_item"));
}

/**
 * e_charset_picker_new:
 * @default_charset: the default character set, or %NULL to use the
 * locale character set.
 *
 * This creates an option menu widget and fills it in with a selection
 * of available character sets. The @default_charset (or locale character
 * set if @default_charset is %NULL) will be listed first, and selected
 * by default (except that iso-8859-1 will always be used instead of
 * US-ASCII). Any other character sets of the same language class as
 * the default will be listed next, followed by the remaining character
 * sets, a separator, and an "Other..." menu item, which can be used to
 * select other charsets.
 *
 * Return value: an option menu widget, filled in and with signals
 * attached.
 */
GtkWidget *
e_charset_picker_new (const char *default_charset)
{
	GtkWidget *menu, *item;
	int def, i;
	const char *locale_charset;
	
	g_get_charset (&locale_charset);
	if (!g_ascii_strcasecmp (locale_charset, "US-ASCII"))
		locale_charset = "iso-8859-1";
	
	if (!default_charset)
		default_charset = locale_charset;
	for (def = 0; def < num_charsets; def++) {
		if (!g_ascii_strcasecmp (charsets[def].name, default_charset))
			break;
	}
	
	menu = gtk_menu_new ();
	for (i = 0; i < num_charsets; i++) {
		item = add_charset (menu, &charsets[i], FALSE);
		if (i == def) {
			activate (item, menu);
			select_item (GTK_MENU_SHELL (menu), item);
		}
	}
	
	/* do the Unknown/Other section */
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), gtk_menu_item_new ());
	
	if (def == num_charsets) {
		ECharset other = { NULL, E_CHARSET_UNKNOWN, NULL };
		
		/* Add an entry for @default_charset */
		other.name = g_strdup (default_charset);
		item = add_charset (menu, &other, TRUE);
		activate (item, menu);
		select_item (GTK_MENU_SHELL (menu), item);
		g_object_set_data_full ((GObject *) menu, "other_charset",
					g_strdup (default_charset), g_free);
		def++;
	}
	
	item = gtk_menu_item_new_with_label (_("Other..."));
	g_signal_connect (item, "activate", G_CALLBACK (activate_other), menu);
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
	
	gtk_widget_show_all (menu);
	
	return menu;
}

/**
 * e_charset_picker_get_charset:
 * @menu: a character set menu from e_charset_picker_new()
 *
 * Return value: the currently-selected character set in @picker,
 * which must be freed with g_free().
 **/
char *
e_charset_picker_get_charset (GtkWidget *menu)
{
	GtkWidget *item;
	char *charset;

	g_return_val_if_fail (GTK_IS_MENU (menu), NULL);
	
	item = gtk_menu_get_active (GTK_MENU (menu));
	charset = g_object_get_data ((GObject *) item, "charset");
	
	return g_strdup (charset);
}

/**
 * e_charset_picker_dialog:
 * @title: title for the dialog box
 * @prompt: prompt string for the dialog box
 * @default_charset: as for e_charset_picker_new()
 * @parent: a parent window for the dialog box, or %NULL
 *
 * This creates a new dialog box with the given @title and @prompt and
 * a character set picker menu. It then runs the dialog and returns
 * the selected character set, or %NULL if the user clicked "Cancel".
 *
 * Return value: the selected character set (which must be freed with
 * g_free()), or %NULL.
 **/
char *
e_charset_picker_dialog (const char *title, const char *prompt,
			 const char *default_charset, GtkWindow *parent)
{
	GtkDialog *dialog;
	GtkWidget *label, *omenu, *picker, *vbox, *hbox;
	char *charset = NULL;

	dialog = GTK_DIALOG (gtk_dialog_new_with_buttons (title,
							  parent,
							  GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
							  GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
							  GTK_STOCK_OK, GTK_RESPONSE_OK,
							  NULL));

	gtk_dialog_set_has_separator (dialog, FALSE);
	gtk_dialog_set_default_response (dialog, GTK_RESPONSE_OK);

	vbox = gtk_vbox_new (FALSE, 6);
	gtk_container_set_border_width (GTK_CONTAINER (vbox), 12);
	gtk_box_pack_start (GTK_BOX (dialog->vbox), vbox, FALSE, FALSE, 0);
	gtk_widget_show (vbox);

	label = gtk_label_new (prompt);
	gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
	gtk_misc_set_alignment (GTK_MISC (label), 0.0, 0.5);
	gtk_box_pack_start (GTK_BOX (vbox), label, FALSE, FALSE, 0);
	gtk_widget_show (label);

	hbox = gtk_hbox_new (FALSE, 12);
	gtk_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 0);
	gtk_widget_show (hbox);

	label = gtk_label_new ("");
	gtk_box_pack_start (GTK_BOX (hbox), label, FALSE, FALSE, 0);
	gtk_widget_show (label);

	picker = e_charset_picker_new (default_charset);
	omenu = gtk_option_menu_new ();
	gtk_option_menu_set_menu (GTK_OPTION_MENU (omenu), picker);
	gtk_box_pack_start (GTK_BOX (hbox), omenu, TRUE, TRUE, 0);
	gtk_widget_show (omenu);

	gtk_container_set_border_width (GTK_CONTAINER (dialog->vbox), 0);
	gtk_container_set_border_width (GTK_CONTAINER (dialog->action_area), 12);

	gtk_widget_show_all (GTK_WIDGET (dialog));

	g_object_ref (dialog);

	if (gtk_dialog_run (dialog) == GTK_RESPONSE_OK)
		charset = e_charset_picker_get_charset (picker);

	gtk_widget_destroy (GTK_WIDGET (dialog));
	g_object_unref (dialog);

	return charset;
}