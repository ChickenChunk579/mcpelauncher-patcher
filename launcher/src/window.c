#include "window.h"

#include <errno.h>
#include <signal.h>
#include <string.h>

#include <glib/gstdio.h>

#include "apk.h"
#include "launch.h"
#include "versions.h"
#include "zip.h"

#define RESOURCE_PREFIX "/dev/rhys/mcpe-launcher"

typedef struct _AppContext AppContext;
typedef struct _RowState RowState;

/* ------------------------------------------------------------------ */
/* Row state: one per installed version, keeps the row widgets and a   */
/* handle to the running game process (if any).                        */
/* ------------------------------------------------------------------ */
struct _RowState {
    AppContext *ctx;
    Version *version;
    GtkWidget *row;
    GtkButton *launch_btn;
    GtkLabel *sub_label;
    GSubprocess *proc;
    guint force_timeout;
};

/* ------------------------------------------------------------------ */
/* Install job: extraction runs on a worker thread and reports back    */
/* through a mutex-protected state struct.                             */
/* ------------------------------------------------------------------ */
typedef struct {
    AppContext *ctx;
    GMutex mutex;
    GCancellable *cancel;

    char *apk_path;
    char *apk_name;
    char *label;
    char *package;
    char *version_name;
    guint32 version_code;
    Version *replace; /* existing version being replaced, or NULL */

    char *game_dir; /* final destination */
    char *data_dir;
    char *tmp_dir;

    gboolean thread_done;
    gboolean success;
    gboolean cancelled;
    gchar error[1024];
    guint done_files;
    guint total_files;
    char current[256];

    GtkWindow *dialog;
    GtkProgressBar *bar;
    GtkLabel *status_label;
    GtkLabel *count_label;
    GtkButton *cancel_btn;
    guint timer;
} InstallJob;

typedef struct {
    AppContext *ctx;
    Version *version;
} ProcWatcher;

/* ------------------------------------------------------------------ */
struct _AppContext {
    AdwApplication *app;
    AdwApplicationWindow *win;
    GtkListBox *list;
    GtkStack *main_stack;
    AdwToastOverlay *toast_overlay;
    GtkButton *add_btn;
    GtkButton *empty_add_btn;
    GtkWidget *settings_client_entry;

    char *base_dir;
    char *log_dir;
    GList *versions;            /* owned Version* */
    GHashTable *rows;           /* Version* -> RowState* */
    LaunchConfig cfg;

    InstallJob *job;            /* active install, or NULL */
};

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static void toast(AppContext *ctx, const char *message)
{
    AdwToast *t = adw_toast_new(message);
    adw_toast_set_timeout(t, 3);
    adw_toast_overlay_add_toast(ctx->toast_overlay, t);
}

/* Dialogs created with adw_*_dialog_new() return a floating reference that
 * adw_dialog_present() takes ownership of. The dialog is destroyed when it
 * closes, so we must NOT unreference it ourselves. */

static void on_simple_response(AdwAlertDialog *dlg, const char *response,
                               gpointer user_data)
{
    (void)response;
    (void)user_data;
    adw_dialog_close(ADW_DIALOG(dlg));
}

static void show_error_dialog(AppContext *ctx, const char *heading,
                              const char *body)
{
    AdwAlertDialog *dlg = ADW_ALERT_DIALOG(adw_alert_dialog_new(heading, body));
    adw_alert_dialog_add_responses(dlg, "ok", "OK", NULL);
    adw_alert_dialog_set_default_response(dlg, "ok");
    g_signal_connect(dlg, "response", G_CALLBACK(on_simple_response), NULL);
    adw_dialog_present(ADW_DIALOG(dlg), GTK_WIDGET(ctx->win));
}

static char *now_str(void)
{
    GDateTime *dt = g_date_time_new_now_local();
    char *s = g_date_time_format(dt, "%Y-%m-%d %H:%M:%S");
    g_date_time_unref(dt);
    return s;
}

static void rmtree(gchar *path)
{
    if (!path)
        return;
    GFile *f = g_file_new_for_path(path);
    GError *err = NULL;
    GFileEnumerator *en = g_file_enumerate_children(
        f, G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NONE, NULL, &err);
    if (en) {
        GFileInfo *info;
        while ((info = g_file_enumerator_next_file(en, NULL, NULL)) != NULL) {
            gchar *child = g_build_filename(path, g_file_info_get_name(info), NULL);
            rmtree(child);
            g_free(child);
            g_object_unref(info);
        }
        g_object_unref(en);
    }
    g_clear_error(&err);
    g_file_delete(f, NULL, NULL);
    g_object_unref(f);
    g_free(path);
}

static char *version_subtitle(const Version *v)
{
    return g_strdup_printf("%s\nAPK %s · installed %s",
                           v->package ? v->package : "com.mojang.minecraftpe",
                           v->apk_name ? v->apk_name : "unknown",
                           v->installed ? v->installed : "unknown");
}

/* ------------------------------------------------------------------ */
/* Settings                                                            */
/* ------------------------------------------------------------------ */

static char *settings_path(void)
{
    gchar *dir = g_build_filename(g_get_user_config_dir(), "mcpe-launcher", NULL);
    gchar *path = g_build_filename(dir, "settings.conf", NULL);
    g_free(dir);
    return path;
}

static void save_settings(AppContext *ctx)
{
    gchar *path = settings_path();
    gchar *dir = g_path_get_dirname(path);
    if (g_mkdir_with_parents(dir, 0700) != 0) {
        g_free(dir);
        g_free(path);
        return;
    }
    g_free(dir);

    GKeyFile *kf = g_key_file_new();
    g_key_file_set_string(kf, "launcher", "client-path",
                          ctx->cfg.client_path ? ctx->cfg.client_path : "");
    g_key_file_set_string(kf, "launcher", "extra-args",
                          ctx->cfg.extra_args ? ctx->cfg.extra_args : "");
    gchar *data = g_key_file_to_data(kf, NULL, NULL);
    g_key_file_free(kf);
    g_file_set_contents(path, data, -1, NULL);
    g_free(data);
    g_free(path);
}

static void load_settings(AppContext *ctx)
{
    gchar *path = settings_path();
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        ctx->cfg.client_path =
            g_key_file_get_string(kf, "launcher", "client-path", NULL);
        ctx->cfg.extra_args =
            g_key_file_get_string(kf, "launcher", "extra-args", NULL);
    }
    g_key_file_free(kf);
    g_free(path);
}

/* ------------------------------------------------------------------ */
/* Row UI                                                              */
/* ------------------------------------------------------------------ */

static void update_row_ui(RowState *st)
{
    if (!st->launch_btn)
        return;
    if (st->proc) {
        gtk_button_set_icon_name(st->launch_btn, "process-stop-symbolic");
        gtk_widget_set_sensitive(GTK_WIDGET(st->launch_btn), TRUE);
        gtk_widget_add_css_class(GTK_WIDGET(st->launch_btn), "destructive-action");
        gtk_widget_remove_css_class(GTK_WIDGET(st->launch_btn),
                                    "suggested-action");
        gtk_widget_set_tooltip_text(GTK_WIDGET(st->launch_btn),
                                    "Stop the running game");
    } else {
        gtk_button_set_icon_name(st->launch_btn, "media-playback-start-symbolic");
        gtk_widget_set_sensitive(GTK_WIDGET(st->launch_btn), TRUE);
        gtk_widget_add_css_class(GTK_WIDGET(st->launch_btn), "suggested-action");
        gtk_widget_remove_css_class(GTK_WIDGET(st->launch_btn),
                                    "destructive-action");
        gtk_widget_set_tooltip_text(GTK_WIDGET(st->launch_btn),
                                    "Launch this version");
    }
}

static void on_launch_clicked(GtkButton *btn, AppContext *ctx);
static void on_delete_clicked(GtkButton *btn, AppContext *ctx);

static void row_build_widgets(AppContext *ctx, RowState *st)
{
    GtkListBoxRow *row = GTK_LIST_BOX_ROW(gtk_list_box_row_new());

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(box, "version-row");

    GtkWidget *avatar =
        adw_avatar_new(48, st->version->label ? st->version->label : "M", TRUE);
    gtk_widget_set_valign(GTK_WIDGET(avatar), GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(avatar));

    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_hexpand(col, TRUE);
    gtk_widget_set_valign(col, GTK_ALIGN_CENTER);

    GtkWidget *title = gtk_label_new(st->version->label);
    gtk_widget_add_css_class(title, "version-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_box_append(GTK_BOX(col), title);

    char *sub = version_subtitle(st->version);
    GtkWidget *sub_l = gtk_label_new(sub);
    g_free(sub);
    gtk_widget_add_css_class(sub_l, "version-subtitle");
    gtk_label_set_xalign(GTK_LABEL(sub_l), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(sub_l), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(sub_l), PANGO_WRAP_WORD_CHAR);
    st->sub_label = GTK_LABEL(sub_l);
    gtk_box_append(GTK_BOX(col), sub_l);

    gtk_box_append(GTK_BOX(box), col);

    st->launch_btn = GTK_BUTTON(gtk_button_new_from_icon_name(
        "media-playback-start-symbolic"));
    gtk_widget_set_valign(GTK_WIDGET(st->launch_btn), GTK_ALIGN_CENTER);
    g_object_set_data(G_OBJECT(st->launch_btn), "version", st->version);
    g_signal_connect(st->launch_btn, "clicked", G_CALLBACK(on_launch_clicked), ctx);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(st->launch_btn));

    GtkWidget *del = gtk_button_new_from_icon_name("user-trash-symbolic");
    gtk_widget_add_css_class(del, "flat");
    gtk_widget_set_valign(del, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(del, "Remove this version");
    g_object_set_data(G_OBJECT(del), "version", st->version);
    g_signal_connect(del, "clicked", G_CALLBACK(on_delete_clicked), ctx);
    gtk_box_append(GTK_BOX(box), del);

    gtk_list_box_row_set_child(row, box);
    gtk_list_box_append(ctx->list, GTK_WIDGET(row));
    st->row = GTK_WIDGET(row);

    update_row_ui(st);
}

static void row_state_free(gpointer data)
{
    RowState *st = data;
    if (!st)
        return;
    if (st->proc)
        g_object_unref(st->proc);
    if (st->force_timeout)
        g_source_remove(st->force_timeout);
    g_free(st);
}

static RowState *row_state_new(AppContext *ctx, Version *v)
{
    RowState *st = g_new0(RowState, 1);
    st->ctx = ctx;
    st->version = v;
    row_build_widgets(ctx, st);
    return st;
}

/* Rebuild the version list from ctx->versions, preserving running
 * processes for rows that are still present. */
static void refresh(AppContext *ctx)
{
    GHashTable *old = ctx->rows;
    GHashTable *new = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                            NULL, row_state_free);

    gtk_list_box_remove_all(ctx->list);
    for (GList *it = ctx->versions; it; it = it->next) {
        Version *v = it->data;
        RowState *st = NULL;
        if (old) {
            st = g_hash_table_lookup(old, v);
            if (st) {
                g_hash_table_steal(old, v);
                row_build_widgets(ctx, st);
            }
        }
        if (!st)
            st = row_state_new(ctx, v);
        g_hash_table_insert(new, v, st);
    }
    if (old)
        g_hash_table_destroy(old);
    ctx->rows = new;

    gtk_stack_set_visible_child_name(ctx->main_stack,
                                     ctx->versions ? "list" : "empty");
}

/* ------------------------------------------------------------------ */
/* Process handling                                                    */
/* ------------------------------------------------------------------ */

static void on_proc_exit(GObject *source, GAsyncResult *res, gpointer user_data)
{
    GSubprocess *proc = G_SUBPROCESS(source);
    ProcWatcher *w = user_data;
    g_subprocess_wait_finish(proc, res, NULL);
    g_object_unref(proc);

    RowState *st = g_hash_table_lookup(w->ctx->rows, w->version);
    if (st && st->proc == proc) {
        st->proc = NULL;
        if (st->force_timeout) {
            g_source_remove(st->force_timeout);
            st->force_timeout = 0;
        }
        update_row_ui(st);
        toast(w->ctx, "Minecraft has exited");
    }
    g_free(w);
}

static gboolean force_stop_cb(gpointer user_data)
{
    RowState *st = user_data;
    st->force_timeout = 0;
    if (st->proc)
        g_subprocess_force_exit(st->proc);
    return G_SOURCE_REMOVE;
}

static void request_stop(AppContext *ctx, RowState *st)
{
    (void)ctx;
    if (!st->proc)
        return;
    g_subprocess_send_signal(st->proc, SIGTERM);
    gtk_button_set_icon_name(st->launch_btn, "media-playback-stop-symbolic");
    gtk_widget_set_sensitive(GTK_WIDGET(st->launch_btn), FALSE);
    gtk_widget_set_tooltip_text(GTK_WIDGET(st->launch_btn), "Stopping…");
    if (!st->force_timeout)
        st->force_timeout = g_timeout_add_seconds(8, force_stop_cb, st);
}

static void on_launch_clicked(GtkButton *btn, AppContext *ctx)
{
    Version *v = g_object_get_data(G_OBJECT(btn), "version");
    RowState *st = g_hash_table_lookup(ctx->rows, v);
    if (!st)
        return;

    if (st->proc) {
        request_stop(ctx, st);
        return;
    }

    if (!g_file_test(v->game_dir, G_FILE_TEST_IS_DIR)) {
        show_error_dialog(ctx, "Missing game files",
                          "The extracted game directory is gone. Remove this "
                          "version and reinstall the APK.");
        return;
    }

    char *client = launch_find_client(&ctx->cfg);
    if (!client) {
        show_error_dialog(
            ctx, "mcpelauncher-client not found",
            "Could not find the mcpelauncher-client binary. Build and install "
            "it with patch.sh, or set its path in the Settings menu.");
        return;
    }
    g_free(ctx->cfg.client_path);
    ctx->cfg.client_path = client;

    GError *err = NULL;
    GSubprocess *proc = launch_version(&ctx->cfg, v, ctx->log_dir, &err);
    if (!proc) {
        show_error_dialog(ctx, "Failed to launch Minecraft",
                          err ? err->message : "Unknown error");
        g_clear_error(&err);
        return;
    }

    st->proc = proc;
    update_row_ui(st);

    ProcWatcher *w = g_new0(ProcWatcher, 1);
    w->ctx = ctx;
    w->version = v;
    g_subprocess_wait_async(proc, NULL, on_proc_exit, w);
}

/* ------------------------------------------------------------------ */
/* Remove version                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    AppContext *ctx;
    Version *version;
} RemoveData;

static void remove_data_destroy(gpointer data, GClosure *closure)
{
    (void)closure;
    g_free(data);
}

static void on_remove_response(AdwAlertDialog *dlg, const char *response,
                               gpointer user_data)
{
    RemoveData *d = user_data;
    if (strcmp(response, "remove") == 0) {
        d->ctx->versions = g_list_remove(d->ctx->versions, d->version);
        GError *err = NULL;
        if (!versions_save(d->ctx->base_dir, d->ctx->versions, &err)) {
            g_warning("Failed to save versions: %s", err ? err->message : "?");
            g_clear_error(&err);
        }
        rmtree(g_strdup(d->version->game_dir));
        rmtree(g_strdup(d->version->data_dir));
        version_free(d->version);
        refresh(d->ctx);
        toast(d->ctx, "Version removed");
    }
    adw_dialog_close(ADW_DIALOG(dlg));
}

static void on_delete_clicked(GtkButton *btn, AppContext *ctx)
{
    Version *v = g_object_get_data(G_OBJECT(btn), "version");
    RowState *st = g_hash_table_lookup(ctx->rows, v);
    if (st && st->proc) {
        toast(ctx, "Stop the running game before removing this version");
        return;
    }

    gchar *body = g_strdup_printf(
        "This will delete the extracted game files and the world data for "
        "%s. This cannot be undone.", st->version->label);
    AdwAlertDialog *dlg =
        ADW_ALERT_DIALOG(adw_alert_dialog_new("Remove Minecraft version?", body));
    g_free(body);

    adw_alert_dialog_add_responses(dlg, "cancel", "Cancel", "remove", "Remove",
                                   NULL);
    adw_alert_dialog_set_response_appearance(dlg, "remove",
                                             ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(dlg, "cancel");

    RemoveData *d = g_new0(RemoveData, 1);
    d->ctx = ctx;
    d->version = v;
    g_signal_connect_data(dlg, "response", G_CALLBACK(on_remove_response), d,
                          remove_data_destroy, G_CONNECT_DEFAULT);
    adw_dialog_present(ADW_DIALOG(dlg), GTK_WIDGET(ctx->win));
}

/* ------------------------------------------------------------------ */
/* Installation: validation, confirm, extract, finalise                */
/* ------------------------------------------------------------------ */

typedef struct {
    AppContext *ctx;
    char *path;
    char *label;
    char *package;
    char *version_name;
    guint32 version_code;
    Version *replace; /* or NULL for a fresh install */
    char *game_dir;
    char *data_dir;
} ConfirmData;

static void confirm_data_destroy(gpointer data, GClosure *closure)
{
    (void)closure;
    ConfirmData *d = data;
    g_free(d->path);
    g_free(d->label);
    g_free(d->package);
    g_free(d->version_name);
    g_free(d->game_dir);
    g_free(d->data_dir);
    g_free(d);
}

/* Make sure the version directory does not collide with another version. */
static void derive_dirs(AppContext *ctx, ConfirmData *d)
{
    if (d->replace) {
        d->game_dir = g_strdup(d->replace->game_dir);
        d->data_dir = g_strdup(d->replace->data_dir);
        return;
    }
    char *safe = version_dir_safe(d->label);
    gchar *candidate = g_build_filename(ctx->base_dir, "versions", safe, NULL);
    int suffix = 2;
    while (TRUE) {
        gboolean taken = FALSE;
        for (GList *it = ctx->versions; it; it = it->next) {
            Version *v = it->data;
            if (v->game_dir && strcmp(v->game_dir, candidate) == 0) {
                taken = TRUE;
                break;
            }
        }
        if (!taken)
            break;
        g_free(candidate);
        candidate = g_strdup_printf("%s/versions/%s-%d", ctx->base_dir, safe,
                                    suffix++);
    }
    g_free(safe);
    d->game_dir = candidate;
    d->data_dir = g_build_filename(ctx->base_dir, "data",
                                   g_path_get_basename(candidate), NULL);
}

static void begin_install(AppContext *ctx, ConfirmData *d);

static void on_confirm_response(AdwAlertDialog *dlg, const char *response,
                                gpointer user_data)
{
    ConfirmData *d = user_data;
    if (strcmp(response, "install") == 0)
        begin_install(d->ctx, d);
    adw_dialog_close(ADW_DIALOG(dlg));
}

static void confirm_prompt(AppContext *ctx, const char *path,
                           const ApkInfo *info, Version *replace)
{
    ConfirmData *d = g_new0(ConfirmData, 1);
    d->ctx = ctx;
    d->path = g_strdup(path);
    d->label = g_strdup(info->label);
    d->package = g_strdup(info->package);
    d->version_name = g_strdup(info->version_name);
    d->version_code = info->version_code;
    d->replace = replace;
    derive_dirs(ctx, d);

    char *size = g_format_size(info->uncompressed_size);
    gchar *arch =
        info->has_libminecraftpe
            ? g_strdup("lib/x86_64 (libminecraftpe.so found)")
            : g_strdup("lib/x86_64 (libminecraftpe.so missing!)");
    gchar *body = g_strdup_printf(
        "%s\n\nPackage: %s\nVersion: %s (code %u)\nNative: %s\nSize: %s\n\n"
        "It will be installed to:\n%s\n\nWorld data is kept separate per "
        "version in:\n%s",
        replace ? "Replacing the existing installation of this version."
                : "The APK looks valid. Proceed with the installation.",
        d->package, d->version_name, d->version_code, arch, size,
        d->game_dir, d->data_dir);
    g_free(size);
    g_free(arch);

    gchar *heading = g_strdup_printf(
        replace ? "Replace Minecraft %s?" : "Install Minecraft %s?", d->label);

    AdwAlertDialog *dlg =
        ADW_ALERT_DIALOG(adw_alert_dialog_new(heading, body));
    g_free(heading);
    g_free(body);

    adw_alert_dialog_add_responses(dlg, "cancel", "Cancel", "install",
                                   replace ? "Replace" : "Install", NULL);
    adw_alert_dialog_set_response_appearance(
        dlg, "install",
        replace ? ADW_RESPONSE_DESTRUCTIVE : ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(dlg, "cancel");

    g_signal_connect_data(dlg, "response", G_CALLBACK(on_confirm_response), d,
                          confirm_data_destroy, G_CONNECT_DEFAULT);
    adw_dialog_present(ADW_DIALOG(dlg), GTK_WIDGET(ctx->win));
}

static void validate_apk(AppContext *ctx, const char *path)
{
    GError *err = NULL;
    ApkInfo info;
    if (!apk_inspect(path, &info, &err)) {
        show_error_dialog(ctx, "Not a Minecraft APK",
                          err ? err->message : "Unknown error");
        g_clear_error(&err);
        return;
    }
    if (!info.has_x86_64) {
        show_error_dialog(
            ctx, "Unsupported APK",
            "This APK has no lib/x86_64 directory, so mcpelauncher-client "
            "cannot run it. Only x86_64 APKs are supported.");
        apk_info_free(&info);
        return;
    }

    Version *existing = versions_find(ctx->versions, info.package,
                                      info.version_name, info.version_code);
    confirm_prompt(ctx, path, &info, existing);
    apk_info_free(&info);
}

/* ---- extraction thread ---- */

static void install_progress_cb(guint done, guint total, const char *current,
                                gpointer user_data)
{
    InstallJob *job = user_data;
    g_mutex_lock(&job->mutex);
    job->done_files = done;
    job->total_files = total;
    g_strlcpy(job->current, current ? current : "", sizeof(job->current));
    g_mutex_unlock(&job->mutex);
}

static gpointer install_thread(gpointer user_data)
{
    InstallJob *job = user_data;
    GError *err = NULL;
    gboolean ok = zip_extract_all(job->apk_path, job->tmp_dir, job->cancel,
                                  install_progress_cb, job, &err);
    gboolean cancelled =
        err && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED);

    g_mutex_lock(&job->mutex);
    job->success = ok;
    job->cancelled = cancelled;
    if (err)
        g_strlcpy(job->error, err->message, sizeof(job->error));
    job->thread_done = TRUE;
    g_mutex_unlock(&job->mutex);

    g_clear_error(&err);
    return NULL;
}

static void install_finish(InstallJob *job, gboolean success,
                           gboolean cancelled, const gchar *error_msg);

static gboolean progress_poll(gpointer user_data)
{
    InstallJob *job = user_data;
    g_mutex_lock(&job->mutex);
    guint done = job->done_files;
    guint total = job->total_files;
    gboolean thread_done = job->thread_done;
    gboolean success = job->success;
    gboolean cancelled = job->cancelled;
    gchar error[1024];
    g_strlcpy(error, job->error, sizeof(error));
    char current[256];
    g_strlcpy(current, job->current, sizeof(current));
    g_mutex_unlock(&job->mutex);

    if (total > 0) {
        gtk_progress_bar_set_fraction(
            job->bar, MIN(1.0, (double)done / (double)total));
        gchar *txt = g_strdup_printf("%u / %u files", done, total);
        gtk_label_set_text(job->count_label, txt);
        g_free(txt);
    }
    gtk_label_set_text(job->status_label, current);

    if (thread_done) {
        install_finish(job, success, cancelled, error);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void on_cancel_clicked(GtkButton *btn, InstallJob *job)
{
    (void)btn;
    g_cancellable_cancel(job->cancel);
    gtk_widget_set_sensitive(GTK_WIDGET(job->cancel_btn), FALSE);
}

static void build_progress_dialog(InstallJob *job)
{
    AppContext *ctx = job->ctx;
    GtkWindow *dlg = GTK_WINDOW(gtk_window_new());
    gtk_window_set_title(dlg, "Installing Minecraft");
    gtk_window_set_transient_for(dlg, GTK_WINDOW(ctx->win));
    gtk_window_set_modal(dlg, TRUE);
    gtk_window_set_default_size(dlg, 480, -1);
    gtk_window_set_resizable(dlg, FALSE);
    gtk_window_set_destroy_with_parent(dlg, TRUE);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(box, 24);
    gtk_widget_set_margin_bottom(box, 24);
    gtk_widget_set_margin_start(box, 24);
    gtk_widget_set_margin_end(box, 24);

    gchar *title = g_strdup_printf("Installing Minecraft %s", job->label);
    GtkWidget *head = gtk_label_new(title);
    g_free(title);
    gtk_widget_add_css_class(head, "title-1");
    gtk_label_set_xalign(GTK_LABEL(head), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(head), TRUE);
    gtk_box_append(GTK_BOX(box), head);

    GtkWidget *bar = gtk_progress_bar_new();
    gtk_widget_set_hexpand(bar, TRUE);
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(bar), FALSE);
    gtk_box_append(GTK_BOX(box), bar);

    gchar *count = g_strdup_printf("Extracting %s…", job->apk_name);
    GtkWidget *count_l = gtk_label_new(count);
    g_free(count);
    gtk_widget_add_css_class(count_l, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(count_l), 0.0f);
    gtk_box_append(GTK_BOX(box), count_l);

    GtkWidget *status = gtk_label_new("Starting…");
    gtk_widget_add_css_class(status, "install-file-label");
    gtk_label_set_xalign(GTK_LABEL(status), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(status), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_append(GTK_BOX(box), status);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(hbox, GTK_ALIGN_END);
    GtkWidget *cancel = gtk_button_new_with_label("Cancel");
    gtk_box_append(GTK_BOX(hbox), cancel);
    gtk_box_append(GTK_BOX(box), hbox);

    gtk_window_set_child(dlg, box);

    job->dialog = dlg;
    job->bar = GTK_PROGRESS_BAR(bar);
    job->status_label = GTK_LABEL(status);
    job->count_label = GTK_LABEL(count_l);
    job->cancel_btn = GTK_BUTTON(cancel);
    g_signal_connect(cancel, "clicked", G_CALLBACK(on_cancel_clicked), job);

    gtk_window_present(dlg);
}

static void begin_install(AppContext *ctx, ConfirmData *d)
{
    if (ctx->job) {
        toast(ctx, "An installation is already in progress");
        return;
    }

    InstallJob *job = g_new0(InstallJob, 1);
    job->ctx = ctx;
    g_mutex_init(&job->mutex);
    job->cancel = g_cancellable_new();
    job->apk_path = g_strdup(d->path);
    job->apk_name = g_path_get_basename(d->path);
    job->label = g_strdup(d->label);
    job->package = g_strdup(d->package);
    job->version_name = g_strdup(d->version_name);
    job->version_code = d->version_code;
    job->replace = d->replace;
    job->game_dir = g_strdup(d->game_dir);
    job->data_dir = g_strdup(d->data_dir);
    job->tmp_dir = g_strdup_printf("%s.tmp", job->game_dir);

    gchar *versions_root = g_build_filename(ctx->base_dir, "versions", NULL);
    g_mkdir_with_parents(versions_root, 0755);
    g_free(versions_root);
    g_mkdir_with_parents(job->data_dir, 0700);

    gtk_widget_set_sensitive(GTK_WIDGET(ctx->add_btn), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(ctx->empty_add_btn), FALSE);

    ctx->job = job;
    build_progress_dialog(job);

    GThread *th = g_thread_new("mcpe-install", install_thread, job);
    g_thread_unref(th);
    job->timer = g_timeout_add(100, progress_poll, job);
}

static void install_finish(InstallJob *job, gboolean success,
                           gboolean cancelled, const gchar *error_msg)
{
    AppContext *ctx = job->ctx;
    ctx->job = NULL;

    if (job->timer) {
        g_source_remove(job->timer);
        job->timer = 0;
    }
    if (job->dialog)
        gtk_window_destroy(job->dialog);
    gtk_widget_set_sensitive(GTK_WIDGET(ctx->add_btn), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(ctx->empty_add_btn), TRUE);

    if (!success) {
        rmtree(g_strdup(job->tmp_dir));
        if (cancelled) {
            toast(ctx, "Installation cancelled");
        } else {
            show_error_dialog(ctx, "Installation failed",
                              error_msg && *error_msg ? error_msg
                                                      : "Unknown error");
        }
        goto out;
    }

    if (job->replace)
        rmtree(g_strdup(job->game_dir));

    if (g_rename(job->tmp_dir, job->game_dir) != 0) {
        rmtree(g_strdup(job->tmp_dir));
        show_error_dialog(ctx, "Installation failed", g_strerror(errno));
        goto out;
    }

    if (job->replace) {
        Version *v = job->replace;
        g_free(v->apk_name);
        v->apk_name = g_strdup(job->apk_name);
        g_free(v->installed);
        v->installed = now_str();
    } else {
        Version *v = g_new0(Version, 1);
        v->label = g_strdup(job->label);
        v->package = g_strdup(job->package);
        v->version_name = g_strdup(job->version_name);
        v->version_code = job->version_code;
        v->apk_name = g_strdup(job->apk_name);
        v->installed = now_str();
        v->game_dir = g_strdup(job->game_dir);
        v->data_dir = g_strdup(job->data_dir);
        ctx->versions = g_list_append(ctx->versions, v);
    }

    GError *err = NULL;
    if (!versions_save(ctx->base_dir, ctx->versions, &err)) {
        g_warning("Failed to save versions: %s", err ? err->message : "?");
        g_clear_error(&err);
    }
    refresh(ctx);

    gchar *msg = g_strdup_printf("Minecraft %s installed", job->label);
    toast(ctx, msg);
    g_free(msg);

out:
    if (job->cancel)
        g_object_unref(job->cancel);
    g_mutex_clear(&job->mutex);
    g_free(job->apk_path);
    g_free(job->apk_name);
    g_free(job->label);
    g_free(job->package);
    g_free(job->version_name);
    g_free(job->game_dir);
    g_free(job->data_dir);
    g_free(job->tmp_dir);
    g_free(job);
}

/* ------------------------------------------------------------------ */
/* Add APK file dialog                                                 */
/* ------------------------------------------------------------------ */

static void validate_apk(AppContext *ctx, const char *path);

static void on_file_picked(GObject *source, GAsyncResult *res,
                           gpointer user_data)
{
    GtkFileDialog *fd = GTK_FILE_DIALOG(source);
    AppContext *ctx = user_data;
    GError *err = NULL;
    GFile *file = gtk_file_dialog_open_finish(fd, res, &err);
    if (!file) {
        if (err && !g_error_matches(err, GTK_DIALOG_ERROR,
                                    GTK_DIALOG_ERROR_DISMISSED) &&
            !g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            show_error_dialog(ctx, "Could not open file", err->message);
        g_clear_error(&err);
        g_object_unref(fd);
        return;
    }
    g_object_unref(fd);

    char *path = g_file_get_path(file);
    g_object_unref(file);
    if (!path)
        return;
    validate_apk(ctx, path);
    g_free(path);
}

static void on_add_clicked(GtkButton *btn, AppContext *ctx)
{
    (void)btn;
    if (ctx->job) {
        toast(ctx, "An installation is already in progress");
        return;
    }

    GtkFileDialog *fd = gtk_file_dialog_new();
    gtk_file_dialog_set_title(fd, "Select a Minecraft APK");

    GFile *home = g_file_new_for_path(g_get_home_dir());
    GFile *dl = g_file_get_child(home, "Downloads");
    if (g_file_query_exists(dl, NULL))
        gtk_file_dialog_set_initial_folder(fd, dl);
    g_object_unref(home);
    g_object_unref(dl);

    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Android APK (*.apk)");
    gtk_file_filter_add_suffix(filter, "apk");

    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_filters(fd, G_LIST_MODEL(filters));
    gtk_file_dialog_set_default_filter(fd, filter);
    g_object_unref(filters); /* the dialog holds its own reference */

    gtk_file_dialog_open(fd, GTK_WINDOW(ctx->win), NULL, on_file_picked, ctx);
    /* fd is unreferenced in on_file_picked after open_finish */
}

/* ------------------------------------------------------------------ */
/* Settings dialog                                                     */
/* ------------------------------------------------------------------ */

static void on_client_path_changed(GtkEditable *editable, gpointer user_data)
{
    AppContext *ctx = user_data;
    g_free(ctx->cfg.client_path);
    ctx->cfg.client_path = g_strdup(gtk_editable_get_text(editable));
    save_settings(ctx);
}

static void on_extra_args_changed(GtkEditable *editable, gpointer user_data)
{
    AppContext *ctx = user_data;
    g_free(ctx->cfg.extra_args);
    ctx->cfg.extra_args = g_strdup(gtk_editable_get_text(editable));
    save_settings(ctx);
}

static void on_client_reset(GtkButton *btn, AppContext *ctx)
{
    (void)btn;
    if (ctx->settings_client_entry)
        gtk_editable_set_text(GTK_EDITABLE(ctx->settings_client_entry), "");
    /* the "changed" handler above persists the empty path -> auto-detect */
}

static void on_open_data_dir(GtkButton *btn, AppContext *ctx)
{
    (void)btn;
    GError *err = NULL;
    char *uri = g_file_get_uri(g_file_new_for_path(ctx->base_dir));
    if (!g_app_info_launch_default_for_uri(uri, NULL, &err)) {
        show_error_dialog(ctx, "Could not open data folder",
                          err ? err->message : "Unknown error");
        g_clear_error(&err);
    }
    g_free(uri);
}

static void open_settings(AppContext *ctx)
{
    AdwPreferencesDialog *pd =
        ADW_PREFERENCES_DIALOG(adw_preferences_dialog_new());
    /* AdwPreferencesDialog is not a GtkWindow; present with a parent below. */

    GtkWidget *page = adw_preferences_page_new();
    adw_preferences_dialog_add(pd, ADW_PREFERENCES_PAGE(page));

    GtkWidget *g1 = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(g1), "Launcher client");

    GtkWidget *client_row = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(client_row),
                                  "mcpelauncher-client path");
    gtk_editable_set_text(GTK_EDITABLE(client_row),
                          ctx->cfg.client_path ? ctx->cfg.client_path : "");
    ctx->settings_client_entry = client_row;
    g_signal_connect(client_row, "changed",
                     G_CALLBACK(on_client_path_changed), ctx);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g1), client_row);

    GtkWidget *reset = gtk_button_new_from_icon_name("edit-clear-all-symbolic");
    gtk_widget_add_css_class(reset, "flat");
    gtk_widget_set_tooltip_text(reset, "Reset to auto-detection");
    g_signal_connect(reset, "clicked", G_CALLBACK(on_client_reset), ctx);
    adw_entry_row_add_suffix(ADW_ENTRY_ROW(client_row), reset);

    GtkWidget *extra_row = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(extra_row),
                                  "Extra launch arguments");
    gtk_editable_set_text(GTK_EDITABLE(extra_row),
                          ctx->cfg.extra_args ? ctx->cfg.extra_args : "");
    g_signal_connect(extra_row, "changed",
                     G_CALLBACK(on_extra_args_changed), ctx);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g1), extra_row);

    GtkWidget *g2 = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(g2), "Data");

    GtkWidget *dir_row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dir_row),
                                  "Versions folder");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(dir_row), ctx->base_dir);
    GtkWidget *open_btn = gtk_button_new_from_icon_name("folder-open-symbolic");
    gtk_widget_add_css_class(open_btn, "flat");
    g_signal_connect(open_btn, "clicked", G_CALLBACK(on_open_data_dir), ctx);
    adw_action_row_add_suffix(ADW_ACTION_ROW(dir_row), open_btn);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g2), dir_row);

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(g1));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(g2));

    adw_dialog_present(ADW_DIALOG(pd), GTK_WIDGET(ctx->win));
}

/* ------------------------------------------------------------------ */
/* About                                                               */
/* ------------------------------------------------------------------ */

static void on_about_action(GSimpleAction *action, GVariant *param,
                            gpointer user_data)
{
    (void)action;
    (void)param;
    AppContext *ctx = user_data;
    AdwAboutDialog *aw = ADW_ABOUT_DIALOG(adw_about_dialog_new());
    adw_about_dialog_set_application_name(aw, "MCPE Launcher");
    adw_about_dialog_set_application_icon(aw, "dev.rhys.mcpe-launcher");
    adw_about_dialog_set_version(aw, "0.1.0");
    adw_about_dialog_set_comments(
        aw, "Manage and launch Minecraft: Bedrock Edition versions with "
            "mcpelauncher-client");
    adw_about_dialog_set_website(aw,
                                 "https://github.com/rhys/mcpelauncher-patcher");
    adw_about_dialog_set_license_type(aw, GTK_LICENSE_MIT_X11);
    adw_dialog_present(ADW_DIALOG(aw), GTK_WIDGET(ctx->win));
}

static void on_settings_action(GSimpleAction *action, GVariant *param,
                               gpointer user_data)
{
    (void)action;
    (void)param;
    AppContext *ctx = user_data;
    open_settings(ctx);
}

/* ------------------------------------------------------------------ */
/* Bootstrap                                                           */
/* ------------------------------------------------------------------ */

static void build_window(AppContext *ctx)
{
    GtkBuilder *builder =
        gtk_builder_new_from_resource(RESOURCE_PREFIX "/window.ui");
    if (!builder) {
        g_critical("Failed to load UI resource");
        return;
    }

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_resource(css, RESOURCE_PREFIX "/style.css");
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    ctx->win = ADW_APPLICATION_WINDOW(gtk_builder_get_object(builder, "win"));
    g_object_ref_sink(ctx->win);
    gtk_window_set_application(GTK_WINDOW(ctx->win), GTK_APPLICATION(ctx->app));

    ctx->list = GTK_LIST_BOX(gtk_builder_get_object(builder, "version_list"));
    ctx->main_stack = GTK_STACK(gtk_builder_get_object(builder, "main_stack"));
    ctx->toast_overlay =
        ADW_TOAST_OVERLAY(gtk_builder_get_object(builder, "toast_overlay"));
    ctx->add_btn = GTK_BUTTON(gtk_builder_get_object(builder, "add_btn"));
    ctx->empty_add_btn =
        GTK_BUTTON(gtk_builder_get_object(builder, "empty_add_btn"));

    g_signal_connect(ctx->add_btn, "clicked", G_CALLBACK(on_add_clicked), ctx);
    g_signal_connect(ctx->empty_add_btn, "clicked",
                     G_CALLBACK(on_add_clicked), ctx);

    g_object_unref(builder);

    refresh(ctx);
}

static void on_activate(GApplication *app, gpointer user_data)
{
    AppContext *ctx = user_data;
    (void)app;
    if (!ctx->win)
        build_window(ctx);
    gtk_window_present(GTK_WINDOW(ctx->win));
}

static void app_context_free(AppContext *ctx)
{
    if (ctx->rows) {
        GHashTableIter iter;
        gpointer value;
        g_hash_table_iter_init(&iter, ctx->rows);
        while (g_hash_table_iter_next(&iter, NULL, &value)) {
            RowState *st = value;
            if (st->proc)
                g_subprocess_send_signal(st->proc, SIGTERM);
        }
    }
    if (ctx->job)
        g_cancellable_cancel(ctx->job->cancel);
    if (ctx->win)
        g_object_unref(ctx->win);
    if (ctx->rows)
        g_hash_table_destroy(ctx->rows);
    g_list_free_full(ctx->versions, (GDestroyNotify)version_free);
    launch_config_free(&ctx->cfg);
    g_free(ctx->base_dir);
    g_free(ctx->log_dir);
    g_free(ctx);
}

static void on_shutdown(GApplication *app, gpointer user_data)
{
    (void)app;
    app_context_free(user_data);
}

void app_context_attach(AdwApplication *app)
{
    AppContext *ctx = g_new0(AppContext, 1);
    ctx->app = app;
    ctx->base_dir = g_build_filename(g_get_user_data_dir(), "mcpe-launcher", NULL);
    ctx->log_dir = g_build_filename(ctx->base_dir, "logs", NULL);
    load_settings(ctx);
    ctx->versions = versions_load(ctx->base_dir);
    ctx->rows = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                                      row_state_free);

    const GActionEntry actions[] = {
        { .name = "settings", .activate = on_settings_action },
        { .name = "about", .activate = on_about_action },
    };
    g_action_map_add_action_entries(G_ACTION_MAP(app), actions,
                                    G_N_ELEMENTS(actions), ctx);

    g_signal_connect(app, "activate", G_CALLBACK(on_activate), ctx);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), ctx);

    g_object_set_data(G_OBJECT(app), "mcpe-launcher-ctx", ctx);
}