/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <stdlib.h>
#include <locale.h>
#include <libebook/libebook.h>

#include "test-cache-utils.h"

struct {
	gboolean empty_book;
	const gchar *path;
} params[] = {
	{ FALSE, "/EBookCacheCursor/DefaultSummary" },
	{ TRUE,  "/EBookCacheCursor/EmptySummary" }
};

gint
main (gint argc,
      gchar **argv)
{
	TCUStepData *data;
	gint ii;

#if !GLIB_CHECK_VERSION (2, 35, 1)
	g_type_init ();
#endif
	g_test_init (&argc, &argv, NULL);

	for (ii = 0; ii < G_N_ELEMENTS (params); ii++) {

		data = tcu_step_test_new (
			params[ii].path, "/fr_CA/Move/Forward", "fr_CA.UTF-8",
			params[ii].empty_book);
		tcu_step_test_add_assertion (data, 5, 11, 1, 2, 5, 6);
		tcu_step_test_add_assertion (data, 6, 4, 3, 7, 8, 15, 17);
		tcu_step_test_add (data, FALSE);

		data = tcu_step_test_new (
			params[ii].path, "/fr_CA/Move/ForwardOnNameless", "fr_CA.UTF-8",
			params[ii].empty_book);
		tcu_step_test_add_assertion (data, 1, 11);
		tcu_step_test_add_assertion (data, 3, 1, 2, 5);
		tcu_step_test_add (data, FALSE);

		data = tcu_step_test_new (
			params[ii].path, "/fr_CA/Move/Backwards", "fr_CA.UTF-8",
			params[ii].empty_book);
		tcu_step_test_add_assertion (data, -5, 20, 19, 9, 12, 13);
		tcu_step_test_add_assertion (data, -8, 14, 10, 18, 16, 17, 15, 8, 7);
		tcu_step_test_add (data, FALSE);

		data = tcu_step_test_new (
			params[ii].path, "/fr_CA/Filtered/Move/Forward", "fr_CA.UTF-8",
			params[ii].empty_book);
		tcu_step_test_add_assertion (data, 5, 11, 1, 2, 5, 3);
		tcu_step_test_add_assertion (data, 8, 8, 17, 16, 18, 10, 14, 12, 9);
		tcu_step_test_add (data, TRUE);

		data = tcu_step_test_new (
			params[ii].path, "/fr_CA/Filtered/Move/Backwards", "fr_CA.UTF-8",
			params[ii].empty_book);
		tcu_step_test_add_assertion (data, -5, 9, 12, 14, 10, 18);
		tcu_step_test_add_assertion (data, -8, 16, 17, 8, 3, 5, 2, 1, 11);
		tcu_step_test_add (data, TRUE);
	}

	return g_test_run ();
}