#pragma once

#include <glib.h>

typedef struct {
    char *label;                  /* display name, usually the version string */
    char *package;                /* parsed from AndroidManifest.xml */
    char *version_name;           /* e.g. 1.26.52.3 */
    guint32 version_code;
    gboolean has_x86_64;          /* lib/x86_64/ present */
    gboolean has_libminecraftpe;  /* lib/x86_64/libminecraftpe.so present */
    guint64 uncompressed_size;    /* total uncompressed payload, bytes */
} ApkInfo;

/* Inspect an APK. Fails with APK_ERROR when it is clearly not a Minecraft
 * APK (missing/malformed manifest, or a package that is not
 * com.mojang.minecraftpe). Compatibility flags are always filled in. */
gboolean apk_inspect(const char *path, ApkInfo *out, GError **error);

void apk_info_free(ApkInfo *info);

GQuark apk_error_quark(void);

#define APK_ERROR (apk_error_quark())