#include "apk.h"
#include "axml.h"
#include "zip.h"

#include <string.h>

#define MCPE_PACKAGE "com.mojang.minecraftpe"

GQuark apk_error_quark(void)
{
    return g_quark_from_static_string("mcpe-launcher-apk-error");
}

void apk_info_free(ApkInfo *info)
{
    if (!info)
        return;
    g_free(info->label);
    g_free(info->package);
    g_free(info->version_name);
    memset(info, 0, sizeof(*info));
}

/* True when the entry is a file under the x86_64 native lib directory. */
static gboolean under_x86_64(const char *name)
{
    static const gsize prefix = sizeof("lib/x86_64/") - 1;
    return g_str_has_prefix(name, "lib/x86_64/") && name[prefix] != '\0';
}

gboolean apk_inspect(const char *path, ApkInfo *out, GError **error)
{
    memset(out, 0, sizeof(*out));

    guint n = 0;
    ZipEntry *entries = zip_read_entries(path, &n, error);
    if (!entries)
        return FALSE;

    ZipEntry manifest_entry;
    gboolean have_manifest = FALSE;
    guint64 total = 0;

    for (guint i = 0; i < n; i++) {
        const char *name = entries[i].name;
        if (strcmp(name, "AndroidManifest.xml") == 0) {
            manifest_entry = entries[i];
            have_manifest = TRUE;
        }
        if (under_x86_64(name)) {
            out->has_x86_64 = TRUE;
            if (strcmp(name, "lib/x86_64/libminecraftpe.so") == 0)
                out->has_libminecraftpe = TRUE;
        }
        if (name[0] != '\0' && name[strlen(name) - 1] != '/')
            total += entries[i].uncomp_size;
    }
    out->uncompressed_size = total;

    if (!have_manifest) {
        zip_entries_free(entries, n);
        g_set_error(error, APK_ERROR, 0,
                    "No AndroidManifest.xml found — this is not a Minecraft APK.");
        return FALSE;
    }

    GBytes *bytes = NULL;
    if (!zip_extract_one(path, &manifest_entry, &bytes, error)) {
        zip_entries_free(entries, n);
        return FALSE;
    }

    gsize mlen = 0;
    const guint8 *mdata = g_bytes_get_data(bytes, &mlen);
    AXmlManifest m;
    gboolean parsed = axml_parse_manifest(mdata, mlen, &m, error);
    g_bytes_unref(bytes);

    if (!parsed) {
        zip_entries_free(entries, n);
        if (!error || !*error)
            g_set_error(error, APK_ERROR, 0,
                        "Could not read AndroidManifest.xml — is this really a "
                        "Minecraft APK?");
        return FALSE;
    }

    if (!m.package || strcmp(m.package, MCPE_PACKAGE) != 0) {
        gchar *pkg = g_strdup(m.package ? m.package : "(unknown)");
        axml_manifest_free(&m);
        zip_entries_free(entries, n);
        g_set_error(error, APK_ERROR, 0,
                    "Not a Minecraft APK (package is \"%s\").", pkg);
        g_free(pkg);
        return FALSE;
    }

    out->package = g_strdup(m.package);
    out->version_name = m.version_name ? g_strdup(m.version_name) : g_strdup("unknown");
    out->version_code = m.version_code;

    if (m.version_name && *m.version_name) {
        out->label = g_strdup(m.version_name);
    } else {
        char *base = g_path_get_basename(path);
        char *dot = strrchr(base, '.');
        if (dot && g_ascii_strcasecmp(dot, ".apk") == 0)
            *dot = '\0';
        out->label = g_strdup(*base ? base : "minecraft");
        g_free(base);
    }

    axml_manifest_free(&m);
    zip_entries_free(entries, n);
    return TRUE;
}