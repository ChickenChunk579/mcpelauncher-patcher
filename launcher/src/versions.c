#include "versions.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <string.h>

static char *metadata_path(const char *base_dir)
{
    return g_build_filename(base_dir, "installed.conf", NULL);
}

void version_free(Version *v)
{
    if (!v)
        return;
    g_free(v->label);
    g_free(v->package);
    g_free(v->version_name);
    g_free(v->apk_name);
    g_free(v->installed);
    g_free(v->game_dir);
    g_free(v->data_dir);
    g_free(v);
}

static gint version_compare_by_label(gconstpointer a, gconstpointer b)
{
    return g_strcmp0(((const Version *)a)->label, ((const Version *)b)->label);
}

static Version *version_from_group(GKeyFile *kf, const char *group)
{
    Version *v = g_new0(Version, 1);
    v->label = g_strdup(group);
    v->package = g_key_file_get_string(kf, group, "package", NULL);
    v->version_name = g_key_file_get_string(kf, group, "version-name", NULL);
    v->apk_name = g_key_file_get_string(kf, group, "apk-name", NULL);
    v->installed = g_key_file_get_string(kf, group, "installed", NULL);
    gchar *code = g_key_file_get_string(kf, group, "version-code", NULL);
    if (code) {
        v->version_code = g_ascii_strtoll(code, NULL, 10);
        g_free(code);
    }
    v->game_dir = g_key_file_get_string(kf, group, "game-dir", NULL);
    v->data_dir = g_key_file_get_string(kf, group, "data-dir", NULL);
    return v;
}

GList *versions_load(const char *base_dir)
{
    gchar *path = metadata_path(base_dir);
    GKeyFile *kf = g_key_file_new();
    GError *err = NULL;
    if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, &err)) {
        if (err && !g_error_matches(err, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
            g_warning("Failed to read %s: %s", path, err->message);
        }
        g_clear_error(&err);
        g_key_file_free(kf);
        g_free(path);
        return NULL;
    }

    GList *list = NULL;
    gsize n = 0;
    gchar **groups = g_key_file_get_groups(kf, &n);
    for (gsize i = 0; i < n; i++) {
        Version *v = version_from_group(kf, groups[i]);
        if (v->label && v->game_dir && *v->game_dir)
            list = g_list_prepend(list, v);
        else
            version_free(v);
    }
    g_strfreev(groups);
    g_key_file_free(kf);
    g_free(path);
    /* Stable order by label. */
    return g_list_sort(list, version_compare_by_label);
}

gboolean versions_save(const char *base_dir, GList *versions, GError **error)
{
    gchar *path = metadata_path(base_dir);
    gchar *dir = g_path_get_dirname(path);
    if (g_mkdir_with_parents(dir, 0755) != 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "Cannot create %s", dir);
        g_free(dir);
        g_free(path);
        return FALSE;
    }
    g_free(dir);

    GKeyFile *kf = g_key_file_new();
    for (GList *it = versions; it; it = it->next) {
        Version *v = it->data;
        g_key_file_set_string(kf, v->label, "package",
                              v->package ? v->package : "");
        g_key_file_set_string(kf, v->label, "version-name",
                              v->version_name ? v->version_name : "");
        if (v->version_code >= 0) {
            gchar *code = g_strdup_printf("%" G_GINT64_FORMAT, v->version_code);
            g_key_file_set_string(kf, v->label, "version-code", code);
            g_free(code);
        }
        g_key_file_set_string(kf, v->label, "apk-name",
                              v->apk_name ? v->apk_name : "");
        g_key_file_set_string(kf, v->label, "installed",
                              v->installed ? v->installed : "");
        g_key_file_set_string(kf, v->label, "game-dir",
                              v->game_dir ? v->game_dir : "");
        g_key_file_set_string(kf, v->label, "data-dir",
                              v->data_dir ? v->data_dir : "");
    }

    gchar *contents = g_key_file_to_data(kf, NULL, NULL);
    g_key_file_free(kf);
    gboolean ok = g_file_set_contents(path, contents, -1, error);
    g_free(contents);
    g_free(path);
    return ok;
}

gboolean version_same(const Version *v, const char *package,
                      const char *version_name, gint64 version_code)
{
    if (!v || g_strcmp0(package, v->package) != 0)
        return FALSE;
    if (g_strcmp0(version_name, v->version_name) != 0)
        return FALSE;
    if (v->version_code != version_code)
        return FALSE;
    return TRUE;
}

GList *versions_contain_impl(const GList *versions, const char *package,
                             const char *version_name, gint64 version_code)
{
    for (const GList *it = versions; it; it = it->next) {
        if (version_same(it->data, package, version_name, version_code))
            return (GList *)it;
    }
    return NULL;
}

gboolean versions_contain(const GList *versions, const char *package,
                          const char *version_name, gint64 version_code)
{
    return versions_contain_impl(versions, package, version_name,
                                 version_code) != NULL;
}

Version *versions_find(const GList *versions, const char *package,
                       const char *version_name, gint64 version_code)
{
    GList *hit = versions_contain_impl(versions, package, version_name,
                                       version_code);
    return hit ? hit->data : NULL;
}

char *version_dir_safe(const char *label)
{
    GString *out = g_string_sized_new(strlen(label) + 1);
    for (const gchar *p = label; *p; p++) {
        guchar c = (guchar)*p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')
            g_string_append_c(out, c);
        else if (c == '/' || c == '\\' || c == 0)
            g_string_append_c(out, '_');
        else
            g_string_append_c(out, '_');
    }
    if (out->len == 0)
        g_string_append(out, "version");
    return g_string_free(out, FALSE);
}