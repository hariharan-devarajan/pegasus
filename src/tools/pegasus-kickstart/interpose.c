#define _GNU_SOURCE

#include <stdio.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <stdarg.h>
#include <dirent.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

/* TODO Intercept fread(), fwrite(), fscanf(), fprintf(), vfscanf(), vfprintf(), fgets(), fputs(), fgetc(), fputc(), etc. */
/* TODO Intercept pread() and pwrite() calls and *64 versions */
/* TODO Intercept readv() and writev() and related */
/* TODO Handle I/O for stdout/stderr? */
/* TODO Thread safety? */
/* TODO Add r/w/a mode support? */
/* TODO Interpose mkstemp family and tmpfile? */
/* TODO Interpose rename? */
/* TODO Interpose truncate? */
/* TODO Interpose unlink? */
/* TODO What happens if one interposed library function calls another (e.g. fopen calls fopen64)? I think internal calls are not traced. */
/* TODO Interpose network functions (connect, accept) */

/* These are all the functions we are interposing */
static typeof(open) *orig_open = NULL;
static typeof(open64) *orig_open64 = NULL;
static typeof(openat) *orig_openat = NULL;
static typeof(openat64) *orig_openat64 = NULL;
static typeof(creat) *orig_creat = NULL;
static typeof(creat64) *orig_creat64 = NULL;
static typeof(fopen) *orig_fopen = NULL;
static typeof(fopen64) *orig_fopen64 = NULL;
static typeof(freopen) *orig_freopen = NULL;
static typeof(freopen64) *orig_freopen64 = NULL;
static typeof(close) *orig_close = NULL;
static typeof(fclose) *orig_fclose = NULL;
static typeof(read) *orig_read = NULL;
static typeof(write) *orig_write = NULL;

typedef struct {
    char *path;
    size_t bread;
    size_t bwrite;
} Descriptor;

static Descriptor **descriptors = NULL;
static int max_descriptors = 0;

/* This is the trace file where we write information about the process */
static FILE* trace = NULL;

static FILE *fopen_untraced(const char *path, const char *mode);
static int fclose_untraced(FILE *fp);

/* Open the trace file */
static int topen() {

    char *kickstart_prefix = getenv("KICKSTART_PREFIX");
    if (kickstart_prefix == NULL) {
        fprintf(stderr, "libinterpose: Unable to open trace file: KICKSTART_PREFIX not set in environment");
        return -1;
    }

    char filename[BUFSIZ];
    snprintf(filename, BUFSIZ, "%s.%d", kickstart_prefix, getpid());

    trace = fopen_untraced(filename, "w+");
    if (trace == NULL) {
        fprintf(stderr, "libinterpose: Unable to open trace file");
        return -1;
    }

    return 0;
}

/* Print something to the trace file if it is open */
static int tprintf(const char *format, ...) {
    if (trace == NULL) {
        return 0;
    }
    va_list args;
    va_start(args, format);
    int rc = vfprintf(trace, format, args);
    va_end(args);
    return rc;
}

/* Close trace file */
static int tclose() {
    if (trace == NULL) {
        return 0;
    }

    return fclose_untraced(trace);
}

/* Get the current time in seconds since the epoch */
static double get_time() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + ((double)tv.tv_usec / 1e6);
}

/* Read /proc/self/exe to get path to executable */
static void read_exe() {
    char exe[BUFSIZ];
    int size = readlink("/proc/self/exe", exe, BUFSIZ);
    if (size < 0) {
        perror("libinterpose: Unable to readlink /proc/self/exe");
        return;
    }
    exe[size] = '\0';
    tprintf("exe: %s\n", exe);
}

/* Return 1 if line begins with tok */
static int startswith(const char *line, const char *tok) {
    return strstr(line, tok) == line;
}

/* Read useful information from /proc/self/status */
static void read_status() {
    char statf[] = "/proc/self/status";

    /* If the status file is missing, then just skip it */
    if (access(statf, F_OK) < 0) {
        return;
    }

    FILE *f = fopen_untraced(statf, "r");
    if (f == NULL) {
        perror("libinterpose: Unable to fopen /proc/self/status");
        return;
    }

    char line[BUFSIZ];
    while (fgets(line, BUFSIZ, f) != NULL) {
        if (startswith(line, "Pid")) {
            tprintf(line);
        } else if (startswith(line, "PPid")) {
            tprintf(line);
        } else if (startswith(line, "Tgid")) {
            tprintf(line);
        } else if (startswith(line,"VmPeak")) {
            tprintf(line);
        } else if (startswith(line,"VmHWM")) {
            tprintf(line);
        } else if (startswith(line,"Threads")) {
            tprintf(line);
        }
    }

    fclose_untraced(f);
}

/* Read /proc/self/stat to get CPU usage */
static void read_stat() {
    char statf[] = "/proc/self/stat";

    /* If the stat file is missing, then just skip it */
    if (access(statf, F_OK) < 0) {
        return;
    }

    FILE *f = fopen_untraced(statf,"r");
    if (f == NULL) {
        perror("libinterpose: Unable to fopen /proc/self/stat");
        return;
    }

    unsigned long utime, stime;
    fscanf(f, "%*d %*s %*c %*d %*d %*d %*d %*d "
              "%*u %*u %*u %*u %*u %lu %lu %*d %*d",
           &utime, &stime);

    fclose_untraced(f);

    /* Adjust by number of clock ticks per second */
    long clocks = sysconf(_SC_CLK_TCK);
    double real_utime;
    double real_stime;
    real_utime = ((double)utime) / clocks;
    real_stime = ((double)stime) / clocks;

    tprintf("utime: %lf\n", real_utime);
    tprintf("stime: %lf\n", real_stime);
}

/* Read /proc/self/io to get I/O usage */
static void read_io() {
    char iofile[] = "/proc/self/io";

    /* This proc file was added in Linux 2.6.20. It won't be
     * there on older kernels, or on kernels without task IO 
     * accounting. If it is missing, just bail out.
     */
    if (access(iofile, F_OK) < 0) {
        return;
    }

    FILE *f = fopen_untraced(iofile, "r");
    if (f == NULL) {
        perror("libinterpose: Unable to fopen /proc/self/io");
        return;
    }

    char line[BUFSIZ];
    while (fgets(line, BUFSIZ, f) != NULL) {
        if (startswith(line, "rchar")) {
            tprintf(line);
        } else if (startswith(line, "wchar")) {
            tprintf(line);
        } else if (startswith(line,"syscr")) {
            tprintf(line);
        } else if (startswith(line,"syscw")) {
            tprintf(line);
        } else if (startswith(line,"read_bytes")) {
            tprintf(line);
        } else if (startswith(line,"write_bytes")) {
            tprintf(line);
        } else if (startswith(line,"cancelled_write_bytes")) {
            tprintf(line);
        }
    }

    fclose_untraced(f);
}


static void trace_file(const char *path, int fd) {
    /* Skip all the common system paths, which we don't care about */
    if (startswith(path, "/lib") ||
        startswith(path, "/usr") ||
        startswith(path, "/dev") ||
        startswith(path, "/etc") ||
        startswith(path, "/proc")||
        startswith(path, "/sys")) {
        return;
    }

    Descriptor *f = (Descriptor *)calloc(sizeof(Descriptor), 1);
    f->path = strdup(path);
    descriptors[fd] = f;
}

static void trace_open(const char *path, int fd) {
    char fullpath[BUFSIZ];
    if (realpath(path, fullpath) == NULL) {
        fprintf(stderr, "libinterpose: Unable to get real path for '%s': %s\n",
                path, strerror(errno));
        return;
    }

    trace_file(fullpath, fd);
}

static void trace_openat(int fd) {
    char linkpath[64];
    snprintf(linkpath, 64, "/proc/%d/fd/%d", getpid(), fd);

    char fullpath[BUFSIZ];
    int len = readlink(linkpath, fullpath, BUFSIZ);
    if (len <= 0) {
        fprintf(stderr, "libinterpose: Unable to get real path for fd %d: %s\n",
                fd, strerror(errno));
        return;
    }
    if (len == BUFSIZ) {
        fprintf(stderr, "libinterpose: Path too long for fd %d: %s\n",
                fd, strerror(errno));
        return;
    }
    /* readlink doesn't add a null byte */
    fullpath[len] = '\0';

    trace_file(fullpath, fd);
}

static void trace_read(int fd, ssize_t amount) {
    if (descriptors[fd] == NULL) {
        return;
    }

    descriptors[fd]->bread += amount;
}

static void trace_write(int fd, ssize_t amount) {
    if (descriptors[fd] == NULL) {
        return;
    }

    descriptors[fd]->bwrite += amount;
}

static void trace_close(int fd) {
    /* All descriptors we aren't interested in won't be in the table */
    if (descriptors[fd] == NULL) {
        return;
    }

    Descriptor *f = descriptors[fd];

    struct stat st;
    if (stat(f->path, &st) < 0) {
        fprintf(stderr, "libinterpose: Unable to stat '%s': %s\n",
                f->path, strerror(errno));
        return;
    }

    tprintf("file: %s %lu %lu %lu\n", f->path, st.st_size, f->bread, f->bwrite);

    /* Free the entry */
    descriptors[fd] = NULL;
    free(f->path);
    free(f);
}

/* Library initialization function */
static void __attribute__((constructor)) interpose_init(void) {
    /* Open the trace file */
    topen();

    /* Get file descriptor limit and allocate descriptor table */
    struct rlimit nofile_limit;
    getrlimit(RLIMIT_NOFILE, &nofile_limit);
    max_descriptors = nofile_limit.rlim_max;
    descriptors = (Descriptor **)calloc(sizeof(Descriptor *), max_descriptors);

    tprintf("start: %lf\n", get_time());
}

/* Library finalizer function */
static void __attribute__((destructor)) interpose_fini(void) {

    /* Look for descriptors not explicitly closed */
    for(int i=0; i<max_descriptors; i++) {
        if (descriptors[i] != NULL) {
            trace_close(i);
        }
    }

    read_exe();
    read_status();
    read_stat();
    read_io();

    tprintf("stop: %lf\n", get_time());

    /* Close trace file */
    tclose();
}


/** INTERPOSED FUNCTIONS **/


int open(const char *path, int oflag, ...) {
    if (orig_open == NULL) {
        orig_open = dlsym(RTLD_NEXT, "open");
    }

    mode_t mode = 0700;
    if (oflag & O_CREAT) {
        va_list list;
        va_start(list, oflag);
        mode = va_arg(list, int);
        va_end(list);
    }

    int rc = (*orig_open)(path, oflag, mode);

    if (rc >= 0) {
        trace_open(path, rc);
    }

    return rc;
}

int open64(const char *path, int oflag, ...) {
    if (orig_open64 == NULL) {
        orig_open64 = dlsym(RTLD_NEXT, "open64");
    }

    mode_t mode = 0700;
    if (oflag & O_CREAT) {
        va_list list;
        va_start(list, oflag);
        mode = va_arg(list, int);
        va_end(list);
    }

    int rc = (*orig_open64)(path, oflag, mode);

    if (rc >= 0) {
        trace_open(path, rc);
    }

    return rc;
}

int openat(int dirfd, const char *path, int oflag, ...) {
    if (orig_openat == NULL) {
        orig_openat = dlsym(RTLD_NEXT, "openat");
    }

    mode_t mode = 0700;
    if (oflag & O_CREAT) {
        va_list list;
        va_start(list, oflag);
        mode = va_arg(list, int);
        va_end(list);
    }

    int rc = (*orig_openat)(dirfd, path, oflag, mode);

    if (rc >= 0) {
        trace_openat(rc);
    }

    return rc;
}

int openat64(int dirfd, const char *path, int oflag, ...) {
    if (orig_openat64 == NULL) {
        orig_openat64 = dlsym(RTLD_NEXT, "openat64");
    }

    mode_t mode = 0700;
    if (oflag & O_CREAT) {
        va_list list;
        va_start(list, oflag);
        mode = va_arg(list, int);
        va_end(list);
    }

    int rc = (*orig_openat64)(dirfd, path, oflag, mode);

    if (rc >= 0) {
        trace_openat(rc);
    }

    return rc;
}

int creat(const char *path, mode_t mode) {
    if (orig_creat == NULL) {
        orig_creat = dlsym(RTLD_NEXT, "creat");
    }

    int rc = (*orig_creat)(path, mode);

    if (rc >= 0) {
        trace_open(path, rc);
    }

    return rc;
}

int creat64(const char *path, mode_t mode) {
    if (orig_creat64 == NULL) {
        orig_creat64 = dlsym(RTLD_NEXT, "creat64");
    }

    int rc = (*orig_creat64)(path, mode);

    if (rc >= 0) {
        trace_open(path, rc);
    }

    return rc;
}

static FILE *fopen_untraced(const char *path, const char *mode) {
    if (orig_fopen == NULL) {
        orig_fopen = dlsym(RTLD_NEXT, "fopen");
    }

    return (*orig_fopen)(path, mode);
}

FILE *fopen(const char *path, const char *mode) {
    FILE *f = fopen_untraced(path, mode);

    if (f != NULL) {
        trace_open(path, fileno(f));
    }

    return f;
}

FILE *fopen64(const char *path, const char *mode) {
    if (orig_fopen64 == NULL) {
        orig_fopen64 = dlsym(RTLD_NEXT, "fopen64");
    }

    FILE *f = (*orig_fopen64)(path, mode);

    if (f != NULL) {
        trace_open(path, fileno(f));
    }

    return f;
}

FILE *freopen(const char *path, const char *mode, FILE *stream) {
    if (orig_freopen == NULL) {
        orig_freopen = dlsym(RTLD_NEXT, "freopen");
    }

    FILE *f = orig_freopen(path, mode, stream);

    if (f != NULL) {
        trace_open(path, fileno(f));
    }

    return f;
}

FILE *freopen64(const char *path, const char *mode, FILE *stream) {
    if (orig_freopen64 == NULL) {
        orig_freopen64 = dlsym(RTLD_NEXT, "freopen64");
    }

    FILE *f = orig_freopen64(path, mode, stream);

    if (f != NULL) {
        trace_open(path, fileno(f));
    }

    return f;
}

int close(int fd) {
    if (orig_close == NULL) {
        orig_close = dlsym(RTLD_NEXT, "close");
    }

    trace_close(fd);

    return (*orig_close)(fd);
}

static int fclose_untraced(FILE *fp) {
    if (orig_fclose == NULL) {
        orig_fclose = dlsym(RTLD_NEXT, "fclose");
    }

    return (*orig_fclose)(fp);
}

int fclose(FILE *fp) {

    if (fp != NULL) {
        trace_close(fileno(fp));
    }

    return fclose_untraced(fp);
}

ssize_t read(int fd, void *buf, size_t count) {
    if (orig_read == NULL) {
        orig_read = dlsym(RTLD_NEXT, "read");
    }

    ssize_t rc = (*orig_read)(fd, buf, count);

    if (rc > 0) {
        trace_read(fd, rc);
    }

    return rc;
}

ssize_t write(int fd, const void *buf, size_t count) {
    if (orig_write == NULL) {
        orig_write = dlsym(RTLD_NEXT, "write");
    }

    ssize_t rc = (*orig_write)(fd, buf, count);

    if (rc > 0) {
        trace_write(fd, rc);
    }

    return rc;
}

