/*
 * e-collection-backend.c
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with the program; if not, see <http://www.gnu.org/licenses/>
 *
 */

/**
 * SECTION: e-collection-backend
 * @include: libebackend/libebackend.h
 * @short_description: A base class for a data source collection backend
 *
 * #ECollectionBackend is a base class for backends which manage a
 * collection of data sources that collectively represent the resources
 * on a remote server.  The resources can include any number of private
 * and shared email stores, calendars and address books.
 *
 * The backend's job is to synchronize local representations of remote
 * resources by adding and removing #EServerSideSource instances in an
 * #ESourceRegistryServer.  If possible the backend should also listen
 * for notifications of newly-added or deleted resources on the remote
 * server or else poll the remote server at regular intervals and then
 * update the data source collection accordingly.
 *
 * As most remote servers require authentication, the backend may also
 * wish to implement the #ESourceAuthenticator interface so it can submit
 * its own #EAuthenticationSession instances to the #ESourceRegistryServer.
 **/

#include "e-collection-backend.h"

#include <config.h>
#include <glib/gi18n-lib.h>

#include <libedataserver/libedataserver.h>

#include <libebackend/e-server-side-source.h>
#include <libebackend/e-source-registry-server.h>

#define E_COLLECTION_BACKEND_GET_PRIVATE(obj) \
	(G_TYPE_INSTANCE_GET_PRIVATE \
	((obj), E_TYPE_COLLECTION_BACKEND, ECollectionBackendPrivate))

struct _ECollectionBackendPrivate {
	GWeakRef server;

	/* Set of ESources */
	GHashTable *children;
	GMutex children_lock;

	gchar *cache_dir;

	/* Resource ID -> ESource */
	GHashTable *unclaimed_resources;
	GMutex unclaimed_resources_lock;

	gulong source_added_handler_id;
	gulong source_removed_handler_id;
};

enum {
	PROP_0,
	PROP_SERVER
};

enum {
	CHILD_ADDED,
	CHILD_REMOVED,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

G_DEFINE_TYPE (
	ECollectionBackend,
	e_collection_backend,
	E_TYPE_BACKEND)

static void
collection_backend_children_insert (ECollectionBackend *backend,
                                    ESource *source)
{
	g_mutex_lock (&backend->priv->children_lock);

	g_hash_table_add (backend->priv->children, g_object_ref (source));

	g_mutex_unlock (&backend->priv->children_lock);
}

static gboolean
collection_backend_children_remove (ECollectionBackend *backend,
                                    ESource *source)
{
	gboolean removed;

	g_mutex_lock (&backend->priv->children_lock);

	removed = g_hash_table_remove (backend->priv->children, source);

	g_mutex_unlock (&backend->priv->children_lock);

	return removed;
}

static GList *
collection_backend_children_list (ECollectionBackend *backend)
{
	GList *list, *link;

	g_mutex_lock (&backend->priv->children_lock);

	list = g_hash_table_get_keys (backend->priv->children);

	for (link = list; link != NULL; link = g_list_next (link))
		g_object_ref (link->data);

	g_mutex_unlock (&backend->priv->children_lock);

	return list;
}

static GFile *
collection_backend_new_user_file (ECollectionBackend *backend)
{
	GFile *file;
	gchar *safe_uid;
	gchar *basename;
	gchar *filename;
	const gchar *cache_dir;

	/* This is like e_server_side_source_new_user_file()
	 * except that it uses the backend's cache directory. */

	safe_uid = e_uid_new ();
	e_filename_make_safe (safe_uid);

	cache_dir = e_collection_backend_get_cache_dir (backend);
	basename = g_strconcat (safe_uid, ".source", NULL);
	filename = g_build_filename (cache_dir, basename, NULL);

	file = g_file_new_for_path (filename);

	g_free (basename);
	g_free (filename);
	g_free (safe_uid);

	return file;
}

static ESource *
collection_backend_new_source (ECollectionBackend *backend,
                               GFile *file,
                               GError **error)
{
	ESourceRegistryServer *server;
	ESource *child_source;
	ESource *collection_source;
	EServerSideSource *server_side_source;
	const gchar *cache_dir;
	const gchar *collection_uid;

	server = e_collection_backend_ref_server (backend);
	child_source = e_server_side_source_new (server, file, error);
	g_object_unref (server);

	if (child_source == NULL)
		return NULL;

	server_side_source = E_SERVER_SIDE_SOURCE (child_source);

	/* Clients may change the source but may not remove it. */
	e_server_side_source_set_writable (server_side_source, TRUE);
	e_server_side_source_set_removable (server_side_source, FALSE);

	/* Changes should be written back to the cache directory. */
	cache_dir = e_collection_backend_get_cache_dir (backend);
	e_server_side_source_set_write_directory (
		server_side_source, cache_dir);

	/* Configure the child source as a collection member. */
	collection_source = e_backend_get_source (E_BACKEND (backend));
	collection_uid = e_source_get_uid (collection_source);
	e_source_set_parent (child_source, collection_uid);

	return child_source;
}

static void
collection_backend_load_resources (ECollectionBackend *backend)
{
	ESourceRegistryServer *server;
	ECollectionBackendClass *class;
	GDir *dir;
	GFile *file;
	const gchar *name;
	const gchar *cache_dir;
	GError *error = NULL;

	/* This is based on e_source_registry_server_load_file()
	 * and e_source_registry_server_load_directory(). */

	class = E_COLLECTION_BACKEND_GET_CLASS (backend);
	g_return_if_fail (class->dup_resource_id != NULL);

	cache_dir = e_collection_backend_get_cache_dir (backend);

	dir = g_dir_open (cache_dir, 0, &error);
	if (error != NULL) {
		g_warn_if_fail (dir == NULL);
		g_warning ("%s: %s", G_STRFUNC, error->message);
		g_error_free (error);
		return;
	}

	g_return_if_fail (dir != NULL);

	file = g_file_new_for_path (cache_dir);
	server = e_collection_backend_ref_server (backend);

	g_mutex_lock (&backend->priv->unclaimed_resources_lock);

	while ((name = g_dir_read_name (dir)) != NULL) {
		GFile *child;
		ESource *source;
		gchar *resource_id;

		/* Ignore files with no ".source" suffix. */
		if (!g_str_has_suffix (name, ".source"))
			continue;

		child = g_file_get_child (file, name);
		source = collection_backend_new_source (backend, child, &error);
		g_object_unref (child);

		if (error != NULL) {
			g_warn_if_fail (source == NULL);
			g_warning ("%s: %s", G_STRFUNC, error->message);
			g_clear_error (&error);
			continue;
		}

		g_return_if_fail (E_IS_SERVER_SIDE_SOURCE (source));

		resource_id = class->dup_resource_id (backend, source);

		/* Hash table takes ownership of the resource ID. */
		if (resource_id != NULL)
			g_hash_table_insert (
				backend->priv->unclaimed_resources,
				resource_id, g_object_ref (source));

		g_object_unref (source);
	}

	g_mutex_unlock (&backend->priv->unclaimed_resources_lock);

	g_object_unref (file);
	g_object_unref (server);
	g_dir_close (dir);
}

static ESource *
collection_backend_claim_resource (ECollectionBackend *backend,
                                   const gchar *resource_id,
                                   GError **error)
{
	GHashTable *unclaimed_resources;
	ESource *source;

	g_mutex_lock (&backend->priv->unclaimed_resources_lock);

	unclaimed_resources = backend->priv->unclaimed_resources;
	source = g_hash_table_lookup (unclaimed_resources, resource_id);

	if (source != NULL) {
		g_object_ref (source);
		g_hash_table_remove (unclaimed_resources, resource_id);
	} else {
		GFile *file = collection_backend_new_user_file (backend);
		source = collection_backend_new_source (backend, file, error);
		g_object_unref (file);
	}

	g_mutex_unlock (&backend->priv->unclaimed_resources_lock);

	return source;
}

static gboolean
collection_backend_child_is_calendar (ESource *child_source)
{
	const gchar *extension_name;

	extension_name = E_SOURCE_EXTENSION_CALENDAR;
	if (e_source_has_extension (child_source, extension_name))
		return TRUE;

	extension_name = E_SOURCE_EXTENSION_MEMO_LIST;
	if (e_source_has_extension (child_source, extension_name))
		return TRUE;

	extension_name = E_SOURCE_EXTENSION_TASK_LIST;
	if (e_source_has_extension (child_source, extension_name))
		return TRUE;

	return FALSE;
}

static gboolean
collection_backend_child_is_contacts (ESource *child_source)
{
	const gchar *extension_name;

	extension_name = E_SOURCE_EXTENSION_ADDRESS_BOOK;
	if (e_source_has_extension (child_source, extension_name))
		return TRUE;

	return FALSE;
}

static gboolean
collection_backend_child_is_mail (ESource *child_source)
{
	const gchar *extension_name;

	extension_name = E_SOURCE_EXTENSION_MAIL_ACCOUNT;
	if (e_source_has_extension (child_source, extension_name))
		return TRUE;

	extension_name = E_SOURCE_EXTENSION_MAIL_IDENTITY;
	if (e_source_has_extension (child_source, extension_name))
		return TRUE;

	extension_name = E_SOURCE_EXTENSION_MAIL_TRANSPORT;
	if (e_source_has_extension (child_source, extension_name))
		return TRUE;

	return FALSE;
}

static gboolean
include_master_source_enabled_transform (GBinding *binding,
                                         const GValue *source_value,
                                         GValue *target_value,
                                         gpointer backend)
{
	g_value_set_boolean (
		target_value,
		g_value_get_boolean (source_value) &&
		e_source_get_enabled (e_backend_get_source (backend)));

	return TRUE;
}

static void
collection_backend_bind_child_enabled (ECollectionBackend *backend,
                                       ESource *child_source)
{
	ESource *collection_source;
	ESourceCollection *extension;
	const gchar *extension_name;

	/* See if the child source's "enabled" property can be
	 * bound to any ESourceCollection "enabled" properties. */

	extension_name = E_SOURCE_EXTENSION_COLLECTION;
	collection_source = e_backend_get_source (E_BACKEND (backend));
	extension = e_source_get_extension (collection_source, extension_name);

	if (collection_backend_child_is_calendar (child_source)) {
		g_object_bind_property_full (
			extension, "calendar-enabled",
			child_source, "enabled",
			G_BINDING_SYNC_CREATE,
			include_master_source_enabled_transform,
			include_master_source_enabled_transform,
			backend,
			NULL);
		return;
	}

	if (collection_backend_child_is_contacts (child_source)) {
		g_object_bind_property_full (
			extension, "contacts-enabled",
			child_source, "enabled",
			G_BINDING_SYNC_CREATE,
			include_master_source_enabled_transform,
			include_master_source_enabled_transform,
			backend,
			NULL);
		return;
	}

	if (collection_backend_child_is_mail (child_source)) {
		g_object_bind_property_full (
			extension, "mail-enabled",
			child_source, "enabled",
			G_BINDING_SYNC_CREATE,
			include_master_source_enabled_transform,
			include_master_source_enabled_transform,
			backend,
			NULL);
		return;
	}

	g_object_bind_property (
		collection_source, "enabled",
		child_source, "enabled",
		G_BINDING_SYNC_CREATE);
}

static void
collection_backend_source_added_cb (ESourceRegistryServer *server,
                                    ESource *source,
                                    ECollectionBackend *backend)
{
	ESource *collection_source;
	ESource *parent_source;
	const gchar *uid;

	/* If the newly-added source is our own child, emit "child-added". */

	collection_source = e_backend_get_source (E_BACKEND (backend));

	uid = e_source_get_parent (source);
	if (uid == NULL)
		return;

	parent_source = e_source_registry_server_ref_source (server, uid);
	g_return_if_fail (parent_source != NULL);

	if (e_source_equal (collection_source, parent_source))
		g_signal_emit (backend, signals[CHILD_ADDED], 0, source);

	g_object_unref (parent_source);
}

static void
collection_backend_source_removed_cb (ESourceRegistryServer *server,
                                      ESource *source,
                                      ECollectionBackend *backend)
{
	ESource *collection_source;
	ESource *parent_source;
	const gchar *uid;

	/* If the removed source was our own child, emit "child-removed".
	 * Note that the source is already unlinked from the GNode tree. */

	collection_source = e_backend_get_source (E_BACKEND (backend));

	uid = e_source_get_parent (source);
	if (uid == NULL)
		return;

	parent_source = e_source_registry_server_ref_source (server, uid);
	g_return_if_fail (parent_source != NULL);

	if (e_source_equal (collection_source, parent_source))
		g_signal_emit (backend, signals[CHILD_REMOVED], 0, source);

	g_object_unref (parent_source);
}

static gboolean
collection_backend_populate_idle_cb (gpointer user_data)
{
	ECollectionBackend *backend;
	ECollectionBackendClass *class;

	backend = E_COLLECTION_BACKEND (user_data);

	class = E_COLLECTION_BACKEND_GET_CLASS (backend);
	g_return_val_if_fail (class->populate != NULL, FALSE);

	class->populate (backend);

	return FALSE;
}

static void
collection_backend_set_server (ECollectionBackend *backend,
                               ESourceRegistryServer *server)
{
	g_return_if_fail (E_IS_SOURCE_REGISTRY_SERVER (server));

	g_weak_ref_set (&backend->priv->server, server);
}

static void
collection_backend_set_property (GObject *object,
                                 guint property_id,
                                 const GValue *value,
                                 GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_SERVER:
			collection_backend_set_server (
				E_COLLECTION_BACKEND (object),
				g_value_get_object (value));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
collection_backend_get_property (GObject *object,
                                 guint property_id,
                                 GValue *value,
                                 GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_SERVER:
			g_value_take_object (
				value,
				e_collection_backend_ref_server (
				E_COLLECTION_BACKEND (object)));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
collection_backend_dispose (GObject *object)
{
	ECollectionBackendPrivate *priv;
	ESourceRegistryServer *server;

	priv = E_COLLECTION_BACKEND_GET_PRIVATE (object);

	server = g_weak_ref_get (&priv->server);
	if (server != NULL) {
		g_signal_handler_disconnect (
			server, priv->source_added_handler_id);
		g_signal_handler_disconnect (
			server, priv->source_removed_handler_id);
		g_weak_ref_set (&priv->server, NULL);
		g_object_unref (server);
	}

	g_mutex_lock (&priv->children_lock);
	g_hash_table_remove_all (priv->children);
	g_mutex_unlock (&priv->children_lock);

	g_mutex_lock (&priv->unclaimed_resources_lock);
	g_hash_table_remove_all (priv->unclaimed_resources);
	g_mutex_unlock (&priv->unclaimed_resources_lock);

	/* Chain up to parent's dispose() method. */
	G_OBJECT_CLASS (e_collection_backend_parent_class)->dispose (object);
}

static void
collection_backend_finalize (GObject *object)
{
	ECollectionBackendPrivate *priv;

	priv = E_COLLECTION_BACKEND_GET_PRIVATE (object);

	g_hash_table_destroy (priv->children);
	g_mutex_clear (&priv->children_lock);

	g_hash_table_destroy (priv->unclaimed_resources);
	g_mutex_clear (&priv->unclaimed_resources_lock);

	/* Chain up to parent's finalize() method. */
	G_OBJECT_CLASS (e_collection_backend_parent_class)->finalize (object);
}

static void
collection_backend_constructed (GObject *object)
{
	ECollectionBackend *backend;
	ESourceRegistryServer *server;
	ESource *source;
	GNode *node;
	const gchar *collection_uid;
	const gchar *user_cache_dir;
	gulong handler_id;

	backend = E_COLLECTION_BACKEND (object);

	/* Chain up to parent's constructed() method. */
	G_OBJECT_CLASS (e_collection_backend_parent_class)->
		constructed (object);

	source = e_backend_get_source (E_BACKEND (backend));

	/* Determine the backend's cache directory. */

	user_cache_dir = e_get_user_cache_dir ();
	collection_uid = e_source_get_uid (source);
	backend->priv->cache_dir = g_build_filename (
		user_cache_dir, "sources", collection_uid, NULL);
	g_mkdir_with_parents (backend->priv->cache_dir, 0700);

	/* This requires the cache directory to be set. */
	collection_backend_load_resources (backend);

	/* Emit "child-added" signals for the children we already have. */

	node = e_server_side_source_get_node (E_SERVER_SIDE_SOURCE (source));
	node = g_node_first_child (node);

	while (node != NULL) {
		ESource *child = E_SOURCE (node->data);
		g_signal_emit (backend, signals[CHILD_ADDED], 0, child);
		node = g_node_next_sibling (node);
	}

	/* Listen for "source-added" and "source-removed" signals
	 * from the server, which may trigger our own "child-added"
	 * and "child-removed" signals. */

	server = e_collection_backend_ref_server (backend);

	handler_id = g_signal_connect (
		server, "source-added",
		G_CALLBACK (collection_backend_source_added_cb), backend);

	backend->priv->source_added_handler_id = handler_id;

	handler_id = g_signal_connect (
		server, "source-removed",
		G_CALLBACK (collection_backend_source_removed_cb), backend);

	backend->priv->source_removed_handler_id = handler_id;

	g_object_unref (server);

	/* Populate the newly-added collection from an idle callback
	 * so persistent child sources have a chance to be added first. */

	g_idle_add_full (
		G_PRIORITY_LOW,
		collection_backend_populate_idle_cb,
		g_object_ref (backend),
		(GDestroyNotify) g_object_unref);
}

static gboolean
collection_backend_authenticate_sync (EBackend *backend,
                                      ESourceAuthenticator *authenticator,
                                      GCancellable *cancellable,
                                      GError **error)
{
	ECollectionBackend *collection_backend;
	ESourceRegistryServer *server;
	EAuthenticationSession *session;
	ESource *source;
	const gchar *source_uid;
	gboolean success;

	source = e_backend_get_source (backend);
	source_uid = e_source_get_uid (source);

	collection_backend = E_COLLECTION_BACKEND (backend);
	server = e_collection_backend_ref_server (collection_backend);
	session = e_source_registry_server_new_auth_session (
		server, authenticator, source_uid);

	success = e_source_registry_server_authenticate_sync (
		server, session, cancellable, error);

	g_object_unref (session);
	g_object_unref (server);

	return success;
}

static void
collection_backend_populate (ECollectionBackend *backend)
{
	/* Placeholder so subclasses can safely chain up. */
}

static gchar *
collection_backend_dup_resource_id (ECollectionBackend *backend,
                                    ESource *source)
{
	const gchar *extension_name;
	gchar *resource_id = NULL;

	extension_name = E_SOURCE_EXTENSION_RESOURCE;

	if (e_source_has_extension (source, extension_name)) {
		ESourceResource *extension;

		extension = e_source_get_extension (source, extension_name);
		resource_id = e_source_resource_dup_identity (extension);
	}

	return resource_id;
}

static void
collection_backend_child_added (ECollectionBackend *backend,
                                ESource *child_source)
{
	ESource *collection_source;
	const gchar *extension_name;
	gboolean is_mail = FALSE;

	collection_backend_children_insert (backend, child_source);
	collection_backend_bind_child_enabled (backend, child_source);

	collection_source = e_backend_get_source (E_BACKEND (backend));

	extension_name = E_SOURCE_EXTENSION_MAIL_ACCOUNT;
	is_mail |= e_source_has_extension (child_source, extension_name);

	extension_name = E_SOURCE_EXTENSION_MAIL_IDENTITY;
	is_mail |= e_source_has_extension (child_source, extension_name);

	extension_name = E_SOURCE_EXTENSION_MAIL_TRANSPORT;
	is_mail |= e_source_has_extension (child_source, extension_name);

	/* Synchronize mail-related display names with the collection. */
	if (is_mail)
		g_object_bind_property (
			collection_source, "display-name",
			child_source, "display-name",
			G_BINDING_SYNC_CREATE);

	/* Collection children are not removable. */
	e_server_side_source_set_removable (
		E_SERVER_SIDE_SOURCE (child_source), FALSE);

	/* Collection children inherit the authentication session type. */
	g_object_bind_property (
		collection_source, "auth-session-type",
		child_source, "auth-session-type",
		G_BINDING_SYNC_CREATE);

	/* Collection children inherit OAuth 2.0 support if available. */
	g_object_bind_property (
		collection_source, "oauth2-support",
		child_source, "oauth2-support",
		G_BINDING_SYNC_CREATE);
}

static void
collection_backend_child_removed (ECollectionBackend *backend,
                                  ESource *child_source)
{
	collection_backend_children_remove (backend, child_source);
}

static gboolean
collection_backend_create_resource_sync (ECollectionBackend *backend,
                                         ESource *source,
                                         GCancellable *cancellable,
                                         GError **error)
{
	EAsyncClosure *closure;
	GAsyncResult *result;
	gboolean success;

	closure = e_async_closure_new ();

	e_collection_backend_create_resource (
		backend, source, cancellable,
		e_async_closure_callback, closure);

	result = e_async_closure_wait (closure);

	success = e_collection_backend_create_resource_finish (
		backend, result, error);

	e_async_closure_free (closure);

	return success;
}

static void
collection_backend_create_resource (ECollectionBackend *backend,
                                    ESource *source,
                                    GCancellable *cancellable,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data)
{
	GSimpleAsyncResult *simple;

	simple = g_simple_async_result_new_error (
		G_OBJECT (backend), callback, user_data,
		G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		_("%s does not support creating remote resources"),
		G_OBJECT_TYPE_NAME (backend));

	g_simple_async_result_complete_in_idle (simple);

	g_object_unref (simple);
}

static gboolean
collection_backend_create_resource_finish (ECollectionBackend *backend,
                                           GAsyncResult *result,
                                           GError **error)
{
	GSimpleAsyncResult *simple;

	simple = G_SIMPLE_ASYNC_RESULT (result);

	/* Assume success unless a GError is set. */
	return !g_simple_async_result_propagate_error (simple, error);
}

static gboolean
collection_backend_delete_resource_sync (ECollectionBackend *backend,
                                         ESource *source,
                                         GCancellable *cancellable,
                                         GError **error)
{
	EAsyncClosure *closure;
	GAsyncResult *result;
	gboolean success;

	closure = e_async_closure_new ();

	e_collection_backend_delete_resource (
		backend, source, cancellable,
		e_async_closure_callback, closure);

	result = e_async_closure_wait (closure);

	success = e_collection_backend_delete_resource_finish (
		backend, result, error);

	e_async_closure_free (closure);

	return success;
}

static void
collection_backend_delete_resource (ECollectionBackend *backend,
                                    ESource *source,
                                    GCancellable *cancellable,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data)
{
	GSimpleAsyncResult *simple;

	simple = g_simple_async_result_new_error (
		G_OBJECT (backend), callback, user_data,
		G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		_("%s does not support deleting remote resources"),
		G_OBJECT_TYPE_NAME (backend));

	g_simple_async_result_complete_in_idle (simple);

	g_object_unref (simple);
}

static gboolean
collection_backend_delete_resource_finish (ECollectionBackend *backend,
                                           GAsyncResult *result,
                                           GError **error)
{
	GSimpleAsyncResult *simple;

	simple = G_SIMPLE_ASYNC_RESULT (result);

	/* Assume success unless a GError is set. */
	return !g_simple_async_result_propagate_error (simple, error);
}

static void
e_collection_backend_class_init (ECollectionBackendClass *class)
{
	GObjectClass *object_class;
	EBackendClass *backend_class;

	g_type_class_add_private (class, sizeof (ECollectionBackendPrivate));

	object_class = G_OBJECT_CLASS (class);
	object_class->set_property = collection_backend_set_property;
	object_class->get_property = collection_backend_get_property;
	object_class->dispose = collection_backend_dispose;
	object_class->finalize = collection_backend_finalize;
	object_class->constructed = collection_backend_constructed;

	backend_class = E_BACKEND_CLASS (class);
	backend_class->authenticate_sync = collection_backend_authenticate_sync;

	class->populate = collection_backend_populate;
	class->dup_resource_id = collection_backend_dup_resource_id;
	class->child_added = collection_backend_child_added;
	class->child_removed = collection_backend_child_removed;
	class->create_resource_sync = collection_backend_create_resource_sync;
	class->create_resource = collection_backend_create_resource;
	class->create_resource_finish = collection_backend_create_resource_finish;
	class->delete_resource_sync = collection_backend_delete_resource_sync;
	class->delete_resource = collection_backend_delete_resource;
	class->delete_resource_finish = collection_backend_delete_resource_finish;

	g_object_class_install_property (
		object_class,
		PROP_SERVER,
		g_param_spec_object (
			"server",
			"Server",
			"The server to which the backend belongs",
			E_TYPE_SOURCE_REGISTRY_SERVER,
			G_PARAM_READWRITE |
			G_PARAM_CONSTRUCT_ONLY |
			G_PARAM_STATIC_STRINGS));

	/**
	 * ECollectionBackend::child-added:
	 * @backend: the #ECollectionBackend which emitted the signal
	 * @child_source: the newly-added child #EServerSideSource
	 *
	 * Emitted when an #EServerSideSource is added to @backend's
	 * #ECollectionBackend:server as a child of @backend's collection
	 * #EBackend:source.
	 *
	 * You can think of this as a filtered version of
	 * #ESourceRegistryServer's #ESourceRegistryServer::source-added
	 * signal which only lets through sources relevant to @backend.
	 **/
	signals[CHILD_ADDED] = g_signal_new (
		"child-added",
		G_OBJECT_CLASS_TYPE (object_class),
		G_SIGNAL_RUN_LAST,
		G_STRUCT_OFFSET (ECollectionBackendClass, child_added),
		NULL, NULL, NULL,
		G_TYPE_NONE, 1,
		E_TYPE_SERVER_SIDE_SOURCE);

	/**
	 * ECollectionBackend::child-removed:
	 * @backend: the #ECollectionBackend which emitted the signal
	 * @child_source: the child #EServerSideSource that got removed
	 *
	 * Emitted when an #EServerSideSource that is a child of
	 * @backend's collection #EBackend:source is removed from
	 * @backend's #ECollectionBackend:server.
	 *
	 * You can think of this as a filtered version of
	 * #ESourceRegistryServer's #ESourceRegistryServer::source-removed
	 * signal which only lets through sources relevant to @backend.
	 **/
	signals[CHILD_REMOVED] = g_signal_new (
		"child-removed",
		G_OBJECT_CLASS_TYPE (object_class),
		G_SIGNAL_RUN_LAST,
		G_STRUCT_OFFSET (ECollectionBackendClass, child_removed),
		NULL, NULL, NULL,
		G_TYPE_NONE, 1,
		E_TYPE_SERVER_SIDE_SOURCE);
}

static void
e_collection_backend_init (ECollectionBackend *backend)
{
	GHashTable *children;
	GHashTable *unclaimed_resources;

	children = g_hash_table_new_full (
		(GHashFunc) e_source_hash,
		(GEqualFunc) e_source_equal,
		(GDestroyNotify) g_object_unref,
		(GDestroyNotify) NULL);

	unclaimed_resources = g_hash_table_new_full (
		(GHashFunc) g_str_hash,
		(GEqualFunc) g_str_equal,
		(GDestroyNotify) g_free,
		(GDestroyNotify) g_object_unref);

	backend->priv = E_COLLECTION_BACKEND_GET_PRIVATE (backend);
	backend->priv->children = children;
	g_mutex_init (&backend->priv->children_lock);
	backend->priv->unclaimed_resources = unclaimed_resources;
	g_mutex_init (&backend->priv->unclaimed_resources_lock);
}

/**
 * e_collection_backend_new_child:
 * @backend: an #ECollectionBackend
 * @resource_id: a stable and unique resource ID
 *
 * Creates a new #EServerSideSource as a child of the collection
 * #EBackend:source owned by @backend.  If possible, the #EServerSideSource
 * is drawn from a cache of previously used sources indexed by @resource_id
 * so that locally cached data from previous sessions can be reused.
 *
 * The returned data source should be passed to
 * e_source_registry_server_add_source() to export it over D-Bus.
 *
 * Return: a newly-created data source
 *
 * Since: 3.6
 **/
ESource *
e_collection_backend_new_child (ECollectionBackend *backend,
                                const gchar *resource_id)
{
	ESource *collection_source;
	ESource *child_source;
	GError *error = NULL;

	g_return_val_if_fail (E_IS_COLLECTION_BACKEND (backend), NULL);
	g_return_val_if_fail (resource_id != NULL, NULL);

	/* This being a newly-created or existing data source, claiming
	 * it should never fail but we'll check for errors just the same.
	 * It's unlikely enough that we don't need a GError parameter. */
	child_source = collection_backend_claim_resource (
		backend, resource_id, &error);

	if (error != NULL) {
		g_warn_if_fail (child_source == NULL);
		g_warning ("%s: %s", G_STRFUNC, error->message);
		g_error_free (error);
		return NULL;
	}

	collection_source = e_backend_get_source (E_BACKEND (backend));

	g_print (
		"%s: Pairing %s with resource %s\n",
		e_source_get_display_name (collection_source),
		e_source_get_uid (child_source), resource_id);

	return child_source;
}

/**
 * e_collection_backend_ref_server:
 * @backend: an #ECollectionBackend
 *
 * Returns the #ESourceRegistryServer to which @backend belongs.
 *
 * The returned #ESourceRegistryServer is referenced for thread-safety.
 * Unreference the #ESourceRegistryServer with g_object_unref() when
 * finished with it.
 *
 * Returns: the #ESourceRegisterServer for @backend
 *
 * Since: 3.6
 **/
ESourceRegistryServer *
e_collection_backend_ref_server (ECollectionBackend *backend)
{
	g_return_val_if_fail (E_IS_COLLECTION_BACKEND (backend), NULL);

	return g_weak_ref_get (&backend->priv->server);
}

/**
 * e_collection_backend_get_cache_dir:
 * @backend: an #ECollectionBackend
 *
 * Returns the private cache directory path for @backend, which is named
 * after the #ESource:uid of @backend's collection #EBackend:source.
 *
 * The cache directory is meant to store key files for backend-created
 * data sources.  See also: e_server_side_source_set_write_directory()
 *
 * Returns: the cache directory for @backend
 *
 * Since: 3.6
 **/
const gchar *
e_collection_backend_get_cache_dir (ECollectionBackend *backend)
{
	g_return_val_if_fail (E_IS_COLLECTION_BACKEND (backend), NULL);

	return backend->priv->cache_dir;
}

/**
 * e_collection_backend_dup_resource_id:
 * @backend: an #ECollectionBackend
 * @child_source: an #ESource managed by @backend
 *
 * Extracts the resource ID for @child_source, which is supposed to be a
 * stable and unique server-assigned identifier for the remote resource
 * described by @child_source.  If @child_source is not actually a child
 * of the collection #EBackend:source owned by @backend, the function
 * returns %NULL.
 *
 * The returned string should be freed with g_free() when no longer needed.
 *
 * Returns: a newly-allocated resource ID for @child_source, or %NULL
 *
 * Since: 3.6
 **/
gchar *
e_collection_backend_dup_resource_id (ECollectionBackend *backend,
                                      ESource *child_source)
{
	ECollectionBackend *backend_for_child_source;
	ECollectionBackendClass *class;
	ESourceRegistryServer *server;
	gboolean child_is_ours = FALSE;

	g_return_val_if_fail (E_IS_COLLECTION_BACKEND (backend), NULL);
	g_return_val_if_fail (E_IS_SOURCE (child_source), NULL);

	class = E_COLLECTION_BACKEND_GET_CLASS (backend);
	g_return_val_if_fail (class->dup_resource_id != NULL, NULL);

	/* Make sure the ESource belongs to the ECollectionBackend to
	 * avoid accidentally creating a new extension while trying to
	 * extract a resource ID that isn't there.  Better to test this
	 * up front than rely on ECollectionBackend subclasses to do it. */
	server = e_collection_backend_ref_server (backend);
	backend_for_child_source =
		e_source_registry_server_ref_backend (server, child_source);
	g_object_unref (server);

	if (backend_for_child_source != NULL) {
		child_is_ours = (backend_for_child_source == backend);
		g_object_unref (backend_for_child_source);
	}

	if (!child_is_ours)
		return NULL;

	return class->dup_resource_id (backend, child_source);
}

/**
 * e_collection_backend_claim_all_resources:
 * @backend: an #ECollectionBackend
 *
 * Claims all previously used sources that have not yet been claimed by
 * e_collection_backend_new_child() and returns them in a #GList.  Note
 * that previously used sources can only be claimed once, so subsequent
 * calls to this function for @backend will return %NULL.
 *
 * The @backend is then expected to compare the returned list with a
 * current list of resources from a remote server, create new #ESource
 * instances as needed with e_collection_backend_new_child(), discard
 * unneeded #ESource instances with e_source_remove(), and export the
 * remaining instances with e_source_registry_server_add_source().
 *
 * The sources returned in the list are referenced for thread-safety.
 * They must each be unreferenced with g_object_unref() when finished
 * with them.  Free the returned #GList itself with g_list_free().
 *
 * An easy way to free the list properly in one step is as follows:
 *
 * |[
 *   g_list_free_full (list, g_object_unref);
 * ]|
 *
 * Returns: a list of previously used sources
 *
 * Since: 3.6
 **/
GList *
e_collection_backend_claim_all_resources (ECollectionBackend *backend)
{
	GHashTable *unclaimed_resources;
	GList *resources;

	g_return_val_if_fail (E_IS_COLLECTION_BACKEND (backend), NULL);

	g_mutex_lock (&backend->priv->unclaimed_resources_lock);

	unclaimed_resources = backend->priv->unclaimed_resources;
	resources = g_hash_table_get_values (unclaimed_resources);
	g_list_foreach (resources, (GFunc) g_object_ref, NULL);
	g_hash_table_remove_all (unclaimed_resources);

	g_mutex_unlock (&backend->priv->unclaimed_resources_lock);

	return resources;
}

/**
 * e_collection_backend_list_calendar_sources:
 * @backend: an #ECollectionBackend
 *
 * Returns a list of calendar sources belonging to the data source
 * collection managed by @backend.
 *
 * The sources returned in the list are referenced for thread-safety.
 * They must each be unreferenced with g_object_unref() when finished
 * with them.  Free the returned #GList itself with g_list_free().
 *
 * An easy way to free the list properly in one step is as follows:
 *
 * |[
 *   g_list_free_full (list, g_object_unref);
 * ]|
 *
 * Returns: a list of calendar sources
 *
 * Since: 3.6
 **/
GList *
e_collection_backend_list_calendar_sources (ECollectionBackend *backend)
{
	GList *result_list = NULL;
	GList *list, *link;

	g_return_val_if_fail (E_IS_COLLECTION_BACKEND (backend), NULL);

	list = collection_backend_children_list (backend);

	for (link = list; link != NULL; link = g_list_next (link)) {
		ESource *child_source = E_SOURCE (link->data);
		if (collection_backend_child_is_calendar (child_source))
			result_list = g_list_prepend (
				result_list, g_object_ref (child_source));
	}

	g_list_free_full (list, (GDestroyNotify) g_object_unref);

	return g_list_reverse (result_list);
}

/**
 * e_collection_backend_list_contacts_sources:
 * @backend: an #ECollectionBackend
 *
 * Returns a list of address book sources belonging to the data source
 * collection managed by @backend.
 *
 * The sources returned in the list are referenced for thread-safety.
 * They must each be unreferenced with g_object_unref() when finished
 * with them.  Free the returned #GList itself with g_list_free().
 *
 * An easy way to free the list properly in one step is as follows:
 *
 * |[
 *   g_list_free_full (list, g_object_unref);
 * ]|
 *
 * Returns: a list of address book sources
 *
 * Since: 3.6
 **/
GList *
e_collection_backend_list_contacts_sources (ECollectionBackend *backend)
{
	GList *result_list = NULL;
	GList *list, *link;

	g_return_val_if_fail (E_IS_COLLECTION_BACKEND (backend), NULL);

	list = collection_backend_children_list (backend);

	for (link = list; link != NULL; link = g_list_next (link)) {
		ESource *child_source = E_SOURCE (link->data);
		if (collection_backend_child_is_contacts (child_source))
			result_list = g_list_prepend (
				result_list, g_object_ref (child_source));
	}

	g_list_free_full (list, (GDestroyNotify) g_object_unref);

	return g_list_reverse (result_list);
}

/**
 * e_collection_backend_list_mail_sources:
 * @backend: an #ECollectionBackend
 *
 * Returns a list of mail sources belonging to the data source collection
 * managed by @backend.
 *
 * The sources returned in the list are referenced for thread-safety.
 * They must each be unreferenced with g_object_unref() when finished
 * with them.  Free the returned #GList itself with g_list_free().
 *
 * An easy way to free the list properly in one step is as follows:
 *
 * |[
 *   g_list_free_full (list, g_object_unref);
 * ]|
 *
 * Returns: a list of mail sources
 *
 * Since: 3.6
 **/
GList *
e_collection_backend_list_mail_sources (ECollectionBackend *backend)
{
	GList *result_list = NULL;
	GList *list, *link;

	g_return_val_if_fail (E_IS_COLLECTION_BACKEND (backend), NULL);

	list = collection_backend_children_list (backend);

	for (link = list; link != NULL; link = g_list_next (link)) {
		ESource *child_source = E_SOURCE (link->data);
		if (collection_backend_child_is_mail (child_source))
			result_list = g_list_prepend (
				result_list, g_object_ref (child_source));
	}

	g_list_free_full (list, (GDestroyNotify) g_object_unref);

	return g_list_reverse (result_list);
}

/**
 * e_collection_backend_create_resource_sync
 * @backend: an #ECollectionBackend
 * @source: an #ESource
 * @cancellable: optional #GCancellable object, or %NULL
 * @error: return location for a #GError, or %NULL
 *
 * Creates a server-side resource described by @source.  For example, if
 * @source describes a new calendar, an equivalent calendar is created on
 * the server.
 *
 * It is the implementor's responsibility to examine @source and determine
 * what the equivalent server-side resource would be.  If this cannot be
 * determined without ambiguity, the function must return an error.
 *
 * After the server-side resource is successfully created, the implementor
 * must also add an #ESource to @backend's #ECollectionBackend:server.  This
 * can either be done immediately or in response to some "resource created"
 * notification from the server.  The added #ESource can be @source itself
 * or a different #ESource instance that describes the new resource.
 *
 * If an error occurs, the function will set @error and return %FALSE.
 *
 * Returns: %TRUE on success, %FALSE on failure
 *
 * Since: 3.6
 **/
gboolean
e_collection_backend_create_resource_sync (ECollectionBackend *backend,
                                           ESource *source,
                                           GCancellable *cancellable,
                                           GError **error)
{
	ECollectionBackendClass *class;

	g_return_val_if_fail (E_IS_COLLECTION_BACKEND (backend), FALSE);
	g_return_val_if_fail (E_IS_SOURCE (source), FALSE);

	class = E_COLLECTION_BACKEND_GET_CLASS (backend);
	g_return_val_if_fail (class->create_resource_sync != NULL, FALSE);

	return class->create_resource_sync (
		backend, source, cancellable, error);
}

/**
 * e_collection_backend_create_resource:
 * @backend: an #ECollectionBackend
 * @source: an #ESource
 * @cancellable: optional #GCancellable object, or %NULL
 * @callback: a #GAsyncReadyCallback to call when the request is satisfied
 * @user_data: data to pass to the callback function
 *
 * Asynchronously creates a server-side resource described by @source.
 * For example, if @source describes a new calendar, an equivalent calendar
 * is created on the server.
 *
 * It is the implementor's responsibility to examine @source and determine
 * what the equivalent server-side resource would be.  If this cannot be
 * determined without ambiguity, the function must return an error.
 *
 * After the server-side resource is successfully created, the implementor
 * must also add an #ESource to @backend's #ECollectionBackend:server.  This
 * can either be done immediately or in response to some "resource created"
 * notification from the server.  The added #ESource can be @source itself
 * or a different #ESource instance that describes the new resource.
 *
 * When the operation is finished, @callback will be called.  You can then
 * call e_collection_backend_create_resource_finish() to get the result of
 * the operation.
 *
 * Since: 3.6
 **/
void
e_collection_backend_create_resource (ECollectionBackend *backend,
                                      ESource *source,
                                      GCancellable *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data)
{
	ECollectionBackendClass *class;

	g_return_if_fail (E_IS_COLLECTION_BACKEND (backend));
	g_return_if_fail (E_IS_SOURCE (source));

	class = E_COLLECTION_BACKEND_GET_CLASS (backend);
	g_return_if_fail (class->create_resource != NULL);

	class->create_resource (
		backend, source, cancellable, callback, user_data);
}

/**
 * e_collection_backend_create_resource_finish:
 * @backend: an #ECollectionBackend
 * @result: a #GAsyncResult
 * @error: return location for a #GError, or %NULL
 *
 * Finishes the operation started with e_collection_backend_create_resource().
 *
 * If an error occurred, the function will set @error and return %FALSE.
 *
 * Returns: %TRUE on success, %FALSE on failure
 *
 * Since: 3.6
 **/
gboolean
e_collection_backend_create_resource_finish (ECollectionBackend *backend,
                                             GAsyncResult *result,
                                             GError **error)
{
	ECollectionBackendClass *class;

	g_return_val_if_fail (E_IS_COLLECTION_BACKEND (backend), FALSE);
	g_return_val_if_fail (G_IS_ASYNC_RESULT (result), FALSE);

	class = E_COLLECTION_BACKEND_GET_CLASS (backend);
	g_return_val_if_fail (class->create_resource_finish != NULL, FALSE);

	return class->create_resource_finish (backend, result, error);
}

/**
 * e_collection_backend_delete_resource_sync:
 * @backend: an #ECollectionBackend
 * @source: an #ESource
 * @cancellable: optional #GCancellable object, or %NULL
 * @error: return location for a #GError, or %NULL
 *
 * Deletes a server-side resource described by @source.  The @source must
 * be a child of @backend's collection #EBackend:source.
 *
 * After the server-side resource is successfully deleted, the implementor
 * must also remove @source from the @backend's #ECollectionBackend:server.
 * This can either be done immediately or in response to some "resource
 * deleted" notification from the server.
 *
 * If an error occurs, the function will set @error and return %FALSE.
 *
 * Returns: %TRUE on success, %FALSE on failure
 *
 * Since: 3.6
 **/
gboolean
e_collection_backend_delete_resource_sync (ECollectionBackend *backend,
                                           ESource *source,
                                           GCancellable *cancellable,
                                           GError **error)
{
	ECollectionBackendClass *class;

	g_return_val_if_fail (E_IS_COLLECTION_BACKEND (backend), FALSE);
	g_return_val_if_fail (E_IS_SOURCE (source), FALSE);

	class = E_COLLECTION_BACKEND_GET_CLASS (backend);
	g_return_val_if_fail (class->delete_resource_sync != NULL, FALSE);

	return class->delete_resource_sync (
		backend, source, cancellable, error);
}

/**
 * e_collection_backend_delete_resource:
 * @backend: an #ECollectionBackend
 * @source: an #ESource
 * @cancellable: optional #GCancellable object, or %NULL
 * @callback: a #GAsyncReadyCallback to call when the request is satisfied
 * @user_data: data to pass to the callback function
 *
 * Asynchronously deletes a server-side resource described by @source.
 * The @source must be a child of @backend's collection #EBackend:source.
 *
 * After the server-side resource is successfully deleted, the implementor
 * must also remove @source from the @backend's #ECollectionBackend:server.
 * This can either be done immediately or in response to some "resource
 * deleted" notification from the server.
 *
 * When the operation is finished, @callback will be called.  You can then
 * call e_collection_backend_delete_resource_finish() to get the result of
 * the operation.
 *
 * Since: 3.6
 **/
void
e_collection_backend_delete_resource (ECollectionBackend *backend,
                                      ESource *source,
                                      GCancellable *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data)
{
	ECollectionBackendClass *class;

	g_return_if_fail (E_IS_COLLECTION_BACKEND (backend));
	g_return_if_fail (E_IS_SOURCE (source));

	class = E_COLLECTION_BACKEND_GET_CLASS (backend);
	g_return_if_fail (class->delete_resource != NULL);

	return class->delete_resource (
		backend, source, cancellable, callback, user_data);
}

/**
 * e_collection_backend_delete_resource_finish:
 * @backend: an #ECollectionBackend
 * @result: a #GAsyncResult
 * @error: return location for a #GError, or %NULL
 *
 * Finishes the operation started with e_collection_backend_delete_resource().
 *
 * If an error occurred, the function will set @error and return %FALSE.
 *
 * Returns: %TRUE on success, %FALSE on failure
 *
 * Since: 3.6
 **/
gboolean
e_collection_backend_delete_resource_finish (ECollectionBackend *backend,
                                             GAsyncResult *result,
                                             GError **error)
{
	ECollectionBackendClass *class;

	g_return_val_if_fail (E_IS_COLLECTION_BACKEND (backend), FALSE);
	g_return_val_if_fail (G_IS_ASYNC_RESULT (result), FALSE);

	class = E_COLLECTION_BACKEND_GET_CLASS (backend);
	g_return_val_if_fail (class->delete_resource_finish != NULL, FALSE);

	return class->delete_resource_finish (backend, result, error);
}
