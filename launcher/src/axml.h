#pragma once

#include <glib.h>

/* Minimal parser for Android's binary XML (AXML) format. Enough to read the
 * manifest root element's package name, versionName and versionCode. */
typedef struct {
    char *package;
    char *version_name;
    guint32 version_code;
} AXmlManifest;

gboolean axml_parse_manifest(const guint8 *data, gsize len, AXmlManifest *out,
                             GError **error);

void axml_manifest_free(AXmlManifest *m);