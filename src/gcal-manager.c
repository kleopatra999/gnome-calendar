/* -*- mode: c; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 * gcal-manager.c
 * Copyright (C) 2015 Erick Pérez Castellanos <erickpc@gnome.org>
 *
 * gnome-calendar is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * gnome-calendar is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define G_LOG_DOMAIN "GcalManager"

#include "gcal-debug.h"
#include "gcal-manager.h"
#include "gcal-utils.h"

#include <libedataserverui/libedataserverui.h>

typedef struct
{
  GcalEvent     *event;
  GcalManager   *manager;
} AsyncOpsData;

typedef struct
{
  ECalDataModelSubscriber *subscriber;
  gchar                   *query;

  guint                    sources_left;
  gboolean                 passed_start;
  gboolean                 search_done;
} ViewStateData;

typedef struct
{
  ECalClient     *client;
  gboolean        connected;
} GcalManagerUnit;

typedef struct
{
  gchar           *event_uid;
  GcalManagerUnit *unit;
  GcalManagerUnit *new_unit;
  ECalComponent   *new_component;
  GcalManager     *manager;
} MoveEventData;

struct _GcalManager
{
  GObject          parent;

  /**
   * The list of clients we are managing.
   * Each value is of type GCalStoreUnit
   * And each key is the source uid
   */
  GHashTable      *clients;

  ESourceRegistry *source_registry;
  ECredentialsPrompter *credentials_prompter;

  ECalDataModel   *e_data_model;
  ECalDataModel   *search_data_model;

  ECalDataModel   *shell_search_data_model;
  ViewStateData   *search_view_data;

  GCancellable    *async_ops;

  GoaClient       *goa_client;

  /* state flags */
  gboolean         goa_client_ready;
  gint             sources_at_launch;

  /* timezone */
  icaltimezone    *system_timezone;

  /* property */
  GSettings       *settings;
};

G_DEFINE_TYPE (GcalManager, gcal_manager, G_TYPE_OBJECT)

enum
{
  PROP_0,
  PROP_DEFAULT_CALENDAR,
  PROP_LOADING,
  PROP_SETTINGS,
  NUM_PROPS
};

enum
{
  SOURCE_ADDED,
  SOURCE_CHANGED,
  SOURCE_REMOVED,
  SOURCE_ENABLED,
  QUERY_COMPLETED,
  NUM_SIGNALS
};

static guint       signals[NUM_SIGNALS] = { 0, };
static GParamSpec *properties[NUM_PROPS] = { NULL, };

/* -- start: threading related code provided by Milan Crha */
typedef struct {
  EThreadJobFunc func;
  gpointer user_data;
  GDestroyNotify free_user_data;

  GCancellable *cancellable;
  GError *error;
} ThreadJobData;

static void
thread_job_data_free (gpointer ptr)
{
  ThreadJobData *tjd = ptr;

  if (tjd != NULL)
    {
      /* This should go to UI with more info/description,
       * if it is not G_IOI_ERROR_CANCELLED */
      if (tjd->error != NULL && !g_error_matches (tjd->error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("Job failed: %s\n", tjd->error->message);

      if (tjd->free_user_data != NULL)
        tjd->free_user_data (tjd->user_data);

      g_clear_object (&tjd->cancellable);
      g_clear_error (&tjd->error);
      g_free (tjd);
    }
}

static gpointer
thread_job_thread (gpointer user_data)
{
  ThreadJobData *tjd = user_data;

  g_return_val_if_fail (tjd != NULL, NULL);

  if (tjd->func != NULL)
    tjd->func (tjd->user_data, tjd->cancellable, &tjd->error);

  thread_job_data_free (tjd);

  return g_thread_self ();
}

static GCancellable *
submit_thread_job (EThreadJobFunc func,
                   gpointer user_data,
                   GDestroyNotify free_user_data)
{
  ThreadJobData *tjd;
  GThread *thread;
  GCancellable *cancellable;

  cancellable = g_cancellable_new ();

  tjd = g_new0 (ThreadJobData, 1);
  tjd->func = func;
  tjd->user_data = user_data;
  tjd->free_user_data = free_user_data;
  /* user should be able to cancel this cancellable somehow */
  tjd->cancellable = g_object_ref (cancellable);
  tjd->error = NULL;

  thread = g_thread_try_new (NULL,
                             thread_job_thread, tjd,
                             &tjd->error);
  if (thread != NULL)
    {
      g_thread_unref (thread);
    }
  else
    {
      thread_job_data_free (tjd);
      g_clear_object (&cancellable);
    }

  return cancellable;
}
/* -- end: threading related code provided by Milan Crha -- */

static void
free_async_ops_data (AsyncOpsData *data)
{
  g_object_unref (data->event);
  g_free (data);
}

static void
free_unit_data (GcalManagerUnit *data)
{
  g_object_unref (data->client);
  g_free (data);
}

static gboolean
gather_events (ECalDataModel         *data_model,
               ECalClient            *client,
               const ECalComponentId *id,
               ECalComponent         *comp,
               time_t                 instance_start,
               time_t                 instance_end,
               gpointer               user_data)
{
  GcalEvent *event;
  GError *error;
  GList **result;

  error = NULL;
  result = user_data;
  event = gcal_event_new (e_client_get_source (E_CLIENT (client)), comp, &error);

  if (error)
    {
      g_warning ("Error: %s", error->message);
      g_clear_error (&error);
      return TRUE;
    }

  *result = g_list_append (*result, event);/* FIXME: add me sorted */

  return TRUE;
}

static void
remove_source (GcalManager  *manager,
               ESource      *source)
{
  GcalManagerUnit *unit;

  GCAL_ENTRY;

  g_return_if_fail (GCAL_IS_MANAGER (manager));
  g_return_if_fail (E_IS_SOURCE (source));

  e_cal_data_model_remove_client (manager->e_data_model,
                                  e_source_get_uid (source));
  e_cal_data_model_remove_client (manager->search_data_model,
                                  e_source_get_uid (source));

  unit = g_hash_table_lookup (manager->clients, source);
  if (unit && unit->client)
     g_signal_handlers_disconnect_by_data (unit->client, manager);

  g_hash_table_remove (manager->clients, source);
  g_signal_emit (manager, signals[SOURCE_REMOVED], 0, source);

  GCAL_EXIT;
}

static void
source_changed (GcalManager *manager,
                ESource     *source)
{
  GCAL_ENTRY;

  if (g_hash_table_lookup (manager->clients, source) != NULL &&
      e_source_has_extension (source, E_SOURCE_EXTENSION_CALENDAR))
    {
      g_signal_emit (manager, signals[SOURCE_CHANGED], 0, source);
    }

  GCAL_EXIT;
}

static void
on_client_readonly_changed (EClient    *client,
                            GParamSpec *pspec,
                            gpointer    user_data)
{
  GcalManager *manager;
  ESource *source;
  GcalManagerUnit *unit;

  GCAL_ENTRY;

  manager = GCAL_MANAGER (user_data);
  source = e_client_get_source (client);

  unit = g_hash_table_lookup (manager->clients, source);
  if (unit && is_source_enabled (source))
    source_changed (manager, source);

  GCAL_EXIT;
}

static void
on_client_refreshed (GObject      *source_object,
                     GAsyncResult *result,
                     gpointer      user_data)
{
  GError *error = NULL;

  GCAL_ENTRY;

  if (e_client_refresh_finish (E_CLIENT (source_object), result, &error))
    {
      ESource *source = e_client_get_source (E_CLIENT (source_object));
      /* FIXME: add notification to UI */
      g_debug ("Client of source: %s refreshed succesfully", e_source_get_uid (source));
    }
  else
    {
      /* FIXME: do something when there was some error */
      g_warning ("Error synchronizing client");
    }

  GCAL_EXIT;
}

static void
on_client_connected (GObject      *source_object,
                     GAsyncResult *result,
                     gpointer      user_data)
{
  GcalManagerUnit *unit;
  ESourceOffline *offline_extension;
  GcalManager *manager;
  ECalClient *client;
  ESource *source;
  GError *error;
  gboolean enabled;

  GCAL_ENTRY;

  manager = GCAL_MANAGER (user_data);
  source = e_client_get_source (E_CLIENT (source_object));
  enabled = is_source_enabled (source);

  manager->sources_at_launch--;

  if (manager->sources_at_launch == 0)
    g_object_notify_by_pspec (G_OBJECT (manager), properties[PROP_LOADING]);

  error = NULL;
  client = E_CAL_CLIENT (e_cal_client_connect_finish (result, &error));

  if (error)
    {
      remove_source (GCAL_MANAGER (user_data), source);
      g_warning ("%s: Failed to open/connect '%s': %s",
                 G_STRFUNC,
                 e_source_get_display_name (source),
                 error->message);

      g_object_unref (source);
      g_error_free (error);
      return;
    }

  unit = g_new0 (GcalManagerUnit, 1);
  unit->connected = TRUE;
  unit->client = g_object_ref (client);

  g_hash_table_insert (manager->clients, source, unit);

  g_debug ("Source %s (%s) connected",
           e_source_get_display_name (source),
           e_source_get_uid (source));

  /* notify the readonly property */
  g_signal_connect (client, "notify::readonly", G_CALLBACK (on_client_readonly_changed), user_data);

  if (enabled)
    {
      e_cal_data_model_add_client (manager->e_data_model, client);
      e_cal_data_model_add_client (manager->search_data_model, client);
      if (manager->shell_search_data_model != NULL)
        e_cal_data_model_add_client (manager->shell_search_data_model, client);
    }

  /* refresh client when it's added */
  if (enabled && e_client_check_refresh_supported (E_CLIENT (client)))
    e_client_refresh (E_CLIENT (client), NULL, on_client_refreshed, user_data);

  /* Cache all the online calendars, so the user can see them offline */
  offline_extension = e_source_get_extension (source, E_SOURCE_EXTENSION_OFFLINE);
  e_source_offline_set_stay_synchronized (offline_extension, TRUE);

  e_source_registry_commit_source (manager->source_registry,
                                   source,
                                   NULL,
                                   NULL,
                                   NULL);

  g_signal_emit (GCAL_MANAGER (user_data), signals[SOURCE_ADDED], 0, source, enabled);

  g_clear_object (&client);

  GCAL_EXIT;
}

/**
 * load_source:
 * @manager: Manager instance
 * @source: Loaded source
 *
 * Create @GcalManagerUnit data, add it to internal hash of sources.
 * Open/connect to calendars available
 *
 **/
static void
load_source (GcalManager *manager,
             ESource     *source)
{
  GCAL_ENTRY;

  if (g_hash_table_lookup (manager->clients, source) == NULL &&
      e_source_has_extension (source, E_SOURCE_EXTENSION_CALENDAR))
    {
      /* NULL: because maybe the operation cannot be really cancelled */
      e_cal_client_connect (source,
                            E_CAL_CLIENT_SOURCE_TYPE_EVENTS, 1, NULL,
                            on_client_connected,
                            manager);
    }
  else
    {
      g_warning ("%s: Skipping already loaded source: %s",
                 G_STRFUNC,
                 e_source_get_uid (source));
    }

  GCAL_EXIT;
}

static void
on_event_created (GObject      *source_object,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  AsyncOpsData *data;
  ECalClient *client;
  gchar *new_uid;
  GError *error;

  GCAL_ENTRY;

  data = (AsyncOpsData*) user_data;
  client = E_CAL_CLIENT (source_object);
  new_uid = NULL;
  error = NULL;

  if (!e_cal_client_create_object_finish (client, result, &new_uid, &error))
    {
      /* Some error */
      g_warning ("Error creating object: %s", error->message);
      g_error_free (error);
    }
  else
    {
      gcal_manager_set_default_source (data->manager, gcal_event_get_source (data->event));
      g_debug ("Event: %s created successfully", new_uid);
    }

  g_free (new_uid);
  free_async_ops_data (data);

  GCAL_EXIT;
}

/**
 * on_component_updated:
 * @source_object: {@link ECalClient} source
 * @result: result of the operation
 * @user_data: manager instance
 *
 * Called when an component is modified. Currently, it only checks for
 * error, but a more sophisticated implementation will come in no time.
 *
 **/
static void
on_event_updated (GObject      *source_object,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  GError *error = NULL;

  GCAL_ENTRY;

  if (! e_cal_client_modify_object_finish (E_CAL_CLIENT (source_object),
                                           result,
                                           &error))
    {
      g_warning ("Error updating component: %s", error->message);
      g_error_free (error);
    }
  g_object_unref (E_CAL_COMPONENT (user_data));

  GCAL_EXIT;
}

/**
 * on_event_removed:
 * @source_object: {@link ECalClient} source
 * @result: result of the operation
 * @user_data: an {@link ECalComponent}
 *
 * Called when an component is removed. Currently, it only checks for
 * error, but a more sophisticated implementation will come in no time.*
 *
 **/
static void
on_event_removed (GObject      *source_object,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  ECalClient *client;
  GError *error;

  GCAL_ENTRY;

  client = E_CAL_CLIENT (source_object);
  error = NULL;

  e_cal_client_remove_object_finish (client, result, &error);

  if (error)
    {
      /* FIXME: Notify the user somehow */
      g_warning ("Error removing event: %s", error->message);
      g_error_free (error);
    }

  g_object_unref (user_data);

  GCAL_EXIT;
}

static void
model_state_changed (GcalManager            *manager,
                     ECalClientView         *view,
                     ECalDataModelViewState  state,
                     guint                   percent,
                     const gchar            *message,
                     const GError           *error,
                     ECalDataModel          *data_model)
{
  gchar *filter;

  GCAL_ENTRY;

  filter = e_cal_data_model_dup_filter (data_model);

  if (state == E_CAL_DATA_MODEL_VIEW_STATE_START &&
      g_strcmp0 (manager->search_view_data->query, filter) == 0)
    {
      manager->search_view_data->passed_start = TRUE;
      GCAL_GOTO (out);
    }

  if (manager->search_view_data->passed_start &&
      state == E_CAL_DATA_MODEL_VIEW_STATE_COMPLETE &&
      g_strcmp0 (manager->search_view_data->query, filter) == 0)
    {
      manager->search_view_data->sources_left--;
      manager->search_view_data->search_done = (manager->search_view_data->sources_left == 0);

      if (manager->search_view_data->search_done)
        g_signal_emit (manager, signals[QUERY_COMPLETED], 0);
    }

out:
  g_free (filter);

  GCAL_EXIT;
}

static void
show_source_error (const gchar  *where,
                   const gchar  *what,
                   ESource      *source,
                   const GError *error)
{
  if (!error || g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;

  /* TODO Show the error in UI, somehow */
  g_warning ("%s: %s '%s': %s", where, what, e_source_get_display_name (source), error->message);
}

static void
source_invoke_authenticate_cb (GObject      *source_object,
                               GAsyncResult *result,
                               gpointer      user_data)
{
  ESource *source = E_SOURCE (source_object);
  GError *error = NULL;

  GCAL_ENTRY;

  if (!e_source_invoke_authenticate_finish (source, result, &error) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      show_source_error (G_STRFUNC, "Failed to invoke authenticate", source, error);
    }

  g_clear_error (&error);

  GCAL_EXIT;
}

static void
source_trust_prompt_done_cb (GObject      *source_object,
                             GAsyncResult *result,
                             gpointer      user_data)
{
  ETrustPromptResponse response = E_TRUST_PROMPT_RESPONSE_UNKNOWN;
  ESource *source = E_SOURCE (source_object);
  GError *error = NULL;

  GCAL_ENTRY;

  if (!e_trust_prompt_run_for_source_finish (source, result, &response, &error))
    {
      show_source_error (G_STRFUNC, "Failed to prompt for trust for", source, error);
    }
  else if (response == E_TRUST_PROMPT_RESPONSE_ACCEPT ||
           response == E_TRUST_PROMPT_RESPONSE_ACCEPT_TEMPORARILY)
    {
      /* Use NULL credentials to reuse those from the last time. */
      e_source_invoke_authenticate (source, NULL, NULL /* cancellable */, source_invoke_authenticate_cb, NULL);
    }

  g_clear_error (&error);

  GCAL_EXIT;
}

static void
source_credentials_required_cb (ESourceRegistry         *registry,
                                ESource                 *source,
                                ESourceCredentialsReason reason,
                                const gchar             *certificate_pem,
                                GTlsCertificateFlags     certificate_errors,
                                const GError            *op_error,
                                GcalManager             *manager)
{
  ECredentialsPrompter *credentials_prompter;

  GCAL_ENTRY;

  g_return_if_fail (GCAL_IS_MANAGER (manager));

  credentials_prompter = manager->credentials_prompter;

  if (e_credentials_prompter_get_auto_prompt_disabled_for (credentials_prompter, source))
    return;

  if (reason == E_SOURCE_CREDENTIALS_REASON_SSL_FAILED)
    {
      e_trust_prompt_run_for_source (e_credentials_prompter_get_dialog_parent (credentials_prompter),
                                     source,
                                     certificate_pem,
                                     certificate_errors,
                                     op_error ? op_error->message : NULL,
                                     TRUE /* allow_source_save */,
                                     NULL /* cancellable */,
                                     source_trust_prompt_done_cb,
                                     NULL);
    }
  else if (reason == E_SOURCE_CREDENTIALS_REASON_ERROR && op_error)
    {
      show_source_error (G_STRFUNC, "Failed to authenticate", source, op_error);
    }

  GCAL_EXIT;
}

static void
source_get_last_credentials_required_arguments_cb (GObject      *source_object,
                                                   GAsyncResult *result,
                                                   gpointer      user_data)
{
  GcalManager *manager = user_data;
  ESource *source;
  ESourceCredentialsReason reason = E_SOURCE_CREDENTIALS_REASON_UNKNOWN;
  gchar *certificate_pem = NULL;
  GTlsCertificateFlags certificate_errors = 0;
  GError *op_error = NULL;
  GError *error = NULL;

  GCAL_ENTRY;

  g_return_if_fail (E_IS_SOURCE (source_object));

  source = E_SOURCE (source_object);

  if (!e_source_get_last_credentials_required_arguments_finish (source, result, &reason,
          &certificate_pem, &certificate_errors, &op_error, &error))
    {
      /* Can be cancelled only if the manager is disposing/disposed */
      if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        show_source_error (G_STRFUNC, "Failed to get last credentials required arguments for", source, error);

      g_clear_error (&error);
      return;
    }

  g_return_if_fail (GCAL_IS_MANAGER (manager));

  if (reason != E_SOURCE_CREDENTIALS_REASON_UNKNOWN)
    source_credentials_required_cb (NULL, source, reason, certificate_pem, certificate_errors, op_error, manager);

  g_free (certificate_pem);
  g_clear_error (&op_error);

  GCAL_EXIT;
}

static void
gcal_manager_client_ready_cb (GObject      *source,
                              GAsyncResult *result,
                              gpointer      user_data)
{
  GcalManager *manager = GCAL_MANAGER (user_data);
  GError *error = NULL;

  GCAL_ENTRY;

  manager->goa_client = goa_client_new_finish (result, &error);
  manager->goa_client_ready = TRUE;

  if (error != NULL)
    {
      g_warning ("%s: Error retrieving GoaClient: %s",
                 G_STRFUNC,
                 error->message);

      g_error_free (error);
    }

  g_object_notify_by_pspec (G_OBJECT (manager), properties[PROP_LOADING]);

  GCAL_EXIT;
}

static void
gcal_manager_constructed (GObject *object)
{
  GcalManager *manager;

  GList *sources, *l;
  GError *error = NULL;
  ESourceCredentialsProvider *credentials_provider;

  GCAL_ENTRY;

  G_OBJECT_CLASS (gcal_manager_parent_class)->constructed (object);

  manager = GCAL_MANAGER (object);
  manager->system_timezone = e_cal_util_get_system_timezone ();

  manager->clients = g_hash_table_new_full ((GHashFunc) e_source_hash, (GEqualFunc) e_source_equal,
                                         g_object_unref, (GDestroyNotify) free_unit_data);

  /* load GOA client */
  goa_client_new (NULL, // we won't really cancel it
                  (GAsyncReadyCallback) gcal_manager_client_ready_cb,
                  object);

  /* reading sources and schedule its connecting */
  manager->source_registry = e_source_registry_new_sync (NULL, &error);

  if (!manager->source_registry)
    {
      g_warning ("Failed to access calendar configuration: %s", error->message);
      g_error_free (error);
      return;
    }

  g_object_bind_property (manager->source_registry,
                          "default-calendar",
                          manager,
                          "default-calendar",
                          G_BINDING_DEFAULT);

  manager->credentials_prompter = e_credentials_prompter_new (manager->source_registry);

  /* First disable credentials prompt for all but calendar sources... */
  sources = e_source_registry_list_sources (manager->source_registry, NULL);

  for (l = sources; l != NULL; l = g_list_next (l))
    {
      ESource *source = E_SOURCE (l->data);

      /* Mark for skip also currently disabled sources */
      if (!e_source_has_extension (source, E_SOURCE_EXTENSION_CALENDAR) &&
          !e_source_has_extension (source, E_SOURCE_EXTENSION_COLLECTION))
        {
          e_credentials_prompter_set_auto_prompt_disabled_for (manager->credentials_prompter, source, TRUE);
        }
      else
        {
          e_source_get_last_credentials_required_arguments (source,
                                                            NULL,
                                                            source_get_last_credentials_required_arguments_cb,
                                                            object);
        }
    }

  g_list_free_full (sources, g_object_unref);

  credentials_provider = e_credentials_prompter_get_provider (manager->credentials_prompter);

  /* ...then enable credentials prompt for credential source of the calendar sources,
     which can be a collection source.  */
  sources = e_source_registry_list_sources (manager->source_registry, E_SOURCE_EXTENSION_CALENDAR);

  for (l = sources; l != NULL; l = g_list_next (l))
    {
      ESource *source, *cred_source;

      source = l->data;
      cred_source = e_source_credentials_provider_ref_credentials_source (credentials_provider, source);

      if (cred_source && !e_source_equal (source, cred_source))
	{
          e_credentials_prompter_set_auto_prompt_disabled_for (manager->credentials_prompter, cred_source, FALSE);

          /* Only consider SSL errors */
          if (e_source_get_connection_status (cred_source) != E_SOURCE_CONNECTION_STATUS_SSL_FAILED)
            continue;

          e_source_get_last_credentials_required_arguments (cred_source,
                                                            NULL,
                                                            source_get_last_credentials_required_arguments_cb,
                                                            object);
        }

      g_clear_object (&cred_source);
    }

  g_list_free_full (sources, g_object_unref);

  /* The eds_credentials_prompter responses to REQUIRED and REJECTED reasons,
     the SSL_FAILED should be handled elsewhere. */
  g_signal_connect (manager->source_registry, "credentials-required", G_CALLBACK (source_credentials_required_cb), object);

  e_credentials_prompter_process_awaiting_credentials (manager->credentials_prompter);

  g_signal_connect_swapped (manager->source_registry, "source-added", G_CALLBACK (load_source), object);
  g_signal_connect_swapped (manager->source_registry, "source-removed", G_CALLBACK (remove_source), object);
  g_signal_connect_swapped (manager->source_registry, "source-changed", G_CALLBACK (source_changed), object);

  /* create data model */
  manager->e_data_model = e_cal_data_model_new (submit_thread_job);
  manager->search_data_model = e_cal_data_model_new (submit_thread_job);

  e_cal_data_model_set_expand_recurrences (manager->e_data_model, TRUE);
  e_cal_data_model_set_timezone (manager->e_data_model, manager->system_timezone);
  e_cal_data_model_set_expand_recurrences (manager->search_data_model, TRUE);
  e_cal_data_model_set_timezone (manager->search_data_model, manager->system_timezone);

  sources = e_source_registry_list_enabled (manager->source_registry, E_SOURCE_EXTENSION_CALENDAR);
  manager->sources_at_launch = g_list_length (sources);

  for (l = sources; l != NULL; l = l->next)
    load_source (GCAL_MANAGER (object), l->data);

  g_list_free (sources);

  GCAL_EXIT;
}

static void
gcal_manager_finalize (GObject *object)
{
  GcalManager *manager =GCAL_MANAGER (object);

  GCAL_ENTRY;

  g_clear_object (&manager->settings);
  g_clear_object (&manager->goa_client);
  g_clear_object (&manager->e_data_model);
  g_clear_object (&manager->search_data_model);
  g_clear_object (&manager->shell_search_data_model);

  if (manager->search_view_data != NULL)
    {
      g_free (manager->search_view_data->query);
      g_free (manager->search_view_data);
    }

  g_hash_table_destroy (manager->clients);

  GCAL_EXIT;
}

static void
gcal_manager_set_property (GObject      *object,
                           guint         property_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
  GcalManager *self = GCAL_MANAGER (object);

  GCAL_ENTRY;

  switch (property_id)
    {
    case PROP_DEFAULT_CALENDAR:
        {
          ESource *source;

          source = e_source_registry_ref_default_calendar (self->source_registry);
          g_object_unref (source);

          /* Only notify a change when they're different, otherwise we'll end up in a notify loop */
          if (g_value_get_object (value) == source)
            break;

          e_source_registry_set_default_calendar (self->source_registry, g_value_get_object (value));
          g_object_notify (object, "default-calendar");
        }
      break;

    case PROP_SETTINGS:
      if (g_set_object (&self->settings, g_value_get_object (value)))
        g_object_notify (object, "settings");
      return;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }

  GCAL_EXIT;
}

static void
gcal_manager_get_property (GObject    *object,
                           guint       property_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
  GcalManager *self = GCAL_MANAGER (object);

  GCAL_ENTRY;

  switch (property_id)
    {
    case PROP_DEFAULT_CALENDAR:
      g_value_take_object (value, e_source_registry_ref_default_calendar (self->source_registry));
      break;

    case PROP_LOADING:
      g_value_set_boolean (value, gcal_manager_get_loading (self));
      break;

    case PROP_SETTINGS:
      g_value_set_object (value, self->settings);
      return;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }

  G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);

  GCAL_EXIT;
}

static void
gcal_manager_class_init (GcalManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gcal_manager_finalize;
  object_class->constructed = gcal_manager_constructed;
  object_class->set_property = gcal_manager_set_property;
  object_class->get_property = gcal_manager_get_property;

  /**
   * GcalManager:default-calendar:
   *
   * The default calendar.
   */
  properties[PROP_DEFAULT_CALENDAR] = g_param_spec_object ("default-calendar",
                                                           "Default calendar",
                                                           "The default calendar",
                                                           E_TYPE_SOURCE,
                                                           G_PARAM_READWRITE);

  /**
   * GcalManager:loading:
   *
   * Whether the manager is loading or not.
   */
  properties[PROP_LOADING] = g_param_spec_boolean ("loading",
                                                   "Loading",
                                                   "Whether it's still loading or not",
                                                   TRUE,
                                                   G_PARAM_READABLE);

  /**
   * GcalManager:settings:
   *
   * The settings.
   */
  properties[PROP_SETTINGS] = g_param_spec_object ("settings",
                                                   "Application settings",
                                                   "The settings of the application passed down from GcalApplication",
                                                   G_TYPE_SETTINGS,
                                                   G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE);

  g_object_class_install_properties (object_class, NUM_PROPS, properties);

  /* signals */
  signals[SOURCE_ADDED] = g_signal_new ("source-added",
                                        GCAL_TYPE_MANAGER,
                                        G_SIGNAL_RUN_LAST,
                                        0, NULL, NULL, NULL,
                                        G_TYPE_NONE,
                                        2,
                                        G_TYPE_POINTER,
                                        G_TYPE_BOOLEAN);

  signals[SOURCE_CHANGED] = g_signal_new ("source-changed",
                                          GCAL_TYPE_MANAGER,
                                          G_SIGNAL_RUN_LAST,
                                          0, NULL, NULL, NULL,
                                          G_TYPE_NONE,
                                          1,
                                          E_TYPE_SOURCE);

  signals[SOURCE_REMOVED] = g_signal_new ("source-removed",
                                          GCAL_TYPE_MANAGER,
                                          G_SIGNAL_RUN_LAST,
                                          0, NULL, NULL, NULL,
                                          G_TYPE_NONE,
                                          1,
                                          G_TYPE_POINTER);

  signals[SOURCE_ENABLED] = g_signal_new ("source-enabled",
                                          GCAL_TYPE_MANAGER,
                                          G_SIGNAL_RUN_LAST,
                                          0, NULL, NULL, NULL,
                                          G_TYPE_NONE,
                                          2,
                                          E_TYPE_SOURCE,
                                          G_TYPE_BOOLEAN);

  signals[QUERY_COMPLETED] = g_signal_new ("query-completed",
                                           GCAL_TYPE_MANAGER,
                                           G_SIGNAL_RUN_LAST,
                                           0,
                                           NULL, NULL, NULL,
                                           G_TYPE_NONE,
                                           0);
}

static void
gcal_manager_init (GcalManager *self)
{
  ;
}

/* Public API */
/**
 * gcal_manager_new_with_settings:
 * @settings:
 *
 * Since: 0.0.1
 * Return value: the new #GcalManager
 * Returns: (transfer full):
 **/
GcalManager*
gcal_manager_new_with_settings (GSettings *settings)
{
  return g_object_new (GCAL_TYPE_MANAGER,
                       "settings", settings,
                       NULL);
}

/**
 * gcal_manager_get_source:
 * @manager:
 *
 * Retrieve a source according to it's UID. The source
 * is referenced for thread-safety and must be unreferenced
 * after user.
 *
 * Returns: (Transfer full) an {@link ESource}, or NULL.
 **/
ESource*
gcal_manager_get_source (GcalManager *manager,
                         const gchar *uid)
{
  return e_source_registry_ref_source (manager->source_registry, uid);
}

/**
 * gcal_manager_get_sources:
 * @manager:
 *
 * Retrieve a list of the enabled sources used in the application.
 *
 * Returns: (Transfer full) a {@link GList} object to be freed with g_list_free()
 **/
GList*
gcal_manager_get_sources (GcalManager *manager)
{
  GHashTableIter iter;
  gpointer key, value;
  GList *aux = NULL;

  GCAL_ENTRY;

  g_hash_table_iter_init (&iter, manager->clients);

  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      GcalManagerUnit *unit = value;

      if (!is_source_enabled (key))
        continue;

      aux = g_list_append (aux, key);
    }

  GCAL_RETURN (aux);
}

/**
 * gcal_manager_get_sources_connected:
 * @manager:
 *
 * Returns a {@link GList} with every source connected on the app, whether they are enabled or not.
 *
 * Returns: (Transfer full) a {@link GList} object to be freed with g_list_free()
 **/
GList*
gcal_manager_get_sources_connected (GcalManager *manager)
{
  return g_hash_table_get_keys (manager->clients);
}

/**
 * gcal_manager_get_default_source:
 * @manager: App singleton {@link GcalManager} instance
 *
 * Returns: (Transfer full): an {@link ESource} object. Free with g_object_unref().
 **/
ESource*
gcal_manager_get_default_source (GcalManager *manager)
{
  return e_source_registry_ref_default_calendar (manager->source_registry);
}

/**
 * gcal_manager_set_default_source:
 * @manager: App singleton {@link GcalManager} instance
 * @source: the new default source.
 *
 * Returns:
 **/
void
gcal_manager_set_default_source (GcalManager *manager,
                                 ESource     *source)
{
  e_source_registry_set_default_calendar (manager->source_registry, source);
}

icaltimezone*
gcal_manager_get_system_timezone (GcalManager *manager)
{
  return manager->system_timezone;
}

void
gcal_manager_setup_shell_search (GcalManager             *self,
                                 ECalDataModelSubscriber *subscriber)
{
  g_return_if_fail (GCAL_IS_MANAGER (self));

  if (!self->shell_search_data_model)
    return;

  self->shell_search_data_model = e_cal_data_model_new (submit_thread_job);
  g_signal_connect_swapped (self->shell_search_data_model,
                            "view-state-changed",
                            G_CALLBACK (model_state_changed),
                            self);

  e_cal_data_model_set_expand_recurrences (self->shell_search_data_model, TRUE);
  e_cal_data_model_set_timezone (self->shell_search_data_model, self->system_timezone);

  self->search_view_data = g_new0 (ViewStateData, 1);
  self->search_view_data->subscriber = subscriber;
}

/**
 * gcal_manager_set_shell_search_query:
 * @manager: A #GcalManager instance
 * @query: query string (an s-exp)
 *
 * Set the query terms of the #ECalDataModel used in the shell search
 **/
void
gcal_manager_set_shell_search_query (GcalManager *manager,
                                     const gchar *query)
{
  GCAL_ENTRY;

  manager->search_view_data->passed_start = FALSE;
  manager->search_view_data->search_done = FALSE;
  manager->search_view_data->sources_left = g_hash_table_size (manager->clients);

  if (manager->search_view_data->query != NULL)
    g_free (manager->search_view_data->query);
  manager->search_view_data->query = g_strdup (query);

  e_cal_data_model_set_filter (manager->shell_search_data_model, query);
}

void
gcal_manager_set_shell_search_subscriber (GcalManager             *manager,
                                          ECalDataModelSubscriber *subscriber,
                                          time_t                   range_start,
                                          time_t                   range_end)
{
  GCAL_ENTRY;

  e_cal_data_model_subscribe (manager->shell_search_data_model, subscriber, range_start, range_end);

  GCAL_EXIT;
}

gboolean
gcal_manager_shell_search_done (GcalManager *manager)
{
  return manager->search_view_data->search_done;
}

GList*
gcal_manager_get_shell_search_events (GcalManager *manager)
{
  time_t range_start, range_end;
  GList *list = NULL;

  GCAL_ENTRY;

  e_cal_data_model_get_subscriber_range (manager->shell_search_data_model,
                                         manager->search_view_data->subscriber,
                                         &range_start,
                                         &range_end);

  e_cal_data_model_foreach_component (manager->shell_search_data_model,
                                      range_start,
                                      range_end,
                                      gather_events,
                                      &list);

  GCAL_RETURN (list);
}

void
gcal_manager_set_subscriber (GcalManager             *manager,
                             ECalDataModelSubscriber *subscriber,
                             time_t                   range_start,
                             time_t                   range_end)
{
  GCAL_ENTRY;

  e_cal_data_model_subscribe (manager->e_data_model,
                              subscriber,
                              range_start,
                              range_end);

  GCAL_EXIT;
}

void
gcal_manager_set_search_subscriber (GcalManager             *manager,
                                    ECalDataModelSubscriber *subscriber,
                                    time_t                   range_start,
                                    time_t                   range_end)
{
  GCAL_ENTRY;

  e_cal_data_model_subscribe (manager->search_data_model,
                              subscriber,
                              range_start,
                              range_end);

  GCAL_EXIT;
}

/**
 * gcal_manager_set_query:
 * @self: A #GcalManager instance
 * @query: (nullable): query terms or %NULL
 *
 * Set the query terms of the #ECalDataModel or clear it if %NULL is
 * passed
 *
 **/
void
gcal_manager_set_query (GcalManager *self,
                        const gchar *query)
{
  GCAL_ENTRY;

  g_return_if_fail (GCAL_IS_MANAGER (self));

  e_cal_data_model_set_filter (self->search_data_model,
                               query != NULL ? query : "#t");

  GCAL_EXIT;
}

/**
 * gcal_manager_query_client_data:
 *
 * Queries for a specific data field of
 * the {@link ECalClient}.
 *
 * Returns: (Transfer none) a string representing the retrieved value, or NULL.
 */
gchar*
gcal_manager_query_client_data (GcalManager *manager,
                                ESource     *source,
                                const gchar *field)
{
  GcalManagerUnit *unit;
  gchar *out;

  GCAL_ENTRY;

  unit = g_hash_table_lookup (manager->clients, source);

  if (!unit)
    GCAL_RETURN (NULL);

  g_object_get (unit->client, field, &out, NULL);

  GCAL_RETURN (out);
}

/**
 * gcal_manager_add_source:
 * @manager: a #GcalManager
 * @base_uri: URI defining the ESourceGroup the client will belongs
 * @relative_uri: URI, relative to the base URI
 * @color: a string representing a color parseable for gdk_rgba_parse
 *
 * Add a new calendar by its URI.
 * The calendar is enabled by default
 *
 * Return value: The UID of calendar's source, NULL in other case
 * Returns: (transfer full):
 *
 * Since: 0.0.1
 */
gchar*
gcal_manager_add_source (GcalManager *manager,
                         const gchar *name,
                         const gchar *backend,
                         const gchar *color)
{
  ESource *source;
  ESourceCalendar *extension;
  GError *error;

  GCAL_ENTRY;

  source = e_source_new (NULL, NULL, NULL);
  extension = E_SOURCE_CALENDAR (e_source_get_extension (source, E_SOURCE_EXTENSION_CALENDAR));

  g_object_set (extension,
                "backend-name", backend,
                "color", color,
                NULL);

  e_source_set_display_name (source, name);

  error = NULL;
  e_source_registry_commit_source_sync (manager->source_registry,
                                        source,
                                        NULL,
                                        &error);
  if (error)
    {
      g_warning ("Failed to store calendar configuration: %s", error->message);
      g_object_unref (source);
      g_clear_error (&error);

      GCAL_RETURN (NULL);
    }

  load_source (manager, source);

  GCAL_RETURN (e_source_dup_uid (source));
}

/**
 * gcal_manager_enable_source:
 * @manager: a #GcalManager
 * @source: the target ESource
 *
 * Enable the given ESource.
 */
void
gcal_manager_enable_source (GcalManager *manager,
                            ESource     *source)
{
  ESourceSelectable *selectable;
  GcalManagerUnit *unit;

  GCAL_ENTRY;

  unit = g_hash_table_lookup (manager->clients, source);
  selectable = e_source_get_extension (source, E_SOURCE_EXTENSION_CALENDAR);

  if (is_source_enabled (source))
    {
      g_debug ("Source '%s' already enabled", e_source_get_uid (source));
      GCAL_EXIT;
      return;
    }

  e_cal_data_model_add_client (manager->e_data_model, unit->client);
  e_cal_data_model_add_client (manager->search_data_model, unit->client);

  if (manager->shell_search_data_model)
    e_cal_data_model_add_client (manager->shell_search_data_model, unit->client);

  g_signal_emit (manager, signals[SOURCE_ENABLED], 0, source, TRUE);

  /* Save the source */
  e_source_selectable_set_selected (selectable, TRUE);
  gcal_manager_save_source (manager, source);

  GCAL_EXIT;
}

/**
 * gcal_manager_disable_source:
 * @manager: a #GcalManager
 * @source: the target ESource
 *
 * Disable the given ESource.
 */
void
gcal_manager_disable_source (GcalManager *manager,
                             ESource     *source)
{
  ESourceSelectable *selectable;
  GcalManagerUnit *unit;
  const gchar *source_uid;

  GCAL_ENTRY;

  unit = g_hash_table_lookup (manager->clients, source);
  selectable = e_source_get_extension (source, E_SOURCE_EXTENSION_CALENDAR);

  if (!is_source_enabled (source))
    {
      g_debug ("Source '%s' already disabled", e_source_get_uid (source));
      GCAL_EXIT;
      return;
    }

  source_uid = e_source_get_uid (source);

  e_cal_data_model_remove_client (manager->e_data_model, source_uid);
  e_cal_data_model_remove_client (manager->search_data_model, source_uid);

  if (manager->shell_search_data_model != NULL)
    e_cal_data_model_remove_client (manager->shell_search_data_model, source_uid);

  g_signal_emit (manager, signals[SOURCE_ENABLED], 0, source, FALSE);

  /* Save the source */
  e_source_selectable_set_selected (selectable, FALSE);
  gcal_manager_save_source (manager, source);

  GCAL_EXIT;
}

/**
 * gcal_manager_save_source:
 * @manager: a #GcalManager
 * @source: the target ESource
 *
 * Commit the given ESource.
 */
void
gcal_manager_save_source (GcalManager *manager,
                          ESource     *source)
{
  GError *error = NULL;

  GCAL_ENTRY;

  e_source_registry_commit_source_sync (manager->source_registry, source, NULL, &error);

  if (error)
    {
      /* FIXME: Notify the user somehow */
      g_warning ("Error saving source: %s", error->message);
      g_error_free (error);
    }

  GCAL_EXIT;
}

void
gcal_manager_refresh (GcalManager *manager)
{
  GList *clients;
  GList *l;

  GCAL_ENTRY;

  clients = g_hash_table_get_values (manager->clients);

  /* refresh clients */
  for (l = clients; l != NULL; l = l->next)
    {
      GcalManagerUnit *unit = l->data;

      if (!unit->connected || ! e_client_check_refresh_supported (E_CLIENT (unit->client)))
        continue;

      e_client_refresh (E_CLIENT (unit->client),
                        NULL,
                        on_client_refreshed,
                        manager);
    }

  g_list_free (clients);

  GCAL_EXIT;
}

gboolean
gcal_manager_is_client_writable (GcalManager *manager,
                                 ESource     *source)
{
  GcalManagerUnit *unit;

  GCAL_ENTRY;

  unit = g_hash_table_lookup (manager->clients, source);

  if (!unit)
    GCAL_RETURN (FALSE);

  GCAL_RETURN (unit->connected && !e_client_is_readonly (E_CLIENT (unit->client)));
}

void
gcal_manager_create_event (GcalManager        *manager,
                           GcalEvent          *event)
{
  GcalManagerUnit *unit;
  icalcomponent *new_event_icalcomp;
  ECalComponent *component;
  AsyncOpsData *data;
  ESource *source;

  GCAL_ENTRY;

  source = gcal_event_get_source (event);
  component = gcal_event_get_component (event);
  unit = g_hash_table_lookup (manager->clients, source);

  new_event_icalcomp = e_cal_component_get_icalcomponent (component);

  data = g_new0 (AsyncOpsData, 1);
  data->event = g_object_ref (event);
  data->manager = manager;

  e_cal_client_create_object (unit->client,
                              new_event_icalcomp,
                              manager->async_ops,
                              on_event_created,
                              data);

  GCAL_EXIT;
}

void
gcal_manager_update_event (GcalManager *manager,
                           GcalEvent   *event)
{
  GcalManagerUnit *unit;
  ECalComponent *component;

  GCAL_ENTRY;

  unit = g_hash_table_lookup (manager->clients, gcal_event_get_source (event));
  component = gcal_event_get_component (event);

  /*
   * While we're updating the event, we don't want the component
   * to be destroyed, so take a reference of the component while
   * we're performing the update on it.
   */
  g_object_ref (component);

  e_cal_client_modify_object (unit->client,
                              e_cal_component_get_icalcomponent (component),
                              E_CAL_OBJ_MOD_THIS,
                              NULL,
                              on_event_updated,
                              component);

  GCAL_EXIT;
}

void
gcal_manager_remove_event (GcalManager   *manager,
                           GcalEvent     *event)
{
  GcalManagerUnit *unit;
  ECalComponentId *id;
  ECalComponent *component;

  GCAL_ENTRY;

  component = gcal_event_get_component (event);
  unit = g_hash_table_lookup (manager->clients, gcal_event_get_source (event));
  id = e_cal_component_get_id (component);

  /*
   * While we're removing the event, we don't want the component
   * to be destroyed, so take a reference of the component while
   * we're deleting it.
   */
  g_object_ref (component);

  e_cal_client_remove_object (unit->client,
                              id->uid,
                              id->rid,
                              E_CAL_OBJ_MOD_THIS,
                              manager->async_ops,
                              on_event_removed,
                              component);

  e_cal_component_free_id (id);

  GCAL_EXIT;
}

void
gcal_manager_move_event_to_source (GcalManager *manager,
                                   GcalEvent   *event,
                                   ESource     *dest)
{
  ECalComponent *ecomponent;
  ECalComponent *clone;
  icalcomponent *comp;
  GcalManagerUnit *unit;
  ECalComponentId *id;
  GError *error;

  GCAL_ENTRY;

  g_return_if_fail (GCAL_IS_MANAGER (manager));

  error = NULL;

  /* First, try to create the component on the destination source */
  unit = g_hash_table_lookup (manager->clients, dest);

  ecomponent = gcal_event_get_component (event);
  clone = e_cal_component_clone (ecomponent);
  comp = e_cal_component_get_icalcomponent (clone);

  e_cal_client_create_object_sync (unit->client,
                                   comp,
                                   NULL,
                                   NULL,
                                   &error);

  if (error)
    {
      g_warning ("Error moving source: %s", error->message);
      g_clear_error (&error);
      GCAL_EXIT;
      return;
    }

  /*
   * After we carefully confirm that a clone of the component was
   * created, try to remove the old component. Data loss it the last
   * thing we want to happen here.
   */
  unit = g_hash_table_lookup (manager->clients, gcal_event_get_source (event));

  id = e_cal_component_get_id (ecomponent);

  e_cal_client_remove_object_sync (unit->client,
                                   id->uid,
                                   id->rid,
                                   E_CAL_OBJ_MOD_THIS,
                                   manager->async_ops,
                                   &error);

  if (error)
    {
      g_warning ("Error moving source: %s", error->message);
      g_clear_error (&error);
      GCAL_EXIT;
      return;
    }

  e_cal_component_free_id (id);

  GCAL_EXIT;
}

/**
 * gcal_manager_get_events:
 *
 * Returns a list with {@link GcalEvent} objects owned by the caller, the list and the objects.
 * The components inside the list are owned by the caller as well. So, don't ref the component.
 *
 * Returns: An {@link GList} object
 */
GList*
gcal_manager_get_events (GcalManager  *manager,
                         icaltimetype *start_date,
                         icaltimetype *end_date)
{
  time_t range_start, range_end;
  GList *list = NULL;

  GCAL_ENTRY;

  range_start = icaltime_as_timet_with_zone (*start_date, manager->system_timezone);
  range_end = icaltime_as_timet_with_zone (*end_date, manager->system_timezone);

  e_cal_data_model_foreach_component (manager->e_data_model, range_start, range_end, gather_events, &list);

  GCAL_RETURN (list);
}

gboolean
gcal_manager_get_loading (GcalManager *self)
{
  g_return_val_if_fail (GCAL_IS_MANAGER (self), FALSE);

  return !self->goa_client_ready || self->sources_at_launch > 0;
}

GcalEvent*
gcal_manager_get_event_from_shell_search (GcalManager *manager,
                                          const gchar *uuid)
{
  GcalEvent *new_event;
  GList *l, *list;
  time_t range_start, range_end;

  GCAL_ENTRY;

  list = NULL;
  new_event = NULL;

  e_cal_data_model_get_subscriber_range (manager->shell_search_data_model, manager->search_view_data->subscriber,
                                         &range_start, &range_end);
  e_cal_data_model_foreach_component (manager->shell_search_data_model, range_start, range_end, gather_events, &list);

  for (l = list; l != NULL; l = g_list_next (l))
    {
      GcalEvent *event;

      event = l->data;

      /* Found the event */
      if (g_strcmp0 (gcal_event_get_uid (event), uuid) == 0)
        new_event = event;
      else
        g_object_unref (event);
    }

  g_list_free (list);

  GCAL_RETURN (new_event);
}

GoaClient*
gcal_manager_get_goa_client (GcalManager *manager)
{
  g_return_val_if_fail (GCAL_IS_MANAGER (manager), FALSE);

  GCAL_ENTRY;

  GCAL_RETURN (manager->goa_client);
}
