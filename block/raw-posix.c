/*
 * Block driver for RAW files (posix)
 *
 * Copyright (c) 2006 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "qemu-common.h"
#include "qemu-timer.h"
#include "qemu-char.h"
#include "qemu-log.h"
#include "block_int.h"
#include "module.h"
#include "compatfd.h"
#include <assert.h>
#include "block/raw-posix-aio.h"

#ifdef CONFIG_COCOA
#include <paths.h>
#include <sys/param.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOBSD.h>
#include <IOKit/storage/IOMediaBSDClient.h>
#include <IOKit/storage/IOMedia.h>
#include <IOKit/storage/IOCDMedia.h>
//#include <IOKit/storage/IOCDTypes.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

#ifdef __sun__
#define _POSIX_PTHREAD_SEMANTICS 1
#include <signal.h>
#include <sys/dkio.h>
#endif
#ifdef __linux__
#include <sys/ioctl.h>
#include <sys/vfs.h>
#include <linux/cdrom.h>
#include <linux/fd.h>
#include <linux/magic.h>
#endif
#if defined (__FreeBSD__) || defined(__FreeBSD_kernel__)
#include <signal.h>
#include <sys/disk.h>
#include <sys/cdio.h>
#endif

#ifdef __OpenBSD__
#include <sys/ioctl.h>
#include <sys/disklabel.h>
#include <sys/dkio.h>
#endif

#ifdef __DragonFly__
#include <sys/ioctl.h>
#include <sys/diskslice.h>
#endif

#ifdef CONFIG_XFS
#include <xfs/xfs.h>
#endif

//#define DEBUG_FLOPPY

//#define DEBUG_BLOCK
#if defined(DEBUG_BLOCK)
#define DEBUG_BLOCK_PRINT(formatCstr, ...) do { if (qemu_log_enabled()) \
    { qemu_log(formatCstr, ## __VA_ARGS__); qemu_log_flush(); } } while (0)
#else
#define DEBUG_BLOCK_PRINT(formatCstr, ...)
#endif

/* OS X does not have O_DSYNC */
#ifndef O_DSYNC
#ifdef O_SYNC
#define O_DSYNC O_SYNC
#elif defined(O_FSYNC)
#define O_DSYNC O_FSYNC
#endif
#endif

/* Approximate O_DIRECT with O_DSYNC if O_DIRECT isn't available */
#ifndef O_DIRECT
#define O_DIRECT O_DSYNC
#endif

#define FTYPE_FILE   0
#define FTYPE_CD     1
#define FTYPE_FD     2

/* if the FD is not accessed during that time (in ms), we try to
   reopen it to see if the disk has been changed */
#define FD_OPEN_TIMEOUT 1000

#define MAX_BLOCKSIZE	4096

typedef struct BDRVRawState {
    int fd;
    int type;
    int open_flags;
#if defined(__linux__)
    /* linux floppy specific */
    int64_t fd_open_time;
    int64_t fd_error_time;
    int fd_got_error;
    int fd_media_changed;
#endif
#ifdef CONFIG_LINUX_AIO
    int use_aio;
    void *aio_ctx;
#endif
    bool force_linearize;
#ifdef CONFIG_XFS
    bool is_xfs : 1;
#endif
} BDRVRawState;

typedef struct BDRVRawReopenState {
    int fd;
    int open_flags;
#ifdef CONFIG_LINUX_AIO
    int use_aio;
#endif
} BDRVRawReopenState;

static int fd_open(BlockDriverState *bs);
static int64_t raw_getlength(BlockDriverState *bs);

#if defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
static int cdrom_reopen(BlockDriverState *bs);
#endif

#if defined(__linux__)
static bool is_vectored_io_slow(int fd, int bdrv_flags)
{
    struct statfs stfs;
    int ret;
    char *env_flag = getenv("QEMU_FORCE_LINEARIZE");

    if (env_flag && strcasecmp(env_flag, "off") == 0) {
        return false;
    }

    do {
        ret = fstatfs(fd, &stfs);
    } while (ret != 0 && errno == EINTR);

    /*
     * Linux NFS client splits vectored direct I/O requests into separate NFS
     * requests so it is faster to submit a single buffer instead.
     */
    if (!ret && stfs.f_type == NFS_SUPER_MAGIC &&
        (bdrv_flags & BDRV_O_NOCACHE)) {
        return true;
    }
    return false;
}
#else /* !defined(__linux__) */
static bool is_vectored_io_slow(int fd, int bdrv_flags)
{
    return false;
}
#endif

static void raw_parse_flags(int bdrv_flags, int *open_flags)
{
    assert(open_flags != NULL);

    *open_flags |= O_BINARY;
    *open_flags &= ~O_ACCMODE;
    if (bdrv_flags & BDRV_O_RDWR) {
        *open_flags |= O_RDWR;
    } else {
        *open_flags |= O_RDONLY;
    }

    /* Use O_DSYNC for write-through caching, no flags for write-back caching,
     * and O_DIRECT for no caching. */
    if ((bdrv_flags & BDRV_O_NOCACHE)) {
        *open_flags |= O_DIRECT;
    }
    if (!(bdrv_flags & BDRV_O_CACHE_WB)) {
        *open_flags |= O_DSYNC;
    }
}

#ifdef CONFIG_LINUX_AIO
static int raw_set_aio(void **aio_ctx, int *use_aio, int bdrv_flags)
{
    int ret = -1;
    assert(aio_ctx != NULL);
    assert(use_aio != NULL);
    /*
     * Currently Linux do AIO only for files opened with O_DIRECT
     * specified so check NOCACHE flag too
     */
    if ((bdrv_flags & (BDRV_O_NOCACHE|BDRV_O_NATIVE_AIO)) ==
                      (BDRV_O_NOCACHE|BDRV_O_NATIVE_AIO)) {

        /* if non-NULL, laio_init() has already been run */
        if (*aio_ctx == NULL) {
            *aio_ctx = laio_init();
            if (!*aio_ctx) {
                goto error;
            }
        }
        *use_aio = 1;
    } else {
        *use_aio = 0;
    }

    ret = 0;

error:
    return ret;
}
#endif

static int raw_open_common(BlockDriverState *bs, const char *filename,
                           int bdrv_flags, int open_flags)
{
    BDRVRawState *s = bs->opaque;
    int fd, ret;

    s->open_flags = open_flags;
    raw_parse_flags(bdrv_flags, &s->open_flags);

    s->fd = -1;
    fd = qemu_open(filename, s->open_flags, 0644);
    if (fd < 0) {
        ret = -errno;
        if (ret == -EROFS)
            ret = -EACCES;
        return ret;
    }
    s->fd = fd;
    s->force_linearize = is_vectored_io_slow(fd, bdrv_flags);

    /* We're falling back to POSIX AIO in some cases so init always */
    if (paio_init() < 0) {
        goto out_close;
    }

#ifdef CONFIG_LINUX_AIO
    if (raw_set_aio(&s->aio_ctx, &s->use_aio, bdrv_flags)) {
        goto out_close;
    }
#endif

#ifdef CONFIG_XFS
    if (platform_test_xfs_fd(s->fd)) {
        s->is_xfs = 1;
    }
#endif

    return 0;

out_close:
    close(fd);
    return -errno;
}

static int raw_open(BlockDriverState *bs, const char *filename, int flags)
{
    BDRVRawState *s = bs->opaque;
    int open_flags = 0;

    s->type = FTYPE_FILE;
    if (flags & BDRV_O_CREAT)
        open_flags = O_CREAT | O_TRUNC;

    return raw_open_common(bs, filename, flags, open_flags);
}

static int raw_reopen_prepare(BDRVReopenState *state,
                              BlockReopenQueue *queue, Error **errp)
{
    BDRVRawState *s;
    BDRVRawReopenState *raw_s;
    int ret = 0;

    assert(state != NULL);
    assert(state->bs != NULL);

    s = state->bs->opaque;

    state->opaque = g_malloc0(sizeof(BDRVRawReopenState));
    raw_s = state->opaque;

#ifdef CONFIG_LINUX_AIO
    raw_s->use_aio = s->use_aio;

    /* we can use s->aio_ctx instead of a copy, because the use_aio flag is
     * valid in the 'false' condition even if aio_ctx is set, and raw_set_aio()
     * won't override aio_ctx if aio_ctx is non-NULL */
    if (raw_set_aio(&s->aio_ctx, &raw_s->use_aio, state->flags)) {
        return -1;
    }
#endif

    if (s->type == FTYPE_FD || s->type == FTYPE_CD) {
        raw_s->open_flags |= O_NONBLOCK;
    }

    raw_parse_flags(state->flags, &raw_s->open_flags);

    raw_s->fd = -1;

    int fcntl_flags = O_APPEND | O_ASYNC | O_NONBLOCK;
#ifdef O_NOATIME
    fcntl_flags |= O_NOATIME;
#endif

    if ((raw_s->open_flags & ~fcntl_flags) == (s->open_flags & ~fcntl_flags)) {
        /* dup the original fd */
        /* TODO: use qemu fcntl wrapper */
#ifdef F_DUPFD_CLOEXEC
        raw_s->fd = fcntl(s->fd, F_DUPFD_CLOEXEC, 0);
#else
        raw_s->fd = dup(s->fd);
        if (raw_s->fd != -1) {
            qemu_set_cloexec(raw_s->fd);
        }
#endif
        if (raw_s->fd >= 0) {
            ret = fcntl_setfl(raw_s->fd, raw_s->open_flags);
            if (ret) {
                close(raw_s->fd);
                raw_s->fd = -1;
            }
        }
    }

    /* If we cannot use fcntl, or fcntl failed, fall back to qemu_open() */
    if (raw_s->fd == -1) {
        assert(!(raw_s->open_flags & O_CREAT));
        raw_s->fd = qemu_open(state->bs->filename, raw_s->open_flags);
        if (raw_s->fd == -1) {
            ret = -1;
        }
    }
    return ret;
}


static void raw_reopen_commit(BDRVReopenState *state)
{
    BDRVRawReopenState *raw_s = state->opaque;
    BDRVRawState *s = state->bs->opaque;

    s->open_flags = raw_s->open_flags;

    close(s->fd);
    s->fd = raw_s->fd;
#ifdef CONFIG_LINUX_AIO
    s->use_aio = raw_s->use_aio;
#endif

    g_free(state->opaque);
    state->opaque = NULL;
}


static void raw_reopen_abort(BDRVReopenState *state)
{
    BDRVRawReopenState *raw_s = state->opaque;

     /* nothing to do if NULL, we didn't get far enough */
    if (raw_s == NULL) {
        return;
    }

    if (raw_s->fd >= 0) {
        close(raw_s->fd);
        raw_s->fd = -1;
    }
    g_free(state->opaque);
    state->opaque = NULL;
}


/* XXX: use host sector size if necessary with:
#ifdef DIOCGSECTORSIZE
        {
            unsigned int sectorsize = 512;
            if (!ioctl(fd, DIOCGSECTORSIZE, &sectorsize) &&
                sectorsize > bufsize)
                bufsize = sectorsize;
        }
#endif
#ifdef CONFIG_COCOA
        u_int32_t   blockSize = 512;
        if ( !ioctl( fd, DKIOCGETBLOCKSIZE, &blockSize ) && blockSize > bufsize) {
            bufsize = blockSize;
        }
#endif
*/

static BlockDriverAIOCB *raw_aio_submit(BlockDriverState *bs,
        int64_t sector_num, QEMUIOVector *qiov, int nb_sectors,
        BlockDriverCompletionFunc *cb, void *opaque, int type)
{
    BDRVRawState *s = bs->opaque;

    if (fd_open(bs) < 0)
        return NULL;

    /*
     * Check if buffers need to be copied into a single linear buffer.
     */
    if (s->force_linearize && qiov->niov > 1) {
        type |= QEMU_AIO_MISALIGNED;
    }

    /*
     * If O_DIRECT is used the buffer needs to be aligned on a sector
     * boundary.  Check if this is the case or telll the low-level
     * driver that it needs to copy the buffer.
     */
    if ((bs->open_flags & BDRV_O_NOCACHE) && !bdrv_qiov_is_aligned(bs, qiov)) {
        type |= QEMU_AIO_MISALIGNED;
    }

#ifdef CONFIG_LINUX_AIO
    if (s->use_aio && (type & QEMU_AIO_MISALIGNED) == 0) {
        return laio_submit(bs, s->aio_ctx, s->fd, sector_num, qiov,
                           nb_sectors, cb, opaque, type);
    }
#endif

    return paio_submit(bs, s->fd, sector_num, qiov, nb_sectors,
                       cb, opaque, type);
}

static BlockDriverAIOCB *raw_aio_readv(BlockDriverState *bs,
        int64_t sector_num, QEMUIOVector *qiov, int nb_sectors,
        BlockDriverCompletionFunc *cb, void *opaque)
{
    return raw_aio_submit(bs, sector_num, qiov, nb_sectors,
                          cb, opaque, QEMU_AIO_READ);
}

static BlockDriverAIOCB *raw_aio_writev(BlockDriverState *bs,
        int64_t sector_num, QEMUIOVector *qiov, int nb_sectors,
        BlockDriverCompletionFunc *cb, void *opaque)
{
    return raw_aio_submit(bs, sector_num, qiov, nb_sectors,
                          cb, opaque, QEMU_AIO_WRITE);
}

static BlockDriverAIOCB *raw_aio_flush(BlockDriverState *bs,
        BlockDriverCompletionFunc *cb, void *opaque)
{
    BDRVRawState *s = bs->opaque;

    if (fd_open(bs) < 0)
        return NULL;

    return paio_submit(bs, s->fd, 0, NULL, 0, cb, opaque, QEMU_AIO_FLUSH);
}

static void raw_close(BlockDriverState *bs)
{
    BDRVRawState *s = bs->opaque;
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
}

static int raw_truncate(BlockDriverState *bs, int64_t offset)
{
    BDRVRawState *s = bs->opaque;
    struct stat st;

    if (fstat(s->fd, &st)) {
        return -errno;
    }

    if (S_ISREG(st.st_mode)) {
        if (ftruncate(s->fd, offset) < 0) {
            return -errno;
        }
    } else if (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode)) {
       if (offset > raw_getlength(bs)) {
           return -EINVAL;
       }
    } else {
        return -ENOTSUP;
    }

    return 0;
}

#ifdef __OpenBSD__
static int64_t raw_getlength(BlockDriverState *bs)
{
    BDRVRawState *s = bs->opaque;
    int fd = s->fd;
    struct stat st;

    if (fstat(fd, &st))
        return -1;
    if (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode)) {
        struct disklabel dl;

        if (ioctl(fd, DIOCGDINFO, &dl))
            return -1;
        return (uint64_t)dl.d_secsize *
            dl.d_partitions[DISKPART(st.st_rdev)].p_size;
    } else
        return st.st_size;
}
#else /* !__OpenBSD__ */
static int64_t  raw_getlength(BlockDriverState *bs)
{
    BDRVRawState *s = bs->opaque;
    int fd = s->fd;
    int64_t size;
#ifdef CONFIG_BSD
    struct stat sb;
#if defined (__FreeBSD__) || defined(__FreeBSD_kernel__)
    int reopened = 0;
#endif
#endif
#ifdef __sun__
    struct dk_minfo minfo;
    int rv;
#endif
    int ret;

    ret = fd_open(bs);
    if (ret < 0)
        return ret;

#ifdef CONFIG_BSD
#if defined (__FreeBSD__) || defined(__FreeBSD_kernel__)
again:
#endif
    if (!fstat(fd, &sb) && (S_IFCHR & sb.st_mode)) {
#ifdef DIOCGMEDIASIZE
	if (ioctl(fd, DIOCGMEDIASIZE, (off_t *)&size))
#elif defined(DIOCGPART)
        {
                struct partinfo pi;
                if (ioctl(fd, DIOCGPART, &pi) == 0)
                        size = pi.media_size;
                else
                        size = 0;
        }
        if (size == 0)
#endif
#ifdef CONFIG_COCOA
        size = LONG_LONG_MAX;
#else
        size = lseek(fd, 0LL, SEEK_END);
#endif
#if defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
        switch(s->type) {
        case FTYPE_CD:
            /* XXX FreeBSD acd returns UINT_MAX sectors for an empty drive */
            if (size == 2048LL * (unsigned)-1)
                size = 0;
            /* XXX no disc?  maybe we need to reopen... */
            if (size <= 0 && !reopened && cdrom_reopen(bs) >= 0) {
                reopened = 1;
                goto again;
            }
        }
#endif
    } else
#endif
#ifdef __sun__
    /*
     * use the DKIOCGMEDIAINFO ioctl to read the size.
     */
    rv = ioctl ( fd, DKIOCGMEDIAINFO, &minfo );
    if ( rv != -1 ) {
        size = minfo.dki_lbsize * minfo.dki_capacity;
    } else /* there are reports that lseek on some devices
              fails, but irc discussion said that contingency
              on contingency was overkill */
#endif
    {
        size = lseek(fd, 0, SEEK_END);
    }
    return size;
}
#endif

static int raw_create(const char *filename, QEMUOptionParameter *options)
{
    int fd;
    int result = 0;
    int64_t total_size = 0;

    /* Read out options */
    while (options && options->name) {
        if (!strcmp(options->name, BLOCK_OPT_SIZE)) {
            total_size = options->value.n / BDRV_SECTOR_SIZE;
        }
        options++;
    }

    fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY,
              0644);
    if (fd < 0) {
        result = -errno;
    } else {
        if (ftruncate(fd, total_size * BDRV_SECTOR_SIZE) != 0) {
            result = -errno;
        }
        if (close(fd) != 0) {
            result = -errno;
        }
    }
    return result;
}

#ifdef CONFIG_XFS
static int xfs_discard(BDRVRawState *s, int64_t sector_num, int nb_sectors)
{
    struct xfs_flock64 fl;

    memset(&fl, 0, sizeof(fl));
    fl.l_whence = SEEK_SET;
    fl.l_start = sector_num << 9;
    fl.l_len = (int64_t)nb_sectors << 9;

    if (xfsctl(NULL, s->fd, XFS_IOC_UNRESVSP64, &fl) < 0) {
        DEBUG_BLOCK_PRINT("cannot punch hole (%s)\n", strerror(errno));
        return -errno;
    }

    return 0;
}
#endif

static coroutine_fn int raw_co_discard(BlockDriverState *bs,
    int64_t sector_num, int nb_sectors)
{
#ifdef CONFIG_XFS
    BDRVRawState *s = bs->opaque;

    if (s->is_xfs) {
        return xfs_discard(s, sector_num, nb_sectors);
    }
#endif

    return 0;
}

static QEMUOptionParameter raw_create_options[] = {
    {
        .name = BLOCK_OPT_SIZE,
        .type = OPT_SIZE,
        .help = "Virtual disk size"
    },
    { NULL }
};

static BlockDriver bdrv_file = {
    .format_name = "file",
    .protocol_name = "file",
    .instance_size = sizeof(BDRVRawState),
    .bdrv_probe = NULL, /* no probe for protocols */
    .bdrv_file_open = raw_open,
    .bdrv_reopen_prepare = raw_reopen_prepare,
    .bdrv_reopen_commit = raw_reopen_commit,
    .bdrv_reopen_abort = raw_reopen_abort,
    .bdrv_close = raw_close,
    .bdrv_create = raw_create,
    .bdrv_co_discard = raw_co_discard,

    .bdrv_aio_readv = raw_aio_readv,
    .bdrv_aio_writev = raw_aio_writev,
    .bdrv_aio_flush = raw_aio_flush,

    .bdrv_truncate = raw_truncate,
    .bdrv_getlength = raw_getlength,

    .create_options = raw_create_options,
};

/***********************************************/
/* host device */

#ifdef CONFIG_COCOA
static kern_return_t FindEjectableCDMedia( io_iterator_t *mediaIterator );
static kern_return_t GetBSDPath( io_iterator_t mediaIterator, char *bsdPath, CFIndex maxPathSize );

kern_return_t FindEjectableCDMedia( io_iterator_t *mediaIterator )
{
    kern_return_t       kernResult;
    mach_port_t     masterPort;
    CFMutableDictionaryRef  classesToMatch;

    kernResult = IOMasterPort( MACH_PORT_NULL, &masterPort );
    if ( KERN_SUCCESS != kernResult ) {
        printf( "IOMasterPort returned %d\n", kernResult );
    }

    classesToMatch = IOServiceMatching( kIOCDMediaClass );
    if ( classesToMatch == NULL ) {
        printf( "IOServiceMatching returned a NULL dictionary.\n" );
    } else {
    CFDictionarySetValue( classesToMatch, CFSTR( kIOMediaEjectableKey ), kCFBooleanTrue );
    }
    kernResult = IOServiceGetMatchingServices( masterPort, classesToMatch, mediaIterator );
    if ( KERN_SUCCESS != kernResult )
    {
        printf( "IOServiceGetMatchingServices returned %d\n", kernResult );
    }

    return kernResult;
}

kern_return_t GetBSDPath( io_iterator_t mediaIterator, char *bsdPath, CFIndex maxPathSize )
{
    io_object_t     nextMedia;
    kern_return_t   kernResult = KERN_FAILURE;
    *bsdPath = '\0';
    nextMedia = IOIteratorNext( mediaIterator );
    if ( nextMedia )
    {
        CFTypeRef   bsdPathAsCFString;
    bsdPathAsCFString = IORegistryEntryCreateCFProperty( nextMedia, CFSTR( kIOBSDNameKey ), kCFAllocatorDefault, 0 );
        if ( bsdPathAsCFString ) {
            size_t devPathLength;
            strcpy( bsdPath, _PATH_DEV );
            strcat( bsdPath, "r" );
            devPathLength = strlen( bsdPath );
            if ( CFStringGetCString( bsdPathAsCFString, bsdPath + devPathLength, maxPathSize - devPathLength, kCFStringEncodingASCII ) ) {
                kernResult = KERN_SUCCESS;
            }
            CFRelease( bsdPathAsCFString );
        }
        IOObjectRelease( nextMedia );
    }

    return kernResult;
}

#endif

static int hdev_probe_device(const char *filename)
{
    struct stat st;

    /* allow a dedicated CD-ROM driver to match with a higher priority */
    if (strstart(filename, "/dev/cdrom", NULL))
        return 50;

    if (stat(filename, &st) >= 0 &&
            (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode))) {
        return 100;
    }

    return 0;
}

static int hdev_open(BlockDriverState *bs, const char *filename, int flags)
{
    BDRVRawState *s = bs->opaque;

#ifdef CONFIG_COCOA
    if (strstart(filename, "/dev/cdrom", NULL)) {
        kern_return_t kernResult;
        io_iterator_t mediaIterator;
        char bsdPath[ MAXPATHLEN ];
        int fd;

        kernResult = FindEjectableCDMedia( &mediaIterator );
        kernResult = GetBSDPath( mediaIterator, bsdPath, sizeof( bsdPath ) );

        if ( bsdPath[ 0 ] != '\0' ) {
            strcat(bsdPath,"s0");
            /* some CDs don't have a partition 0 */
            fd = open(bsdPath, O_RDONLY | O_BINARY | O_LARGEFILE);
            if (fd < 0) {
                bsdPath[strlen(bsdPath)-1] = '1';
            } else {
                close(fd);
            }
            filename = bsdPath;
        }

        if ( mediaIterator )
            IOObjectRelease( mediaIterator );
    }
#endif

    s->type = FTYPE_FILE;
#if defined(__linux__)
    if (strstart(filename, "/dev/sg", NULL)) {
        bs->sg = 1;
    }
#endif

    return raw_open_common(bs, filename, flags, 0);
}

#if defined(__linux__)
/* Note: we do not have a reliable method to detect if the floppy is
   present. The current method is to try to open the floppy at every
   I/O and to keep it opened during a few hundreds of ms. */
static int fd_open(BlockDriverState *bs)
{
    BDRVRawState *s = bs->opaque;
    int last_media_present;

    if (s->type != FTYPE_FD)
        return 0;
    last_media_present = (s->fd >= 0);
    if (s->fd >= 0 &&
        (qemu_get_clock(rt_clock) - s->fd_open_time) >= FD_OPEN_TIMEOUT) {
        close(s->fd);
        s->fd = -1;
#ifdef DEBUG_FLOPPY
        printf("Floppy closed\n");
#endif
    }
    if (s->fd < 0) {
        if (s->fd_got_error &&
            (qemu_get_clock(rt_clock) - s->fd_error_time) < FD_OPEN_TIMEOUT) {
#ifdef DEBUG_FLOPPY
            printf("No floppy (open delayed)\n");
#endif
            return -EIO;
        }
        s->fd = open(bs->filename, s->open_flags & ~O_NONBLOCK);
        if (s->fd < 0) {
            s->fd_error_time = qemu_get_clock(rt_clock);
            s->fd_got_error = 1;
            if (last_media_present)
                s->fd_media_changed = 1;
#ifdef DEBUG_FLOPPY
            printf("No floppy\n");
#endif
            return -EIO;
        }
#ifdef DEBUG_FLOPPY
        printf("Floppy opened\n");
#endif
    }
    if (!last_media_present)
        s->fd_media_changed = 1;
    s->fd_open_time = qemu_get_clock(rt_clock);
    s->fd_got_error = 0;
    return 0;
}

static int hdev_ioctl(BlockDriverState *bs, unsigned long int req, void *buf)
{
    BDRVRawState *s = bs->opaque;

    return ioctl(s->fd, req, buf);
}

static BlockDriverAIOCB *hdev_aio_ioctl(BlockDriverState *bs,
        unsigned long int req, void *buf,
        BlockDriverCompletionFunc *cb, void *opaque)
{
    BDRVRawState *s = bs->opaque;

    if (fd_open(bs) < 0)
        return NULL;
    return paio_ioctl(bs, s->fd, req, buf, cb, opaque);
}

#elif defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
static int fd_open(BlockDriverState *bs)
{
    BDRVRawState *s = bs->opaque;

    /* this is just to ensure s->fd is sane (its called by io ops) */
    if (s->fd >= 0)
        return 0;
    return -EIO;
}
#else /* !linux && !FreeBSD */

static int fd_open(BlockDriverState *bs)
{
    return 0;
}

#endif /* !linux && !FreeBSD */

static int hdev_create(const char *filename, QEMUOptionParameter *options)
{
    int fd;
    int ret = 0;
    struct stat stat_buf;
    int64_t total_size = 0;

    /* Read out options */
    while (options && options->name) {
        if (!strcmp(options->name, "size")) {
            total_size = options->value.n / BDRV_SECTOR_SIZE;
        }
        options++;
    }

    fd = open(filename, O_WRONLY | O_BINARY);
    if (fd < 0)
        return -EIO;

    if (fstat(fd, &stat_buf) < 0)
        ret = -EIO;
    else if (!S_ISBLK(stat_buf.st_mode) && !S_ISCHR(stat_buf.st_mode))
        ret = -EIO;
    else if (lseek(fd, 0, SEEK_END) < total_size * BDRV_SECTOR_SIZE)
        ret = -ENOSPC;

    close(fd);
    return ret;
}

static int hdev_has_zero_init(BlockDriverState *bs)
{
    return 0;
}

static BlockDriver bdrv_host_device = {
    .format_name        = "host_device",
    .protocol_name        = "host_device",
    .instance_size      = sizeof(BDRVRawState),
    .bdrv_probe_device  = hdev_probe_device,
    .bdrv_file_open     = hdev_open,
    .bdrv_close         = raw_close,
    .bdrv_reopen_prepare = raw_reopen_prepare,
    .bdrv_reopen_commit  = raw_reopen_commit,
    .bdrv_reopen_abort   = raw_reopen_abort,
    .bdrv_create        = hdev_create,
    .create_options     = raw_create_options,
    .bdrv_has_zero_init = hdev_has_zero_init,

    .bdrv_aio_readv	= raw_aio_readv,
    .bdrv_aio_writev	= raw_aio_writev,
    .bdrv_aio_flush	= raw_aio_flush,

    .bdrv_truncate      = raw_truncate,
    .bdrv_getlength	= raw_getlength,

    /* generic scsi device */
#ifdef __linux__
    .bdrv_ioctl         = hdev_ioctl,
    .bdrv_aio_ioctl     = hdev_aio_ioctl,
#endif
};

#ifdef __linux__
static int floppy_open(BlockDriverState *bs, const char *filename, int flags)
{
    BDRVRawState *s = bs->opaque;
    int ret;

    s->type = FTYPE_FD;

    /* open will not fail even if no floppy is inserted, so add O_NONBLOCK */
    ret = raw_open_common(bs, filename, flags, O_NONBLOCK);
    if (ret)
        return ret;

    /* close fd so that we can reopen it as needed */
    close(s->fd);
    s->fd = -1;
    s->fd_media_changed = 1;

    return 0;
}

static int floppy_probe_device(const char *filename)
{
    if (strstart(filename, "/dev/fd", NULL))
        return 100;
    return 0;
}


static int floppy_is_inserted(BlockDriverState *bs)
{
    return fd_open(bs) >= 0;
}

static int floppy_media_changed(BlockDriverState *bs)
{
    BDRVRawState *s = bs->opaque;
    int ret;

    /*
     * XXX: we do not have a true media changed indication.
     * It does not work if the floppy is changed without trying to read it.
     */
    fd_open(bs);
    ret = s->fd_media_changed;
    s->fd_media_changed = 0;
#ifdef DEBUG_FLOPPY
    printf("Floppy changed=%d\n", ret);
#endif
    return ret;
}

static void floppy_eject(BlockDriverState *bs, bool eject_flag)
{
    BDRVRawState *s = bs->opaque;
    int fd;

    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
    fd = open(bs->filename, s->open_flags | O_NONBLOCK);
    if (fd >= 0) {
        if (ioctl(fd, FDEJECT, 0) < 0)
            perror("FDEJECT");
        close(fd);
    }
}

static BlockDriver bdrv_host_floppy = {
    .format_name        = "host_floppy",
    .protocol_name      = "host_floppy",
    .instance_size      = sizeof(BDRVRawState),
    .bdrv_probe_device	= floppy_probe_device,
    .bdrv_file_open     = floppy_open,
    .bdrv_close         = raw_close,
    .bdrv_reopen_prepare = raw_reopen_prepare,
    .bdrv_reopen_commit  = raw_reopen_commit,
    .bdrv_reopen_abort   = raw_reopen_abort,
    .bdrv_create        = hdev_create,
    .create_options     = raw_create_options,
    .bdrv_has_zero_init = hdev_has_zero_init,

    .bdrv_aio_readv     = raw_aio_readv,
    .bdrv_aio_writev    = raw_aio_writev,
    .bdrv_aio_flush	= raw_aio_flush,

    .bdrv_truncate      = raw_truncate,
    .bdrv_getlength	= raw_getlength,

    /* removable device support */
    .bdrv_is_inserted   = floppy_is_inserted,
    .bdrv_media_changed = floppy_media_changed,
    .bdrv_eject         = floppy_eject,
};

static int cdrom_open(BlockDriverState *bs, const char *filename, int flags)
{
    BDRVRawState *s = bs->opaque;

    s->type = FTYPE_CD;

    /* open will not fail even if no CD is inserted, so add O_NONBLOCK */
    return raw_open_common(bs, filename, flags, O_NONBLOCK);
}

static int cdrom_probe_device(const char *filename)
{
    int fd, ret;
    int prio = 0;

    if (strstart(filename, "/dev/cd", NULL))
        prio = 50;

    fd = open(filename, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        goto out;
    }

    /* Attempt to detect via a CDROM specific ioctl */
    ret = ioctl(fd, CDROM_DRIVE_STATUS, CDSL_CURRENT);
    if (ret >= 0)
        prio = 100;

    close(fd);
out:
    return prio;
}

static int cdrom_is_inserted(BlockDriverState *bs)
{
    BDRVRawState *s = bs->opaque;
    int ret;

    ret = ioctl(s->fd, CDROM_DRIVE_STATUS, CDSL_CURRENT);
    if (ret == CDS_DISC_OK)
        return 1;
    return 0;
}

static void cdrom_eject(BlockDriverState *bs, bool eject_flag)
{
    BDRVRawState *s = bs->opaque;

    if (eject_flag) {
        if (ioctl(s->fd, CDROMEJECT, NULL) < 0)
            perror("CDROMEJECT");
    } else {
        if (ioctl(s->fd, CDROMCLOSETRAY, NULL) < 0)
            perror("CDROMEJECT");
    }
}

static void cdrom_lock_medium(BlockDriverState *bs, bool locked)
{
    BDRVRawState *s = bs->opaque;

    if (ioctl(s->fd, CDROM_LOCKDOOR, locked) < 0) {
        /*
         * Note: an error can happen if the distribution automatically
         * mounts the CD-ROM
         */
        /* perror("CDROM_LOCKDOOR"); */
    }
}

static BlockDriver bdrv_host_cdrom = {
    .format_name        = "host_cdrom",
    .protocol_name      = "host_cdrom",
    .instance_size      = sizeof(BDRVRawState),
    .bdrv_probe_device	= cdrom_probe_device,
    .bdrv_file_open     = cdrom_open,
    .bdrv_close         = raw_close,
    .bdrv_reopen_prepare = raw_reopen_prepare,
    .bdrv_reopen_commit  = raw_reopen_commit,
    .bdrv_reopen_abort   = raw_reopen_abort,
    .bdrv_create        = hdev_create,
    .create_options     = raw_create_options,
    .bdrv_has_zero_init = hdev_has_zero_init,

    .bdrv_aio_readv     = raw_aio_readv,
    .bdrv_aio_writev    = raw_aio_writev,
    .bdrv_aio_flush	= raw_aio_flush,

    .bdrv_truncate      = raw_truncate,
    .bdrv_getlength     = raw_getlength,

    /* removable device support */
    .bdrv_is_inserted   = cdrom_is_inserted,
    .bdrv_eject         = cdrom_eject,
    .bdrv_lock_medium   = cdrom_lock_medium,

    /* generic scsi device */
    .bdrv_ioctl         = hdev_ioctl,
    .bdrv_aio_ioctl     = hdev_aio_ioctl,
};
#endif /* __linux__ */

#if defined (__FreeBSD__) || defined(__FreeBSD_kernel__)
static int cdrom_open(BlockDriverState *bs, const char *filename, int flags)
{
    BDRVRawState *s = bs->opaque;
    int ret;

    s->type = FTYPE_CD;

    ret = raw_open_common(bs, filename, flags, 0);
    if (ret)
        return ret;

    /* make sure the door isnt locked at this time */
    ioctl(s->fd, CDIOCALLOW);
    return 0;
}

static int cdrom_probe_device(const char *filename)
{
    if (strstart(filename, "/dev/cd", NULL) ||
            strstart(filename, "/dev/acd", NULL))
        return 100;
    return 0;
}

static int cdrom_reopen(BlockDriverState *bs)
{
    BDRVRawState *s = bs->opaque;
    int fd;

    /*
     * Force reread of possibly changed/newly loaded disc,
     * FreeBSD seems to not notice sometimes...
     */
    if (s->fd >= 0)
        close(s->fd);
    fd = open(bs->filename, s->open_flags, 0644);
    if (fd < 0) {
        s->fd = -1;
        return -EIO;
    }
    s->fd = fd;

    /* make sure the door isnt locked at this time */
    ioctl(s->fd, CDIOCALLOW);
    return 0;
}

static int cdrom_is_inserted(BlockDriverState *bs)
{
    return raw_getlength(bs) > 0;
}

static void cdrom_eject(BlockDriverState *bs, bool eject_flag)
{
    BDRVRawState *s = bs->opaque;

    if (s->fd < 0)
        return;

    (void) ioctl(s->fd, CDIOCALLOW);

    if (eject_flag) {
        if (ioctl(s->fd, CDIOCEJECT) < 0)
            perror("CDIOCEJECT");
    } else {
        if (ioctl(s->fd, CDIOCCLOSE) < 0)
            perror("CDIOCCLOSE");
    }

    cdrom_reopen(bs);
}

static void cdrom_lock_medium(BlockDriverState *bs, bool locked)
{
    BDRVRawState *s = bs->opaque;

    if (s->fd < 0)
        return;
    if (ioctl(s->fd, (locked ? CDIOCPREVENT : CDIOCALLOW)) < 0) {
        /*
         * Note: an error can happen if the distribution automatically
         * mounts the CD-ROM
         */
        /* perror("CDROM_LOCKDOOR"); */
    }
}

static BlockDriver bdrv_host_cdrom = {
    .format_name        = "host_cdrom",
    .protocol_name      = "host_cdrom",
    .instance_size      = sizeof(BDRVRawState),
    .bdrv_probe_device	= cdrom_probe_device,
    .bdrv_file_open     = cdrom_open,
    .bdrv_close         = raw_close,
    .bdrv_reopen_prepare = raw_reopen_prepare,
    .bdrv_reopen_commit  = raw_reopen_commit,
    .bdrv_reopen_abort   = raw_reopen_abort,
    .bdrv_create        = hdev_create,
    .create_options     = raw_create_options,
    .bdrv_has_zero_init = hdev_has_zero_init,

    .bdrv_aio_readv     = raw_aio_readv,
    .bdrv_aio_writev    = raw_aio_writev,
    .bdrv_aio_flush	= raw_aio_flush,

    .bdrv_truncate      = raw_truncate,
    .bdrv_getlength     = raw_getlength,

    /* removable device support */
    .bdrv_is_inserted   = cdrom_is_inserted,
    .bdrv_eject         = cdrom_eject,
    .bdrv_lock_medium   = cdrom_lock_medium,
};
#endif /* __FreeBSD__ */

#ifdef CONFIG_LINUX_AIO
/**
 * Return the file descriptor for Linux AIO
 *
 * This function is a layering violation and should be removed when it becomes
 * possible to call the block layer outside the global mutex.  It allows the
 * caller to hijack the file descriptor so I/O can be performed outside the
 * block layer.
 */
int raw_get_aio_fd(BlockDriverState *bs)
{
    BDRVRawState *s;

    if (!bs->drv) {
        return -ENOMEDIUM;
    }

    if (bs->drv == bdrv_find_format("raw")) {
        bs = bs->file;
    }

    /* raw-posix has several protocols so just check for raw_aio_readv */
    if (bs->drv->bdrv_aio_readv != raw_aio_readv) {
        return -ENOTSUP;
    }

    s = bs->opaque;
    if (!s->use_aio) {
        return -ENOTSUP;
    }
    return s->fd;
}
#endif /* CONFIG_LINUX_AIO */

static void bdrv_file_init(void)
{
    /*
     * Register all the drivers.  Note that order is important, the driver
     * registered last will get probed first.
     */
    bdrv_register(&bdrv_file);
    bdrv_register(&bdrv_host_device);
#ifdef __linux__
    bdrv_register(&bdrv_host_floppy);
    bdrv_register(&bdrv_host_cdrom);
#endif
#if defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
    bdrv_register(&bdrv_host_cdrom);
#endif
}

block_init(bdrv_file_init);
