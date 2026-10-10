#include "launch.h"

#include "versions.h"

#include <stdlib.h>
#include <string.h>

void launch_config_free(LaunchConfig *cfg)
{
    if (!cfg)
        return;
    g_free(cfg->client_path);
    g_free(cfg->extra_args);
    memset(cfg, 0, sizeof(*cfg));
}

static gboolean is_executable_file(const char *path)
{
    if (!path || !*path)
        return FALSE;
    return g_file_test(path, G_FILE_TEST_IS_EXECUTABLE);
}

char *launch_find_client(const LaunchConfig *cfg)
{
    if (cfg && cfg->client_path && *cfg->client_path)
        if (is_executable_file(cfg->client_path))
            return g_strdup(cfg->client_path);

    const char *env = g_getenv("MCPELAUNCHER_CLIENT");
    if (env && *env && is_executable_file(env))
        return g_strdup(env);

    gchar *local = g_build_filename(g_get_home_dir(), ".local", "bin",
                                    "mcpelauncher-client", NULL);
    if (is_executable_file(local)) {
        gchar *real = g_canonicalize_filename(local, NULL);
        g_free(local);
        return real ? real : g_strdup(local);
    }
    g_free(local);

    char *found = g_find_program_in_path("mcpelauncher-client");
    return found;
}

GSubprocess *launch_version(const LaunchConfig *cfg, const Version *v,
                            const char *log_dir, GError **error)
{
    if (!cfg->client_path || !*cfg->client_path) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOENT,
                    "mcpelauncher-client binary was not found. Run patch.sh to "
                    "build it, or set its path in Settings.");
        return NULL;
    }

    gchar *safe = version_dir_safe(v->label);
    gchar *log_name = g_strdup_printf("%s.log", safe);
    gchar *log = g_build_filename(log_dir, log_name, NULL);
    g_free(log_name);
    g_free(safe);
    g_mkdir_with_parents(log_dir, 0755);

    GPtrArray *argv = g_ptr_array_new();
    g_ptr_array_add(argv, (gpointer)cfg->client_path);
    g_ptr_array_add(argv, (gpointer)"-dg");
    g_ptr_array_add(argv, (gpointer)v->game_dir);
    g_ptr_array_add(argv, (gpointer)"-dd");
    g_ptr_array_add(argv, (gpointer)v->data_dir);

    gchar **extra = NULL;
    int extra_n = 0;
    GError *perr = NULL;
    if (cfg->extra_args && *cfg->extra_args) {
        if (!g_shell_parse_argv(cfg->extra_args, &extra_n, &extra, &perr)) {
            g_ptr_array_free(argv, TRUE);
            g_free(log);
            g_propagate_error(error, perr);
            return NULL;
        }
        for (int i = 0; i < extra_n; i++)
            g_ptr_array_add(argv, (gpointer)extra[i]);
        /* Free the array of pointers only; the strings stay owned by argv. */
        g_free(extra);
    }
    g_ptr_array_add(argv, NULL);

    GSubprocessLauncher *sl = g_subprocess_launcher_new(0);
    g_subprocess_launcher_set_stdout_file_path(sl, log);
    g_subprocess_launcher_set_stderr_file_path(sl, log);

    GSubprocess *proc = g_subprocess_launcher_spawnv(
        sl, (const gchar *const *)argv->pdata, error);

    g_object_unref(sl);
    g_ptr_array_free(argv, TRUE);
    g_free(log);
    return proc;
}