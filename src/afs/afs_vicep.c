/*
 * Copyright (c) 2006, Hartmut Reuter,
 * RZG, Max-Planck-Institut f. Plasmaphysik.
 * All Rights Reserved.
 *
 */

#include <afsconfig.h>
#include "afs/param.h"

#if !defined(UKERNEL)

#include "afs/sysincludes.h"	/* Standard vendor system headers */
#ifndef AFS_LINUX22_ENV
#include "rpc/types.h"
#endif
#ifdef AFS_LINUX26_ENV
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/module.h>
#endif /* AFS_LINUX26_ENV */
#ifdef	AFS_ALPHA_ENV
#undef kmem_alloc
#undef kmem_free
#undef mem_alloc
#undef mem_free
#undef register
#endif /* AFS_ALPHA_ENV */
#include "afs/afsincludes.h"	/* Afs-based standard headers */
#include "afs/afs_stats.h"	/* statistics */
#include "afs/afs_cbqueue.h"	
#include "afs/nfsclient.h"
#include "afs/afs_osidnlc.h"

#ifdef AFS_CACHE_BYPASS
#include "afs_bypasscache.h"
#endif

#ifdef AFS_LINUX26_ENV
#ifdef STRUCT_TASK_STRUCT_HAS_CRED
extern struct cred *cache_creds;
#endif
#endif
/* #define OLD_LUSTRE_HACK	1 */
#undef OLD_LUSTRE_HACK

afs_int32 lustre_hack = 0 ;
extern int cacheDiskType;
extern afs_uint32 afs_protocols;
#ifdef RX_PRINTFS
extern afs_int32 do_printfs;
#endif

afs_int32 vicep_fastread = 0;
afs_int32 vicep_nosync = 0;

#define MAXSERVERUUIDS 16
int nServerUuids = 0;
#define MAXCELLCHARS 	64	/* as in afs_cell.c */
#define MAXVISIBLEOSDS 16
#define MAX_L 	0x7fffffffffffffff

static afs_int32 open_vicep_file(struct vcache *avc, char *path);

int afs_nVisibleOsds = 0;
struct visible_osd {
    afs_int32 cell;
    afs_int32 osd;
    afs_int32 lun;
    char path[64];
};

struct visible_fs_part {
    afs_int32 cell;
    afsUUID uuid;
    afs_int32 lun;
    char path[64];
};

struct visible_osd afs_visibleOsd[MAXVISIBLEOSDS];
struct visible_fs_part afs_visiblePart[MAXSERVERUUIDS];
afs_int32 openAfsServerVnodes = 0;

/* rock and operations for store with VICEP_ACCESS */
#define VICEP_WRITE_EXPIRATION 60 	/* Ask fileserver once a minute */
#define MAX_FASTREAD_USERS 8
struct vpacRock {
    union {
        afs_uint64 obj_id;
	afs_uint32 tag;
	afs_uint64 inode;
    } file;
    afs_int32 ServerUuidIndex;
#if defined(AFS_LINUX26_ENV)
    struct file *fp;
#elif defined(AFS_AIX53_ENV)
    struct vnode *fp;
#endif
    afs_uint64 bytes_rcvd;
    afs_uint64 bytes_sent;
    afs_uint32 osd;
    afs_uint32 user[MAX_FASTREAD_USERS];
    afs_int32 refCnt;
    afs_uint64 transid;		/* for fast read */
    afs_uint32 expires		/* for fast read */;
    afs_rwlock_t lock;
    char closeMe;
    char fsync;
};
    
struct vpac_Variables {
    void *ops;
#if defined(AFS_LINUX26_ENV)
    struct file *fp;
#elif defined(AFS_AIX52_ENV)
    struct vnode *fp;
#endif
    struct vcache *avc;
    struct afs_conn *fs_conn;
    struct vpacRock *vpacRock;
    struct asyncError aE;
    afs_uint64 base;
    afs_uint64 maxlength;
    afs_uint64 transid;
    afs_uint64 bytes_rcvd;
    afs_uint64 bytes_sent;
    afs_uint32 osd;
    afs_uint32 expires;
    char *tbuffer;
    char *bp;
    afs_uint32 bufsize;
    afs_int32 writing;
    AFSFetchStatus OutStatus;
    AFSCallBack CallBack;
    struct async a;
    struct osi_file *fP;
    void *bypassparms;
};

#define ALLOC_VICEP(p, s) if (sizeof(s) > AFS_SMALLOCSIZ) p = (s *)afs_osi_Alloc(sizeof(s)); else p = (s *) osi_AllocSmallSpace(sizeof(s))
#define FREE_VICEP(p, s) if (sizeof(s) > AFS_SMALLOCSIZ) afs_osi_Free(p,sizeof(s)); else osi_FreeSmallSpace(p)

void
vpac_fsync(struct vcache *avc)
{
#if defined(AFS_LINUX26_ENV)
    if (avc->vpacRock) {
	struct vpacRock *r = (struct vpacRock *)avc->vpacRock;
	r->fsync = 1;
    }
#endif
}

static char c_xlate[80] =
    "+=0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

static void
flip(char *s, afs_uint64 a)
{
    afs_uint64 n;
    int i = 0;

    if (a == 0)
	s[i++] = c_xlate[0];
    else {
	for (n = a &0x3f; a; n = ((a >>= 6) & 0x3f)) {
	    s[i++] = c_xlate[n];
	}
    }
    s[i] = '\0';
}

static afs_int32
build_path(char *part, afs_uint32 lun, afs_uint32 vol, afs_uint64 oid, 
			afs_int32 algorithm, char *path)
{
    afs_uint64 i;
    char *p;

    if (part) {		/* Path provided by afsd */
	sprintf(path, "%s/AFSIDat/", part);
    } else {
        if (lun > 255)
	    return EINVAL;
        if (lun > 25) {
	    sprintf(path, "/vicepaa/AFSIDat/");
	    path[6] = c_xlate[(lun/26) + 37];
	    path[7] = c_xlate[(lun %26) + 38];
        } else { 
	    sprintf(path, "/vicepa/AFSIDat/");
	    path[6] = c_xlate[lun + 38];
        }
    }
    if (algorithm != 1)
	return ENOENT;
    p = path + strlen(path);
    i = vol;
    flip(p, (i & 0xff));
    p = path + strlen(path);
    *p++ = '/';
    flip(p, i);
    p = path + strlen(path);
    *p++ = '/';
    i = (oid >> 14) & 0xff;
    flip(p, i);
    p = path + strlen(path);
    *p++ = '/';
    i = (oid >> 9) & 0x1ff;
    flip(p, i);
    p = path + strlen(path);
    *p++ = '/';
    flip(p, oid);
    /* printf("AFS build_path pid %llu oid %llu path %s\n", pid, oid, path); */
    return 0;
}

static afs_int32
build_path_for_osd(afs_uint64 pid, afs_uint64 oid, struct visible_osd *info, 
			char *path)
{
    afs_uint32 lun, vol;
    afs_int32 code;

    SplitInt64(pid, lun, vol);
    if (info->path[0] == 0 && lun != info->lun) {
	afs_warn("AFS build_path: osd info has lun %u, using lun %u from info\n",
		info->lun, lun);
	lun = info->lun;
    }
    code = build_path(info->path, lun, vol, oid, 1, path);
    return code;
}

afs_int32
vpac_storeUfsPrepare(void *r, afs_uint32 size, afs_uint32 *tlen)
{
    *tlen = (size > AFS_LRALLOCSIZ ?  AFS_LRALLOCSIZ : size);
    return 0;
}

afs_int32
vpac_storeMemPrepare(void *r, afs_uint32 size, afs_uint32 *tlen)
{
    *tlen = size;
    return 0;
}

afs_int32
vpac_storeUfsRead(void *r, struct osi_file *tfile, afs_uint32 offset,
		  afs_uint32 tlen, afs_uint32 *bytesread, char **abuf)
{
    afs_int32 code;
    struct vpac_Variables *v = (struct vpac_Variables *)r;

    *bytesread = 0;
    v->bp = v->tbuffer;
    *abuf = v->bp;
    code = afs_osi_Read(tfile, -1, v->bp, tlen);
    if (code < 0) 
	return EIO;
    *bytesread = code;
    if (code == tlen)
        return 0;
#if defined(KERNEL_HAVE_UERROR)
    if (getuerror())
	return EIO;
#endif
    if (code == 0)
	return EIO;
    return 0;   
}

afs_int32
vpac_storeMemRead(void *r, struct osi_file *tfile, afs_uint32 offset,
		  afs_uint32 tlen, afs_uint32 *bytesread, char **abuf)
{
    struct vpac_Variables *v = (struct vpac_Variables *)r;
    struct memCacheEntry *mceP = (struct memCacheEntry *)tfile;

    /*
     * We obtain here the read lock, but we release it only in
     * vpac_storeMemWrite because we know with our return code 0
     * we will get there immediatly!
     */
    v->fP = tfile;
    ObtainReadLock(&mceP->afs_memLock);
    v->bp = mceP->data + offset;
    if (offset > mceP->size)
        *bytesread = 0;
    else if (offset + tlen > mceP->size)
        *bytesread = mceP->size - offset;
    else
        *bytesread = tlen;
    *abuf = v->bp;
    return 0;   
}

afs_int32
vpac_storeWrite(void *r, char *abuf, afs_uint32 tlen, afs_uint32 *byteswritten)
{
#if !defined(AFS_LINUX26_ENV) && !defined(AFS_AIX53_ENV)
    return EIO;
#else
    afs_int32 code;
#if defined(AFS_LINUX26_ENV)
    mm_segment_t fs;
    unsigned long savelim = current->TASK_STRUCT_RLIM[RLIMIT_FSIZE].rlim_cur;
    uid_t fsuid = current_fsuid();
#endif
    struct vpac_Variables *v = (struct vpac_Variables *)r;

    ObtainWriteLock(&v->vpacRock->lock, 801);
#if defined(AFS_LINUX26_ENV)
    if (!v->fp) {
	afs_warn("vpac_storeWrite: no fp\n");
        ReleaseWriteLock(&v->vpacRock->lock);
	return EIO;
    }
#if !defined(STRUCT_TASK_STRUCT_HAS_CRED)
    current_fsuid() = 0;
#endif
    current->TASK_STRUCT_RLIM[RLIMIT_FSIZE].rlim_cur = RLIM_INFINITY;
    fs = get_fs();
    set_fs(KERNEL_DS);
    RX_AFS_GUNLOCK();
    code = 0;
    if (v->fp->f_op->llseek && v->fp->f_dentry) {
        if (v->fp->f_op->llseek(v->fp, v->base, 0) != v->base)
            code = -1;
    } else
        v->fp->f_pos = v->base;
    if (!code) {
	if (v->fp->f_op->write) 
            code = v->fp->f_op->write(v->fp, abuf, tlen, &v->fp->f_pos);
	else {
	    afs_warn("v->fp->f_op->write was 0\n");
	    code = -1;
	}
    } else
	afs_warn("vpac_storeWrite: llseek failed\n");
    set_fs(fs);
    RX_AFS_GLOCK();
#if !defined(STRUCT_TASK_STRUCT_HAS_CRED)
    current_fsuid() = fsuid;
#endif
    current->TASK_STRUCT_RLIM[RLIMIT_FSIZE].rlim_cur = savelim;
#elif defined(AFS_AIX52_ENV)
    afs_uint32 resid;

    code = gop_rdwr(UIO_WRITE, v->fp, abuf, tlen, &v->base, AFS_UIOSYS,
                                             NULL, &resid);
    if (code) {
	afs_warn("vpac_storeWrite: gop_rdwr failed for write with code %d\n", code);
        ReleaseWriteLock(&v->vpacRock->lock);
        return EIO;
    }
    code = tlen - resid;
#endif
    v->aE.asyncError_u.no_new_version = 0;
    v->base += tlen;
    if (code != tlen) {
        afs_Trace3(afs_iclSetp, CM_TRACE_WASHERE,
                   ICL_TYPE_STRING, __FILE__,
                   ICL_TYPE_INT32, __LINE__,
                   ICL_TYPE_INT32, code);
	afs_warn("vpac_storeWrite: gop_rdwr wrote only %d instead of %d\n", 
		code, tlen);
        ReleaseWriteLock(&v->vpacRock->lock);
        return EIO;
    } 
    *byteswritten = tlen;
    v->bytes_rcvd += tlen;
    ReleaseWriteLock(&v->vpacRock->lock);
    return 0;
#endif
}

afs_int32
vpac_storeWriteUnlocked(void *r, char *abuf, afs_uint32 tlen,
		        afs_uint32 *byteswritten)
{
    afs_int32 code;
    RX_AFS_GLOCK();
    code = vpac_storeWrite(r, abuf, tlen, byteswritten);
    RX_AFS_GUNLOCK();
    return code;
}

afs_int32
vpac_storeMemWrite(void *r, char *abuf, afs_uint32 tlen, afs_uint32 *byteswritten)
{
    afs_int32 code;
    struct vpac_Variables *v = (struct vpac_Variables *)r;
    struct memCacheEntry *mceP = (struct memCacheEntry *)v->fP;

    code = vpac_storeWrite(r, abuf, tlen, byteswritten);
    /*
     * We release here the read lock we got in vpac_storeMemRead before.
     */
    ReleaseReadLock(&mceP->afs_memLock);
    return code;
}

afs_int32
vpac_storePadd(void *rock, afs_uint32 size)
{
    afs_int32 code;
    afs_uint32 tlen, bytesXfered;
    struct vpac_Variables *v = (struct vpac_Variables *)rock;
    
    if (!v->tbuffer)
	v->tbuffer = osi_AllocLargeSpace(AFS_LRALLOCSIZ);
    memset(v->tbuffer, 0, AFS_LRALLOCSIZ);
    v->bp = v->tbuffer;
    while (size) {
        tlen = (size > AFS_LRALLOCSIZ ?  AFS_LRALLOCSIZ : size);
	code = vpac_storeWrite(rock, v->tbuffer, tlen, &bytesXfered);
	if (code)
	    return code;
	size -= tlen;
    }
    return 0;
}

afs_int32
vpac_storeStatus(void *rock)
{
    return 1;
}

afs_int32
vpac_storeClose(void *rock, struct AFSFetchStatus *OutStatus, int *doProcessFS)
{
#if !defined(AFS_LINUX26_ENV) && !defined(AFS_AIX53_ENV)
    return EIO;
#else
    struct vpac_Variables *v = (struct vpac_Variables *)rock;
    struct vpacRock *r = (struct vpacRock *)v->vpacRock;
    afs_int32 code = 0;

    *doProcessFS = 0;
    if (r && (!vicep_nosync || r->fsync)) { 
	/* Make sure data are also visible on the fileserver */
#ifdef AFS_LINUX22_ENV
        mm_segment_t fs;
        uid_t fsuid = current_fsuid();
        struct file *tfp = r->fp;
#if !defined(STRUCT_TASK_STRUCT_HAS_CRED)
        current_fsuid() = 0;
#endif
        fs = get_fs();
        set_fs(KERNEL_DS);
        RX_AFS_GUNLOCK();
        tfp->f_op = fops_get(tfp->f_dentry->d_inode->i_fop);
	if (tfp->f_op && tfp->f_op->fsync)
            code = file_fsync(tfp, tfp->f_dentry, 0);
        RX_AFS_GLOCK();
#if !defined(STRUCT_TASK_STRUCT_HAS_CRED)
        current_fsuid() = fsuid;
#endif
        set_fs(fs);
#endif
    }

    if (v->transid) {
        struct AFSStoreStatus InStatus;
        InStatus.Mask = AFS_SETMODTIME;
        InStatus.ClientModTime = v->avc->f.m.Date;
        RX_AFS_GUNLOCK();
        code = RXAFS_EndAsyncStore(v->fs_conn->id, &v->avc->f.fid.Fid,
                                v->transid, v->avc->f.m.Length, 
				v->bytes_rcvd, v->bytes_sent, v->osd,
				0, &v->aE, &InStatus, OutStatus);
        RX_AFS_GLOCK();
        if (!code) {
            *doProcessFS = 1;
            v->transid = 0;
        }
    }
    return code;
#endif
}

afs_int32
vpacDestroy(void **rock, afs_int32 error)
{
    afs_int32 code = error;
    struct vpac_Variables *v = (struct vpac_Variables *)*rock;
    struct AFSStoreStatus InStatus;
    
    *rock = NULL;
    if (v->transid) {
        afs_int32 code2;
        InStatus.Mask = AFS_SETMODTIME;
        InStatus.ClientModTime = v->avc->f.m.Date;
        RX_AFS_GUNLOCK();
        if (v->writing)
            code2 = RXAFS_EndAsyncStore(v->fs_conn->id, &v->avc->f.fid.Fid,
                                v->transid, v->avc->f.m.Length, 
				v->bytes_rcvd, v->bytes_sent, v->osd,
				error, &v->aE, &InStatus, &v->OutStatus);
        else
            code2 = RXAFS_EndAsyncFetch(v->fs_conn->id, &v->avc->f.fid.Fid,
                                        v->transid, v->bytes_sent, v->osd);
        RX_AFS_GLOCK();
    }
    if (v->tbuffer)
	osi_FreeLargeSpace(v->tbuffer);
    if (v->vpacRock) {
	if (v->vpacRock->refCnt > 0) {
/*         printf("vpac_destroy decr'ing refCnt for %u.%u.%u from %d to %d\n",
		v->avc->f.fid.Fid.Volume,
		v->avc->f.fid.Fid.Vnode,
		v->avc->f.fid.Fid.Unique,
		v->vpacRock->refCnt,
		v->vpacRock->refCnt -1); */
	    v->vpacRock->refCnt--;
	}
	if (v->vpacRock->closeMe && v->vpacRock->refCnt == 0) {
	    afs_close_vicep_file(v->avc, NULL, v->avc->lock.excl_locked? 1:0);
	}
    }
    FREE_VICEP(v, struct vpac_Variables);
    return code;
}

static
struct storeOps vpac_storeUfsOps = {
#if (defined(AFS_SGI_ENV) && !defined(__c99))
    vpac_storeUfsPrepare,
    vpac_storeUfsRead,
    vpac_storeWrite,
    vpac_storeStatus,
    vpac_storePadd,
    vpac_storeClose,
    vpacDestroy,
    afs_GenericStoreProc
#else
    .prepare =  vpac_storeUfsPrepare,
    .read =     vpac_storeUfsRead,
#ifdef AFS_LINUX26_ENV
    .write =    vpac_storeWriteUnlocked,
#else
    .write =    vpac_storeWrite,
#endif
    .status =   vpac_storeStatus,
    .padd =     vpac_storePadd,
    .close =    vpac_storeClose,
    .destroy =  vpacDestroy,
#ifdef AFS_LINUX26_ENV
    .storeproc = afs_linux_storeproc
#else
    .storeproc = afs_GenericStoreProc
#endif
#endif
};

static
struct storeOps vpac_storeMemOps = {
#if (defined(AFS_SGI_ENV) && !defined(__c99))
    vpac_storeMemPrepare,
    vpac_storeMemRead,
    vpac_storeMemWrite,
    vpac_storeStatus,
    vpac_storePadd,
    vpac_storeClose,
    vpacDestroy,
    afs_GenericStoreProc
#else
    .prepare =  vpac_storeMemPrepare,
    .read =     vpac_storeMemRead,
    .write =    vpac_storeMemWrite,
    .status =   vpac_storeStatus,
    .padd =     vpac_storePadd,
    .close =    vpac_storeClose,
    .destroy =  vpacDestroy,
    .storeproc = afs_GenericStoreProc
#endif
};

static afs_int32
common_storeInit(struct vcache *avc, struct afs_conn *tc, afs_offs_t base, 
		afs_size_t bytes, afs_size_t length,
  		int sync, struct vrequest * areq,
		struct storeOps **ops, void **rock,
		afs_uint64 transid, afs_uint32 expires, afs_uint64 maxlength,
		afs_uint32 osd)
{
#if !defined(AFS_LINUX26_ENV) && !defined(AFS_AIX53_ENV)
    return EIO;
#else
    struct vpac_Variables *v;
    struct vpacRock *r;
    struct RWparm p;
    afs_int32 code;

    if (!avc->vpacRock || !tc)
	return -1;
    /* make sure no callbacks happened in the meantime */
    if (!(avc->f.states & CStatd)) {
	afs_warn("vpac_storeInit: callback for %u.%u.%u proceeding with %s\n",
			avc->f.fid.Fid.Volume, avc->f.fid.Fid.Vnode,
			avc->f.fid.Fid.Unique,
			(avc->protocol == 2) ? "rxosd":"rxfs");
	return -1;
    }
    ALLOC_VICEP(v, struct vpac_Variables);
    if (!v) 
        osi_Panic("vpac_storeInit: ALLOC_VICEP returned NULL\n");
    memset(v, 0, sizeof(struct vpac_Variables));
    v->writing = 1;
    v->aE.asyncError_u.no_new_version = 1;
    v->avc = avc;
    v->fs_conn = tc;
    r = (struct vpacRock *)avc->vpacRock;
    v->fp = r->fp;
    v->base = base;
    v->osd = osd;
    if (r->transid) { 	/* we must end the read transaction first */
	RX_AFS_GUNLOCK();
	code = RXAFS_EndAsyncFetch(tc->id, &avc->f.fid.Fid, r->transid,
			r->bytes_sent, r->osd);
	RX_AFS_GLOCK();
	r->transid = 0;
	r->expires = 0;
	r->bytes_sent = 0;
    }
    if (transid) {
	v->transid = transid;
 	v->expires = expires;
	v->maxlength = maxlength;
    } else {
        v->a.type = 3;
	v->a.async_u.p3.path.path_info_val = NULL;
	v->a.async_u.p3.path.path_info_len = 0;
	p.type = 4;
	p.RWparm_u.p4.offset = base;
	p.RWparm_u.p4.length = bytes;
	p.RWparm_u.p4.filelength = avc->f.m.Length;
        RX_AFS_GUNLOCK();
        code = RXAFS_StartAsyncStore(tc->id, (struct AFSFid *) &avc->f.fid.Fid,
                                    &p, &v->a, &v->maxlength, &v->transid,
				    &v->expires, &v->OutStatus);
        RX_AFS_GLOCK();
        if (code) {
	    afs_warn("RXAFS_StartAsyncStore for %u.%u.%u.%u returns %d\n",
				avc->f.fid.Cell, avc->f.fid.Fid.Volume,
				avc->f.fid.Fid.Vnode, avc->f.fid.Fid.Unique, code);
    	    FREE_VICEP(v, struct vpac_Variables);
	    return code;
        }
	v->expires += osi_Time();
        if (r->file.inode != v->a.async_u.p3.ino) {
	    char path[80];
	    afs_int32 i = r->ServerUuidIndex;
	    afs_close_vicep_file(avc, NULL, avc->lock.excl_locked? 1:0);
	    if (avc->vpacRock) { /* Fall through to rxfs protocol */
	        code = ENOENT;
	        afs_warn("Inode number changed for %u.%u.%u.%u, but old file is still open, returning %d\n",
			    avc->f.fid.Cell, avc->f.fid.Fid.Volume,
			    avc->f.fid.Fid.Vnode, avc->f.fid.Fid.Unique, code);
	        vpacDestroy((void *)&v, code);
	        return code;
	    }
	    if (v->a.type == 4) {
	        code = build_path(afs_visiblePart[i].path,
			v->a.async_u.p4.lun, 
			v->a.async_u.p4.rwvol,
			v->a.async_u.p4.ino,  
			v->a.async_u.p4.algorithm, (char *)&path);
	    } else {
		sprintf(path, "%s/%s", afs_visiblePart[i].path, 
			v->a.async_u.p3.path.path_info_val);
		osi_free(v->a.async_u.p3.path.path_info_val,
			 v->a.async_u.p3.path.path_info_len);
	    }
	    if (!code) {
	        code = open_vicep_file(avc, path);
                if (avc->vpacRock) {
		    struct vpacRock *r = avc->vpacRock;
		    if (v->a.type == 3)
		       r->file.inode = v->a.async_u.p3.ino;
		    else if (v->a.type == 4)
		       r->file.inode = v->a.async_u.p4.ino;
		    r->ServerUuidIndex = i;
		    avc->protocol |= VICEP_ACCESS; 
                    openAfsServerVnodes++;
		}
	    }
	    if (code) {	/* Fall through to rxfs protocol */
	        vpacDestroy((void *)&v, code);
		return code;
	    }
        }
    }
    if (cacheDiskType == AFS_FCACHE_TYPE_UFS) {
	v->tbuffer = osi_AllocLargeSpace(AFS_LRALLOCSIZ);
	if (!v->tbuffer) 
	    osi_Panic
              ("vpac_storeInit: osi_AllocLargeSpace for iovecs returned NULL\n");
	*ops = &vpac_storeUfsOps;
    } else 
	*ops = &vpac_storeMemOps;
/*      printf("vpac_storeInit incr'ing refCnt for %u.%u.%u from %d to %d\n",
		v->avc->f.fid.Fid.Volume,
		v->avc->f.fid.Fid.Vnode,
		v->avc->f.fid.Fid.Unique,
		r->refCnt,
		r->refCnt +1); */
    r->refCnt++;
    v->vpacRock = r;
    v->ops = (void *) *ops;
    *rock = (void *)v;
    return 0;
#endif
}

afs_int32
vpac_storeInit(struct vcache *avc, struct afs_conn *tc, afs_offs_t base, 
		afs_size_t bytes, afs_size_t length,
  		int sync, struct vrequest * areq,
		struct storeOps **ops, void **rock)
{
    afs_int32 code;

    code = common_storeInit(avc, tc, base, bytes, length, sync, areq, ops,
				rock, 0, 0, 0, 0);

    return code;
}

afs_int32
fake_vpac_storeInit(struct vcache *avc, struct afs_conn *tc, afs_offs_t base, 
		afs_size_t bytes, afs_size_t length,
  		int sync, struct vrequest * areq,
		struct storeOps **ops, void **rock,
		afs_uint64 transid, afs_uint32 expires, afs_uint64 maxlength,
		afs_uint32 osd)
{
    afs_int32 code;

    code = common_storeInit(avc, tc, base, bytes, length, sync, areq, ops,
				rock, transid, expires, maxlength, osd);

    return code;
}

/* Operations for fetch with VICEP_ACCESS */

afs_int32
vpac_fetchRead(void *r, afs_uint32 tlen, afs_uint32 *bytesread)
{
#if !defined(AFS_LINUX26_ENV) && !defined(AFS_AIX53_ENV)
    return EIO;
#else
    afs_int32 code;
    struct vpac_Variables *v = (struct vpac_Variables *)r;
#if defined (AFS_LINUX26_ENV)
    mm_segment_t fs;
    unsigned long savelim = current->TASK_STRUCT_RLIM[RLIMIT_FSIZE].rlim_cur;
    uid_t fsuid = current_fsuid();
#endif
    
    *bytesread = 0;
    ObtainWriteLock(&v->vpacRock->lock, 802);
    if (!v->fp) {
	afs_warn("vpac_fetchRead: no fp\n");
        ReleaseWriteLock(&v->vpacRock->lock);
	return EIO;
    }
#if defined (AFS_LINUX26_ENV)
    if (tlen > v->bufsize)
	tlen = v->bufsize;
#if !defined(STRUCT_TASK_STRUCT_HAS_CRED)
    current_fsuid() = 0;
#endif
    current->TASK_STRUCT_RLIM[RLIMIT_FSIZE].rlim_cur = RLIM_INFINITY;
    fs = get_fs();
    set_fs(KERNEL_DS);
    RX_AFS_GUNLOCK();
    code = 0;
    if (v->fp->f_op->llseek && v->fp->f_dentry) {
        if (v->fp->f_op->llseek(v->fp, v->base, 0) != v->base) {
	    afs_warn("vpac_fetchRead: llseek failed at %llu\n", v->base);
            code = -1;
	}
    } else
        v->fp->f_pos = v->base;
#if 0
    if (!vicep_fastread && !vicep_nosync && !code) {
        code = locks_mandatory_area(FLOCK_VERIFY_READ, v->fp->f_dentry->d_inode,
                                        v->fp, v->base, tlen);
        if (code)
	    afs_warn("vpac_fetchRead: locks_mandatory_area failed at %llu\n", v->base);
    }
#endif
    if (!code) 
        code = v->fp->f_op->read(v->fp, v->bp, tlen, &v->fp->f_pos);
    set_fs(fs);
    RX_AFS_GLOCK();
#if !defined(STRUCT_TASK_STRUCT_HAS_CRED)
    current_fsuid() = fsuid;
#endif
    current->TASK_STRUCT_RLIM[RLIMIT_FSIZE].rlim_cur = savelim;
#elif defined(AFS_AIX52_ENV)
    afs_uint32 resid;

    code = gop_rdwr(UIO_READ, v->fp, v->bp, tlen, &v->base, AFS_UIOSYS,
                              NULL, &resid);
    if (code) {
	afs_warn("vpac_fetchRead: gop_rdwr returned with %d\n", code);
        ReleaseWriteLock(&v->vpacRock->lock);
        return EIO;
    }
    code = tlen - resid;
#endif
    if (code != tlen) {
        afs_Trace3(afs_iclSetp, CM_TRACE_WASHERE,
                   ICL_TYPE_STRING, __FILE__,
                   ICL_TYPE_INT32, __LINE__,
                   ICL_TYPE_INT32, code);
	afs_warn("vpac_fetchRead: read only %d instead of %d at offset %llu of %u.%u.%u (length %llu) i_size %llu\n", 
		code, tlen, v->base, v->avc->f.fid.Fid.Volume, 
		v->avc->f.fid.Fid.Vnode, v->avc->f.fid.Fid.Unique, 
		v->avc->f.m.Length, i_size_read(v->fp->f_dentry->d_inode));
        ReleaseWriteLock(&v->vpacRock->lock);
        return EIO;
    } 
    v->base += tlen;
    if (v->transid) 
	v->bytes_sent += tlen;
    else {
	struct vpacRock *rk = (struct vpacRock *)v->avc->vpacRock;
	rk->bytes_sent += tlen;
    }
    *bytesread = tlen;
    ReleaseWriteLock(&v->vpacRock->lock);
    return 0;
#endif
}

afs_int32
vpac_fetchMemRead(void *r, afs_uint32 tlen, afs_uint32 *bytesread)
{
    afs_int32 code;
    struct vpac_Variables *v = (struct vpac_Variables *)r;
    struct memCacheEntry *mceP = (struct memCacheEntry *)v->fP;

    ObtainWriteLock(&mceP->afs_memLock, 893);
    code = vpac_fetchRead(r, tlen, bytesread);
    if (!code)
        mceP->size = *bytesread;
    ReleaseWriteLock(&mceP->afs_memLock);
    return code;
}

#if defined(AFS_CACHE_BYPASS) && defined(AFS_LINUX24_ENV)
afs_int32
vpac_fetchBypassCacheRead(void *r, afs_uint32 size, afs_uint32 *bytesread)
{
    afs_int32 code = 0;
    struct vpac_Variables *v = (struct vpac_Variables *)r;
    afs_uint32 length = size;
    int nio, curpage, bytes;
    struct page *pp;
    mm_segment_t fs;
    uid_t fsuid = current_fsuid();
    struct nocache_read_request *bparms =
                                (struct nocache_read_request *) v->bypassparms;
    
    *bytesread = 0;
    if (!bparms) {
	afs_warn("vpac_fetchBypassCacheRead: no bypassparms\n");
	return EIO;
    }
    ObtainWriteLock(&v->vpacRock->lock, 802);
    if (!v->fp) {
	afs_warn("vpac_fetchRead: no fp\n");
        ReleaseWriteLock(&v->vpacRock->lock);
	return EIO;
    }
    nio = bparms->auio->uio_iovcnt;
    length = size;
#if !defined(STRUCT_TASK_STRUCT_HAS_CRED)
    current_fsuid() = 0;
#endif
    fs = get_fs();
    set_fs(KERNEL_DS);
    RX_AFS_GUNLOCK();
    code = 0;
    if (v->fp->f_op->llseek && v->fp->f_dentry) {
        if (v->fp->f_op->llseek(v->fp, v->base, 0) != v->base) {
	    afs_warn("vpac_fetchRead: llseek failed at %llu\n", v->base);
            code = -1;
	}
    } else
        v->fp->f_pos = v->base;
#if 0
    if (!vicep_fastread && !vicep_nosync && !code) {
        code = locks_mandatory_area(FLOCK_VERIFY_READ, v->fp->f_dentry->d_inode,
                                        v->fp, v->base, length);
        if (code)
	    afs_warn("vpac_fetchRead: locks_mandatory_area failed at %llu\n", v->base);
    }
#endif
    bytes = 0;
    if (!code) {
	for (curpage = 0; curpage < nio; curpage++) {
	    char * address;
            pp = (struct page *)bparms->auio->uio_iov[curpage].iov_base;
	    if (pp) {
	        address = kmap(pp);
                code = v->fp->f_op->read(v->fp, address, 
                                     bparms->auio->uio_iov[curpage].iov_len,
				     &v->fp->f_pos);
	        kunmap(pp);
	        if (code <= 0) 
		    break;
	        bytes += code;
	        code = 0;
		SetPageUptodate(pp);
		if (PageLocked(pp))
                    unlock_page(pp);
                else
                    afs_warn("rxfs_fetchBypassCacheRead: page not locked!\n");
                put_page(pp); /* decrement refcount */
	    }
	}
	if (code != 0)
	    code = EIO;
    }
    /*  bytes = vfs_readv(v->fp, xiov, nio, &v->fp->f_pos); */
    set_fs(fs);
    RX_AFS_GLOCK();
#if !defined(STRUCT_TASK_STRUCT_HAS_CRED)
    current_fsuid() = fsuid;
#endif
    if (bytes < 0) {
        afs_warn("rxfs_fetchBypassCacheRead: rx_Read error. Return code was %d\n",
                 bytes);
        unlock_and_release_pages(bparms->auio);
        code = -34;
        goto done;
    }
    if (bytes == 0) {
        afs_warn("rxfs_fetchBypassCacheRead: rx_Read returned zero. Aborting\n");
        unlock_and_release_pages(bparms->auio);
        goto done;
    }
    if (v->transid) 
	v->bytes_sent += bytes;
    else {
	struct vpacRock *rk = (struct vpacRock *)v->avc->vpacRock;
	rk->bytes_sent += bytes;
    }
    *bytesread = bytes;

done:
    ReleaseWriteLock(&v->vpacRock->lock);
    return code;
}
#endif /* AFS_CACHE_BYPASS && AFS_LINUX24_ENV */

afs_int32
vpac_fetchUfsWrite(void *r, struct osi_file *fP, afs_uint32 offset, 
		   afs_uint32 tlen, afs_uint32 *byteswritten)
{
    afs_int32 code;
    struct vpac_Variables *v = (struct vpac_Variables *)r;

    code = afs_osi_Write(fP, -1, v->tbuffer, tlen);
    if (code != tlen)
	return EIO;
    *byteswritten = tlen;
    return 0;
}

afs_int32
vpac_fetchMemWrite(void *r, struct osi_file *fP, afs_uint32 offset, 
		   afs_uint32 tlen, afs_uint32 *byteswritten)
{
    *byteswritten = tlen;
    return 0;
}

#if defined(AFS_CACHE_BYPASS) && defined(AFS_LINUX26_ENV)
afs_int32
vpac_fetchBypassCacheWrite(void *r, struct osi_file *fP, afs_uint32 offset,
                   afs_uint32 tlen, afs_uint32 *byteswritten)
{
    *byteswritten = tlen;
    return 0;
}
#endif

afs_int32
vpac_fetchClose(void *rock, struct vcache *avc, struct dcache *adc, 
					struct afs_FetchOutput *o)
{
#if !defined(AFS_LINUX26_ENV) && !defined(AFS_AIX53_ENV)
    return EIO;
#else
    struct vpac_Variables *v = (struct vpac_Variables *)rock;
    afs_uint64 length = avc->f.m.Length;
    afs_uint64 ilength;

#if defined (AFS_LINUX26_ENV)
    ilength = i_size_read(v->fp->f_dentry->d_inode);
    if (ilength > length)
        length = ilength;
#elif defined(AFS_AIX52_ENV)
    struct vattr tvattr;
    afs_int32 code;
    
    code = VNOP_GETATTR(v->fp, &tvattr, &afs_osi_cred);
    if (code)
        afs_warn("vpac_fetchClose: VNOP_GETATTR returns %d\n", code);
    length = tvattr.va_size;
#endif
    SplitInt64(length, o->OutStatus.Length_hi, o->OutStatus.Length);
    if (adc)
        hset(adc->f.versionNo, avc->f.m.DataVersion);
    o->OutStatus.dataVersionHigh = avc->f.m.DataVersion.high;
    o->OutStatus.DataVersion = avc->f.m.DataVersion.low;
    o->OutStatus.InterfaceVersion = DONT_PROCESS_FS;
    return 0;
#endif
}


static
struct fetchOps vpac_fetchUfsOps = {
    0,
    vpac_fetchRead,
    vpac_fetchUfsWrite,
    vpac_fetchClose,
    vpacDestroy
};

static
struct fetchOps vpac_fetchMemOps = {
    0,
    vpac_fetchMemRead,
    vpac_fetchMemWrite,
    vpac_fetchClose,
    vpacDestroy
};

#if defined(AFS_CACHE_BYPASS) && defined(AFS_LINUX26_ENV)
static
struct fetchOps vpac_fetchBypassCacheOps = {
    0,
    vpac_fetchBypassCacheRead,
    vpac_fetchBypassCacheWrite,
    vpac_fetchClose,
    vpacDestroy
};
#endif

static afs_int32
common_fetchInit(struct afs_conn *tc, struct vcache *avc, afs_offs_t base, 
		afs_uint32 bytes, afs_uint32 *length, 
		void *bypassparms,
  		struct osi_file *fP, struct vrequest *areq,
	        struct fetchOps **ops, void **rock, afs_uint64 transid,
		afs_uint32 expires, afs_uint32 osd)
{
#if !defined(AFS_LINUX26_ENV) && !defined(AFS_AIX53_ENV)
    return EIO;
#else
    afs_int32 code;
    struct vpac_Variables *v;
    struct vpacRock *r;
    struct memCacheEntry *mceP = (struct memCacheEntry *)fP;
    afs_int64 tlength;
    afs_int64 ilength;
#ifdef AFS_AIX52_ENV
    struct vattr tvattr;
#endif
#if defined(AFS_CACHE_BYPASS) && defined(AFS_LINUX26_ENV)
    struct nocache_read_request *bparms;

    bparms  = (struct nocache_read_request *) bypassparms;
#endif

    if (!avc->vpacRock || !tc)
	return -1;
    /* make sure no callbacks happened in the meantime */
    if (!(avc->f.states & CStatd)) {
	afs_warn("vpac_fetchInit: callback for %u.%u.%u proceeding with %s\n",
			avc->f.fid.Fid.Volume, avc->f.fid.Fid.Vnode,
			avc->f.fid.Fid.Unique,
			(avc->protocol == 2) ? "rxosd":"rxfs");
	return -1;
    }
    r = (struct vpacRock *) avc->vpacRock;
    if (!r || !r->fp) {	/* File in vicep partition not open */
	return EINVAL;
    }
    ALLOC_VICEP(v, struct vpac_Variables);
    if (!v) 
        osi_Panic("vpac_fetchInit: ALLOC_VICEP returned NULL\n");
    memset(v, 0, sizeof(struct vpac_Variables));
    v->fP = fP;
    v->avc = avc;
    v->fs_conn = tc;
    v->fp = r->fp;
    v->base = base;
    v->osd = osd;
    if (r->transid) {
	if (r->expires + 1 <= osi_Time()) {
	    RX_AFS_GUNLOCK();
	    code = RXAFS_ExtendAsyncFetch(tc->id, &avc->f.fid.Fid, r->transid,
					&r->expires);
	    RX_AFS_GLOCK();
	    if (code) {
	        r->transid = 0;
		r->expires = 0;
		return code;
	    }
	    r->expires += osi_Time();
	}
    }
    if (!(r->transid)) {
        if (transid) { /* Called from fake_vpac_fetchInit from rxosd_fetchInit */
	    if (vicep_fastread) {
	        r->transid = transid;
	        r->expires = expires;
		r->user[0] = tc->user->uid;
		r->osd = osd;
	    } else {
	        v->transid = transid;
	        v->expires = expires;
	    }
        } else { 	/* called from vpac_fetchInit (visible fileserver part.) */
	    struct RWparm p;
	    afs_uint64 *transidP;
	    p.type = 1;
            v->a.type = 3;
	    v->a.async_u.p3.path.path_info_val = NULL;
	    v->a.async_u.p3.path.path_info_len = 0;
	    
            RX_AFS_GUNLOCK();
	    if (vicep_fastread) {
	        p.RWparm_u.p1.offset = 0;
	        p.RWparm_u.p1.length = MAX_L;
		transidP = &r->transid;
	    } else {
	        p.RWparm_u.p1.offset = base;
	        p.RWparm_u.p1.length = bytes;
		transidP = &v->transid;
	    }
            code = RXAFS_StartAsyncFetch(tc->id, 
			    (struct AFSFid *) &avc->f.fid.Fid,
			    &p, &v->a, transidP, &r->expires,
			    &v->OutStatus, &v->CallBack);
            RX_AFS_GLOCK();
            if (code) {
		afs_warn("RXAFS_StartAsyncFetch for %u.%u.%u.%u returns %d\n",
				avc->f.fid.Cell, avc->f.fid.Fid.Volume,
				avc->f.fid.Fid.Vnode, avc->f.fid.Fid.Unique, code);
#if defined(AFS_BYPASS_CACHE) && defined(AFS_LINUX24_ENV)
        	if (bypassparms) {
        	    unlock_and_release_pages(bparms->auio);
        	}
#endif
    		FREE_VICEP(v, struct vpac_Variables);
	        return code;
            }
        }
    }
    tlength = avc->f.m.Length;
#if defined(AFS_LINUX26_ENV)
    ilength = i_size_read(v->fp->f_dentry->d_inode);
    if (!lustre_hack) 		/* not LUSTRE */
	tlength = ilength;
    else if (ilength > tlength)	/* LUSTRE sometimes doesn't fill it correctly */
	tlength = ilength;
    tlength -= base;
#elif defined(AFS_AIX52_ENV)
    code = VNOP_GETATTR(v->fp, &tvattr, &afs_osi_cred);
    if (code)
        afs_warn("vpac_fetchInit: VNOP_GETATTR returns %d\n", code);
    tlength = tvattr.va_size;
#endif
    if (tlength < 0)
	tlength = 0;
    *length = tlength < bytes ? tlength : bytes;
    if (lustre_hack && !*length ) {
	if (avc->f.fid.Fid.Vnode & 1) { /* directory */
	    *length = avc->f.m.Length;
	} else {
	    afs_warn("Bad i_size %llu for %u.%u.%u at offset %llu\n",
			ilength,
			avc->f.fid.Fid.Volume,
			avc->f.fid.Fid.Vnode,
			avc->f.fid.Fid.Unique,
			base);
	    *length = bytes;
	}
    }
#if defined(AFS_CACHE_BYPASS) && defined(AFS_LINUX26_ENV)
    if (bypassparms) {
	v->bypassparms = bypassparms;
	*ops = &vpac_fetchBypassCacheOps;
    } else
#endif
    if (cacheDiskType == AFS_FCACHE_TYPE_UFS) {
	v->tbuffer = osi_AllocLargeSpace(AFS_LRALLOCSIZ);
	if (!v->tbuffer) 
	    osi_Panic
              ("vpac_fetchInit: osi_AllocLargeSpace for iovecs returned NULL\n");
 	v->bufsize = AFS_LRALLOCSIZ;
	v->bp = v->tbuffer;
	*ops = &vpac_fetchUfsOps;
    } else {
	if (*length > mceP->dataSize) {
	    afs_int32 code;
	    code = afs_MemExtendEntry(mceP, *length);
	    if (code) {
		FREE_VICEP(v, struct vpac_Variables);
                return code;
            }
	}
	*ops = &vpac_fetchMemOps;
	v->bufsize = mceP->dataSize;
	v->bp = mceP->data;
    }
/*  printf("vpac_fetchInit incr'ing refCnt for %u.%u.%u from %d to %d\n",
		v->avc->f.fid.Fid.Volume,
		v->avc->f.fid.Fid.Vnode,
		v->avc->f.fid.Fid.Unique,
		r->refCnt,
		r->refCnt +1); */
    r->refCnt++;
    v->vpacRock = r;
    v->ops = (void *) *ops;
    *rock = (void *)v;
    return 0;
#endif
}

afs_int32
vpac_fetchInit(struct afs_conn *tc, struct vcache *avc, afs_offs_t base, 
		afs_uint32 bytes, afs_uint32 *length,
		void *bypassparms,
  		struct osi_file *fP, struct vrequest *areq,
	        struct fetchOps **ops, void **rock)
{
    afs_int32 code;

    code = common_fetchInit(tc, avc, base, bytes, length, bypassparms, fP,
				areq, ops, rock, 0, 0, 0);
    return code;
}

/*
 *  Called from rxosd_fetchInit if osd file is visible
 *
 */
afs_int32
fake_vpac_fetchInit(struct afs_conn *tc, struct vcache *avc, afs_offs_t base, 
		afs_uint32 bytes, afs_uint32 *length, void *bypassparms,
  		struct osi_file *fP, struct vrequest *areq,
	        struct fetchOps **ops, void **rock, afs_uint64 transid,
		afs_uint32 expires, afs_uint32 osd)
{
    afs_int32 code;

    code = common_fetchInit(tc, avc, base, bytes, length, bypassparms, fP,
				areq, ops, rock, transid, expires, osd);
    return code;
}

static afs_int32
open_vicep_file(struct vcache *avc, char *path)
{
#if !defined(AFS_LINUX26_ENV) && !defined(AFS_AIX53_ENV)
    return ENOENT;
#else
    afs_int32 code = 0;
#ifdef AFS_LINUX26_ENV
    mm_segment_t fs;
    struct nameidata nd;
    uid_t fsuid;
    struct file *fp = 0;
#endif
#if defined(AFS_AIX53_ENV)
    struct vnode *fp = 0;
#endif
    struct vpacRock *r = 0;

    if (avc->vpacRock) 
	return 0;
    RX_AFS_GUNLOCK();
#if defined(AFS_LINUX26_ENV)
    fsuid = current_fsuid();
    fs = get_fs();
    set_fs(KERNEL_DS);
#if !defined(STRUCT_TASK_STRUCT_HAS_CRED)
    current_fsuid() = 0;
#endif
    code = path_lookup(path, 0, &nd);
    if (!code) {
#ifdef NAMEI_DATA_HAS_NO_DENTRY
	fp = nameidata_to_filp(&nd, O_LARGEFILE | O_RDWR);
#else /* NAMEI_DATA_HAS_NO_DENTRY */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,27)
#if defined(STRUCT_TASK_STRUCT_HAS_CRED)
	fp = dentry_open(nd.path.dentry, nd.path.mnt, O_LARGEFILE | O_RDWR,
			 cache_creds);
	if (IS_ERR(fp))
	    fp = dentry_open(nd.path.dentry, nd.path.mnt, O_LARGEFILE | O_RDWR,
			 current_cred());
#else /* defined(STRUCT_TASK_STRUCT_HAS_CRED) */
	fp = dentry_open(nd.path.dentry, nd.path.mnt, O_LARGEFILE | O_RDWR);
#endif /* defined(STRUCT_TASK_STRUCT_HAS_CRED) */
#else /* LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,27) */
	fp = dentry_open(nd.dentry, nd.mnt, O_LARGEFILE | O_RDWR);
#endif /* LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,27) */
#endif /* NAMEI_DATA_HAS_NO_DENTRY */
    }
#if !defined(STRUCT_TASK_STRUCT_HAS_CRED)
    current_fsuid() = fsuid;
#endif
    set_fs(fs);
    if (!code && IS_ERR(fp)) {
	afs_warn("dentry_open returns %ld fsuid %d for %s\n", 
		(long)fp, fsuid, path);
	code = ENOENT;
    } 
#elif defined(AFS_AIX53_ENV)
    code = gop_lookupname(path, AFS_UIOSYS, 1, NULL, &fp);
#else
    code = ENOENT;
#endif
    RX_AFS_GLOCK();
    if (!code) {
	ALLOC_VICEP(r, struct vpacRock);
	memset(r, 0, sizeof(struct vpacRock));
	r->fp = fp;
	if (r->fp) { 
	    avc->vpacRock = (void *)r;
	} else {
	    afs_warn("open_vicep_file failed for %s\n", path);
	    FREE_VICEP(r, struct vpacRock);
	} 
    } else
        code = ENOENT;
    if (code)
	afs_warn("open_vicep_file returns %d for %s\n", code, path);
    return code;
#endif
}

struct GetPathOutputs {
    afs_uint64 ino;
    afs_uint32 lun;
    afs_uint32 algorithm;
    afs_uint32 RWvol;
    afsUUID uuid;
    char path[256];
};

/* Called with write lock on avc */
void
afs_open_vicep_localFile(struct vcache *avc, struct vrequest *treq)
{
    afs_int32 code, i;

    if ((afs_protocols & VICEP_ACCESS) && (avc->f.states & CPartVisible))  {
        long startTime;
        struct afs_conn *tc;

	if (avc->protocol & RX_OSD)
	    return;
        tc = afs_Conn(&avc->f.fid, treq, SHARED_LOCK);
        if (tc) {
	    struct async a;
            startTime = osi_Time();
	    a.type = 3;
	    a.async_u.p3.path.path_info_val = NULL;
	    a.async_u.p3.path.path_info_len = 0;
            RX_AFS_GUNLOCK();
	    code = RXAFS_GetPath(tc->id, &avc->f.fid.Fid, &a );
	    RX_AFS_GLOCK();
	    if (!code) {
		for (i=0; i<nServerUuids; i++) {
		    if (afs_visiblePart[i].lun == a.async_u.p3.lun &&
		        !memcmp(&a.async_u.p3.uuid, &afs_visiblePart[i].uuid, 
			    sizeof(struct afsUUID))) 
			break;
		}
		if (i<nServerUuids) {
		    char path[128];
		    sprintf(path, "%s/%s", afs_visiblePart[i].path,
				a.async_u.p3.path.path_info_val);	    
		    code = open_vicep_file(avc, path);
                    if (avc->vpacRock) {
			struct vpacRock *r = avc->vpacRock;
			r->file.inode = a.async_u.p3.ino;
			r->ServerUuidIndex = i;
			avc->protocol &= ~RX_FILESERVER;
			avc->protocol |= VICEP_ACCESS; 
                        openAfsServerVnodes++;
		    }
		}
		osi_free(a.async_u.p3.path.path_info_val,
			 a.async_u.p3.path.path_info_len);
	    }
	    if (code == RXGEN_OPCODE) {
		struct GetPathOutputs *t;
	        t = (struct GetPathOutputs *)osi_Alloc(sizeof(struct GetPathOutputs));
	        memset(t, 0, sizeof(struct GetPathOutputs));
                RX_AFS_GUNLOCK();
	        code = RXAFS_GetPath0(tc->id, &avc->f.fid.Fid, &t->ino, &t->lun, 
				&t->RWvol, &t->algorithm, &t->uuid);
	        RX_AFS_GLOCK();
	        if (!code) {
		    for (i=0; i<nServerUuids; i++) {
		        if (afs_visiblePart[i].lun == t->lun &&
		          !memcmp(&t->uuid, &afs_visiblePart[i].uuid, 
					    sizeof(struct afsUUID))) 
			    break;
		    }
		    if (i<nServerUuids) {
		        code = build_path(afs_visiblePart[i].path,
				    t->lun, t->RWvol, t->ino, 
				    t->algorithm, t->path);
		        if (!code)
		            code = open_vicep_file(avc, t->path);
                        if (avc->vpacRock) {
			    struct vpacRock *r = avc->vpacRock;
			    r->file.inode = t->ino;
			    r->ServerUuidIndex = i;
			    avc->protocol &= ~RX_FILESERVER;
			    avc->protocol |= VICEP_ACCESS; 
                            openAfsServerVnodes++;
		        }
		    }
                }
                afs_osi_Free(t, sizeof(struct GetPathOutputs));
	    }
        }
    }
}

afs_int32
afs_check_for_visible_osd(struct vcache *avc, afs_uint32 osd)
{
    int i;
    if (afs_protocols & VICEP_ACCESS) {
        for (i=0; i<afs_nVisibleOsds; i++) {
	    if (avc->f.fid.Cell == afs_visibleOsd[i].cell
	      && afs_visibleOsd[i].osd == osd) 
		return 1;
	}
    }
    return 0;
}

afs_int32
afs_open_vicep_osdfile(struct vcache *avc, afs_uint32 osd, struct ometa *p, char *path) 
{
    afs_int32 code = ENOENT;
    int i;
    char fullpath[128];

    if (afs_protocols & VICEP_ACCESS) {
        for (i=0; i<afs_nVisibleOsds; i++) {
	    if (avc->f.fid.Cell == afs_visibleOsd[i].cell
	      && afs_visibleOsd[i].osd == osd) {
		if (avc->vpacRock) {
		    struct vpacRock *r = (struct vpacRock *) avc->vpacRock;
		    if (p->vsn == 1) {
		        if (r->file.obj_id != p->ometa_u.t.obj_id) /* Changed by CopyOnWrite */
			    afs_close_vicep_file(avc, NULL, 
						avc->lock.excl_locked? 1:0);
		    } else if (p->vsn == 2) {
		        if (r->file.tag != p->ometa_u.f.tag) /* Changed by CopyOnWrite */
			    afs_close_vicep_file(avc, NULL, 
						avc->lock.excl_locked? 1:0);
		    } else
			afs_warn("unknown ometa vsn %u\n", p->vsn);
		}
		sprintf(fullpath, "%s/%s", afs_visibleOsd[i].path, path);
		code = open_vicep_file(avc, fullpath);
                if (code)
		    afs_warn("Couldn't open %s\n", path);
		else if (avc->vpacRock) {
		    struct vpacRock *r = (struct vpacRock *) avc->vpacRock;
		    if (p->vsn == 1)
		        r->file.obj_id = p->ometa_u.t.obj_id;
		    else if(p->vsn == 2)
		        r->file.tag = p->ometa_u.f.tag;
		    else
			afs_warn("unknown ometa vsn %u\n", p->vsn);
		    openAfsServerVnodes++;
		} else
		    afs_warn("no vpacRock\n");
	        break;
	    }
        }
    }
    return code;
}

void
afs_close_vicep_file(struct vcache *avc, struct vrequest *areq, 
			afs_int32 locked)
{
#if defined(AFS_LINUX26_ENV) || defined(AFS_AIX53_ENV)
    struct afs_conn *tc;
    afs_int32 code;
#ifdef AFS_LINUX22_ENV
    struct file *tfp;
    mm_segment_t fs;
    uid_t fsuid = current_fsuid();
#endif
    if (avc->vpacRock) {
        int code2;
	struct vpacRock *r = (struct vpacRock *) avc->vpacRock;
/*	printf("afs_close_vicep_file for %u.%u.%u refCnt is %d\n",
			avc->f.fid.Fid.Volume,
			avc->f.fid.Fid.Vnode,
			avc->f.fid.Fid.Unique,
			r->refCnt); */
	if (r->refCnt > 0) {
/*	    afs_warn("afs_close_vicep_file for %u.%u.%u returns without closing, refcnt = %d, %s req %s\n",
			avc->f.fid.Fid.Volume,
			avc->f.fid.Fid.Vnode,
			avc->f.fid.Fid.Unique,
			r->refCnt,
			areq ? "with" : "w/o",
			locked ? " locked":""); */
	    r->closeMe++;
	    return;
	}
	if (r->transid) { /* permanently open file for read */
  	    if (areq) 
	        tc = afs_Conn(&avc->f.fid, areq, 0);
	    else {
	        struct vrequest treq;
		afs_InitReq(&treq, afs_osi_credp);
	        tc = afs_Conn(&avc->f.fid, &treq, 0);
	    }
	    if (tc) {
	        RX_AFS_GUNLOCK();
		code = RXAFS_EndAsyncFetch(tc->id, &avc->f.fid.Fid,
				r->transid, r->bytes_sent, r->osd);
	        RX_AFS_GLOCK();
	    }
	    r->transid = 0;
	    r->bytes_sent = 0;
	}
	if (!locked)
            ObtainWriteLock(&avc->lock, 411);
	if (avc->vpacRock) { /* May have disappeared in a race condition */
#ifdef AFS_LINUX22_ENV
            tfp = r->fp;
#if !defined(STRUCT_TASK_STRUCT_HAS_CRED)
            current_fsuid() = 0;
#endif
            fs = get_fs();
            set_fs(KERNEL_DS);
            RX_AFS_GUNLOCK();
            tfp->f_op = fops_get(tfp->f_dentry->d_inode->i_fop);
            code2 = filp_close(tfp, NULL);
	    /* printf("afs_close_vicep_file %u.%u.%u code2=%d, oprn files: %d\n",
			avc->f.fid.Fid.Volume, avc->f.fid.Fid.Vnode,
			avc->f.fid.Fid.Unique, code2, openAfsServerVnodes); */
            RX_AFS_GLOCK();
            set_fs(fs);
#if !defined(STRUCT_TASK_STRUCT_HAS_CRED)
            current_fsuid() = fsuid;
#endif
            if (code2)
                afs_Trace3(afs_iclSetp, CM_TRACE_WASHERE,
                       ICL_TYPE_STRING, __FILE__,
                       ICL_TYPE_INT32, __LINE__,
                       ICL_TYPE_INT32, code2);
#elif defined(AFS_AIX53_ENV)
            struct vnode *tnv = r->fp;
            ObtainWriteLock(&avc->lock, 411);
            AFS_RELE(tnv);
#endif
	    avc->vpacRock = NULL;
	    FREE_VICEP(r, struct vpacRock);
	    if (avc->protocol & VICEP_ACCESS)
		avc->protocol |= RX_FILESERVER;
	    if (!locked)
                ReleaseWriteLock(&avc->lock);
	}
        openAfsServerVnodes--;
        afs_Trace3(afs_iclSetp, CM_TRACE_WASHERE,
                   ICL_TYPE_STRING, __FILE__,
                   ICL_TYPE_INT32, __LINE__,
                   ICL_TYPE_INT32, openAfsServerVnodes);
    }
#endif
}

afs_int32
afs_compare_serveruuid(char *a)
{
    afs_int32 i;

    for (i=0; i<nServerUuids; i++) {
	if (memcmp(a, (char *)&afs_visiblePart[i].uuid, sizeof(afsUUID)) == 0)
	    return 1;
    }
    return 0;
}

/*
	parm2	afsUUID uuid 
	parm3   char 	cell[64]
	parm4 	char    path[64] 
	parm5	afs_uint32 lun
*/

afs_int32
afs_set_serveruuid(long parm2, long parm3, long parm4, long parm5)
{
    afs_int32 code = 0;
    char cellname[MAXCELLCHARS];
    struct cell *tc = 0;
    afsUUID tuuid;
    int i;

    AFS_COPYIN((char *)parm2, (caddr_t) & tuuid,
                           sizeof(struct afsUUID), code);
    if (code)
	return code;
    AFS_COPYIN((char *)parm3, cellname, MAXCELLCHARS, code);
    if (code)
	return code;
    tc = afs_GetCellByName(cellname, READ_LOCK);
    if (!tc) {
	afs_warn("afs_GetCellByName failed for %s\n", cellname);
	return EFAULT;
    }
    for (i = 0; i < nServerUuids; i++) {
        if (!memcmp((char *)&afs_visiblePart[i].uuid, (char *)&tuuid,
                                sizeof(struct afsUUID)) 
	  && afs_visiblePart[i].cell == tc->cellNum
	  && afs_visiblePart[i].lun == parm5)
            break;
    }
    if (i < MAXSERVERUUIDS) {
	AFS_COPYIN(parm4, (char *)&afs_visiblePart[i].path, 64, code);
	if (code)
	    return code;
        afs_visiblePart[i].cell = tc->cellNum;
        afs_visiblePart[i].lun = parm5;
        memcpy((char *)&afs_visiblePart[i].uuid, (char *)&tuuid,
                           sizeof(struct afsUUID));
	afs_warn("Visible partition of server with uuid %08x-%04x-%04x-%02x-%02x-%02x%02x%02x%02x%02x%02x\n",
                        tuuid.time_low, tuuid.time_mid, tuuid.time_hi_and_version,
             (unsigned char)tuuid.clock_seq_hi_and_reserved,
             (unsigned char)tuuid.clock_seq_low, (unsigned char)tuuid.node[0],
             (unsigned char)tuuid.node[1], (unsigned char)tuuid.node[2],
             (unsigned char)tuuid.node[3], (unsigned char)tuuid.node[4],
             (unsigned char)tuuid.node[5]);
        afs_warn("Visible fileserver partition lun %u cell %u == %s at %s\n",
			afs_visiblePart[i].lun,
			afs_visiblePart[i].cell,
			cellname,
			afs_visiblePart[i].path);
	if (i == nServerUuids)
                nServerUuids++;
    } else
	code = EFAULT;
	
    return code;
}

/*
	parm2	char	cell[64]
	parm3   char 	path[64]
	parm4 	afs_uint32 osd 
	parm5	afs_uint32 lun
*/

afs_int32
afs_set_visible_osd(long parm2, long parm3, long parm4, long parm5)
{
    afs_int32 code = 0;
    char cellname[MAXCELLCHARS];
    struct cell *tc = 0;
    int i;

    AFS_COPYIN((char *)parm2, cellname, MAXCELLCHARS, code);
    if (code)
	afs_warn("AFS_COPYIN for cellname failed with code %d\n", code);
    else {
        tc = afs_GetCellByName(cellname, READ_LOCK);
	if (!tc) {
	    afs_warn("afs_GetCellByName failed for %s\n", cellname);
	    code = EFAULT;
	}
    }
    if (!code) {
        for (i = 0; i < afs_nVisibleOsds; i++) {
            if (afs_visibleOsd[i].cell == tc->cellNum 
	      && afs_visibleOsd[i].osd == parm4)
                break;
        }
        if (i < MAXVISIBLEOSDS) {
	    AFS_COPYIN(parm3, (char *)&afs_visibleOsd[i].path, 64, code);
	    if (code)
		return code;
            afs_visibleOsd[i].cell = tc->cellNum;
            afs_visibleOsd[i].osd = parm4;
            afs_visibleOsd[i].lun = parm5;
	    afs_warn("Visible OSD %u lun %u cell %u == %s at %s\n",
			afs_visibleOsd[i].osd,
			afs_visibleOsd[i].lun,
			afs_visibleOsd[i].cell,
			cellname,
			afs_visibleOsd[i].path);
	    if (i == afs_nVisibleOsds)
                afs_nVisibleOsds++;
        } else
	    code = EFAULT;
    }
    return code;
}

afs_int32
afs_fast_vpac_check(struct vcache *avc, struct afs_conn *tc, afs_int32 storing,
			afs_uint32 *osd)
{
    struct vpacRock *r;
    afs_int32 code, i;

    if (avc->vpacRock) {
        r = (struct vpacRock *) avc->vpacRock;    
	if (!(r->fp)) { 
	    afs_warn("afs_fast_vpac_check no fp in vpacRock\n");
	    return -1;
	}
	if (r->transid) {
	    if (storing) {
                RX_AFS_GUNLOCK();
		code = RXAFS_EndAsyncFetch(tc->id, &avc->f.fid.Fid, r->transid,
					r->bytes_sent, r->osd);
                RX_AFS_GLOCK();
		r->transid = 0;
		r->bytes_sent = 0;
	    } else { 
		int found = 0;
    		if (!(avc->f.states & CStatd)) {
		    afs_warn("afs_fast_vpac_check !CStatd\n");
		    return -1;
    		}
		for (i=0; i<MAX_FASTREAD_USERS; i++) {
		    if (tc->user->uid == r->user[i])
			found = 1;
		    if (!r->user[i])
			break;
		}
		if (!found || r->expires < osi_Time()) {
		    afs_uint32 texpires;
	            RX_AFS_GUNLOCK();
	            code = RXAFS_ExtendAsyncFetch(tc->id, &avc->f.fid.Fid, 
						r->transid, &texpires);
	            RX_AFS_GLOCK();
	            if (code) {
		        if (r->expires < osi_Time()) {
		            r->transid = 0;
		            r->expires = 0;
		        }  
		        return code;
		    }
		    if (!found && i<MAX_FASTREAD_USERS) {
			r->user[i] = tc->user->uid;
		    }
		    r->expires = texpires;
	            r->expires += osi_Time();
		}
		*osd = r->osd;
	    }
	    return 0;
	}
    }
    return ENOENT;
}
#endif 
