#pragma once

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "versions.h"

typedef struct {
    char *client_path; /* empty = auto-detect */
    char *extra_args;  /* extra CLI flags passed to mcpelauncher-client */
} LaunchConfig;

void launch_config_free(LaunchConfig *cfg);

/* Find the mcpelauncher-client binary. Preference order: config path, the
 * MCPELAUNCHER_CLIENT env var, ~/.local/bin, then $PATH. Returns an
 * allocated string or NULL. */
char *launch_find_client(const LaunchConfig *cfg);

/* Spawn mcpelauncher-client with -dg gamedir -dd datadir. Output is logged
 * to <log_dir>/<label>.log. Caller owns the returned GSubprocess. */
GSubprocess *launch_version(const LaunchConfig *cfg, const Version *v,
                            const char *log_dir, GError **error);