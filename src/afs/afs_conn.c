/*
 * Copyright 2000, International Business Machines Corporation and others.
 * All Rights Reserved.
 * 
 * This software has been released under the terms of the IBM Public
 * License.  For details, see the LICENSE file in the top-level source
 * directory or online at http://www.openafs.org/dl/license10.html
 */

/*
 * Implements:
 */
#include <afsconfig.h>
#include "afs/param.h"


#include "afs/stds.h"
#include "afs/sysincludes.h"	/* Standard vendor system headers */

#if !defined(UKERNEL)
#if !defined(AFS_LINUX20_ENV)
#include <net/if.h>
#endif
#include <netinet/in.h>

#ifdef AFS_SGI62_ENV
#include "h/hashing.h"
#endif
#if !defined(AFS_HPUX110_ENV) && !defined(AFS_LINUX20_ENV) && !defined(AFS_DARWIN_ENV)
#include <netinet/in_var.h>
#endif /* ! AFS_HPUX110_ENV */
#endif /* !defined(UKERNEL) */

#include "afsincludes.h"	/* Afs-based standard headers */
#include "afs/afs_stats.h"	/* afs statistics */

#if	defined(AFS_SUN56_ENV)
#include <inet/led.h>
#include <inet/common.h>
#if     defined(AFS_SUN58_ENV)
#include <netinet/ip6.h>
#endif
#include <inet/ip.h>
#endif

/* Exported variables */
afs_rwlock_t afs_xconn;		/* allocation lock for new things */
afs_rwlock_t afs_xinterface;	/* for multiple client address */
afs_int32 cryptall = 0;		/* encrypt all communications */


unsigned int VNOSERVERS = 0;

/**
 * Pick a security object to use for a connection to a given server,
 * by a given user
 *
 * @param[in] conn
 *	The AFS connection for which the security object is required
 * @param[out] secLevel
 *	The security level of the returned object
 *
 * @return
 *	An rx security object. This function is guaranteed to return
 *	an object, although that object may be rxnull (with a secLevel
 *	of 0)
 */
static struct rx_securityClass *
afs_pickSecurityObject(struct afs_conn *conn, int *secLevel)
{
    struct rx_securityClass *secObj = NULL;

    /* Do we have tokens ? */
    if (conn->user->vid != UNDEFVID) {
	*secLevel = 2;
	/* kerberos tickets on channel 2 */
	secObj = rxkad_NewClientSecurityObject(
		    cryptall ? rxkad_crypt : rxkad_clear,
                    (struct ktc_encryptionKey *)conn->user->ct.HandShakeKey,
		    conn->user->ct.AuthHandle,
		    conn->user->stLen, conn->user->stp);
     }
     if (secObj == NULL) {
	*secLevel = 0;
	secObj = rxnull_NewClientSecurityObject();
     }

     return secObj;
}


/**
 * Try setting up a connection to the server containing the specified fid.
 * Gets the volume, checks if it's up and does the connection by server address.
 *
 * @param afid 
 * @param areq Request filled in by the caller.
 * @param locktype Type of lock that will be used.
 *
 * @return The conn struct, or NULL.
 */
struct afs_conn *
afs_Conn(struct VenusFid *afid, struct vrequest *areq,
	 afs_int32 locktype)
{
    u_short fsport = AFS_FSPORT;
    struct volume *tv;
    struct afs_conn *tconn = NULL;
    struct srvAddr *lowp = NULL;
    struct unixuser *tu;
    int notbusy;
    int i;
    struct srvAddr *sa1p;

    AFS_STATCNT(afs_Conn);
    /* Get fid's volume. */
    tv = afs_GetVolume(afid, areq, READ_LOCK);
    if (!tv) {
	if (areq) {
	    afs_FinalizeReq(areq);
	    areq->volumeError = 1;
	}
	return NULL;
    }

    if (tv->serverHost[0] && tv->serverHost[0]->cell) {
	fsport = tv->serverHost[0]->cell->fsport;
    } else {
	VNOSERVERS++;
    }

    /* First is always lowest rank, if it's up */
    if ((tv->status[0] == not_busy) && tv->serverHost[0]
	&& !(tv->serverHost[0]->addr->sa_flags & SRVR_ISDOWN) &&
	!(((areq->idleError > 0) || (areq->tokenError > 0))
	  && (areq->skipserver[0] == 1)))
	lowp = tv->serverHost[0]->addr;

    /* Otherwise we look at all of them. There are seven levels of
     * not_busy. This means we will check a volume seven times before it
     * is marked offline. Ideally, we only need two levels, but this
     * serves a second purpose of waiting some number of seconds before
     * the client decides the volume is offline (ie: a clone could finish
     * in this time).
     */
    for (notbusy = not_busy; (!lowp && (notbusy <= end_not_busy)); notbusy++) {
	for (i = 0; i < AFS_MAXHOSTS && tv->serverHost[i]; i++) {
	    if (((areq->tokenError > 0)||(areq->idleError > 0)) 
		&& (areq->skipserver[i] == 1))
		continue;
	    if (tv->status[i] != notbusy) {
		if (tv->status[i] == rd_busy || tv->status[i] == rdwr_busy) {
		    if (!areq->busyCount)
			areq->busyCount++;
		} else if (tv->status[i] == offline) {
		    if (!areq->volumeError)
			areq->volumeError = VOLMISSING;
		}
		continue;
	    }
	    for (sa1p = tv->serverHost[i]->addr; sa1p; sa1p = sa1p->next_sa) {
		if (sa1p->sa_flags & SRVR_ISDOWN)
		    continue;
		if (!lowp || (lowp->sa_iprank > sa1p->sa_iprank))
		    lowp = sa1p;
	    }
	}
    }
    afs_PutVolume(tv, READ_LOCK);

    if (lowp) {
	tu = afs_GetUser(areq->uid, afid->Cell, SHARED_LOCK);
	tconn = afs_ConnBySA(lowp, fsport, afid->Cell, tu, 0 /*!force */ ,
			     1 /*create */ , locktype);

	afs_PutUser(tu, SHARED_LOCK);
    }

    return tconn;
}				/*afs_Conn */


/**
 * Connects to a server by it's server address.
 *
 * @param sap Server address.
 * @param aport Server port.
 * @param acell
 * @param tu Connect as this user.
 * @param force_if_down
 * @param create
 * @param locktype Specifies type of lock to be used for this function.
 *
 * @return The new connection.
 */
struct afs_conn *
afs_ConnBySA(struct srvAddr *sap, unsigned short aport, afs_int32 acell,
	     struct unixuser *tu, int force_if_down, afs_int32 create,
	     afs_int32 locktype)
{
    struct afs_conn *tc = 0;
    struct rx_securityClass *csec;	/*Security class object */
    int isec;			/*Security index */
    int service;

    if (!sap || ((sap->sa_flags & SRVR_ISDOWN) && !force_if_down)) {
	/* sa is known down, and we don't want to force it.  */
	return NULL;
    }

    ObtainSharedLock(&afs_xconn, 15);
    /* Get conn by port and user. */
    for (tc = sap->conns; tc; tc = tc->next) {
	if (tc->user == tu && tc->port == aport) {
	    afs_int32 i, free = 0;
	    if (aport != AFS_RXOSDPORT)
		break;
	    for (i=0; i<RX_MAXCALLS; i++) {
		if ((tc->id->call[i])) {
		    if (tc->id->call[i]->state == RX_STATE_DALLY
		      || tc->id->call[i]->state == RX_STATE_NOTINIT)
		    	free = 1;
		} else
		    free = 1;
	    }
	    if (free)
	        break;
	}
    }

    if (!tc && !create) {
	/* Not found and can't create a new one. */
	ReleaseSharedLock(&afs_xconn);
	return NULL;
    }
    
    if (AFS_IS_DISCONNECTED && !AFS_IN_SYNC) {
        afs_warnuser("afs_ConnBySA: disconnected\n");
        ReleaseSharedLock(&afs_xconn);
        return NULL;
    }

    if (!tc) {
	/* No such connection structure exists.  Create one and splice it in.
	 * Make sure the server record has been marked as used (for the purposes
	 * of calculating up & down times, it's now considered to be an
	 * ``active'' server).  Also make sure the server's lastUpdateEvalTime
	 * gets set, marking the time of its ``birth''.
	 */
	UpgradeSToWLock(&afs_xconn, 37);
	tc = (struct afs_conn *)afs_osi_Alloc(sizeof(struct afs_conn));
	memset(tc, 0, sizeof(struct afs_conn));

	tc->user = tu;
	tc->port = aport;
	tc->srvr = sap;
	tc->refCount = 0;	/* bumped below */
	tc->forceConnectFS = 1;
	tc->id = (struct rx_connection *)0;
	tc->next = sap->conns;
	sap->conns = tc;
	afs_ActivateServer(sap);

	ConvertWToSLock(&afs_xconn);
    } /* end of if (!tc) */
    tc->refCount++;

    if (tu->states & UTokensBad) {
	/* we may still have an authenticated RPC connection here,
	 * we'll have to create a new, unauthenticated, connection.
	 * Perhaps a better way to do this would be to set
	 * conn->forceConnectFS on all conns when the token first goes
	 * bad, but that's somewhat trickier, due to locking
	 * constraints (though not impossible).
	 */
	if (tc->id && (rx_SecurityClassOf(tc->id) != 0)) {
	    tc->forceConnectFS = 1;	/* force recreation of connection */
	}
	tu->vid = UNDEFVID;	/* forcibly disconnect the authentication info */
    }

    if (tc->forceConnectFS) {
	UpgradeSToWLock(&afs_xconn, 38);
	csec = (struct rx_securityClass *)0;
	if (tc->id) {
	    AFS_GUNLOCK();
	    rx_DestroyConnection(tc->id);
	    AFS_GLOCK();
	}
	/*
	 * Stupid hack to determine if using vldb service or file system
	 * service.
	 */
	if (aport == sap->server->cell->vlport)
	    service = 52;
	else if (aport == AFS_RXOSDPORT)
	    service = 900;
	else
	    service = 1;
	isec = 0;

	csec = afs_pickSecurityObject(tc, &isec);

	AFS_GUNLOCK();
	tc->id = rx_NewConnection(sap->sa_ip, aport, service, csec, isec);
	AFS_GLOCK();
	if (service == 52) {
	    rx_SetConnHardDeadTime(tc->id, afs_rx_harddead);
	}
	/* set to a RX_CALL_TIMEOUT error to allow MTU retry to trigger */
	rx_SetServerConnIdleDeadErr(tc->id, RX_CALL_DEAD);
	rx_SetConnIdleDeadTime(tc->id, afs_rx_idledead);
	rx_SetMsgsizeRetryErr(tc->id, RX_MSGSIZE);

	/*
	 * Only do this for the base connection, not per-user.
	 * Will need to be revisited if/when CB gets security.
	 */
	if ((isec == 0) && (service != 52) && !(tu->states & UTokensBad) &&
	    (tu->vid == UNDEFVID))
	    rx_SetConnSecondsUntilNatPing(tc->id, 20);

	tc->forceConnectFS = 0;	/* apparently we're appropriately connected now */
	if (csec)
	    rxs_Release(csec);
	ConvertWToSLock(&afs_xconn);
    } /* end of if (tc->forceConnectFS)*/

    ReleaseSharedLock(&afs_xconn);
    return tc;
}

/**
 * forceConnectFS is set whenever we must recompute the connection. UTokensBad
 * is true only if we know that the tokens are bad.  We thus clear this flag
 * when we get a new set of tokens..
 * Having force... true and UTokensBad true simultaneously means that the tokens
 * went bad and we're supposed to create a new, unauthenticated, connection.
 *
 * @param aserver Server to connect to.
 * @param aport Connection port.
 * @param acell The cell where all of this happens.
 * @param areq The request.
 * @param aforce Force connection?
 * @param locktype Type of lock to be used.
 *
 * @return The established connection.
 */
struct afs_conn *
afs_ConnByHost(struct server *aserver, unsigned short aport, afs_int32 acell,
	       struct vrequest *areq, int aforce, afs_int32 locktype)
{
    struct unixuser *tu;
    struct afs_conn *tc = 0;
    struct srvAddr *sa = 0;

    AFS_STATCNT(afs_ConnByHost);

    if (AFS_IS_DISCONNECTED && !AFS_IN_SYNC) {
        afs_warnuser("afs_ConnByHost: disconnected\n");
        return NULL;
    }

/* 
  1.  look for an existing connection
  2.  create a connection at an address believed to be up
      (if aforce is true, create a connection at the first address)
*/

    tu = afs_GetUser(areq->uid, acell, SHARED_LOCK);

    for (sa = aserver->addr; sa; sa = sa->next_sa) {
	tc = afs_ConnBySA(sa, aport, acell, tu, aforce,
			  0 /*don't create one */ ,
			  locktype);
	if (tc)
	    break;
    }

    if (!tc) {
	for (sa = aserver->addr; sa; sa = sa->next_sa) {
	    tc = afs_ConnBySA(sa, aport, acell, tu, aforce,
			      1 /*create one */ ,
			      locktype);
	    if (tc)
		break;
	}
    }

    afs_PutUser(tu, SHARED_LOCK);
    return tc;

}				/*afs_ConnByHost */


/**
 * Connect by multiple hosts.
 * Try to connect to one of the hosts from the ahosts array.
 *
 * @param ahosts Multiple hosts to connect to.
 * @param aport Connection port.
 * @param acell The cell where all of this happens.
 * @param areq The request.
 * @param locktype Type of lock to be used.
 *
 * @return The established connection or NULL.
 */
struct afs_conn *
afs_ConnByMHosts(struct server *ahosts[], unsigned short aport,
		 afs_int32 acell, struct vrequest *areq,
		 afs_int32 locktype)
{
    afs_int32 i;
    struct afs_conn *tconn;
    struct server *ts;

    /* try to find any connection from the set */
    AFS_STATCNT(afs_ConnByMHosts);
    for (i = 0; i < AFS_MAXCELLHOSTS; i++) {
	if ((ts = ahosts[i]) == NULL)
	    break;
	tconn = afs_ConnByHost(ts, aport, acell, areq, 0, locktype);
	if (tconn) {
	    return tconn;
	}
    }
    return NULL;

}				/*afs_ConnByMHosts */


/**
 * Decrement reference count to this connection.
 * @param ac
 * @param locktype
 */
void
afs_PutConn(struct afs_conn *ac, afs_int32 locktype)
{
    AFS_STATCNT(afs_PutConn);
    ac->refCount--;
}				/*afs_PutConn */


/** 
 * For multi homed clients, a RPC may timeout because of a 
 * client network interface going down. We need to reopen new 
 * connections in this case.
 *
 * @param sap Server address.
 */
void
ForceNewConnections(struct srvAddr *sap)
{
    struct afs_conn *tc = 0;

    if (!sap)
	return;			/* defensive check */

    ObtainWriteLock(&afs_xconn, 413);
    for (tc = sap->conns; tc; tc = tc->next)
	tc->forceConnectFS = 1;
    ReleaseWriteLock(&afs_xconn);
}
