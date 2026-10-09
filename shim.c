#define _SIGSET_NWORDS (1024 / (8 * sizeof (unsigned long int)))

typedef struct {
    long quot;
    long rem;
} ldiv_t;

typedef struct
{
    unsigned long int __val[_SIGSET_NWORDS];
} __sigset_t;

typedef __sigset_t sigset_t;


typedef unsigned long size_t;

typedef struct _IO_FILE FILE;

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

#define _SIGSET_NWORDS (1024 / (8 * sizeof (unsigned long int)))

typedef __sigset_t sigset_t;
typedef unsigned long size_t;

/* --- Custom _IO_FILE Definition --- */
/* Replace this layout with your actual FILE struct layout. */
typedef struct _IO_FILE {
    int _fileno;       /* The underlying file descriptor */
    int _flags;        /* Stream state flags (EOF, ERR, etc.) */
    /* Buffering pointers would normally follow here */
} FILE;

/* --- POSIX & Linux Constants (x86_64) --- */
#define O_RDONLY    00
#define O_WRONLY    01
#define O_RDWR      02
#define O_CREAT     0100
#define O_TRUNC     01000
#define O_APPEND    02000
#define O_LARGEFILE 0100000

#define F_SETFL     4

/* --- Direct System Call Wrapper for x86_64 --- */
static long sys_call3(long num, long arg1, long arg2, long arg3) {
    long ret;
    __asm__ __volatile__(
        "syscall"
        : "=a" (ret)
        : "a" (num), "D" (arg1), "S" (arg2), "d" (arg3)
        : "rcx", "r11", "memory"
    );
    return ret;
}

#define sys_open(path, flags, mode) sys_call3(2, (long)(path), (long)(flags), (long)(mode))
#define sys_close(fd)               sys_call3(3, (long)(fd), 0, 0)
#define sys_fcntl(fd, cmd, arg)     sys_call3(7, (long)(fd), (long)(cmd), (long)(arg))
#define sys_dup2(oldfd, newfd)      sys_call3(33, (long)(oldfd), (long)(newfd), 0)

/* --- Implementation --- */
FILE *freopen64(const char *filename, const char *mode, FILE *stream) {
    if (!stream) {
        return (FILE*)0;
    }

    /* 1. Parse mode string manually without string.h */
    int flags = 0;
    if (mode[0] == 'r') {
        flags = (mode[1] == '+') ? O_RDWR : O_RDONLY;
    } else if (mode[0] == 'w') {
        flags = (mode[1] == '+') ? (O_RDWR | O_CREAT | O_TRUNC) : (O_WRONLY | O_CREAT | O_TRUNC);
    } else if (mode[0] == 'a') {
        flags = (mode[1] == '+') ? (O_RDWR | O_CREAT | O_APPEND) : (O_WRONLY | O_CREAT | O_APPEND);
    } else {
        return (FILE*)0;
    }

    /* Explicitly enforce large file tracking */
    flags |= O_LARGEFILE;

    /* 2. Clear out existing stream buffers here if you maintain them */
    /* e.g., stream->_ptr = stream->_base; stream->_cnt = 0; */

    int old_fd = stream->_fileno;
    int new_fd = -1;

    /* 3. Handle changing the underlying descriptor */
    if (filename != (char*)0) {
        /* Open new file with default permissions 0666 */
        new_fd = (int)sys_open(filename, flags, 0666);
        if (new_fd < 0) {
            return (FILE*)0; 
        }

        /* Atomically clone new_fd into old_fd slot, closing the old slot */
        if (sys_dup2(new_fd, old_fd) < 0) {
            sys_close(new_fd);
            return (FILE*)0;
        }
        sys_close(new_fd);
    } else {
        /* If filename is NULL, redirect current stream parameters instead */
        if (sys_fcntl(old_fd, F_SETFL, flags) < 0) {
            return (FILE*)0;
        }
    }

    /* 4. Reset error and EOF status markers */
    stream->_flags &= ~0x0020; /* Example mask bit for clearing EOF/ERR */

    return stream;
}




int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset) {
	return 0;
}

ldiv_t ldiv(long numer, long denom) {
	ldiv_t result;

    result.quot = numer / denom;
    
    result.rem = numer % denom;

    return result;
}
