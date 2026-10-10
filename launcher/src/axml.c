#include "axml.h"

#include <string.h>

#define CHUNK_STRING_POOL 0x0001
#define CHUNK_XML 0x0003
#define CHUNK_START_ELEMENT 0x0102
#define STRING_POOL_UTF8 0x0100

/* Sentinel used by AXML for "no string reference". */
#define NO_STRING 0xFFFFFFFFu

static guint32 rd32(const guint8 *p)
{
    return (guint32)p[0] | ((guint32)p[1] << 8) | ((guint32)p[2] << 16) |
           ((guint32)p[3] << 24);
}

static guint16 rd16(const guint8 *p)
{
    return (guint16)p[0] | ((guint16)p[1] << 8);
}

typedef struct {
    const guint8 *data;
    gsize len;
    gboolean utf8;
    gsize offsets_off; /* absolute offset of the string offset array */
    gsize strings_off; /* absolute offset of the string data */
    guint count;
} StringPool;

static char *pool_string(const StringPool *sp, guint idx)
{
    if (!sp->data || idx >= sp->count)
        return NULL;
    if (sp->offsets_off + (gsize)idx * 4 + 4 > sp->len)
        return NULL;
    guint32 off = rd32(sp->data + sp->offsets_off + (gsize)idx * 4);
    gsize pos = sp->strings_off + off;
    if (pos >= sp->len)
        return NULL;

    if (sp->utf8) {
        gsize len = sp->data[pos++];
        if (len & 0x80) {
            if (pos >= sp->len)
                return NULL;
            len = ((len & 0x7f) << 8) | sp->data[pos++];
        }
        if (pos + len > sp->len)
            return NULL;
        return g_strndup((const char *)(sp->data + pos), len);
    }

    /* UTF-16LE with the AXML varint length scheme. */
    if (pos + 2 > sp->len)
        return NULL;
    guint c = rd16(sp->data + pos);
    pos += 2;
    if (c & 0x8000) {
        if (pos + 2 > sp->len)
            return NULL;
        c = ((c & 0x7fff) << 16) | rd16(sp->data + pos);
        pos += 2;
    }
    if (c > (sp->len - pos) / 2)
        return NULL;

    /* g_utf16_to_utf8 needs a NUL terminator, so copy the string first. */
    gunichar2 *u = g_new(gunichar2, c + 1);
    for (guint i = 0; i < c; i++)
        u[i] = (gunichar2)(sp->data[pos + i * 2] | (sp->data[pos + i * 2 + 1] << 8));
    u[c] = 0;
    gchar *s = g_utf16_to_utf8(u, -1, NULL, NULL, NULL);
    g_free(u);
    return s;
}

gboolean axml_parse_manifest(const guint8 *data, gsize len, AXmlManifest *out,
                             GError **error)
{
    memset(out, 0, sizeof(*out));
    StringPool sp = { .data = NULL };

    gsize pos = 0;
    gsize end = len;
    gboolean found = FALSE;

    /* Some APKs wrap the whole binary XML in a RES_XML_TYPE (0x0003)
     * container chunk; descend into it so the string pool and element
     * chunks are visible. */
    if (len >= 8) {
        guint16 outer_type = rd16(data);
        guint32 outer_size = rd32(data + 4);
        if (outer_type == CHUNK_XML && outer_size >= 8 && outer_size <= len) {
            pos = 8; /* skip the container header */
            end = outer_size;
        }
    }

    while (pos + 8 <= end) {
        guint16 type = rd16(data + pos);
        guint32 size = rd32(data + pos + 4);
        if (size < 8 || pos + size > end)
            break;

        if (type == CHUNK_STRING_POOL && !sp.data) {
            if (size >= 28) {
                sp.data = data;
                sp.len = len;
                sp.count = rd32(data + pos + 8);
                sp.utf8 = (rd32(data + pos + 16) & STRING_POOL_UTF8) != 0;
                sp.strings_off = pos + rd32(data + pos + 20);
                sp.offsets_off = pos + 28;
            }
        } else if (type == CHUNK_START_ELEMENT) {
            guint16 header_size = rd16(data + pos + 2);
            gsize base = pos + header_size; /* start of the attrExt */
            if (base + 20 > len)
                break;
            guint16 attr_start = rd16(data + base + 8);
            guint16 attr_size = rd16(data + base + 10);
            guint16 attr_count = rd16(data + base + 12);
            gsize attrs = pos + header_size + attr_start;

            for (guint i = 0; i < attr_count; i++) {
                gsize a = attrs + (gsize)i * attr_size;
                if (a + 20 > len)
                    break;
                guint name_idx = rd32(data + a + 4);
                guint raw_idx = rd32(data + a + 8);
                guint32 typed_data = rd32(data + a + 16);

                char *name = pool_string(&sp, name_idx);
                if (!name)
                    continue;
                if (strcmp(name, "package") == 0 && !out->package &&
                    raw_idx != NO_STRING)
                    out->package = pool_string(&sp, raw_idx);
                else if (strcmp(name, "versionName") == 0 &&
                         !out->version_name && raw_idx != NO_STRING)
                    out->version_name = pool_string(&sp, raw_idx);
                else if (strcmp(name, "versionCode") == 0)
                    out->version_code = typed_data;
                g_free(name);
            }
            found = TRUE; /* first start element is the manifest root */
            break;
        }

        pos += size;
    }

    if (!found) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                    "AndroidManifest.xml is malformed");
        return FALSE;
    }
    return TRUE;
}

void axml_manifest_free(AXmlManifest *m)
{
    if (!m)
        return;
    g_free(m->package);
    g_free(m->version_name);
    memset(m, 0, sizeof(*m));
}