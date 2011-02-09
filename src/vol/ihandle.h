/*
 * Copyright 2000, International Business Machines Corporation and others.
 * All Rights Reserved.
 * 
 * This software has been released under the terms of the IBM Public
 * License.  For details, see the LICENSE file in the top-level source
 * directory or online at http://www.openafs.org/dl/license10.html
 */

/* An IHandle_t is an abstraction allowing the file and volume operations to
 * pass the elements required to identify a file to the underlying file
 * systen. For the usual Vice inode operations, this is no more than the
 * usual device and inode numbers. For the user space file system used on NT
 * we also need the volume id to identify the file. 
 *
 * An FdHandle_t is an abstraction used to associate file descroptors
 * with Inode handles. IH_OPEN is used to get a file descriptor that
 * can be used in subsequent I/O operations. File descriptor handles are
 * cached by IO_CLOSE. To make sure a file descriptor is really closed call
 * IH_REALLYCLOSE.
 *
 * The IHandle_t also provides a place to do other optimizations. In the
 * NT user space file system, we keep a separate special file for the
 * link counts and using the IHandle_t allows keeping the details of
 * that at a lower level than the IDEC and IINC calls.
 *
 * To use the IHandle_t there are a new set of IH_xxxx/FDH_XXXX operations.
 * Each takes a pointer to an IHandle_t or an FdHandle_t as the first
 * argument. This pointer is  considered an in/out variable. In particular,
 * the open file descriptors for a given Inode are stored in a linked list
 * of FdHandle_t hanging off of each IHandle_t. IH_OPEN returns NULL on error,
 * and a valid FdHandle_t otherwise. All other IH_xxxx/FDH_xxxx macros return
 * -1 on error and 0 on success.
 *
 * Inode handle operations:
 * IH_INIT - Initialize the Inode handle with the device, volume id, and ino
 * IH_COPY - Copy Inode handle info to a new handle with no open descriptors.
 * IH_REALLYCLOSE - Close all cached file descriptors for Inode handle
 * IH_RELEASE - release a Inode handle, close all cached file descriptors
 * IH_CONDSYNC -  snyc the Inode if it has any open file descriptors
 *
 * Inode operation replacements:
 * IH_CREATE - create a file in the underlying filesystem and setup the
 *	information needed to reference this file in the IHandle_t.
 * IH_OPEN - open the file belonging to the associated inode and set the
 *	file descriptor.
 * IH_IREAD/IH_IWRITE - read/write an Inode.
 * IH_INC/IH_DEC - increment/decrement the link count.
 *
 * Replacements for C runtime file operations
 * FDH_READ/FDH_WRITE - read/write using the file descriptor.
 * FDH_READV/FDH_WRITEV - readv/writev (Unix only)
 * FDH_SEEK - set file handle's read/write position
 * FDH_CLOSE - return a file descriptor to the cache
 * FDH_REALLYCLOSE - Close a file descriptor, do not return to the cache
 * FDH_SYNC - Unconditionally sync an open file.
 * FDH_TRUNC - Truncate a file
 *
 * status information:
 * FDH_SIZE - returns the size of the file.
 * FDH_NLINK - returns the link count of the file.
 *
 * Miscellaneous:
 * FDH_FDOPEN - create a descriptor for buffered I/O
 * STREAM_READ/STREAM_WRITE - buffered file I/O
 */

#ifndef _IHANDLE_H_
#define _IHANDLE_H_

#ifdef AFS_PTHREAD_ENV
#include <pthread.h>
extern pthread_once_t ih_glock_once;
extern pthread_mutex_t ih_glock_mutex;
extern void ih_glock_init(void);
#define IH_LOCK \
    do { osi_Assert(pthread_once(&ih_glock_once, ih_glock_init) == 0); \
	MUTEX_ENTER(&ih_glock_mutex); \
    } while (0)
#define IH_UNLOCK MUTEX_EXIT(&ih_glock_mutex)
#else /* AFS_PTHREAD_ENV */
#define IH_LOCK
#define IH_UNLOCK
#endif /* AFS_PTHREAD_ENV */

#ifndef DLL_INIT_LIST
/*
 * Macro to initialize a doubly linked list, lifted from Encina
 */
#define DLL_INIT_LIST(head, tail)	\
    do {			 	\
	(head) = NULL;			\
	(tail) = NULL;			\
    } while(0)

/*
 * Macro to remove an element from a doubly linked list
 */
#define DLL_DELETE(ptr,head,tail,next,prev)	\
    do {					\
	if ((ptr)->next) 			\
	    (ptr)->next->prev = (ptr)->prev;	\
	else					\
	    (tail) = (ptr)->prev;		\
	if ((ptr)->prev) 			\
	    (ptr)->prev->next = (ptr)->next;	\
	else 					\
	    (head) = (ptr)->next;		\
	(ptr)->next = (ptr)->prev = NULL;	\
	osi_Assert(!(head) || !((head)->prev)); \
    } while(0)

/*
 * Macro to insert an element at the tail of a doubly linked list
 */
#define DLL_INSERT_TAIL(ptr,head,tail,next,prev) \
    do {					 \
	(ptr)->next = NULL;			 \
        (ptr)->prev = (tail);			 \
	(tail) = (ptr);				 \
	if ((ptr)->prev) 			 \
	    (ptr)->prev->next = (ptr);		 \
	else					 \
	    (head) = (ptr);			 \
	osi_Assert((head) && ((head)->prev == NULL)); \
    } while(0)

/*
 * Macro to insert an element at the head of a doubly linked list
 */
#define DLL_INSERT_HEAD(ptr,head,tail,next,prev) \
    do {                                         \
        (ptr)->next = (head);                    \
        (ptr)->prev = NULL;                      \
        (head) = (ptr);                          \
        if ((ptr)->next)                         \
            (ptr)->next->prev = (ptr);           \
        else                                     \
            (tail) = (ptr);                      \
        osi_Assert((tail) && ((tail)->next == NULL)); \
    } while(0)

#endif /* DLL_INIT_LIST */

#ifdef AFS_NT40_ENV
typedef __int64 Inode;
#else
#include <afs/afssyscalls.h>
#endif

/* The dir package's page hashing function is dependent upon the layout of
 * IHandle_t as well as the containing DirHandle in viced/viced.h. Make
 * Sure the volume id is still used as the hash after any changes to either
 * structure.
 */

/* forward declaration */
struct IHandle_s;

/* File descriptors are HANDLE's on NT. The following typedef helps catch
 * type errors. duplicated in libadmin/vos/afs_vosAdmin.c
 */
#ifdef AFS_NT40_ENV
typedef HANDLE FD_t;
#else
typedef int FD_t;
#endif
#define INVALID_FD ((FD_t)-1)

/* file descriptor handle */
typedef struct FdHandle_s {
    int fd_status;		/* status flags */
    FD_t fd_fd;			/* file descriptor */
    struct IHandle_s *fd_ih;	/* Pointer to Inode handle */
    struct FdHandle_s *fd_next;	/* LRU/Avail list pointers */
    struct FdHandle_s *fd_prev;
    struct FdHandle_s *fd_ihnext;	/* Inode handle's list of file descriptors */
    struct FdHandle_s *fd_ihprev;
} FdHandle_t;

/* File descriptor status values */
#define FD_HANDLE_AVAIL		1	/* handle is not open and available */
#define FD_HANDLE_OPEN		2	/* handle is open and not in use */
#define FD_HANDLE_INUSE		3	/* handle is open and in use */

/* buffered file descriptor handle */
#define STREAM_HANDLE_BUFSIZE	2048	/* buffer size for STR_READ/STR_WRITE */
typedef struct StreamHandle_s {
    FD_t str_fd;		/* file descriptor */
    int str_direction;		/* current read/write direction */
    afs_sfsize_t str_buflen;	/* bytes remaining in buffer */
    afs_foff_t str_bufoff;	/* current offset into buffer */
    int str_error;		/* error code */
    int str_eof;		/* end of file flag */
    struct StreamHandle_s *str_next;	/* Avail list pointers */
    struct StreamHandle_s *str_prev;
    char str_buffer[STREAM_HANDLE_BUFSIZE];	/* data buffer */
} StreamHandle_t;

#define STREAM_DIRECTION_NONE	1	/* stream is in initial mode */
#define STREAM_DIRECTION_READ	2	/* stream is in input mode */
#define STREAM_DIRECTION_WRITE	3	/* stream is in output mode */

/* number handles allocated at a shot */
#define I_HANDLE_MALLOCSIZE	((size_t)((4096/sizeof(IHandle_t))))
#define FD_HANDLE_MALLOCSIZE	((size_t)((4096/sizeof(FdHandle_t))))
#define STREAM_HANDLE_MALLOCSIZE 1


/* READ THIS.
 *
 * On modern platforms tuned for I/O intensive workloads, there may be
 * thousands of file descriptors available (64K on 32-bit Solaris 7,
 * for example), and threading in Solaris 9 and Linux 2.6 (NPTL) are
 * tuned for (many) thousands of concurrent threads at peak.
 *
 * On these platforms, it makes sense to allow administrators to set
 * appropriate limits for their hardware.  Clients may now set desired
 * values in the exported vol_io_params, of type ih_init_params.
 */

typedef struct ih_init_params
{
    afs_uint32 fd_handle_setaside; /* for non-cached i/o, trad. was 128 */
    afs_uint32 fd_initial_cachesize; /* what was 'default' */
    afs_uint32 fd_max_cachesize; /* max open files if large-cache activated */
} ih_init_params;


/* Number of file descriptors needed for non-cached I/O */
#define FD_HANDLE_SETASIDE	128 /* Match to MAX_FILESERVER_THREAD */

/* Which systems have 8-bit fileno?  On GNU/Linux systems, the
 * fileno member of FILE is an int.  On NetBSD 5, it's a short.
 * Ditto for OpenBSD 4.5. Through Solaris 10 8/07 it's unsigned char.
 */

/* Don't try to have more than 256 files open at once if you are planning
 * to use fopen or fdopen. The FILE structure has an eight bit field for
 * the file descriptor.  */
#define FD_DEFAULT_CACHESIZE (255-FD_HANDLE_SETASIDE)

/* We need some limit on the number of files open at once. Some systems
 * say we can open lots of files, but when we do they run out of slots
 * in the file table.
 */
#define FD_MAX_CACHESIZE (2000 - FD_HANDLE_SETASIDE)

#if defined(BUILDING_RXOSD) && defined(AFS_RXOSD_SPECIAL)
#include <dirent.h>
struct ih_posix_ops {
    int         (*open)(const char *, int, ...);
    int         (*close)(int);
    int         (*read)(int, void *, size_t);
    ssize_t     (*readv)(int, const struct iovec *, int);
    ssize_t     (*write)(int, const void *, size_t);
    ssize_t     (*writev)(int, const struct iovec *, int);
    afs_int64   (*lseek)(int, afs_int64, int);
    int         (*fsync)(int);
    int         (*unlink)(const char *);
    int         (*mkdir)(const char *, mode_t);
    int         (*rmdir)(const char *);
    int         (*chmod)(const char *, mode_t);
    int         (*chown)(const char *, uid_t, gid_t);
    int         (*stat64)(char *, struct afs_stat *);
    int         (*fstat64)(int, struct afs_stat *);
    int         (*rename)(const char *, const char *);
    DIR *       (*opendir)(const char *);
    struct dirent * (*readdir)(DIR *);
    int         (*closedir)(DIR *);
    int         (*hardlink)(const char*, const char*);
#if AFS_HAVE_STATVFS || AFS_HAVE_STATVFS64
    int         (*statvfs)(const char *, struct afs_statvfs *);
#else
    int         (*statfs)(const char *, struct afs_statfs *);
#endif
};
#endif

/* On modern platforms, this is sized higher than the note implies.
 * For HP, see http://forums11.itrc.hp.com/service/forums/questionanswer.do?admit=109447626+1242508538748+28353475&threadId=302950
 * On AIX, it's said to be self-tuning (sar -v)
 * On Solaris, http://www.princeton.edu/~unix/Solaris/troubleshoot/kerntune.html
 * says stdio limit (FILE) may exist, but then backtracks and says the 64bit
 * solaris and POLL (rather than select) io avoid the issue.  Solaris Internals
 * states that Solaris 7 and above deal with up to 64K on 32bit.
 * However, extended FILE must be enabled to use this. See
 * enable_extended_FILE_stdio(3C)
 */

/* Inode handle */
typedef struct IHandle_s {
    afs_uint32 ih_vid;		/* Parent volume id. */
    int ih_dev;			/* device id. */
    int ih_flags;		/* Flags */
    int ih_synced;		/* should be synced next time */
    Inode ih_ino;		/* Inode number */
    int ih_refcnt;		/* reference count */
    struct FdHandle_s *ih_fdhead;	/* List of open file desciptors */
    struct FdHandle_s *ih_fdtail;
    struct IHandle_s *ih_next;	/* Links for avail list/hash chains */
    struct IHandle_s *ih_prev;
#if defined(BUILDING_RXOSD) && defined(AFS_RXOSD_SPECIAL)
    struct ih_posix_ops *ih_ops;
#endif
} IHandle_t;

/* Flags for the Inode handle */
#define IH_REALLY_CLOSED		1
#define IH_HSM_FILESYSTEM               8
#ifdef AFS_RXOSD_SUPPORT
#define IH_LINKTABLE_V1                 0x100
#define IH_LINKTABLE_V2                 0x200
#define IH_LINKTABLE_VERSIONS           0x300
#ifdef AFS_DCACHE_SUPPORT
#define IH_DCACHE_FILE                  0x800
#endif
#endif

/* Hash function for inode handles */
#define I_HANDLE_HASH_SIZE	2048	/* power of 2 */

/* The casts to int's ensure NT gets the xor operation correct. */
#define IH_HASH(D, V, I) ((int)(((D)^(V)^((int)(I)))&(I_HANDLE_HASH_SIZE-1)))

/*
 * Hash buckets for inode handles
 */
typedef struct IHashBucket_s {
    IHandle_t *ihash_head;
    IHandle_t *ihash_tail;
} IHashBucket_t;

/* Prototypes for handle support routines. */
#ifdef AFS_NAMEI_ENV
#ifdef AFS_NT40_ENV
#include "ntops.h"
#else
#include "namei_ops.h"
#endif
extern void ih_clear(IHandle_t * h);
extern Inode ih_create(IHandle_t * h, int dev, char *part, Inode nI, int p1,
		       int p2, int p3, int p4);
extern FILE *ih_fdopen(FdHandle_t * h, char *fdperms);
#endif /* AFS_NAMEI_ENV */

/*
 * Prototypes for file descriptor cache routines
 */
extern void ih_PkgDefaults(void);
extern void ih_Initialize(void);
extern void ih_UseLargeCache(void);
extern IHandle_t *ih_init(int /*@alt Device@ */ dev, int /*@alt VolId@ */ vid,
			  Inode ino);
extern IHandle_t *ih_copy(IHandle_t * ihP);
extern FdHandle_t *ih_open(IHandle_t * ihP);
extern FdHandle_t *ih_reopen(IHandle_t * ihP);
extern FdHandle_t *ih_fakeopen(IHandle_t * ihP, int open_fd);
extern int fd_close(FdHandle_t * fdP);
extern int fd_reallyclose(FdHandle_t * fdP);
extern StreamHandle_t *stream_fdopen(FD_t fd);
extern StreamHandle_t *stream_open(const char *file, const char *mode);
extern afs_sfsize_t stream_read(void *ptr, afs_fsize_t size,
				afs_fsize_t nitems, StreamHandle_t * streamP);
extern afs_sfsize_t stream_write(void *ptr, afs_fsize_t size,
				 afs_fsize_t nitems,
				 StreamHandle_t * streamP);
extern int stream_seek(StreamHandle_t * streamP, afs_foff_t offset,
		       int whence);
extern int stream_flush(StreamHandle_t * streamP);
extern int stream_close(StreamHandle_t * streamP, int reallyClose);
extern int ih_reallyclose(IHandle_t * ihP);
extern int ih_release(IHandle_t * ihP);
extern int ih_condsync(IHandle_t * ihP);

/* Macros common to user space and inode API's. */
#define IH_INIT(H, D, V, I) ((H) = ih_init((D), (V), (I)))

#define IH_COPY(D, S) ((D) = ih_copy(S))

#define IH_NLINK(H) ih_nlink(H)

#define IH_OPEN(H) ih_open(H)

#define IH_REOPEN(H) ih_reopen(H)

#define FDH_CLOSE(H) (fd_close(H), (H)=NULL, 0)

#define FDH_REALLYCLOSE(H) (fd_reallyclose(H), (H)=NULL, 0)

#define FDH_FDOPEN(H, A) stream_fdopen((H)->fd_fd)

#define STREAM_FDOPEN(A, B) stream_fdopen(A)

#define STREAM_OPEN(A, B) stream_open(A, B)

#define STREAM_READ(A, B, C, H) stream_read(A, B, C, H)

#define STREAM_WRITE(A, B, C, H) stream_write(A, B, C, H)

#define STREAM_SEEK(H, A, B) stream_seek(H, A, B)

#define STREAM_FLUSH(H) stream_flush(H)

#define STREAM_ERROR(H) ((H)->str_error)

#define STREAM_EOF(H) ((H)->str_eof)

#define STREAM_CLOSE(H) stream_close(H, 0)

#define STREAM_REALLYCLOSE(H) stream_close(H, 1)

#define IH_RELEASE(H) (ih_release(H), (H)=NULL, 0)

#define IH_REALLYCLOSE(H) ih_reallyclose(H)

#define IH_CONDSYNC(H) ih_condsync(H)

#ifdef AFS_NAMEI_ENV

#ifdef AFS_NT40_ENV
#define IH_CREATE(H, D, P, N, P1, P2, P3, P4) \
	nt_icreate(H, P, P1, P2, P3, P4)

#define OS_IOPEN(H) nt_iopen(H)
#define OS_OPEN(F, M, P) nt_open(F, M, P)
#define OS_CLOSE(FD) nt_close(FD)

#define OS_READ(FD, B, S) nt_read(FD, B, S)
#define OS_WRITE(FD, B, S) nt_write(FD, B, S)
#define OS_SEEK(FD, O, F) nt_seek(FD, O, F)

#define OS_SYNC(FD) nt_fsync(FD)
#define OS_TRUNC(FD, L) nt_ftruncate(FD, L)
#define OS_SIZE(FD) nt_size(FD)

#define IH_INC(H, I, P) nt_inc(H, I, P)
#define IH_DEC(H, I, P) nt_dec(H, I, P)
#define IH_IREAD(H, O, B, S) nt_iread(H, O, B, S)
#define IH_IWRITE(H, O, B, S) nt_iwrite(H, O, B, S)

#else /* AFS_NT40_ENV */

/*@+fcnmacros +macrofcndecl@*/
#ifdef S_SPLINT_S
extern Inode IH_CREATE(IHandle_t * H, int /*@alt Device @ */ D,
		       char *P, Inode N, int /*@alt VolumeId @ */ P1,
		       int /*@alt VnodeId @ */ P2,
		       int /*@alt Unique @ */ P3,
		       int /*@alt unsigned @ */ P4);
extern FD_t OS_IOPEN(IHandle_t * H);
extern int OS_OPEN(const char *F, int M, mode_t P);
extern int OS_CLOSE(int FD);
extern ssize_t OS_READ(int FD, void *B, size_t S);
extern ssize_t OS_WRITE(int FD, void *B, size_t S);
extern int OS_SYNC(int FD);
extern afs_sfsize_t OS_SIZE(int FD);
extern int IH_INC(IHandle_t * H, Inode I, int /*@alt VolId, VolumeId @ */ P);
extern int IH_DEC(IHandle_t * H, Inode I, int /*@alt VolId, VolumeId @ */ P);
extern afs_sfsize_t IH_IREAD(IHandle_t * H, afs_foff_t O, void *B,
			     afs_fsize_t S);
extern afs_sfsize_t IH_IWRITE(IHandle_t * H, afs_foff_t O, void *B,
			      afs_fsize_t S);
#ifdef O_LARGEFILE
extern off64_t OS_SEEK(int FD, off64_t O, int F);
extern int OS_TRUNC(int FD, off64_t L);
#else /* !O_LARGEFILE */
extern off_t OS_SEEK(int FD, off_t O, int F);
extern int OS_TRUNC(int FD, off_t L);
#endif /* !O_LARGEFILE */
#endif /*S_SPLINT_S */

#define IH_CREATE(H, D, P, N, P1, P2, P3, P4) \
	namei_icreate(H, P, P1, P2, P3, P4)

#define OS_IOPEN(H) namei_iopen(H)
#ifdef O_LARGEFILE
#define OS_OPEN(F, M, P) open64(F, M, P)
#else /* !O_LARGEFILE */
#define OS_OPEN(F, M, P) open(F, M, P)
#endif /* !O_LARGEFILE */
#define OS_CLOSE(FD) close(FD)

#define OS_READ(FD, B, S) read(FD, B, S)
#define OS_WRITE(FD, B, S) write(FD, B, S)
#ifdef O_LARGEFILE
#define OS_SEEK(FD, O, F) lseek64(FD, (off64_t) (O), F)
#else /* !O_LARGEFILE */
#define OS_SEEK(FD, O, F) lseek(FD, (off_t) (O), F)
#endif /* !O_LARGEFILE */

#define OS_SYNC(FD) fsync(FD)
#ifdef O_LARGEFILE
#define OS_TRUNC(FD, L) ftruncate64(FD, (off64_t) (L))
#else /* !O_LARGEFILE */
#define OS_TRUNC(FD, L) ftruncate(FD, (off_t) (L))
#endif /* !O_LARGEFILE */
#define OS_SIZE(FD) ih_size(FD)
extern afs_sfsize_t ih_size(int fd);

#define IH_INC(H, I, P) namei_inc(H, I, P)
#define IH_DEC(H, I, P) namei_dec(H, I, P)
#define IH_IREAD(H, O, B, S) namei_iread(H, O, B, S)
#define IH_IWRITE(H, O, B, S) namei_iwrite(H, O, B, S)
/*@=fcnmacros =macrofcndecl@*/
#endif /* AFS_NT40_ENV */

#else /* AFS_NAMEI_ENV */
extern Inode ih_icreate(IHandle_t * ih, int dev, char *part, Inode nI, int p1,
			int p2, int p3, int p4);

#define IH_CREATE(H, D, P, N, P1, P2, P3, P4) \
	ih_icreate(H, D, P, N, P1, P2, P3, P4)

#ifdef AFS_LINUX22_ENV
#define OS_IOPEN(H) -1
#else
#ifdef O_LARGEFILE
#define OS_IOPEN(H) (IOPEN((H)->ih_dev, (H)->ih_ino, O_RDWR|O_LARGEFILE))
#else
#define OS_IOPEN(H) (IOPEN((H)->ih_dev, (H)->ih_ino, O_RDWR))
#endif
#endif
#define OS_OPEN(F, M, P) open(F, M, P)
#define OS_CLOSE(FD) close(FD)

#define OS_READ(FD, B, S) read(FD, B, S)
#define OS_WRITE(FD, B, S) write(FD, B, S)
#ifdef O_LARGEFILE
#define OS_SEEK(FD, O, F) lseek64(FD, (off64_t) (O), F)
#else /* !O_LARGEFILE */
#define OS_SEEK(FD, O, F) lseek(FD, (off_t) (O), F)
#endif /* !O_LARGEFILE */

#define OS_SYNC(FD) fsync(FD)
#ifdef O_LARGEFILE
#define OS_TRUNC(FD, L) ftruncate64(FD, (off64_t) (L))
#else /* !O_LARGEFILE */
#define OS_TRUNC(FD, L) ftruncate(FD, (off_t) (L))
#endif /* !O_LARGEFILE */
#define OS_SIZE(FD) ih_size(FD)
extern afs_sfsize_t ih_size(int fd);

#ifdef AFS_LINUX22_ENV
#define IH_INC(H, I, P) -1
#define IH_DEC(H, I, P) -1
#define IH_IREAD(H, O, B, S) -1
#define IH_IWRITE(H, O, B, S) -1
#else
#define IH_INC(H, I, P) IINC((H)->ih_dev, I, P)
#define IH_DEC(H, I, P) IDEC((H)->ih_dev, I, P)
#define IH_IREAD(H, O, B, S) inode_read((H)->ih_dev, (H)->ih_ino, (H)->ih_vid,\
					O, B, S)
#define IH_IWRITE(H, O, B, S) \
	inode_write((H)->ih_dev, (H)->ih_ino, (H)->ih_vid, O, B, S)
#endif /* AFS_LINUX22_ENV */


#endif /* AFS_NAMEI_ENV */

#if defined(BUILDING_RXOSD) && defined(AFS_RXOSD_SPECIAL)
#define FDH_READ(H, B, S) ((H)->fd_ih->ih_ops->read)((H)->fd_fd, B, S)
#define FDH_READV(H, I, N) ((H)->fd_ih->ih_ops->readv)((H)->fd_fd, I, N)
#define FDH_WRITE(H, B, S) ((H)->fd_ih->ih_ops->write)((H)->fd_fd, B, S)
#define FDH_WRITEV(H, I, N) ((H)->fd_ih->ih_ops->writev)((H)->fd_fd, I, N)
#define FDH_SEEK(H, O, F) ((H)->fd_ih->ih_ops->lseek)((H)->fd_fd, O, F)
#define IH_OPENDIR(N, H) ((H)->ih_ops->opendir)(N)
#define IH_READDIR(D, H) ((H)->ih_ops->readdir)(D)
#define IH_CLOSEDIR(D, H) ((H)->ih_ops->closedir)(D)
#else
#ifndef AFS_NT40_ENV
#define FDH_READV(H, I, N) readv((H)->fd_fd, I, N)
#define FDH_WRITEV(H, I, N) writev((H)->fd_fd, I, N)
#endif
#define FDH_READ(H, B, S) OS_READ((H)->fd_fd, B, S)
#define FDH_WRITE(H, B, S) OS_WRITE((H)->fd_fd, B, S)
#define FDH_SEEK(H, O, F) OS_SEEK((H)->fd_fd, O, F)
#define IH_OPENDIR(N, H) opendir(N)
#define IH_READDIR(D, H) readdir(D)
#define IH_CLOSEDIR(D, H) closedir(D)
#endif

#define FDH_SYNC(H) ((H->fd_ih!=NULL) ? ( H->fd_ih->ih_synced = 1) - 1 : 1)
#define FDH_TRUNC(H, L) OS_TRUNC((H)->fd_fd, L)
#define FDH_SIZE(H) OS_SIZE((H)->fd_fd)

#endif /* _IHANDLE_H_ */
