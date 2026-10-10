#pragma once

#include <glib.h>

typedef struct {
    char *label;
    char *package;
    char *version_name;
    gint64 version_code;
    char *apk_name;
    char *installed; /* ISO-8601-ish timestamp string */
    char *game_dir;
    char *data_dir;
} Version;

/* Load all installed versions (metadata is stored under <base>).
 * Never fails; a missing/corrupt file yields an empty list. */
GList *versions_load(const char *base_dir);

/* Persist the whole list. Call after any change. */
gboolean versions_save(const char *base_dir, GList *versions, GError **error);

void version_free(Version *v);

/* True if any entry has the same package/version-name/version-code. */
gboolean versions_contain(const GList *versions, const char *package,
                          const char *version_name, gint64 version_code);

/* Find the entry described by the given APK info, if already installed. */
Version *versions_find(const GList *versions, const char *package,
                       const char *version_name, gint64 version_code);

/* Make a filesystem-safe version of a label (used for directory names). */
char *version_dir_safe(const char *label);