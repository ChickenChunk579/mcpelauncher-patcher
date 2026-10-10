#include "zip.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <zlib.h>

#define ZIP_EOCD 0x06054b50u
#define ZIP_CDHDR 0x02014b50u
#define ZIP_LOCAL 0x04034b50u

static guint32 rd32(const guint8 *p)
{
    return (guint32)p[0] | ((guint32)p[1] << 8) | ((guint32)p[2] << 16) |
           ((guint32)p[3] << 24);
}

static guint16 rd16(const guint8 *p)
{
    return (guint16)p[0] | ((guint16)p[1] << 8);
}

static int wr_err(GError **error, int code, const char *fmt, ...)
{
    if (!error)
        return code;
    va_list ap;
    va_start(ap, fmt);
    gchar *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    g_set_error(error, G_FILE_ERROR, code, "%s", msg);
    g_free(msg);
    return code;
}

ZipEntry *zip_read_entries(const char *path, guint *n_out, GError **error)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        wr_err(error, g_file_error_from_errno(errno), "Cannot open %s: %s",
               path, g_strerror(errno));
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        wr_err(error, G_FILE_ERROR_IO, "Cannot seek in %s", path);
        fclose(f);
        return NULL;
    }
    long file_size = ftell(f);
    if (file_size < 22) {
        wr_err(error, G_FILE_ERROR_INVAL, "%s is too small to be a valid APK/zip",
               path);
        fclose(f);
        return NULL;
    }

    const gsize tail_len = MIN((gsize)file_size, 65557);
    guint8 *tail = g_malloc(tail_len);
    gboolean ok = (fseek(f, file_size - (long)tail_len, SEEK_SET) == 0 &&
                   fread(tail, 1, tail_len, f) == tail_len);
    if (!ok) {
        g_free(tail);
        wr_err(error, G_FILE_ERROR_IO, "Failed to read the end of %s", path);
        fclose(f);
        return NULL;
    }

    /* Find the end-of-central-directory record by scanning backwards.  */
    const guint8 *eocd = NULL;
    for (gsize i = tail_len; i >= 22; i--) {
        if (rd32(tail + i - 22) == ZIP_EOCD) {
            eocd = tail + i - 22;
            break;
        }
    }
    if (!eocd) {
        g_free(tail);
        wr_err(error, G_FILE_ERROR_INVAL, "%s is not a valid zip archive", path);
        fclose(f);
        return NULL;
    }

    guint count = rd16(eocd + 10);
    guint32 cd_offset = rd32(eocd + 16);
    if (count == 0xFFFF || cd_offset == 0xFFFFFFFFu) {
        g_free(tail);
        wr_err(error, G_FILE_ERROR_INVAL, "%s uses zip64, which is unsupported", path);
        fclose(f);
        return NULL;
    }

    if (fseek(f, (long)cd_offset, SEEK_SET) != 0) {
        g_free(tail);
        wr_err(error, G_FILE_ERROR_IO, "Corrupt central directory in %s", path);
        fclose(f);
        return NULL;
    }

    ZipEntry *entries = g_new0(ZipEntry, count);
    for (guint i = 0; i < count; i++) {
        guint8 hdr[46];
        if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
            rd32(hdr) != ZIP_CDHDR) {
            g_free(tail);
            zip_entries_free(entries, i);
            wr_err(error, G_FILE_ERROR_INVAL, "Corrupt central directory in %s",
                   path);
            fclose(f);
            return NULL;
        }
        guint nlen = rd16(hdr + 28), elen = rd16(hdr + 30), clen = rd16(hdr + 32);
        entries[i].method = rd16(hdr + 10);
        entries[i].comp_size = rd32(hdr + 20);
        entries[i].uncomp_size = rd32(hdr + 24);
        entries[i].local_offset = (gint64)rd32(hdr + 42);
        entries[i].name = g_malloc(nlen + 1);
        if (fread(entries[i].name, 1, nlen, f) != nlen) {
            g_free(tail);
            zip_entries_free(entries, i + 1);
            wr_err(error, G_FILE_ERROR_IO, "Failed reading %s", path);
            fclose(f);
            return NULL;
        }
        entries[i].name[nlen] = '\0';
        if (elen + clen > 0 && fseek(f, (long)(elen + clen), SEEK_CUR) != 0) {
            g_free(tail);
            zip_entries_free(entries, i + 1);
            wr_err(error, G_FILE_ERROR_IO, "Failed reading %s", path);
            fclose(f);
            return NULL;
        }
    }

    g_free(tail);
    fclose(f);
    if (n_out)
        *n_out = count;
    return entries;
}

void zip_entries_free(ZipEntry *entries, guint n)
{
    if (!entries)
        return;
    for (guint i = 0; i < n; i++)
        g_free(entries[i].name);
    g_free(entries);
}

gboolean zip_find_entry(ZipEntry *entries, guint n, const char *name, ZipEntry *out)
{
    for (guint i = 0; i < n; i++) {
        if (strcmp(entries[i].name, name) == 0) {
            *out = entries[i];
            return TRUE;
        }
    }
    return FALSE;
}

/* Read and decompress a single entry. Caller owns the returned bytes. */
static gboolean read_entry(FILE *f, const ZipEntry *entry, GBytes **out,
                           GError **error)
{
    guint8 local[30];
    if (fseek(f, (long)entry->local_offset, SEEK_SET) != 0 ||
        fread(local, 1, sizeof(local), f) != sizeof(local) ||
        rd32(local) != ZIP_LOCAL) {
        wr_err(error, G_FILE_ERROR_INVAL, "Corrupt local header for %s",
               entry->name);
        return FALSE;
    }
    gint64 data_off = entry->local_offset + 30 + rd16(local + 26) + rd16(local + 28);
    if (fseek(f, (long)data_off, SEEK_SET) != 0) {
        wr_err(error, G_FILE_ERROR_IO, "Failed to read %s", entry->name);
        return FALSE;
    }

    gsize comp_len = entry->comp_size;
    guint8 *comp = g_malloc(MAX(comp_len, 1));
    if (fread(comp, 1, comp_len, f) != comp_len) {
        g_free(comp);
        wr_err(error, G_FILE_ERROR_IO, "Failed to read %s", entry->name);
        return FALSE;
    }

    if (entry->method == 0) {
        *out = g_bytes_new_take(comp, entry->uncomp_size);
        return TRUE;
    }

    if (entry->method != 8) {
        g_free(comp);
        wr_err(error, G_FILE_ERROR_INVAL,
               "Unsupported compression method %u for %s", entry->method,
               entry->name);
        return FALSE;
    }

    guint8 *out_buf = g_malloc(MAX((guint)entry->uncomp_size, 1));
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
        g_free(comp);
        g_free(out_buf);
        wr_err(error, G_FILE_ERROR_FAILED, "zlib initialisation failed");
        return FALSE;
    }
    zs.next_in = comp;
    zs.avail_in = (uInt)comp_len;
    zs.next_out = out_buf;
    zs.avail_out = entry->uncomp_size;
    int rc = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);
    if (rc != Z_STREAM_END) {
        g_free(comp);
        g_free(out_buf);
        wr_err(error, G_FILE_ERROR_INVAL, "Failed to decompress %s", entry->name);
        return FALSE;
    }
    g_free(comp);
    *out = g_bytes_new_take(out_buf, entry->uncomp_size);
    return TRUE;
}

gboolean zip_extract_one(const char *path, const ZipEntry *entry, GBytes **out,
                         GError **error)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        wr_err(error, g_file_error_from_errno(errno), "Cannot open %s: %s",
               path, g_strerror(errno));
        return FALSE;
    }
    gboolean ok = read_entry(f, entry, out, error);
    fclose(f);
    return ok;
}

static gboolean is_dir_entry(const char *name, gsize len)
{
    return len > 0 && name[len - 1] == '/';
}

static gboolean path_segment_invalid(const char *seg, gsize len)
{
    if (len == 0)
        return TRUE;
    if (len == 1 && (seg[0] == '.'))
        return TRUE;
    if (len == 2 && seg[0] == '.' && seg[1] == '.')
        return TRUE;
    /* Reject anything that could contain a path separator or control chars. */
    for (gsize i = 0; i < len; i++) {
        if (seg[i] == '\\' || seg[i] == '/' || (guchar)seg[i] < 0x20)
            return TRUE;
    }
    return FALSE;
}

/* Build a safe relative path from an archive entry name, or NULL if the
 * entry should be skipped (absolute paths, ".." components, ...). */
static char *sanitise_name(const char *name, gboolean *is_dir)
{
    gsize len = strlen(name);
    *is_dir = is_dir_entry(name, len);
    if (len == 0 || name[0] == '/' || name[0] == '\\' ||
        g_str_has_prefix(name, "../"))
        return NULL;

    GString *out = g_string_new(NULL);
    GString *seg = g_string_new(NULL);
    const char *p = name;
    while (*p) {
        if (*p == '/') {
            if (!path_segment_invalid(seg->str, seg->len)) {
                if (out->len > 0)
                    g_string_append_c(out, '/');
                g_string_append_len(out, seg->str, seg->len);
            }
            g_string_truncate(seg, 0);
            p++;
            continue;
        }
        g_string_append_c(seg, *p);
        p++;
    }
    if (seg->len > 0 && !path_segment_invalid(seg->str, seg->len)) {
        if (out->len > 0)
            g_string_append_c(out, '/');
        g_string_append_len(out, seg->str, seg->len);
    }
    g_string_free(seg, TRUE);

    char *res = g_string_free(out, FALSE);
    if (!*res) {
        g_free(res);
        return NULL;
    }
    return res;
}

static gboolean write_all(FILE *f, const guint8 *data, gsize len)
{
    return len == 0 || fwrite(data, 1, len, f) == len;
}

gboolean zip_extract_all(const char *path, const char *dest_dir,
                         GCancellable *cancellable, ZipProgressFunc progress,
                         gpointer user_data, GError **error)
{
    if (g_mkdir_with_parents(dest_dir, 0755) != 0) {
        wr_err(error, g_file_error_from_errno(errno),
               "Cannot create %s: %s", dest_dir, g_strerror(errno));
        return FALSE;
    }

    guint n = 0;
    ZipEntry *entries = zip_read_entries(path, &n, error);
    if (!entries)
        return FALSE;

    guint total = 0;
    for (guint i = 0; i < n; i++) {
        gboolean is_dir;
        if (!sanitise_name(entries[i].name, &is_dir))
            continue;
        if (!is_dir)
            total++;
    }
    if (progress)
        progress(0, total, "Preparing…", user_data);

    FILE *f = fopen(path, "rb");
    if (!f) {
        zip_entries_free(entries, n);
        wr_err(error, g_file_error_from_errno(errno), "Cannot open %s: %s",
               path, g_strerror(errno));
        return FALSE;
    }

    guint done = 0;
    gboolean ok = TRUE;
    for (guint i = 0; i < n; i++) {
        if (g_cancellable_set_error_if_cancelled(cancellable, error)) {
            ok = FALSE;
            break;
        }

        gboolean is_dir;
        char *rel = sanitise_name(entries[i].name, &is_dir);
        if (!rel)
            continue;
        gchar *full = g_build_filename(dest_dir, rel, NULL);
        gchar *parent = g_path_get_dirname(full);
        if (g_mkdir_with_parents(parent, 0755) != 0) {
            wr_err(error, g_file_error_from_errno(errno),
                   "Cannot create %s: %s", parent, g_strerror(errno));
            g_free(parent);
            g_free(full);
            g_free(rel);
            ok = FALSE;
            break;
        }
        g_free(parent);

        if (is_dir) {
            g_free(full);
            g_free(rel);
            continue;
        }

        GBytes *data = NULL;
        if (!read_entry(f, &entries[i], &data, error)) {
            g_free(full);
            g_free(rel);
            ok = FALSE;
            break;
        }
        gsize dlen = 0;
        const guint8 *dbuf = g_bytes_get_data(data, &dlen);
        FILE *outf = fopen(full, "wb");
        if (!outf) {
            wr_err(error, g_file_error_from_errno(errno),
                   "Cannot write %s: %s", full, g_strerror(errno));
            g_bytes_unref(data);
            g_free(full);
            g_free(rel);
            ok = FALSE;
            break;
        }
        gboolean wok = write_all(outf, dbuf, dlen);
        fclose(outf);
        g_bytes_unref(data);
        if (!wok) {
            wr_err(error, G_FILE_ERROR_IO, "Failed to write %s", full);
            g_free(full);
            g_free(rel);
            ok = FALSE;
            break;
        }
        chmod(full, 0644);
        g_free(full);
        g_free(rel);

        done++;
        if (progress)
            progress(done, total, entries[i].name, user_data);
    }

    fclose(f);
    zip_entries_free(entries, n);
    return ok;
}