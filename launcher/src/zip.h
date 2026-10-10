#pragma once

#include <gio/gio.h>
#include <glib.h>

/* One file entry from a zip central directory. */
typedef struct {
    char *name;
    guint method;       /* 0 = stored, 8 = deflate */
    guint comp_size;
    guint uncomp_size;
    gint64 local_offset;
} ZipEntry;

/* Progress callback, called from the extraction thread.
 * `done` counts files written, `total` the total number of files (may be 0
 * until the archive has been scanned). `current` is a description of the
 * entry being processed. */
typedef void (*ZipProgressFunc)(guint done, guint total, const char *current,
                                gpointer user_data);

/* Read the central directory of a zip archive. Returns a newly allocated
 * array or NULL with @error set. Free with zip_entries_free(). */
ZipEntry *zip_read_entries(const char *path, guint *n_out, GError **error);

void zip_entries_free(ZipEntry *entries, guint n);

/* Locate the entry named @name. */
gboolean zip_find_entry(ZipEntry *entries, guint n, const char *name,
                        ZipEntry *out);

/* Extract a single entry into a GBytes. */
gboolean zip_extract_one(const char *path, const ZipEntry *entry, GBytes **out,
                         GError **error);

/* Extract the whole archive into @dest_dir. Entries are sanitised so
 * nothing can escape the destination. Callbacks fire from the calling
 * thread. Returns FALSE on failure/cancellation. */
gboolean zip_extract_all(const char *path, const char *dest_dir,
                         GCancellable *cancellable, ZipProgressFunc progress,
                         gpointer user_data, GError **error);