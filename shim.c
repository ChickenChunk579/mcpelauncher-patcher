// shim.c - No includes allowed to ensure no links to host libc
typedef unsigned long size_t;

// Dummy FILE structure to satisfy the compiler for freopen64
typedef struct _IO_FILE FILE;

// Standalone __strlcpy_chk
size_t __strlcpy_chk(char *dst, const char *src, size_t size, size_t dest_buf_size) {
    size_t srclen = 0;
    while (src[srclen]) srclen++;
    
    if (size != 0) {
        size_t len = (srclen >= size) ? size - 1 : srclen;
        for (size_t i = 0; i < len; i++) {
            dst[i] = src[i];
        }
        dst[len] = '\0';
    }
    return srclen;
}

// Standalone freopen64 stub
// We return NULL because we don't want to actually trigger host IO 
// which would require linking to the host's FILE implementation.
FILE *freopen64(const char *filename, const char *mode, FILE *stream) {
    return (FILE*)0; 
}

