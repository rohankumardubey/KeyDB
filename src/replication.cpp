/* Asynchronous replication implementation.
 *
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2019 John Sully <john at eqalpha dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */


#include "server.h"
#include "cluster.h"

#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <mutex>
#include <algorithm>
#include <uuid/uuid.h>
#include <chrono>

void replicationDiscardCachedMaster(redisMaster *mi);
void replicationResurrectCachedMaster(redisMaster *mi, int newfd);
void replicationSendAck(redisMaster *mi);
void putSlaveOnline(client *replica);
int cancelReplicationHandshake(redisMaster *mi);
static void propagateMasterStaleKeys();

/* --------------------------- Utility functions ---------------------------- */

/* Return the pointer to a string representing the replica ip:listening_port
 * pair. Mostly useful for logging, since we want to log a replica using its
 * IP address and its listening port which is more clear for the user, for
 * example: "Closing connection with replica 10.1.2.3:6380". */
char *replicationGetSlaveName(client *c) {
    static char buf[NET_PEER_ID_LEN];
    char ip[NET_IP_STR_LEN];

    ip[0] = '\0';
    buf[0] = '\0';
    if (c->slave_ip[0] != '\0' ||
        anetPeerToString(c->fd,ip,sizeof(ip),NULL) != -1)
    {
        /* Note that the 'ip' buffer is always larger than 'c->slave_ip' */
        if (c->slave_ip[0] != '\0') memcpy(ip,c->slave_ip,sizeof(c->slave_ip));

        if (c->slave_listening_port)
            anetFormatAddr(buf,sizeof(buf),ip,c->slave_listening_port);
        else
            snprintf(buf,sizeof(buf),"%s:<unknown-replica-port>",ip);
    } else {
        snprintf(buf,sizeof(buf),"client id #%llu",
            (unsigned long long) c->id);
    }
    return buf;
}

static bool FSameUuidNoNil(const unsigned char *a, const unsigned char *b)
{
    unsigned char zeroCheck = 0;
    for (int i = 0; i < UUID_BINARY_LEN; ++i)
    {
        if (a[i] != b[i])
            return false;
        zeroCheck |= a[i];
    }
    return (zeroCheck != 0);    // if the UUID is nil then it is never equal
}

static bool FSameHost(client *clientA, client *clientB)
{
    if (clientA == nullptr || clientB == nullptr)
        return false;

    const unsigned char *a = clientA->uuid;
    const unsigned char *b = clientB->uuid;

    return FSameUuidNoNil(a, b);
}

static bool FMasterHost(client *c)
{
    listIter li;
    listNode *ln;
    listRewind(g_pserver->masters, &li);
    while ((ln = listNext(&li)))
    {
        redisMaster *mi = (redisMaster*)listNodeValue(ln);
        if (FSameUuidNoNil(mi->master_uuid, c->uuid))
            return true;
    }
    return false;
}

static bool FAnyDisconnectedMasters()
{
    listIter li;
    listNode *ln;
    listRewind(g_pserver->masters, &li);
    while ((ln = listNext(&li)))
    {
        redisMaster *mi = (redisMaster*)listNodeValue(ln);
        if (mi->repl_state != REPL_STATE_CONNECTED)
            return true;
    }
    return false;
}

client *replicaFromMaster(redisMaster *mi)
{
    if (mi->master == nullptr)
        return nullptr;

    listIter liReplica;
    listNode *lnReplica;
    listRewind(g_pserver->slaves, &liReplica);
    while ((lnReplica = listNext(&liReplica)) != nullptr)
    {
        client *replica = (client*)listNodeValue(lnReplica);
        if (FSameHost(mi->master, replica))
            return replica;
    }
    return nullptr;
}

/* ---------------------------------- MASTER -------------------------------- */

void createReplicationBacklog(void) {
    serverAssert(g_pserver->repl_backlog == NULL);
    g_pserver->repl_backlog = (char*)zmalloc(g_pserver->repl_backlog_size, MALLOC_LOCAL);
    g_pserver->repl_backlog_histlen = 0;
    g_pserver->repl_backlog_idx = 0;

    /* We don't have any data inside our buffer, but virtually the first
     * byte we have is the next byte that will be generated for the
     * replication stream. */
    g_pserver->repl_backlog_off = g_pserver->master_repl_offset+1;
}

/* This function is called when the user modifies the replication backlog
 * size at runtime. It is up to the function to both update the
 * g_pserver->repl_backlog_size and to resize the buffer and setup it so that
 * it contains the same data as the previous one (possibly less data, but
 * the most recent bytes, or the same data and more free space in case the
 * buffer is enlarged). */
void resizeReplicationBacklog(long long newsize) {
    if (newsize < CONFIG_REPL_BACKLOG_MIN_SIZE)
        newsize = CONFIG_REPL_BACKLOG_MIN_SIZE;
    if (g_pserver->repl_backlog_size == newsize) return;

    g_pserver->repl_backlog_size = newsize;
    if (g_pserver->repl_backlog != NULL) {
        /* What we actually do is to flush the old buffer and realloc a new
         * empty one. It will refill with new data incrementally.
         * The reason is that copying a few gigabytes adds latency and even
         * worse often we need to alloc additional space before freeing the
         * old buffer. */
        zfree(g_pserver->repl_backlog);
        g_pserver->repl_backlog = (char*)zmalloc(g_pserver->repl_backlog_size, MALLOC_LOCAL);
        g_pserver->repl_backlog_histlen = 0;
        g_pserver->repl_backlog_idx = 0;
        /* Next byte we have is... the next since the buffer is empty. */
        g_pserver->repl_backlog_off = g_pserver->master_repl_offset+1;
    }
}

void freeReplicationBacklog(void) {
    serverAssert(GlobalLocksAcquired());
    listIter li;
    listNode *ln;
    listRewind(g_pserver->slaves, &li);
    while ((ln = listNext(&li))) {
        // g_pserver->slaves should be empty, or filled with clients pending close
        client *c = (client*)listNodeValue(ln);
        serverAssert(c->flags & CLIENT_CLOSE_ASAP || FMasterHost(c));
    }
    zfree(g_pserver->repl_backlog);
    g_pserver->repl_backlog = NULL;
}

/* Add data to the replication backlog.
 * This function also increments the global replication offset stored at
 * g_pserver->master_repl_offset, because there is no case where we want to feed
 * the backlog without incrementing the offset. */
void feedReplicationBacklog(const void *ptr, size_t len) {
    serverAssert(GlobalLocksAcquired());
    const unsigned char *p = (const unsigned char*)ptr;

    g_pserver->master_repl_offset += len;

    /* This is a circular buffer, so write as much data we can at every
     * iteration and rewind the "idx" index if we reach the limit. */
    while(len) {
        size_t thislen = g_pserver->repl_backlog_size - g_pserver->repl_backlog_idx;
        if (thislen > len) thislen = len;
        memcpy(g_pserver->repl_backlog+g_pserver->repl_backlog_idx,p,thislen);
        g_pserver->repl_backlog_idx += thislen;
        if (g_pserver->repl_backlog_idx == g_pserver->repl_backlog_size)
            g_pserver->repl_backlog_idx = 0;
        len -= thislen;
        p += thislen;
        g_pserver->repl_backlog_histlen += thislen;
    }
    if (g_pserver->repl_backlog_histlen > g_pserver->repl_backlog_size)
        g_pserver->repl_backlog_histlen = g_pserver->repl_backlog_size;
    /* Set the offset of the first byte we have in the backlog. */
    g_pserver->repl_backlog_off = g_pserver->master_repl_offset -
                              g_pserver->repl_backlog_histlen + 1;
}

/* Wrapper for feedReplicationBacklog() that takes Redis string objects
 * as input. */
void feedReplicationBacklogWithObject(robj *o) {
    char llstr[LONG_STR_SIZE];
    void *p;
    size_t len;

    if (o->encoding == OBJ_ENCODING_INT) {
        len = ll2string(llstr,sizeof(llstr),(long)ptrFromObj(o));
        p = llstr;
    } else {
        len = sdslen((sds)ptrFromObj(o));
        p = ptrFromObj(o);
    }
    feedReplicationBacklog(p,len);
}

void replicationFeedSlave(client *replica, int dictid, robj **argv, int argc, bool fSendRaw)
{
    char llstr[LONG_STR_SIZE];
    std::unique_lock<decltype(replica->lock)> lock(replica->lock);

    /* Send SELECT command to every replica if needed. */
    if (g_pserver->replicaseldb != dictid) {
        robj *selectcmd;

        /* For a few DBs we have pre-computed SELECT command. */
        if (dictid >= 0 && dictid < PROTO_SHARED_SELECT_CMDS) {
            selectcmd = shared.select[dictid];
        } else {
            int dictid_len;

            dictid_len = ll2string(llstr,sizeof(llstr),dictid);
            selectcmd = createObject(OBJ_STRING,
                sdscatprintf(sdsempty(),
                "*2\r\n$6\r\nSELECT\r\n$%d\r\n%s\r\n",
                dictid_len, llstr));
        }

        /* Add the SELECT command into the backlog. */
        /* We don't do this for advanced replication because this will be done later when it adds the whole RREPLAY command */
        if (g_pserver->repl_backlog && fSendRaw) feedReplicationBacklogWithObject(selectcmd);

        /* Send it to slaves */
        addReply(replica,selectcmd);

        if (dictid < 0 || dictid >= PROTO_SHARED_SELECT_CMDS)
            decrRefCount(selectcmd);
    }
    g_pserver->replicaseldb = dictid;

    /* Feed slaves that are waiting for the initial SYNC (so these commands
     * are queued in the output buffer until the initial SYNC completes),
     * or are already in sync with the master. */

    /* Add the multi bulk length. */
    addReplyArrayLen(replica,argc);

    /* Finally any additional argument that was not stored inside the
        * static buffer if any (from j to argc). */
    for (int j = 0; j < argc; j++)
        addReplyBulk(replica,argv[j]);
}

/* Propagate write commands to slaves, and populate the replication backlog
 * as well. This function is used if the instance is a master: we use
 * the commands received by our clients in order to create the replication
 * stream. Instead if the instance is a replica and has sub-slaves attached,
 * we use replicationFeedSlavesFromMaster() */
void replicationFeedSlaves(list *slaves, int dictid, robj **argv, int argc) {
    listNode *ln, *lnReply;
    listIter li, liReply;
    int j, len;
    serverAssert(GlobalLocksAcquired());
    if (dictid < 0)
        dictid = 0; // this can happen if we send a PING before any real operation

    /* If the instance is not a top level master, return ASAP: we'll just proxy
     * the stream of data we receive from our master instead, in order to
     * propagate *identical* replication stream. In this way this replica can
     * advertise the same replication ID as the master (since it shares the
     * master replication history and has the same backlog and offsets). */
    if (!g_pserver->fActiveReplica && listLength(g_pserver->masters)) return;

    /* If there aren't slaves, and there is no backlog buffer to populate,
     * we can return ASAP. */
    if (g_pserver->repl_backlog == NULL && listLength(slaves) == 0) return;

    /* We can't have slaves attached and no backlog. */
    serverAssert(!(listLength(slaves) != 0 && g_pserver->repl_backlog == NULL));

    client *fake = createClient(-1, serverTL - g_pserver->rgthreadvar);
    fake->flags |= CLIENT_FORCE_REPLY;
    bool fSendRaw = !g_pserver->fActiveReplica;
    replicationFeedSlave(fake, dictid, argv, argc, fSendRaw); // Note: updates the repl log, keep above the repl update code below


    long long cchbuf = fake->bufpos;
    listRewind(fake->reply, &liReply);
    while ((lnReply = listNext(&liReply)))
    {
        clientReplyBlock* reply = (clientReplyBlock*)listNodeValue(lnReply);
        cchbuf += reply->used;
    }

    serverAssert(argc > 0);
    serverAssert(cchbuf > 0);

    char uuid[40] = {'\0'};
    uuid_unparse(cserver.uuid, uuid);
    char proto[1024];
    int cchProto = snprintf(proto, sizeof(proto), "*5\r\n$7\r\nRREPLAY\r\n$%d\r\n%s\r\n$%lld\r\n", (int)strlen(uuid), uuid, cchbuf);
    cchProto = std::min((int)sizeof(proto), cchProto);
    long long master_repl_offset_start = g_pserver->master_repl_offset;
    
    char szDbNum[128];
    int cchDictIdNum = snprintf(szDbNum, sizeof(szDbNum), "%d", dictid);
    int cchDbNum = snprintf(szDbNum, sizeof(szDbNum), "$%d\r\n%d\r\n", cchDictIdNum, dictid);
    cchDbNum = std::min<int>(cchDbNum, sizeof(szDbNum)); // snprintf is tricky like that

    char szMvcc[128];
    uint64_t mvccTstamp = getMvccTstamp();
    int cchMvccNum = snprintf(szMvcc, sizeof(szMvcc), "%lu", mvccTstamp);
    int cchMvcc = snprintf(szMvcc, sizeof(szMvcc), "$%d\r\n%lu\r\n", cchMvccNum, mvccTstamp);
    cchMvcc = std::min<int>(cchMvcc, sizeof(szMvcc));    // tricky snprintf

    /* Write the command to the replication backlog if any. */
    if (g_pserver->repl_backlog) 
    {
        if (fSendRaw)
        {
            char aux[LONG_STR_SIZE+3];

            /* Add the multi bulk reply length. */
            aux[0] = '*';
            len = ll2string(aux+1,sizeof(aux)-1,argc);
            aux[len+1] = '\r';
            aux[len+2] = '\n';
            feedReplicationBacklog(aux,len+3);

            for (j = 0; j < argc; j++) {
                long objlen = stringObjectLen(argv[j]);

                /* We need to feed the buffer with the object as a bulk reply
                * not just as a plain string, so create the $..CRLF payload len
                * and add the final CRLF */
                aux[0] = '$';
                len = ll2string(aux+1,sizeof(aux)-1,objlen);
                aux[len+1] = '\r';
                aux[len+2] = '\n';
                feedReplicationBacklog(aux,len+3);
                feedReplicationBacklogWithObject(argv[j]);
                feedReplicationBacklog(aux+len+1,2);
            }
        }
        else
        {
            feedReplicationBacklog(proto, cchProto);
            feedReplicationBacklog(fake->buf, fake->bufpos);
            listRewind(fake->reply, &liReply);
            while ((lnReply = listNext(&liReply)))
            {
                clientReplyBlock* reply = (clientReplyBlock*)listNodeValue(lnReply);
                feedReplicationBacklog(reply->buf(), reply->used);
            }
            const char *crlf = "\r\n";
            feedReplicationBacklog(crlf, 2);
            feedReplicationBacklog(szDbNum, cchDbNum);
            feedReplicationBacklog(szMvcc, cchMvcc);
        }
    }

    /* Write the command to every replica. */
    listRewind(slaves,&li);
    while((ln = listNext(&li))) {
        client *replica = (client*)ln->value;

        /* Don't feed slaves that are still waiting for BGSAVE to start */
        if (replica->replstate == SLAVE_STATE_WAIT_BGSAVE_START) continue;
        if (replica->flags & CLIENT_CLOSE_ASAP) continue;
        std::unique_lock<decltype(replica->lock)> lock(replica->lock, std::defer_lock);
		// When writing to clients on other threads the global lock is sufficient provided we only use AddReply*Async()
		if (FCorrectThread(replica))
			lock.lock();
        if (serverTL->current_client && FSameHost(serverTL->current_client, replica))
        {
            replica->reploff_skipped += g_pserver->master_repl_offset - master_repl_offset_start;
            continue;
        }

        if (!fSendRaw)
            addReplyProtoAsync(replica, proto, cchProto);

        addReplyProtoAsync(replica,fake->buf,fake->bufpos);
        listRewind(fake->reply, &liReply);
        while ((lnReply = listNext(&liReply)))
        {
            clientReplyBlock* reply = (clientReplyBlock*)listNodeValue(lnReply);
            addReplyProtoAsync(replica, reply->buf(), reply->used);
        }
        if (!fSendRaw)
        {
            addReplyAsync(replica,shared.crlf);
            addReplyProtoAsync(replica, szDbNum, cchDbNum);
            addReplyProtoAsync(replica, szMvcc, cchMvcc);
        }
    }

    freeClient(fake);
}

/* This function is used in order to proxy what we receive from our master
 * to our sub-slaves. */
#include <ctype.h>
void replicationFeedSlavesFromMasterStream(list *slaves, char *buf, size_t buflen) {
    listNode *ln;
    listIter li;

    /* Debugging: this is handy to see the stream sent from master
     * to slaves. Disabled with if(0). */
    if (0) {
        printf("%zu:",buflen);
        for (size_t j = 0; j < buflen; j++) {
            printf("%c", isprint(buf[j]) ? buf[j] : '.');
        }
        printf("\n");
    }

    if (g_pserver->repl_backlog) feedReplicationBacklog(buf,buflen);
    listRewind(slaves,&li);

    while((ln = listNext(&li))) {
        client *replica = (client*)ln->value;
        std::unique_lock<decltype(replica->lock)> ulock(replica->lock, std::defer_lock);
		if (FCorrectThread(replica))
			ulock.lock();
        if (FMasterHost(replica))
            continue;   // Active Active case, don't feed back

        /* Don't feed slaves that are still waiting for BGSAVE to start */
        if (replica->replstate == SLAVE_STATE_WAIT_BGSAVE_START) continue;

        addReplyProtoAsync(replica,buf,buflen);
    }
    
    if (listLength(slaves))
        ProcessPendingAsyncWrites();    // flush them to their respective threads
}

void replicationFeedMonitors(client *c, list *monitors, int dictid, robj **argv, int argc) {
    listNode *ln;
    listIter li;
    int j;
    sds cmdrepr = sdsnew("+");
    robj *cmdobj;
    struct timeval tv;
    serverAssert(GlobalLocksAcquired());

    gettimeofday(&tv,NULL);
    cmdrepr = sdscatprintf(cmdrepr,"%ld.%06ld ",(long)tv.tv_sec,(long)tv.tv_usec);
    if (c->flags & CLIENT_LUA) {
        cmdrepr = sdscatprintf(cmdrepr,"[%d lua] ",dictid);
    } else if (c->flags & CLIENT_UNIX_SOCKET) {
        cmdrepr = sdscatprintf(cmdrepr,"[%d unix:%s] ",dictid,g_pserver->unixsocket);
    } else {
        cmdrepr = sdscatprintf(cmdrepr,"[%d %s] ",dictid,getClientPeerId(c));
    }

    for (j = 0; j < argc; j++) {
        if (argv[j]->encoding == OBJ_ENCODING_INT) {
            cmdrepr = sdscatprintf(cmdrepr, "\"%ld\"", (long)ptrFromObj(argv[j]));
        } else {
            cmdrepr = sdscatrepr(cmdrepr,(char*)ptrFromObj(argv[j]),
                        sdslen((sds)ptrFromObj(argv[j])));
        }
        if (j != argc-1)
            cmdrepr = sdscatlen(cmdrepr," ",1);
    }
    cmdrepr = sdscatlen(cmdrepr,"\r\n",2);
    cmdobj = createObject(OBJ_STRING,cmdrepr);

    listRewind(monitors,&li);
    while((ln = listNext(&li))) {
        client *monitor = (client*)ln->value;
		std::unique_lock<decltype(monitor->lock)> lock(monitor->lock, std::defer_lock);
		// When writing to clients on other threads the global lock is sufficient provided we only use AddReply*Async()
		if (FCorrectThread(c))
			lock.lock();
        addReplyAsync(monitor,cmdobj);
    }
    decrRefCount(cmdobj);
}

/* Feed the replica 'c' with the replication backlog starting from the
 * specified 'offset' up to the end of the backlog. */
long long addReplyReplicationBacklog(client *c, long long offset) {
    long long j, skip, len;

    serverLog(LL_DEBUG, "[PSYNC] Replica request offset: %lld", offset);

    if (g_pserver->repl_backlog_histlen == 0) {
        serverLog(LL_DEBUG, "[PSYNC] Backlog history len is zero");
        return 0;
    }

    serverLog(LL_DEBUG, "[PSYNC] Backlog size: %lld",
             g_pserver->repl_backlog_size);
    serverLog(LL_DEBUG, "[PSYNC] First byte: %lld",
             g_pserver->repl_backlog_off);
    serverLog(LL_DEBUG, "[PSYNC] History len: %lld",
             g_pserver->repl_backlog_histlen);
    serverLog(LL_DEBUG, "[PSYNC] Current index: %lld",
             g_pserver->repl_backlog_idx);

    /* Compute the amount of bytes we need to discard. */
    skip = offset - g_pserver->repl_backlog_off;
    serverLog(LL_DEBUG, "[PSYNC] Skipping: %lld", skip);

    /* Point j to the oldest byte, that is actually our
     * g_pserver->repl_backlog_off byte. */
    j = (g_pserver->repl_backlog_idx +
        (g_pserver->repl_backlog_size-g_pserver->repl_backlog_histlen)) %
        g_pserver->repl_backlog_size;
    serverLog(LL_DEBUG, "[PSYNC] Index of first byte: %lld", j);

    /* Discard the amount of data to seek to the specified 'offset'. */
    j = (j + skip) % g_pserver->repl_backlog_size;

    /* Feed replica with data. Since it is a circular buffer we have to
     * split the reply in two parts if we are cross-boundary. */
    len = g_pserver->repl_backlog_histlen - skip;
    serverLog(LL_DEBUG, "[PSYNC] Reply total length: %lld", len);
    while(len) {
        long long thislen =
            ((g_pserver->repl_backlog_size - j) < len) ?
            (g_pserver->repl_backlog_size - j) : len;

        serverLog(LL_DEBUG, "[PSYNC] addReply() length: %lld", thislen);
        addReplySds(c,sdsnewlen(g_pserver->repl_backlog + j, thislen));
        len -= thislen;
        j = 0;
    }
    return g_pserver->repl_backlog_histlen - skip;
}

/* Return the offset to provide as reply to the PSYNC command received
 * from the replica. The returned value is only valid immediately after
 * the BGSAVE process started and before executing any other command
 * from clients. */
long long getPsyncInitialOffset(void) {
    return g_pserver->master_repl_offset;
}

/* Send a FULLRESYNC reply in the specific case of a full resynchronization,
 * as a side effect setup the replica for a full sync in different ways:
 *
 * 1) Remember, into the replica client structure, the replication offset
 *    we sent here, so that if new slaves will later attach to the same
 *    background RDB saving process (by duplicating this client output
 *    buffer), we can get the right offset from this replica.
 * 2) Set the replication state of the replica to WAIT_BGSAVE_END so that
 *    we start accumulating differences from this point.
 * 3) Force the replication stream to re-emit a SELECT statement so
 *    the new replica incremental differences will start selecting the
 *    right database number.
 *
 * Normally this function should be called immediately after a successful
 * BGSAVE for replication was started, or when there is one already in
 * progress that we attached our replica to. */
int replicationSetupSlaveForFullResync(client *replica, long long offset) {
    char buf[128];
    int buflen;

    replica->psync_initial_offset = offset;
    replica->replstate = SLAVE_STATE_WAIT_BGSAVE_END;
    /* We are going to accumulate the incremental changes for this
     * replica as well. Set replicaseldb to -1 in order to force to re-emit
     * a SELECT statement in the replication stream. */
    g_pserver->replicaseldb = -1;

    /* Don't send this reply to slaves that approached us with
     * the old SYNC command. */
    if (!(replica->flags & CLIENT_PRE_PSYNC)) {
        buflen = snprintf(buf,sizeof(buf),"+FULLRESYNC %s %lld\r\n",
                          g_pserver->replid,offset);
        if (write(replica->fd,buf,buflen) != buflen) {
            freeClientAsync(replica);
            return C_ERR;
        }
    }
    return C_OK;
}

/* This function handles the PSYNC command from the point of view of a
 * master receiving a request for partial resynchronization.
 *
 * On success return C_OK, otherwise C_ERR is returned and we proceed
 * with the usual full resync. */
int masterTryPartialResynchronization(client *c) {
    serverAssert(GlobalLocksAcquired());
    long long psync_offset, psync_len;
    char *master_replid = (char*)ptrFromObj(c->argv[1]);
    char buf[128];
    int buflen;

    /* Parse the replication offset asked by the replica. Go to full sync
     * on parse error: this should never happen but we try to handle
     * it in a robust way compared to aborting. */
    if (getLongLongFromObjectOrReply(c,c->argv[2],&psync_offset,NULL) !=
       C_OK) goto need_full_resync;

    /* Is the replication ID of this master the same advertised by the wannabe
     * replica via PSYNC? If the replication ID changed this master has a
     * different replication history, and there is no way to continue.
     *
     * Note that there are two potentially valid replication IDs: the ID1
     * and the ID2. The ID2 however is only valid up to a specific offset. */
    if (strcasecmp(master_replid, g_pserver->replid) &&
        (strcasecmp(master_replid, g_pserver->replid2) ||
         psync_offset > g_pserver->second_replid_offset))
    {
        /* Run id "?" is used by slaves that want to force a full resync. */
        if (master_replid[0] != '?') {
            if (strcasecmp(master_replid, g_pserver->replid) &&
                strcasecmp(master_replid, g_pserver->replid2))
            {
                serverLog(LL_NOTICE,"Partial resynchronization not accepted: "
                    "Replication ID mismatch (Replica asked for '%s', my "
                    "replication IDs are '%s' and '%s')",
                    master_replid, g_pserver->replid, g_pserver->replid2);
            } else {
                serverLog(LL_NOTICE,"Partial resynchronization not accepted: "
                    "Requested offset for second ID was %lld, but I can reply "
                    "up to %lld", psync_offset, g_pserver->second_replid_offset);
            }
        } else {
            serverLog(LL_NOTICE,"Full resync requested by replica %s",
                replicationGetSlaveName(c));
        }
        goto need_full_resync;
    }

    /* We still have the data our replica is asking for? */
    if (!g_pserver->repl_backlog ||
        psync_offset < g_pserver->repl_backlog_off ||
        psync_offset > (g_pserver->repl_backlog_off + g_pserver->repl_backlog_histlen))
    {
        serverLog(LL_NOTICE,
            "Unable to partial resync with replica %s for lack of backlog (Replica request was: %lld).", replicationGetSlaveName(c), psync_offset);
        if (psync_offset > g_pserver->master_repl_offset) {
            serverLog(LL_WARNING,
                "Warning: replica %s tried to PSYNC with an offset that is greater than the master replication offset.", replicationGetSlaveName(c));
        }
        goto need_full_resync;
    }

    /* If we reached this point, we are able to perform a partial resync:
     * 1) Set client state to make it a replica.
     * 2) Inform the client we can continue with +CONTINUE
     * 3) Send the backlog data (from the offset to the end) to the replica. */
    c->flags |= CLIENT_SLAVE;
    c->replstate = SLAVE_STATE_ONLINE;
    c->repl_ack_time = g_pserver->unixtime;
    c->repl_put_online_on_ack = 0;
    listAddNodeTail(g_pserver->slaves,c);

    /* We can't use the connection buffers since they are used to accumulate
     * new commands at this stage. But we are sure the socket send buffer is
     * empty so this write will never fail actually. */
    if (c->slave_capa & SLAVE_CAPA_PSYNC2) {
        buflen = snprintf(buf,sizeof(buf),"+CONTINUE %s\r\n", g_pserver->replid);
    } else {
        buflen = snprintf(buf,sizeof(buf),"+CONTINUE\r\n");
    }
    if (write(c->fd,buf,buflen) != buflen) {
        if (FCorrectThread(c))
            freeClient(c);
        else
            freeClientAsync(c);
        return C_OK;
    }
    psync_len = addReplyReplicationBacklog(c,psync_offset);
    serverLog(LL_NOTICE,
        "Partial resynchronization request from %s accepted. Sending %lld bytes of backlog starting from offset %lld.",
            replicationGetSlaveName(c),
            psync_len, psync_offset);
    /* Note that we don't need to set the selected DB at g_pserver->replicaseldb
     * to -1 to force the master to emit SELECT, since the replica already
     * has this state from the previous connection with the master. */

    refreshGoodSlavesCount();
    return C_OK; /* The caller can return, no full resync needed. */

need_full_resync:
    /* We need a full resync for some reason... Note that we can't
     * reply to PSYNC right now if a full SYNC is needed. The reply
     * must include the master offset at the time the RDB file we transfer
     * is generated, so we need to delay the reply to that moment. */
    return C_ERR;
}

/* Start a BGSAVE for replication goals, which is, selecting the disk or
 * socket target depending on the configuration, and making sure that
 * the script cache is flushed before to start.
 *
 * The mincapa argument is the bitwise AND among all the slaves capabilities
 * of the slaves waiting for this BGSAVE, so represents the replica capabilities
 * all the slaves support. Can be tested via SLAVE_CAPA_* macros.
 *
 * Side effects, other than starting a BGSAVE:
 *
 * 1) Handle the slaves in WAIT_START state, by preparing them for a full
 *    sync if the BGSAVE was successfully started, or sending them an error
 *    and dropping them from the list of slaves.
 *
 * 2) Flush the Lua scripting script cache if the BGSAVE was actually
 *    started.
 *
 * Returns C_OK on success or C_ERR otherwise. */
int startBgsaveForReplication(int mincapa) {
    serverAssert(GlobalLocksAcquired());
    int retval;
    int socket_target = g_pserver->repl_diskless_sync && (mincapa & SLAVE_CAPA_EOF);
    listIter li;
    listNode *ln;

    serverLog(LL_NOTICE,"Starting BGSAVE for SYNC with target: %s",
        socket_target ? "replicas sockets" : "disk");

    rdbSaveInfo rsi, *rsiptr;
    rsiptr = rdbPopulateSaveInfo(&rsi);
    /* Only do rdbSave* when rsiptr is not NULL,
     * otherwise replica will miss repl-stream-db. */
    if (rsiptr) {
        if (socket_target)
            retval = rdbSaveToSlavesSockets(rsiptr);
        else
            retval = rdbSaveBackground(rsiptr);
    } else {
        serverLog(LL_WARNING,"BGSAVE for replication: replication information not available, can't generate the RDB file right now. Try later.");
        retval = C_ERR;
    }

    /* If we failed to BGSAVE, remove the slaves waiting for a full
     * resynchorinization from the list of salves, inform them with
     * an error about what happened, close the connection ASAP. */
    if (retval == C_ERR) {
        serverLog(LL_WARNING,"BGSAVE for replication failed");
        listRewind(g_pserver->slaves,&li);
        while((ln = listNext(&li))) {
            client *replica = (client*)ln->value;
            std::unique_lock<decltype(replica->lock)> lock(replica->lock);

            if (replica->replstate == SLAVE_STATE_WAIT_BGSAVE_START) {
                replica->replstate = REPL_STATE_NONE;
                replica->flags &= ~CLIENT_SLAVE;
                listDelNode(g_pserver->slaves,ln);
                addReplyError(replica,
                    "BGSAVE failed, replication can't continue");
                replica->flags |= CLIENT_CLOSE_AFTER_REPLY;
            }
        }
        return retval;
    }

    /* If the target is socket, rdbSaveToSlavesSockets() already setup
     * the salves for a full resync. Otherwise for disk target do it now.*/
    if (!socket_target) {
        listRewind(g_pserver->slaves,&li);
        while((ln = listNext(&li))) {
            client *replica = (client*)ln->value;
            std::unique_lock<decltype(replica->lock)> lock(replica->lock);

            if (replica->replstate == SLAVE_STATE_WAIT_BGSAVE_START) {
                    replicationSetupSlaveForFullResync(replica,
                            getPsyncInitialOffset());
            }
        }
    }

    /* Flush the script cache, since we need that replica differences are
     * accumulated without requiring slaves to match our cached scripts. */
    if (retval == C_OK) replicationScriptCacheFlush();
    return retval;
}

/* SYNC and PSYNC command implemenation. */
void syncCommand(client *c) {
    /* ignore SYNC if already replica or in monitor mode */
    if (c->flags & CLIENT_SLAVE) return;

    /* Refuse SYNC requests if we are a replica but the link with our master
     * is not ok... */
    if (!g_pserver->fActiveReplica) {
        if (FAnyDisconnectedMasters()) {
            addReplySds(c,sdsnew("-NOMASTERLINK Can't SYNC while not connected with my master\r\n"));
            return;
        }
    }

    /* SYNC can't be issued when the server has pending data to send to
     * the client about already issued commands. We need a fresh reply
     * buffer registering the differences between the BGSAVE and the current
     * dataset, so that we can copy to other slaves if needed. */
    if (clientHasPendingReplies(c)) {
        addReplyError(c,"SYNC and PSYNC are invalid with pending output");
        return;
    }

    serverLog(LL_NOTICE,"Replica %s asks for synchronization",
        replicationGetSlaveName(c));

    /* Try a partial resynchronization if this is a PSYNC command.
     * If it fails, we continue with usual full resynchronization, however
     * when this happens masterTryPartialResynchronization() already
     * replied with:
     *
     * +FULLRESYNC <replid> <offset>
     *
     * So the replica knows the new replid and offset to try a PSYNC later
     * if the connection with the master is lost. */
    if (!strcasecmp((const char*)ptrFromObj(c->argv[0]),"psync")) {
        if (masterTryPartialResynchronization(c) == C_OK) {
            g_pserver->stat_sync_partial_ok++;
            return; /* No full resync needed, return. */
        } else {
            char *master_replid = (char*)ptrFromObj(c->argv[1]);

            /* Increment stats for failed PSYNCs, but only if the
             * replid is not "?", as this is used by slaves to force a full
             * resync on purpose when they are not albe to partially
             * resync. */
            if (master_replid[0] != '?') g_pserver->stat_sync_partial_err++;
        }
    } else {
        /* If a replica uses SYNC, we are dealing with an old implementation
         * of the replication protocol (like keydb-cli --replica). Flag the client
         * so that we don't expect to receive REPLCONF ACK feedbacks. */
        c->flags |= CLIENT_PRE_PSYNC;
    }

    /* Full resynchronization. */
    g_pserver->stat_sync_full++;

    /* Setup the replica as one waiting for BGSAVE to start. The following code
     * paths will change the state if we handle the replica differently. */
    c->replstate = SLAVE_STATE_WAIT_BGSAVE_START;
    if (g_pserver->repl_disable_tcp_nodelay)
        anetDisableTcpNoDelay(NULL, c->fd); /* Non critical if it fails. */
    c->repldbfd = -1;
    c->flags |= CLIENT_SLAVE;
    listAddNodeTail(g_pserver->slaves,c);

    /* Create the replication backlog if needed. */
    if (listLength(g_pserver->slaves) == 1 && g_pserver->repl_backlog == NULL) {
        /* When we create the backlog from scratch, we always use a new
         * replication ID and clear the ID2, since there is no valid
         * past history. */
        changeReplicationId();
        clearReplicationId2();
        createReplicationBacklog();
    }

    /* CASE 1: BGSAVE is in progress, with disk target. */
    if (g_pserver->FRdbSaveInProgress() &&
        g_pserver->rdb_child_type == RDB_CHILD_TYPE_DISK)
    {
        /* Ok a background save is in progress. Let's check if it is a good
         * one for replication, i.e. if there is another replica that is
         * registering differences since the server forked to save. */
        client *replica;
        listNode *ln;
        listIter li;

        listRewind(g_pserver->slaves,&li);
        while((ln = listNext(&li))) {
            replica = (client*)ln->value;
            if (replica->replstate == SLAVE_STATE_WAIT_BGSAVE_END) break;
        }
        
        /* To attach this replica, we check that it has at least all the
         * capabilities of the replica that triggered the current BGSAVE. */
        if (ln && ((c->slave_capa & replica->slave_capa) == replica->slave_capa)) {
            /* Perfect, the server is already registering differences for
             * another replica. Set the right state, and copy the buffer. */
            copyClientOutputBuffer(c,replica);
            replicationSetupSlaveForFullResync(c,replica->psync_initial_offset);
            serverLog(LL_NOTICE,"Waiting for end of BGSAVE for SYNC");
        } else {
            /* No way, we need to wait for the next BGSAVE in order to
             * register differences. */
            serverLog(LL_NOTICE,"Can't attach the replica to the current BGSAVE. Waiting for next BGSAVE for SYNC");
        }

    /* CASE 2: BGSAVE is in progress, with socket target. */
    } else if (g_pserver->FRdbSaveInProgress() &&
               g_pserver->rdb_child_type == RDB_CHILD_TYPE_SOCKET)
    {
        /* There is an RDB child process but it is writing directly to
         * children sockets. We need to wait for the next BGSAVE
         * in order to synchronize. */
        serverLog(LL_NOTICE,"Current BGSAVE has socket target. Waiting for next BGSAVE for SYNC");

    /* CASE 3: There is no BGSAVE is progress. */
    } else {
        if (g_pserver->repl_diskless_sync && (c->slave_capa & SLAVE_CAPA_EOF)) {
            /* Diskless replication RDB child is created inside
             * replicationCron() since we want to delay its start a
             * few seconds to wait for more slaves to arrive. */
            if (g_pserver->repl_diskless_sync_delay)
                serverLog(LL_NOTICE,"Delay next BGSAVE for diskless SYNC");
        } else {
            /* Target is disk (or the replica is not capable of supporting
             * diskless replication) and we don't have a BGSAVE in progress,
             * let's start one. */
            if (g_pserver->aof_child_pid == -1) {
                startBgsaveForReplication(c->slave_capa);
            } else {
                serverLog(LL_NOTICE,
                    "No BGSAVE in progress, but an AOF rewrite is active. "
                    "BGSAVE for replication delayed");
            }
        }
    }
    return;
}

void processReplconfUuid(client *c, robj *arg)
{
    const char *remoteUUID = nullptr;

    if (arg->type != OBJ_STRING)
        goto LError;

    remoteUUID = (const char*)ptrFromObj(arg);
    if (strlen(remoteUUID) != 36)
        goto LError;

    if (uuid_parse(remoteUUID, c->uuid) != 0)
        goto LError;

    char szServerUUID[36 + 2]; // 1 for the '+', another for '\0'
    szServerUUID[0] = '+';
    uuid_unparse(cserver.uuid, szServerUUID+1);
    addReplyProto(c, szServerUUID, 37);
    addReplyProto(c, "\r\n", 2);
    return;

LError:
    addReplyError(c, "Invalid UUID");
    return;
}

void processReplconfLicense(client *c, robj *arg)
{
    if (cserver.license_key != nullptr)
    {
        if (strcmp(cserver.license_key, szFromObj(arg)) == 0) {
            addReplyError(c, "Each replica must have a unique license key");
            c->flags |= CLIENT_CLOSE_AFTER_REPLY;
            return;
        }
    }
    addReply(c, shared.ok);
}

/* REPLCONF <option> <value> <option> <value> ...
 * This command is used by a replica in order to configure the replication
 * process before starting it with the SYNC command.
 *
 * Currently the only use of this command is to communicate to the master
 * what is the listening port of the Slave redis instance, so that the
 * master can accurately list slaves and their listening ports in
 * the INFO output.
 *
 * In the future the same command can be used in order to configure
 * the replication to initiate an incremental replication instead of a
 * full resync. */
void replconfCommand(client *c) {
    int j;

    if ((c->argc % 2) == 0) {
        /* Number of arguments must be odd to make sure that every
         * option has a corresponding value. */
        addReply(c,shared.syntaxerr);
        return;
    }

    /* Process every option-value pair. */
    for (j = 1; j < c->argc; j+=2) {
        if (!strcasecmp((const char*)ptrFromObj(c->argv[j]),"listening-port")) {
            long port;

            if ((getLongFromObjectOrReply(c,c->argv[j+1],
                    &port,NULL) != C_OK))
                return;
            c->slave_listening_port = port;
        } else if (!strcasecmp((const char*)ptrFromObj(c->argv[j]),"ip-address")) {
            sds ip = (sds)ptrFromObj(c->argv[j+1]);
            if (sdslen(ip) < sizeof(c->slave_ip)) {
                memcpy(c->slave_ip,ip,sdslen(ip)+1);
            } else {
                addReplyErrorFormat(c,"REPLCONF ip-address provided by "
                    "replica instance is too long: %zd bytes", sdslen(ip));
                return;
            }
        } else if (!strcasecmp((const char*)ptrFromObj(c->argv[j]),"capa")) {
            /* Ignore capabilities not understood by this master. */
            if (!strcasecmp((const char*)ptrFromObj(c->argv[j+1]),"eof"))
                c->slave_capa |= SLAVE_CAPA_EOF;
            else if (!strcasecmp((const char*)ptrFromObj(c->argv[j+1]),"psync2"))
                c->slave_capa |= SLAVE_CAPA_PSYNC2;
            else if (!strcasecmp((const char*)ptrFromObj(c->argv[j+1]), "activeExpire"))
                c->slave_capa |= SLAVE_CAPA_ACTIVE_EXPIRE;
        } else if (!strcasecmp((const char*)ptrFromObj(c->argv[j]),"ack")) {
            /* REPLCONF ACK is used by replica to inform the master the amount
             * of replication stream that it processed so far. It is an
             * internal only command that normal clients should never use. */
            long long offset;

            if (!(c->flags & CLIENT_SLAVE)) return;
            if ((getLongLongFromObject(c->argv[j+1], &offset) != C_OK))
                return;
            if (offset > c->repl_ack_off)
                c->repl_ack_off = offset;
            c->repl_ack_time = g_pserver->unixtime;
            /* If this was a diskless replication, we need to really put
             * the replica online when the first ACK is received (which
             * confirms replica is online and ready to get more data). */
            if (c->repl_put_online_on_ack && c->replstate == SLAVE_STATE_ONLINE)
                putSlaveOnline(c);
            /* Note: this command does not reply anything! */
            return;
        } else if (!strcasecmp((const char*)ptrFromObj(c->argv[j]),"getack")) {
            /* REPLCONF GETACK is used in order to request an ACK ASAP
             * to the replica. */
            listIter li;
            listNode *ln;
            listRewind(g_pserver->masters, &li);
            while ((ln = listNext(&li)))
            {
                replicationSendAck((redisMaster*)listNodeValue(ln));
            }
            return;
        } else if (!strcasecmp((const char*)ptrFromObj(c->argv[j]),"uuid")) {
            /* REPLCONF uuid is used to set and send the UUID of each host */
            processReplconfUuid(c, c->argv[j+1]);
            return; // the process function replies to the client for both error and success
        } else if (!strcasecmp(szFromObj(c->argv[j]),"license")) {
            processReplconfLicense(c, c->argv[j+1]);
            return;
        } else {
            addReplyErrorFormat(c,"Unrecognized REPLCONF option: %s",
                (char*)ptrFromObj(c->argv[j]));
            return;
        }
    }
    addReply(c,shared.ok);
}

/* This function puts a replica in the online state, and should be called just
 * after a replica received the RDB file for the initial synchronization, and
 * we are finally ready to send the incremental stream of commands.
 *
 * It does a few things:
 *
 * 1) Put the replica in ONLINE state (useless when the function is called
 *    because state is already ONLINE but repl_put_online_on_ack is true).
 * 2) Make sure the writable event is re-installed, since calling the SYNC
 *    command disables it, so that we can accumulate output buffer without
 *    sending it to the replica.
 * 3) Update the count of good slaves. */
void putSlaveOnline(client *replica) {
    replica->replstate = SLAVE_STATE_ONLINE;
    replica->repl_put_online_on_ack = 0;
    replica->repl_ack_time = g_pserver->unixtime; /* Prevent false timeout. */
    AssertCorrectThread(replica);
    if (aeCreateFileEvent(g_pserver->rgthreadvar[replica->iel].el, replica->fd, AE_WRITABLE|AE_WRITE_THREADSAFE,
        sendReplyToClient, replica) == AE_ERR) {
        serverLog(LL_WARNING,"Unable to register writable event for replica bulk transfer: %s", strerror(errno));
        freeClient(replica);
        return;
    }
    refreshGoodSlavesCount();
    serverLog(LL_NOTICE,"Synchronization with replica %s succeeded",
        replicationGetSlaveName(replica));
    
    if (!(replica->slave_capa & SLAVE_CAPA_ACTIVE_EXPIRE) && g_pserver->fActiveReplica)
    {
        serverLog(LL_WARNING, "Warning: replica %s does not support active expiration.  This client may not correctly process key expirations."
            "\n\tThis is OK if you are in the process of an active upgrade.", replicationGetSlaveName(replica));
        serverLog(LL_WARNING, "Connections between active replicas and traditional replicas is deprecated.  This will be refused in future versions."
            "\n\tPlease fix your replica topology");
    }
}

void sendBulkToSlave(aeEventLoop *el, int fd, void *privdata, int mask) {
    client *replica = (client*)privdata;
    UNUSED(el);
    UNUSED(mask);
    serverAssert(ielFromEventLoop(el) == replica->iel);
    char buf[PROTO_IOBUF_LEN];
    ssize_t nwritten, buflen;

    /* Before sending the RDB file, we send the preamble as configured by the
     * replication process. Currently the preamble is just the bulk count of
     * the file in the form "$<length>\r\n". */
    if (replica->replpreamble) {
        serverAssert(replica->replpreamble[0] == '$');
        nwritten = write(fd,replica->replpreamble,sdslen(replica->replpreamble));
        if (nwritten == -1) {
            serverLog(LL_VERBOSE,"Write error sending RDB preamble to replica: %s",
                strerror(errno));
            freeClient(replica);
            return;
        }
        g_pserver->stat_net_output_bytes += nwritten;
        sdsrange(replica->replpreamble,nwritten,-1);
        if (sdslen(replica->replpreamble) == 0) {
            sdsfree(replica->replpreamble);
            replica->replpreamble = NULL;
            /* fall through sending data. */
        } else {
            return;
        }
    }

    /* If the preamble was already transferred, send the RDB bulk data. */
    lseek(replica->repldbfd,replica->repldboff,SEEK_SET);
    buflen = read(replica->repldbfd,buf,PROTO_IOBUF_LEN);
    if (buflen <= 0) {
        serverLog(LL_WARNING,"Read error sending DB to replica: %s",
            (buflen == 0) ? "premature EOF" : strerror(errno));
        freeClient(replica);
        return;
    }
    if ((nwritten = write(fd,buf,buflen)) == -1) {
        if (errno != EAGAIN) {
            serverLog(LL_WARNING,"Write error sending DB to replica: %s",
                strerror(errno));
            freeClient(replica);
        }
        return;
    }
    replica->repldboff += nwritten;
    g_pserver->stat_net_output_bytes += nwritten;
    if (replica->repldboff == replica->repldbsize) {
        close(replica->repldbfd);
        replica->repldbfd = -1;
        aeDeleteFileEvent(el,replica->fd,AE_WRITABLE);
        putSlaveOnline(replica);
    }
}

/* This function is called at the end of every background saving,
 * or when the replication RDB transfer strategy is modified from
 * disk to socket or the other way around.
 *
 * The goal of this function is to handle slaves waiting for a successful
 * background saving in order to perform non-blocking synchronization, and
 * to schedule a new BGSAVE if there are slaves that attached while a
 * BGSAVE was in progress, but it was not a good one for replication (no
 * other replica was accumulating differences).
 *
 * The argument bgsaveerr is C_OK if the background saving succeeded
 * otherwise C_ERR is passed to the function.
 * The 'type' argument is the type of the child that terminated
 * (if it had a disk or socket target). */
void updateSlavesWaitingBgsave(int bgsaveerr, int type)
{
    listNode *ln;
    listIter li;
    int startbgsave = 0;
    int mincapa = -1;
    serverAssert(GlobalLocksAcquired());

    listRewind(g_pserver->slaves,&li);
    while((ln = listNext(&li))) {
        client *replica = (client*)ln->value;

        if (replica->replstate == SLAVE_STATE_WAIT_BGSAVE_START) {
            startbgsave = 1;
            mincapa = (mincapa == -1) ? replica->slave_capa :
                        (mincapa & replica->slave_capa);
        } else if (replica->replstate == SLAVE_STATE_WAIT_BGSAVE_END) {
            struct redis_stat buf;

            /* If this was an RDB on disk save, we have to prepare to send
             * the RDB from disk to the replica socket. Otherwise if this was
             * already an RDB -> Slaves socket transfer, used in the case of
             * diskless replication, our work is trivial, we can just put
             * the replica online. */
            if (type == RDB_CHILD_TYPE_SOCKET) {
                serverLog(LL_NOTICE,
                    "Streamed RDB transfer with replica %s succeeded (socket). Waiting for REPLCONF ACK from replica to enable streaming",
                        replicationGetSlaveName(replica));
                /* Note: we wait for a REPLCONF ACK message from replica in
                 * order to really put it online (install the write handler
                 * so that the accumulated data can be transferred). However
                 * we change the replication state ASAP, since our replica
                 * is technically online now. */
                replica->replstate = SLAVE_STATE_ONLINE;
                replica->repl_put_online_on_ack = 1;
                replica->repl_ack_time = g_pserver->unixtime; /* Timeout otherwise. */
            } else {
                if (bgsaveerr != C_OK) {
                    if (FCorrectThread(replica))
                        freeClient(replica);
                    else
                        freeClientAsync(replica);
                    serverLog(LL_WARNING,"SYNC failed. BGSAVE child returned an error");
                    continue;
                }
                if ((replica->repldbfd = open(g_pserver->rdb_filename,O_RDONLY)) == -1 ||
                    redis_fstat(replica->repldbfd,&buf) == -1) {
                    if (FCorrectThread(replica))
                        freeClient(replica);
                    else
                        freeClientAsync(replica);
                    serverLog(LL_WARNING,"SYNC failed. Can't open/stat DB after BGSAVE: %s", strerror(errno));
                    continue;
                }
                replica->repldboff = 0;
                replica->repldbsize = buf.st_size;
                replica->replstate = SLAVE_STATE_SEND_BULK;
                replica->replpreamble = sdscatprintf(sdsempty(),"$%lld\r\n",
                    (unsigned long long) replica->repldbsize);

                if (FCorrectThread(replica))
                {
                    aeDeleteFileEvent(g_pserver->rgthreadvar[replica->iel].el,replica->fd,AE_WRITABLE);
                    if (aeCreateFileEvent(g_pserver->rgthreadvar[replica->iel].el, replica->fd, AE_WRITABLE, sendBulkToSlave, replica) == AE_ERR) {
                        freeClient(replica);
                    }
                }
                else
                {
                    aePostFunction(g_pserver->rgthreadvar[replica->iel].el, [replica] {
                        // Because the client could have been closed while the lambda waited to run we need to
			// verify the replica is still connected
                        listIter li;
			listNode *ln;
			listRewind(g_pserver->slaves,&li);
			bool fFound = false;
			while ((ln = listNext(&li))) {
			    if (listNodeValue(ln) == replica) {
			        fFound = true;
				break;
			     }
			}
			if (!fFound)
			    return;
                        aeDeleteFileEvent(g_pserver->rgthreadvar[replica->iel].el,replica->fd,AE_WRITABLE);
                        if (aeCreateFileEvent(g_pserver->rgthreadvar[replica->iel].el, replica->fd, AE_WRITABLE, sendBulkToSlave, replica) == AE_ERR) {
                            freeClient(replica);
                        }
                    });
                }
            }
        }
    }

    if (startbgsave)
        startBgsaveForReplication(mincapa);
}

/* Change the current instance replication ID with a new, random one.
 * This will prevent successful PSYNCs between this master and other
 * slaves, so the command should be called when something happens that
 * alters the current story of the dataset. */
void changeReplicationId(void) {
    getRandomHexChars(g_pserver->replid,CONFIG_RUN_ID_SIZE);
    g_pserver->replid[CONFIG_RUN_ID_SIZE] = '\0';
}


int hexchToInt(char ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return (ch - 'a') + 10;
    return (ch - 'A') + 10;
}
void mergeReplicationId(const char *id)
{
    for (int i = 0; i < CONFIG_RUN_ID_SIZE; ++i)
    {
        const char *charset = "0123456789abcdef";
        g_pserver->replid[i] = charset[hexchToInt(g_pserver->replid[i]) ^ hexchToInt(id[i])];
    }
}

/* Clear (invalidate) the secondary replication ID. This happens, for
 * example, after a full resynchronization, when we start a new replication
 * history. */
void clearReplicationId2(void) {
    memset(g_pserver->replid2,'0',sizeof(g_pserver->replid));
    g_pserver->replid2[CONFIG_RUN_ID_SIZE] = '\0';
    g_pserver->second_replid_offset = -1;
}

/* Use the current replication ID / offset as secondary replication
 * ID, and change the current one in order to start a new history.
 * This should be used when an instance is switched from replica to master
 * so that it can serve PSYNC requests performed using the master
 * replication ID. */
void shiftReplicationId(void) {
    memcpy(g_pserver->replid2,g_pserver->replid,sizeof(g_pserver->replid));
    /* We set the second replid offset to the master offset + 1, since
     * the replica will ask for the first byte it has not yet received, so
     * we need to add one to the offset: for example if, as a replica, we are
     * sure we have the same history as the master for 50 bytes, after we
     * are turned into a master, we can accept a PSYNC request with offset
     * 51, since the replica asking has the same history up to the 50th
     * byte, and is asking for the new bytes starting at offset 51. */
    g_pserver->second_replid_offset = g_pserver->master_repl_offset+1;
    changeReplicationId();
    serverLog(LL_WARNING,"Setting secondary replication ID to %s, valid up to offset: %lld. New replication ID is %s", g_pserver->replid2, g_pserver->second_replid_offset, g_pserver->replid);
}

/* ----------------------------------- SLAVE -------------------------------- */

/* Returns 1 if the given replication state is a handshake state,
 * 0 otherwise. */
int slaveIsInHandshakeState(redisMaster *mi) {
    return mi->repl_state >= REPL_STATE_RECEIVE_PONG &&
           mi->repl_state <= REPL_STATE_RECEIVE_PSYNC;
}

/* Avoid the master to detect the replica is timing out while loading the
 * RDB file in initial synchronization. We send a single newline character
 * that is valid protocol but is guaranteed to either be sent entirely or
 * not, since the byte is indivisible.
 *
 * The function is called in two contexts: while we flush the current
 * data with emptyDb(), and while we load the new data received as an
 * RDB file from the master. */
void replicationSendNewlineToMaster(redisMaster *mi) {
    static time_t newline_sent;
    if (time(NULL) != newline_sent) {
        newline_sent = time(NULL);
        if (write(mi->repl_transfer_s,"\n",1) == -1) {
            /* Pinging back in this stage is best-effort. */
        }
    }
}

/* Callback used by emptyDb() while flushing away old data to load
 * the new dataset received by the master. */
void replicationEmptyDbCallback(void *privdata) {
    UNUSED(privdata);
    listIter li;
    listNode *ln;
    listRewind(g_pserver->masters, &li);
    while ((ln = listNext(&li)))
    {
        replicationSendNewlineToMaster((redisMaster*)listNodeValue(ln));
    }
}

/* Once we have a link with the master and the synchroniziation was
 * performed, this function materializes the master client we store
 * at g_pserver->master, starting from the specified file descriptor. */
void replicationCreateMasterClient(redisMaster *mi, int fd, int dbid) {
    serverAssert(mi->master == nullptr);
    mi->master = createClient(fd, serverTL - g_pserver->rgthreadvar);
    mi->master->flags |= CLIENT_MASTER;
    mi->master->authenticated = 1;
    mi->master->reploff = mi->master_initial_offset;
    mi->master->reploff_skipped = 0;
    mi->master->read_reploff = mi->master->reploff;
    mi->master->puser = NULL; /* This client can do everything. */
    
    memcpy(mi->master->uuid, mi->master_uuid, UUID_BINARY_LEN);
    memset(mi->master_uuid, 0, UUID_BINARY_LEN); // make sure people don't use this temp storage buffer

    memcpy(mi->master->replid, mi->master_replid,
        sizeof(mi->master_replid));
    /* If master offset is set to -1, this master is old and is not
     * PSYNC capable, so we flag it accordingly. */
    if (mi->master->reploff == -1)
        mi->master->flags |= CLIENT_PRE_PSYNC;
    if (dbid != -1) selectDb(mi->master,dbid);
}

/* This function will try to re-enable the AOF file after the
 * master-replica synchronization: if it fails after multiple attempts
 * the replica cannot be considered reliable and exists with an
 * error. */
void restartAOFAfterSYNC() {
    unsigned int tries, max_tries = 10;
    for (tries = 0; tries < max_tries; ++tries) {
        if (startAppendOnly() == C_OK) break;
        serverLog(LL_WARNING,
            "Failed enabling the AOF after successful master synchronization! "
            "Trying it again in one second.");
        sleep(1);
    }
    if (tries == max_tries) {
        serverLog(LL_WARNING,
            "FATAL: this replica instance finished the synchronization with "
            "its master, but the AOF can't be turned on. Exiting now.");
        exit(1);
    }
}

/* Asynchronously read the SYNC payload we receive from a master */
#define REPL_MAX_WRITTEN_BEFORE_FSYNC (1024*1024*8) /* 8 MB */
void readSyncBulkPayload(aeEventLoop *el, int fd, void *privdata, int mask) {
    char buf[4096];
    ssize_t nread, readlen, nwritten;
    off_t left;
    UNUSED(el);
    UNUSED(mask);
    // Should we update our database, or create from scratch?
    int fUpdate = g_pserver->fActiveReplica || g_pserver->enable_multimaster;
    redisMaster *mi = (redisMaster*)privdata;

    serverAssert(GlobalLocksAcquired());

    /* Static vars used to hold the EOF mark, and the last bytes received
     * form the server: when they match, we reached the end of the transfer. */
    static char eofmark[CONFIG_RUN_ID_SIZE];
    static char lastbytes[CONFIG_RUN_ID_SIZE];
    static int usemark = 0;

    /* When a mark is used, we want to detect EOF asap in order to avoid
     * writing the EOF mark into the file... */
    int eof_reached = 0;

    /* If repl_transfer_size == -1 we still have to read the bulk length
     * from the master reply. */
    if (mi->repl_transfer_size == -1) {
        if (syncReadLine(fd,buf,1024,g_pserver->repl_syncio_timeout*1000) == -1) {
            serverLog(LL_WARNING,
                "I/O error reading bulk count from MASTER: %s",
                strerror(errno));
            goto error;
        }

        if (buf[0] == '-') {
            serverLog(LL_WARNING,
                "MASTER aborted replication with an error: %s",
                buf+1);
            goto error;
        } else if (buf[0] == '\0') {
            /* At this stage just a newline works as a PING in order to take
             * the connection live. So we refresh our last interaction
             * timestamp. */
            mi->repl_transfer_lastio = g_pserver->unixtime;
            return;
        } else if (buf[0] != '$') {
            serverLog(LL_WARNING,"Bad protocol from MASTER, the first byte is not '$' (we received '%s'), are you sure the host and port are right?", buf);
            goto error;
        }

        /* There are two possible forms for the bulk payload. One is the
         * usual $<count> bulk format. The other is used for diskless transfers
         * when the master does not know beforehand the size of the file to
         * transfer. In the latter case, the following format is used:
         *
         * $EOF:<40 bytes delimiter>
         *
         * At the end of the file the announced delimiter is transmitted. The
         * delimiter is long and random enough that the probability of a
         * collision with the actual file content can be ignored. */
        if (strncmp(buf+1,"EOF:",4) == 0 && strlen(buf+5) >= CONFIG_RUN_ID_SIZE) {
            usemark = 1;
            memcpy(eofmark,buf+5,CONFIG_RUN_ID_SIZE);
            memset(lastbytes,0,CONFIG_RUN_ID_SIZE);
            /* Set any repl_transfer_size to avoid entering this code path
             * at the next call. */
            mi->repl_transfer_size = 0;
            serverLog(LL_NOTICE,
                "MASTER <-> REPLICA sync: receiving streamed RDB from master");
        } else {
            usemark = 0;
            mi->repl_transfer_size = strtol(buf+1,NULL,10);
            serverLog(LL_NOTICE,
                "MASTER <-> REPLICA sync: receiving %lld bytes from master",
                (long long) mi->repl_transfer_size);
        }
        return;
    }

    /* Read bulk data */
    if (usemark) {
        readlen = sizeof(buf);
    } else {
        left = mi->repl_transfer_size - mi->repl_transfer_read;
        readlen = (left < (signed)sizeof(buf)) ? left : (signed)sizeof(buf);
    }

    nread = read(fd,buf,readlen);
    if (nread <= 0) {
        serverLog(LL_WARNING,"I/O error trying to sync with MASTER: %s",
            (nread == -1) ? strerror(errno) : "connection lost");
        cancelReplicationHandshake(mi);
        return;
    }
    g_pserver->stat_net_input_bytes += nread;

    if (usemark) {
        /* Update the last bytes array, and check if it matches our delimiter.*/
        if (nread >= CONFIG_RUN_ID_SIZE) {
            memcpy(lastbytes,buf+nread-CONFIG_RUN_ID_SIZE,CONFIG_RUN_ID_SIZE);
        } else {
            int rem = CONFIG_RUN_ID_SIZE-nread;
            memmove(lastbytes,lastbytes+nread,rem);
            memcpy(lastbytes+rem,buf,nread);
        }
        if (memcmp(lastbytes,eofmark,CONFIG_RUN_ID_SIZE) == 0) eof_reached = 1;
    }

    mi->repl_transfer_lastio = g_pserver->unixtime;
    if ((nwritten = write(mi->repl_transfer_fd,buf,nread)) != nread) {
        serverLog(LL_WARNING,"Write error or short write writing to the DB dump file needed for MASTER <-> REPLICA synchronization: %s", 
            (nwritten == -1) ? strerror(errno) : "short write");
        goto error;
    }
    mi->repl_transfer_read += nread;

    /* Delete the last 40 bytes from the file if we reached EOF. */
    if (usemark && eof_reached) {
        if (ftruncate(mi->repl_transfer_fd,
            mi->repl_transfer_read - CONFIG_RUN_ID_SIZE) == -1)
        {
            serverLog(LL_WARNING,"Error truncating the RDB file received from the master for SYNC: %s", strerror(errno));
            goto error;
        }
    }

    /* Sync data on disk from time to time, otherwise at the end of the transfer
     * we may suffer a big delay as the memory buffers are copied into the
     * actual disk. */
    if (mi->repl_transfer_read >=
        mi->repl_transfer_last_fsync_off + REPL_MAX_WRITTEN_BEFORE_FSYNC)
    {
        off_t sync_size = mi->repl_transfer_read -
                          mi->repl_transfer_last_fsync_off;
        rdb_fsync_range(mi->repl_transfer_fd,
            mi->repl_transfer_last_fsync_off, sync_size);
        mi->repl_transfer_last_fsync_off += sync_size;
    }

    /* Check if the transfer is now complete */
    if (!usemark) {
        if (mi->repl_transfer_read == mi->repl_transfer_size)
            eof_reached = 1;
    }

    if (eof_reached) {
        int aof_is_enabled = g_pserver->aof_state != AOF_OFF;

        /* Ensure background save doesn't overwrite synced data */
        if (g_pserver->FRdbSaveInProgress()) {
            serverLog(LL_NOTICE,
                "Replica is about to load the RDB file received from the "
                "master, but there is a pending RDB child running. "
                "Cancelling RDB the save and removing its temp file to avoid "
                "any race");
            killRDBChild();
        }

        const char *rdb_filename = mi->repl_transfer_tmpfile;

        if (!fUpdate)
        {
            if (rename(mi->repl_transfer_tmpfile,g_pserver->rdb_filename) == -1) {
                serverLog(LL_WARNING,"Failed trying to rename the temp DB into %s in MASTER <-> REPLICA synchronization: %s", 
                    g_pserver->rdb_filename, strerror(errno));
                cancelReplicationHandshake(mi);
                return;
            }
            rdb_filename = g_pserver->rdb_filename;
        }

        serverLog(LL_NOTICE, "MASTER <-> REPLICA sync: %s", fUpdate ? "Keeping old data" : "Flushing old data");
        /* We need to stop any AOFRW fork before flusing and parsing
         * RDB, otherwise we'll create a copy-on-write disaster. */
        if(aof_is_enabled) stopAppendOnly();
        if (!fUpdate)
        {
            signalFlushedDb(-1);
            emptyDb(
                -1,
                g_pserver->repl_slave_lazy_flush ? EMPTYDB_ASYNC : EMPTYDB_NO_FLAGS,
                replicationEmptyDbCallback);
        }

        /* Before loading the DB into memory we need to delete the readable
         * handler, otherwise it will get called recursively since
         * rdbLoad() will call the event loop to process events from time to
         * time for non blocking loading. */
        aeDeleteFileEvent(el,mi->repl_transfer_s,AE_READABLE);
        serverLog(LL_NOTICE, "MASTER <-> REPLICA sync: Loading DB in memory");
        rdbSaveInfo rsi = RDB_SAVE_INFO_INIT;
        if (g_pserver->fActiveReplica)
        {
            rsi.mvccMinThreshold = mi->mvccLastSync;
            if (mi->staleKeyMap != nullptr)
                mi->staleKeyMap->clear();
            else
                mi->staleKeyMap = new (MALLOC_LOCAL) std::map<int, std::vector<robj_sharedptr>>();
            rsi.mi = mi;
        }
        if (rdbLoadFile(rdb_filename, &rsi) != C_OK) {
            serverLog(LL_WARNING,"Failed trying to load the MASTER synchronization DB from disk");
            cancelReplicationHandshake(mi);
            /* Re-enable the AOF if we disabled it earlier, in order to restore
             * the original configuration. */
            if (aof_is_enabled) restartAOFAfterSYNC();
            return;
        }
        /* Final setup of the connected replica <- master link */
        if (fUpdate)
            unlink(mi->repl_transfer_tmpfile);  // if we're not updating this became the backup RDB
        zfree(mi->repl_transfer_tmpfile);
        close(mi->repl_transfer_fd);
        replicationCreateMasterClient(mi, mi->repl_transfer_s,rsi.repl_stream_db);
        mi->repl_state = REPL_STATE_CONNECTED;
        mi->repl_down_since = 0;
        if (fUpdate)
        {
            mergeReplicationId(mi->master->replid);
        }
        else
        {
            /* After a full resynchroniziation we use the replication ID and
            * offset of the master. The secondary ID / offset are cleared since
            * we are starting a new history. */
            memcpy(g_pserver->replid,mi->master->replid,sizeof(g_pserver->replid));
            g_pserver->master_repl_offset = mi->master->reploff;
        }
        clearReplicationId2();
        /* Let's create the replication backlog if needed. Slaves need to
         * accumulate the backlog regardless of the fact they have sub-slaves
         * or not, in order to behave correctly if they are promoted to
         * masters after a failover. */
        if (g_pserver->repl_backlog == NULL) createReplicationBacklog();

        serverLog(LL_NOTICE, "MASTER <-> REPLICA sync: Finished with success");
        /* Restart the AOF subsystem now that we finished the sync. This
         * will trigger an AOF rewrite, and when done will start appending
         * to the new file. */
        if (aof_is_enabled) restartAOFAfterSYNC();
    }
    return;

error:
    cancelReplicationHandshake(mi);
    return;
}

/* Send a synchronous command to the master. Used to send AUTH and
 * REPLCONF commands before starting the replication with SYNC.
 *
 * The command returns an sds string representing the result of the
 * operation. On error the first byte is a "-".
 */
#define SYNC_CMD_READ (1<<0)
#define SYNC_CMD_WRITE (1<<1)
#define SYNC_CMD_FULL (SYNC_CMD_READ|SYNC_CMD_WRITE)
char *sendSynchronousCommand(redisMaster *mi, int flags, int fd, ...) {

    /* Create the command to send to the master, we use redis binary
     * protocol to make sure correct arguments are sent. This function
     * is not safe for all binary data. */
    if (flags & SYNC_CMD_WRITE) {
        char *arg;
        va_list ap;
        sds cmd = sdsempty();
        sds cmdargs = sdsempty();
        size_t argslen = 0;
        va_start(ap,fd);

        while(1) {
            arg = va_arg(ap, char*);
            if (arg == NULL) break;

            cmdargs = sdscatprintf(cmdargs,"$%zu\r\n%s\r\n",strlen(arg),arg);
            argslen++;
        }

        va_end(ap);

        cmd = sdscatprintf(cmd,"*%zu\r\n",argslen);
        cmd = sdscatsds(cmd,cmdargs);
        sdsfree(cmdargs);

        /* Transfer command to the g_pserver-> */
        if (syncWrite(fd,cmd,sdslen(cmd),g_pserver->repl_syncio_timeout*1000)
            == -1)
        {
            sdsfree(cmd);
            return sdscatprintf(sdsempty(),"-Writing to master: %s",
                    strerror(errno));
        }
        sdsfree(cmd);
    }

    /* Read the reply from the g_pserver-> */
    if (flags & SYNC_CMD_READ) {
        char buf[256];

        if (syncReadLine(fd,buf,sizeof(buf),g_pserver->repl_syncio_timeout*1000)
            == -1)
        {
            return sdscatprintf(sdsempty(),"-Reading from master: %s",
                    strerror(errno));
        }
        mi->repl_transfer_lastio = g_pserver->unixtime;
        return sdsnew(buf);
    }
    return NULL;
}

/* Try a partial resynchronization with the master if we are about to reconnect.
 * If there is no cached master structure, at least try to issue a
 * "PSYNC ? -1" command in order to trigger a full resync using the PSYNC
 * command in order to obtain the master run id and the master replication
 * global offset.
 *
 * This function is designed to be called from syncWithMaster(), so the
 * following assumptions are made:
 *
 * 1) We pass the function an already connected socket "fd".
 * 2) This function does not close the file descriptor "fd". However in case
 *    of successful partial resynchronization, the function will reuse
 *    'fd' as file descriptor of the g_pserver->master client structure.
 *
 * The function is split in two halves: if read_reply is 0, the function
 * writes the PSYNC command on the socket, and a new function call is
 * needed, with read_reply set to 1, in order to read the reply of the
 * command. This is useful in order to support non blocking operations, so
 * that we write, return into the event loop, and read when there are data.
 *
 * When read_reply is 0 the function returns PSYNC_WRITE_ERR if there
 * was a write error, or PSYNC_WAIT_REPLY to signal we need another call
 * with read_reply set to 1. However even when read_reply is set to 1
 * the function may return PSYNC_WAIT_REPLY again to signal there were
 * insufficient data to read to complete its work. We should re-enter
 * into the event loop and wait in such a case.
 *
 * The function returns:
 *
 * PSYNC_CONTINUE: If the PSYNC command succeeded and we can continue.
 * PSYNC_FULLRESYNC: If PSYNC is supported but a full resync is needed.
 *                   In this case the master run_id and global replication
 *                   offset is saved.
 * PSYNC_NOT_SUPPORTED: If the server does not understand PSYNC at all and
 *                      the caller should fall back to SYNC.
 * PSYNC_WRITE_ERROR: There was an error writing the command to the socket.
 * PSYNC_WAIT_REPLY: Call again the function with read_reply set to 1.
 * PSYNC_TRY_LATER: Master is currently in a transient error condition.
 *
 * Notable side effects:
 *
 * 1) As a side effect of the function call the function removes the readable
 *    event handler from "fd", unless the return value is PSYNC_WAIT_REPLY.
 * 2) g_pserver->master_initial_offset is set to the right value according
 *    to the master reply. This will be used to populate the 'g_pserver->master'
 *    structure replication offset.
 */

#define PSYNC_WRITE_ERROR 0
#define PSYNC_WAIT_REPLY 1
#define PSYNC_CONTINUE 2
#define PSYNC_FULLRESYNC 3
#define PSYNC_NOT_SUPPORTED 4
#define PSYNC_TRY_LATER 5
int slaveTryPartialResynchronization(redisMaster *mi, aeEventLoop *el, int fd, int read_reply) {
    const char *psync_replid;
    char psync_offset[32];
    sds reply;

    /* Writing half */
    if (!read_reply) {
        /* Initially set master_initial_offset to -1 to mark the current
         * master run_id and offset as not valid. Later if we'll be able to do
         * a FULL resync using the PSYNC command we'll set the offset at the
         * right value, so that this information will be propagated to the
         * client structure representing the master into g_pserver->master. */
        mi->master_initial_offset = -1;

        if (mi->cached_master && !g_pserver->fActiveReplica) {
            psync_replid = mi->cached_master->replid;
            snprintf(psync_offset,sizeof(psync_offset),"%lld", mi->cached_master->reploff+1);
            serverLog(LL_NOTICE,"Trying a partial resynchronization (request %s:%s).", psync_replid, psync_offset);
        } else {
            serverLog(LL_NOTICE,"Partial resynchronization not possible (no cached master)");
            psync_replid = "?";
            memcpy(psync_offset,"-1",3);
        }

        /* Issue the PSYNC command */
        reply = sendSynchronousCommand(mi, SYNC_CMD_WRITE,fd,"PSYNC",psync_replid,psync_offset,NULL);
        if (reply != NULL) {
            serverLog(LL_WARNING,"Unable to send PSYNC to master: %s",reply);
            sdsfree(reply);
            aeDeleteFileEvent(el,fd,AE_READABLE);
            return PSYNC_WRITE_ERROR;
        }
        return PSYNC_WAIT_REPLY;
    }

    /* Reading half */
    reply = sendSynchronousCommand(mi, SYNC_CMD_READ,fd,NULL);
    if (sdslen(reply) == 0) {
        /* The master may send empty newlines after it receives PSYNC
         * and before to reply, just to keep the connection alive. */
        sdsfree(reply);
        return PSYNC_WAIT_REPLY;
    }

    aeDeleteFileEvent(el,fd,AE_READABLE);

    if (!strncmp(reply,"+FULLRESYNC",11)) {
        char *replid = NULL, *offset = NULL;

        /* FULL RESYNC, parse the reply in order to extract the run id
         * and the replication offset. */
        replid = strchr(reply,' ');
        if (replid) {
            replid++;
            offset = strchr(replid,' ');
            if (offset) offset++;
        }
        if (!replid || !offset || (offset-replid-1) != CONFIG_RUN_ID_SIZE) {
            serverLog(LL_WARNING,
                "Master replied with wrong +FULLRESYNC syntax.");
            /* This is an unexpected condition, actually the +FULLRESYNC
             * reply means that the master supports PSYNC, but the reply
             * format seems wrong. To stay safe we blank the master
             * replid to make sure next PSYNCs will fail. */
            memset(mi->master_replid,0,CONFIG_RUN_ID_SIZE+1);
        } else {
            memcpy(mi->master_replid, replid, offset-replid-1);
            mi->master_replid[CONFIG_RUN_ID_SIZE] = '\0';
            mi->master_initial_offset = strtoll(offset,NULL,10);
            serverLog(LL_NOTICE,"Full resync from master: %s:%lld",
                mi->master_replid,
                mi->master_initial_offset);
        }
        /* We are going to full resync, discard the cached master structure. */
        replicationDiscardCachedMaster(mi);
        sdsfree(reply);
        return PSYNC_FULLRESYNC;
    }

    if (!strncmp(reply,"+CONTINUE",9)) {
        /* Partial resync was accepted. */
        serverLog(LL_NOTICE,
            "Successful partial resynchronization with master.");

        /* Check the new replication ID advertised by the master. If it
         * changed, we need to set the new ID as primary ID, and set or
         * secondary ID as the old master ID up to the current offset, so
         * that our sub-slaves will be able to PSYNC with us after a
         * disconnection. */
        char *start = reply+10;
        char *end = reply+9;
        while(end[0] != '\r' && end[0] != '\n' && end[0] != '\0') end++;
        if (end-start == CONFIG_RUN_ID_SIZE) {
            char sznew[CONFIG_RUN_ID_SIZE+1];
            memcpy(sznew,start,CONFIG_RUN_ID_SIZE);
            sznew[CONFIG_RUN_ID_SIZE] = '\0';

            if (strcmp(sznew,mi->cached_master->replid)) {
                /* Master ID changed. */
                serverLog(LL_WARNING,"Master replication ID changed to %s",sznew);

                /* Set the old ID as our ID2, up to the current offset+1. */
                memcpy(g_pserver->replid2,mi->cached_master->replid,
                    sizeof(g_pserver->replid2));
                g_pserver->second_replid_offset = g_pserver->master_repl_offset+1;

                /* Update the cached master ID and our own primary ID to the
                 * new one. */
                memcpy(g_pserver->replid,sznew,sizeof(g_pserver->replid));
                memcpy(mi->cached_master->replid,sznew,sizeof(g_pserver->replid));

                /* Disconnect all the sub-slaves: they need to be notified. */
                if (!g_pserver->fActiveReplica)
                    disconnectSlaves();
            }
        }

        /* Setup the replication to continue. */
        sdsfree(reply);
        replicationResurrectCachedMaster(mi, fd);

        /* If this instance was restarted and we read the metadata to
         * PSYNC from the persistence file, our replication backlog could
         * be still not initialized. Create it. */
        if (g_pserver->repl_backlog == NULL) createReplicationBacklog();
        return PSYNC_CONTINUE;
    }

    /* If we reach this point we received either an error (since the master does
     * not understand PSYNC or because it is in a special state and cannot
     * serve our request), or an unexpected reply from the master.
     *
     * Return PSYNC_NOT_SUPPORTED on errors we don't understand, otherwise
     * return PSYNC_TRY_LATER if we believe this is a transient error. */

    if (!strncmp(reply,"-NOMASTERLINK",13) ||
        !strncmp(reply,"-LOADING",8))
    {
        serverLog(LL_NOTICE,
            "Master is currently unable to PSYNC "
            "but should be in the future: %s", reply);
        sdsfree(reply);
        return PSYNC_TRY_LATER;
    }

    if (strncmp(reply,"-ERR",4)) {
        /* If it's not an error, log the unexpected event. */
        serverLog(LL_WARNING,
            "Unexpected reply to PSYNC from master: %s", reply);
    } else {
        serverLog(LL_NOTICE,
            "Master does not support PSYNC or is in "
            "error state (reply: %s)", reply);
    }
    sdsfree(reply);
    replicationDiscardCachedMaster(mi);
    return PSYNC_NOT_SUPPORTED;
}

/* This handler fires when the non blocking connect was able to
 * establish a connection with the master. */
void syncWithMaster(aeEventLoop *el, int fd, void *privdata, int mask) {
    serverAssert(GlobalLocksAcquired());
    char tmpfile[256], *err = NULL;
    int dfd = -1, maxtries = 5;
    int sockerr = 0, psync_result = PSYNC_FULLRESYNC;
    socklen_t errlen = sizeof(sockerr);
    UNUSED(el);
    UNUSED(mask);

    redisMaster *mi = (redisMaster*)privdata;

    /* If this event fired after the user turned the instance into a master
     * with SLAVEOF NO ONE we must just return ASAP. */
    if (mi->repl_state == REPL_STATE_NONE) {
        close(fd);
        return;
    }

    /* Check for errors in the socket: after a non blocking connect() we
     * may find that the socket is in error state. */
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &sockerr, &errlen) == -1)
        sockerr = errno;
    if (sockerr) {
        serverLog(LL_WARNING,"Error condition on socket for SYNC: %s",
            strerror(sockerr));
        goto error;
    }

    /* Send a PING to check the master is able to reply without errors. */
    if (mi->repl_state == REPL_STATE_CONNECTING) {
        serverLog(LL_NOTICE,"Non blocking connect for SYNC fired the event.");
        /* Delete the writable event so that the readable event remains
         * registered and we can wait for the PONG reply. */
        aeDeleteFileEvent(el,fd,AE_WRITABLE);
        mi->repl_state = REPL_STATE_RECEIVE_PONG;
        /* Send the PING, don't check for errors at all, we have the timeout
         * that will take care about this. */
        err = sendSynchronousCommand(mi, SYNC_CMD_WRITE,fd,"PING",NULL);
        if (err) goto write_error;
        return;
    }

    /* Receive the PONG command. */
    if (mi->repl_state == REPL_STATE_RECEIVE_PONG) {
        err = sendSynchronousCommand(mi, SYNC_CMD_READ,fd,NULL);

        /* We accept only two replies as valid, a positive +PONG reply
         * (we just check for "+") or an authentication error.
         * Note that older versions of Redis replied with "operation not
         * permitted" instead of using a proper error code, so we test
         * both. */
        if (err[0] != '+' &&
            strncmp(err,"-NOAUTH",7) != 0 &&
            strncmp(err,"-ERR operation not permitted",28) != 0)
        {
            serverLog(LL_WARNING,"Error reply to PING from master: '%s'",err);
            sdsfree(err);
            goto error;
        } else {
            serverLog(LL_NOTICE,
                "Master replied to PING, replication can continue...");
        }
        sdsfree(err);
        mi->repl_state = REPL_STATE_SEND_AUTH;
    }

    /* AUTH with the master if required. */
    if (mi->repl_state == REPL_STATE_SEND_AUTH) {
        if (mi->masteruser && mi->masterauth) {
            err = sendSynchronousCommand(mi, SYNC_CMD_WRITE,fd,"AUTH",
                                         mi->masteruser,mi->masterauth,NULL);
            if (err) goto write_error;
            mi->repl_state = REPL_STATE_RECEIVE_AUTH;
            return;
        } else if (mi->masterauth) {
            err = sendSynchronousCommand(mi, SYNC_CMD_WRITE,fd,"AUTH",mi->masterauth,NULL);
            if (err) goto write_error;
            mi->repl_state = REPL_STATE_RECEIVE_AUTH;
            return;
        } else {
            mi->repl_state = REPL_STATE_SEND_UUID;
        }
    }

    /* Receive AUTH reply. */
    if (mi->repl_state == REPL_STATE_RECEIVE_AUTH) {
        err = sendSynchronousCommand(mi, SYNC_CMD_READ,fd,NULL);
        if (err[0] == '-') {
            serverLog(LL_WARNING,"Unable to AUTH to MASTER: %s",err);
            sdsfree(err);
            goto error;
        }
        sdsfree(err);
        mi->repl_state = REPL_STATE_SEND_UUID;
    }

    /* Send UUID */
    if (mi->repl_state == REPL_STATE_SEND_UUID) {
        char szUUID[37] = {0};
        memset(mi->master_uuid, 0, UUID_BINARY_LEN);
        uuid_unparse((unsigned char*)cserver.uuid, szUUID);
        err = sendSynchronousCommand(mi, SYNC_CMD_WRITE,fd,"REPLCONF","uuid",szUUID,NULL);
        if (err) goto write_error;
        mi->repl_state = REPL_STATE_RECEIVE_UUID;
        return;
    }

    /* Receive UUID */
    if (mi->repl_state == REPL_STATE_RECEIVE_UUID) {
        err = sendSynchronousCommand(mi, SYNC_CMD_READ,fd,NULL);
        if (err[0] == '-') {
            serverLog(LL_WARNING, "non-fatal: Master doesn't understand REPLCONF uuid");
        }
        else {
            if (strlen(err) != 37   // 36-byte UUID string and the leading '+'
                || uuid_parse(err+1, mi->master_uuid) != 0)   
            {
                serverLog(LL_WARNING, "Master replied with a UUID we don't understand");
                sdsfree(err);
                goto error;
            }
        }
        sdsfree(err);
        mi->repl_state = REPL_STATE_SEND_KEY;
        // fallthrough
    }

    /* Send LICENSE Key */
    if (mi->repl_state == REPL_STATE_SEND_KEY)
    {
        if (cserver.license_key == nullptr)
        {
            mi->repl_state = REPL_STATE_SEND_PORT;
        }
        else
        {
            err = sendSynchronousCommand(mi, SYNC_CMD_WRITE,fd,"REPLCONF","license",cserver.license_key,NULL);
            if (err) goto write_error;
            mi->repl_state = REPL_STATE_KEY_ACK;
            return;
        }
    }

    /* LICENSE Key Ack */
    if (mi->repl_state == REPL_STATE_KEY_ACK)
    {
        err = sendSynchronousCommand(mi, SYNC_CMD_READ,fd,NULL);
        if (err[0] == '-') {
            serverLog(LL_WARNING, "Recieved error from client: %s", err);
            sdsfree(err);
            goto error;
        }
        sdsfree(err);
        mi->repl_state = REPL_STATE_SEND_PORT;
        // fallthrough
    }

    /* Set the replica port, so that Master's INFO command can list the
     * replica listening port correctly. */
    if (mi->repl_state == REPL_STATE_SEND_PORT) {
        sds port = sdsfromlonglong(g_pserver->slave_announce_port ?
            g_pserver->slave_announce_port : g_pserver->port);
        err = sendSynchronousCommand(mi, SYNC_CMD_WRITE,fd,"REPLCONF",
                "listening-port",port, NULL);
        sdsfree(port);
        if (err) goto write_error;
        sdsfree(err);
        mi->repl_state = REPL_STATE_RECEIVE_PORT;
        return;
    }

    /* Receive REPLCONF listening-port reply. */
    if (mi->repl_state == REPL_STATE_RECEIVE_PORT) {
        err = sendSynchronousCommand(mi, SYNC_CMD_READ,fd,NULL);
        /* Ignore the error if any, not all the Redis versions support
         * REPLCONF listening-port. */
        if (err[0] == '-') {
            serverLog(LL_NOTICE,"(Non critical) Master does not understand "
                                "REPLCONF listening-port: %s", err);
        }
        sdsfree(err);
        mi->repl_state = REPL_STATE_SEND_IP;
    }

    /* Skip REPLCONF ip-address if there is no replica-announce-ip option set. */
    if (mi->repl_state == REPL_STATE_SEND_IP &&
        g_pserver->slave_announce_ip == NULL)
    {
            mi->repl_state = REPL_STATE_SEND_CAPA;
    }

    /* Set the replica ip, so that Master's INFO command can list the
     * replica IP address port correctly in case of port forwarding or NAT. */
    if (mi->repl_state == REPL_STATE_SEND_IP) {
        err = sendSynchronousCommand(mi, SYNC_CMD_WRITE,fd,"REPLCONF",
                "ip-address",g_pserver->slave_announce_ip, NULL);
        if (err) goto write_error;
        sdsfree(err);
        mi->repl_state = REPL_STATE_RECEIVE_IP;
        return;
    }

    /* Receive REPLCONF ip-address reply. */
    if (mi->repl_state == REPL_STATE_RECEIVE_IP) {
        err = sendSynchronousCommand(mi, SYNC_CMD_READ,fd,NULL);
        /* Ignore the error if any, not all the Redis versions support
         * REPLCONF listening-port. */
        if (err[0] == '-') {
            serverLog(LL_NOTICE,"(Non critical) Master does not understand "
                                "REPLCONF ip-address: %s", err);
        }
        sdsfree(err);
        mi->repl_state = REPL_STATE_SEND_CAPA;
    }

    /* Inform the master of our (replica) capabilities.
     *
     * EOF: supports EOF-style RDB transfer for diskless replication.
     * PSYNC2: supports PSYNC v2, so understands +CONTINUE <new repl ID>.
     *
     * The master will ignore capabilities it does not understand. */
    if (mi->repl_state == REPL_STATE_SEND_CAPA) {
        if (g_pserver->fActiveReplica)
        {
            err = sendSynchronousCommand(mi, SYNC_CMD_WRITE,fd,"REPLCONF",
                    "capa","eof","capa","psync2","capa","activeExpire",NULL);
        }
        else
        {
            err = sendSynchronousCommand(mi, SYNC_CMD_WRITE,fd,"REPLCONF",
                    "capa","eof","capa","psync2",NULL);
        }
        if (err) goto write_error;
        sdsfree(err);
        mi->repl_state = REPL_STATE_RECEIVE_CAPA;
        return;
    }

    /* Receive CAPA reply. */
    if (mi->repl_state == REPL_STATE_RECEIVE_CAPA) {
        err = sendSynchronousCommand(mi, SYNC_CMD_READ,fd,NULL);
        /* Ignore the error if any, not all the Redis versions support
         * REPLCONF capa. */
        if (err[0] == '-') {
            serverLog(LL_NOTICE,"(Non critical) Master does not understand "
                                  "REPLCONF capa: %s", err);
        }
        sdsfree(err);
        mi->repl_state = REPL_STATE_SEND_PSYNC;
    }

    /* Try a partial resynchonization. If we don't have a cached master
     * slaveTryPartialResynchronization() will at least try to use PSYNC
     * to start a full resynchronization so that we get the master run id
     * and the global offset, to try a partial resync at the next
     * reconnection attempt. */
    if (mi->repl_state == REPL_STATE_SEND_PSYNC) {
        if (slaveTryPartialResynchronization(mi,el,fd,0) == PSYNC_WRITE_ERROR) {
            err = sdsnew("Write error sending the PSYNC command.");
            goto write_error;
        }
        mi->repl_state = REPL_STATE_RECEIVE_PSYNC;
        return;
    }

    /* If reached this point, we should be in REPL_STATE_RECEIVE_PSYNC. */
    if (mi->repl_state != REPL_STATE_RECEIVE_PSYNC) {
        serverLog(LL_WARNING,"syncWithMaster(): state machine error, "
                             "state should be RECEIVE_PSYNC but is %d",
                             mi->repl_state);
        goto error;
    }

    psync_result = slaveTryPartialResynchronization(mi,el,fd,1);
    if (psync_result == PSYNC_WAIT_REPLY) return; /* Try again later... */

    /* If the master is in an transient error, we should try to PSYNC
        * from scratch later, so go to the error path. This happens when
        * the server is loading the dataset or is not connected with its
        * master and so forth. */
    if (psync_result == PSYNC_TRY_LATER) goto error;

    /* Note: if PSYNC does not return WAIT_REPLY, it will take care of
        * uninstalling the read handler from the file descriptor. */

    if (psync_result == PSYNC_CONTINUE) {
        serverLog(LL_NOTICE, "MASTER <-> REPLICA sync: Master accepted a Partial Resynchronization.");
        return;
    }

    /* PSYNC failed or is not supported: we want our slaves to resync with us
     * as well, if we have any sub-slaves. The master may transfer us an
     * entirely different data set and we have no way to incrementally feed
     * our slaves after that. */
    if (!g_pserver->fActiveReplica)
    {
        disconnectSlavesExcept(mi->master_uuid); /* Force our slaves to resync with us as well. */
        freeReplicationBacklog(); /* Don't allow our chained slaves to PSYNC. */
    }
    else
    {
        if (listLength(g_pserver->slaves))
        {
            changeReplicationId();
            clearReplicationId2();
        }
        else
        {
            freeReplicationBacklog(); /* Don't allow our chained slaves to PSYNC. */
        }
    }

    /* Fall back to SYNC if needed. Otherwise psync_result == PSYNC_FULLRESYNC
     * and the g_pserver->master_replid and master_initial_offset are
     * already populated. */
    if (psync_result == PSYNC_NOT_SUPPORTED) {
        serverLog(LL_NOTICE,"Retrying with SYNC...");
        if (syncWrite(fd,"SYNC\r\n",6,g_pserver->repl_syncio_timeout*1000) == -1) {
            serverLog(LL_WARNING,"I/O error writing to MASTER: %s",
                strerror(errno));
            goto error;
        }
    }

    /* Prepare a suitable temp file for bulk transfer */
    while(maxtries--) {
        auto dt = std::chrono::system_clock::now().time_since_epoch();
        auto dtMillisecond = std::chrono::duration_cast<std::chrono::milliseconds>(dt);
        snprintf(tmpfile,256,
            "temp-%d.%ld.rdb",(int)dtMillisecond.count(),(long int)getpid());
        dfd = open(tmpfile,O_CREAT|O_WRONLY|O_EXCL,0644);
        if (dfd != -1) break;
        sleep(1);
    }
    if (dfd == -1) {
        serverLog(LL_WARNING,"Opening the temp file needed for MASTER <-> REPLICA synchronization: %s",strerror(errno));
        goto error;
    }

    /* Setup the non blocking download of the bulk file. */
    if (aeCreateFileEvent(el,fd, AE_READABLE,readSyncBulkPayload,mi)
            == AE_ERR)
    {
        serverLog(LL_WARNING,
            "Can't create readable event for SYNC: %s (fd=%d)",
            strerror(errno),fd);
        goto error;
    }

    mi->repl_state = REPL_STATE_TRANSFER;
    mi->repl_transfer_size = -1;
    mi->repl_transfer_read = 0;
    mi->repl_transfer_last_fsync_off = 0;
    mi->repl_transfer_fd = dfd;
    mi->repl_transfer_lastio = g_pserver->unixtime;
    mi->repl_transfer_tmpfile = zstrdup(tmpfile);
    return;

error:
    aeDeleteFileEvent(el,fd,AE_READABLE|AE_WRITABLE);
    if (dfd != -1) close(dfd);
    close(fd);
    mi->repl_transfer_s = -1;
    mi->repl_state = REPL_STATE_CONNECT;
    return;

write_error: /* Handle sendSynchronousCommand(SYNC_CMD_WRITE) errors. */
    serverLog(LL_WARNING,"Sending command to master in replication handshake: %s", err);
    sdsfree(err);
    goto error;
}

int connectWithMaster(redisMaster *mi) {
    int fd;

    fd = anetTcpNonBlockBestEffortBindConnect(NULL,
        mi->masterhost,mi->masterport,NET_FIRST_BIND_ADDR);
    if (fd == -1) {
        int sev = g_pserver->enable_multimaster ? LL_NOTICE : LL_WARNING;   // with multimaster its not unheard of to intentiallionall have downed masters
        serverLog(sev,"Unable to connect to MASTER: %s",
            strerror(errno));
        return C_ERR;
    }

    if (aeCreateFileEvent(g_pserver->rgthreadvar[IDX_EVENT_LOOP_MAIN].el,fd,AE_READABLE|AE_WRITABLE,syncWithMaster,mi) ==
            AE_ERR)
    {
        close(fd);
        serverLog(LL_WARNING,"Can't create readable event for SYNC");
        return C_ERR;
    }

    mi->repl_transfer_lastio = g_pserver->unixtime;
    mi->repl_transfer_s = fd;
    mi->repl_state = REPL_STATE_CONNECTING;
    return C_OK;
}

/* This function can be called when a non blocking connection is currently
 * in progress to undo it.
 * Never call this function directly, use cancelReplicationHandshake() instead.
 */
void undoConnectWithMaster(redisMaster *mi) {
    int fd = mi->repl_transfer_s;

    aePostFunction(g_pserver->rgthreadvar[IDX_EVENT_LOOP_MAIN].el, [fd]{
        aeDeleteFileEvent(g_pserver->rgthreadvar[IDX_EVENT_LOOP_MAIN].el,fd,AE_READABLE|AE_WRITABLE);
        close(fd);
    });
    mi->repl_transfer_s = -1;
}

/* Abort the async download of the bulk dataset while SYNC-ing with master.
 * Never call this function directly, use cancelReplicationHandshake() instead.
 */
void replicationAbortSyncTransfer(redisMaster *mi) {
    serverAssert(mi->repl_state == REPL_STATE_TRANSFER);
    undoConnectWithMaster(mi);
    close(mi->repl_transfer_fd);
    unlink(mi->repl_transfer_tmpfile);
    zfree(mi->repl_transfer_tmpfile);
}

/* This function aborts a non blocking replication attempt if there is one
 * in progress, by canceling the non-blocking connect attempt or
 * the initial bulk transfer.
 *
 * If there was a replication handshake in progress 1 is returned and
 * the replication state (g_pserver->repl_state) set to REPL_STATE_CONNECT.
 *
 * Otherwise zero is returned and no operation is perforemd at all. */
int cancelReplicationHandshake(redisMaster *mi) {
    if (mi->repl_state == REPL_STATE_TRANSFER) {
        replicationAbortSyncTransfer(mi);
        mi->repl_state = REPL_STATE_CONNECT;
    } else if (mi->repl_state == REPL_STATE_CONNECTING ||
               slaveIsInHandshakeState(mi))
    {
        undoConnectWithMaster(mi);
        mi->repl_state = REPL_STATE_CONNECT;
    } else {
        return 0;
    }
    return 1;
}

/* Set replication to the specified master address and port. */
struct redisMaster *replicationAddMaster(char *ip, int port) {
    // pre-reqs: We must not already have a replica in the list with the same tuple
    listIter li;
    listNode *ln;
    listRewind(g_pserver->masters, &li);
    while ((ln = listNext(&li)))
    {
        redisMaster *miCheck = (redisMaster*)listNodeValue(ln);
        if (strcasecmp(miCheck->masterhost, ip)==0 && miCheck->masterport == port)
            return nullptr;
    }

    // Pre-req satisfied, lets continue
    int was_master = listLength(g_pserver->masters) == 0;
    redisMaster *mi = nullptr;
    if (!g_pserver->enable_multimaster && listLength(g_pserver->masters)) {
        serverAssert(listLength(g_pserver->masters) == 1);
        mi = (redisMaster*)listNodeValue(listFirst(g_pserver->masters));
    }
    else
    {
        mi = (redisMaster*)zcalloc(sizeof(redisMaster), MALLOC_LOCAL);
        initMasterInfo(mi);
        listAddNodeTail(g_pserver->masters, mi);
    }

    sdsfree(mi->masterhost);
    mi->masterhost = sdsnew(ip);
    mi->masterport = port;
    if (mi->master) {
        if (FCorrectThread(mi->master))
            freeClient(mi->master);
        else
            freeClientAsync(mi->master);
    }
    disconnectAllBlockedClients(); /* Clients blocked in master, now replica. */

    /* Force our slaves to resync with us as well. They may hopefully be able
     * to partially resync with us, but we can notify the replid change. */
    if (!g_pserver->fActiveReplica)
        disconnectSlaves();
    cancelReplicationHandshake(mi);
    /* Before destroying our master state, create a cached master using
     * our own parameters, to later PSYNC with the new master. */
    if (was_master) replicationCacheMasterUsingMyself(mi);
    mi->repl_state = REPL_STATE_CONNECT;
    return mi;
}

void freeMasterInfo(redisMaster *mi)
{
    zfree(mi->masterauth);
    zfree(mi->masteruser);
    delete mi->staleKeyMap;
    zfree(mi);
}

/* Cancel replication, setting the instance as a master itself. */
void replicationUnsetMaster(redisMaster *mi) {
    serverAssert(mi->masterhost != NULL);
    sdsfree(mi->masterhost);
    
    mi->masterhost = NULL;
    /* When a replica is turned into a master, the current replication ID
     * (that was inherited from the master at synchronization time) is
     * used as secondary ID up to the current offset, and a new replication
     * ID is created to continue with a new replication history. */
    shiftReplicationId();
    if (mi->master) {
        if (FCorrectThread(mi->master))
            freeClient(mi->master);
        else
            freeClientAsync(mi->master);
    }
    replicationDiscardCachedMaster(mi);
    cancelReplicationHandshake(mi);
    /* Disconnecting all the slaves is required: we need to inform slaves
     * of the replication ID change (see shiftReplicationId() call). However
     * the slaves will be able to partially resync with us, so it will be
     * a very fast reconnection. */
    if (!g_pserver->fActiveReplica)
        disconnectSlaves();
    mi->repl_state = REPL_STATE_NONE;

    /* We need to make sure the new master will start the replication stream
     * with a SELECT statement. This is forced after a full resync, but
     * with PSYNC version 2, there is no need for full resync after a
     * master switch. */
    g_pserver->replicaseldb = -1;

    /* Once we turn from replica to master, we consider the starting time without
     * slaves (that is used to count the replication backlog time to live) as
     * starting from now. Otherwise the backlog will be freed after a
     * failover if slaves do not connect immediately. */
    g_pserver->repl_no_slaves_since = g_pserver->unixtime;

    listNode *ln = listSearchKey(g_pserver->masters, mi);
    serverAssert(ln != nullptr);
    listDelNode(g_pserver->masters, ln);
    freeMasterInfo(mi);
}

/* This function is called when the replica lose the connection with the
 * master into an unexpected way. */
void replicationHandleMasterDisconnection(redisMaster *mi) {
    if (mi != nullptr)
    {
        mi->master = NULL;
        mi->repl_state = REPL_STATE_CONNECT;
        mi->repl_down_since = g_pserver->unixtime;
        /* We lost connection with our master, don't disconnect slaves yet,
        * maybe we'll be able to PSYNC with our master later. We'll disconnect
        * the slaves only if we'll have to do a full resync with our master. */
    }
}

void replicaofCommand(client *c) {
    /* SLAVEOF is not allowed in cluster mode as replication is automatically
     * configured using the current address of the master node. */
    if (g_pserver->cluster_enabled) {
        addReplyError(c,"REPLICAOF not allowed in cluster mode.");
        return;
    }

    /* The special host/port combination "NO" "ONE" turns the instance
     * into a master. Otherwise the new master address is set. */
    if (!strcasecmp((const char*)ptrFromObj(c->argv[1]),"no") &&
        !strcasecmp((const char*)ptrFromObj(c->argv[2]),"one")) {
        if (listLength(g_pserver->masters)) {
            while (listLength(g_pserver->masters))
            {
                replicationUnsetMaster((redisMaster*)listNodeValue(listFirst(g_pserver->masters)));
            }
            sds client = catClientInfoString(sdsempty(),c);
            serverLog(LL_NOTICE,"MASTER MODE enabled (user request from '%s')",
                client);
            sdsfree(client);
        }
    } else {
        long port;

        if (c->flags & CLIENT_SLAVE)
        {
            /* If a client is already a replica they cannot run this command,
             * because it involves flushing all replicas (including this
             * client) */
            addReplyError(c, "Command is not valid when client is a replica.");
            return;
        }

        if ((getLongFromObjectOrReply(c, c->argv[2], &port, NULL) != C_OK))
            return;

        redisMaster *miNew = replicationAddMaster((char*)ptrFromObj(c->argv[1]), port);
        if (miNew == nullptr)
        {
            // We have a duplicate
            serverLog(LL_NOTICE,"REPLICAOF would result into synchronization "
                                "with the master we are already connected "
                                "with. No operation performed.");
            addReplySds(c,sdsnew("+OK Already connected to specified "
                                "master\r\n"));
            return;
        }

        sds client = catClientInfoString(sdsempty(),c);
        serverLog(LL_NOTICE,"REPLICAOF %s:%d enabled (user request from '%s')",
            miNew->masterhost, miNew->masterport, client);
        sdsfree(client);
    }
    addReplyAsync(c,shared.ok);
}

/* ROLE command: provide information about the role of the instance
 * (master or replica) and additional information related to replication
 * in an easy to process format. */
void roleCommand(client *c) {
    if (listLength(g_pserver->masters) == 0) {
        listIter li;
        listNode *ln;
        void *mbcount;
        int slaves = 0;

        addReplyArrayLen(c,3);
        addReplyBulkCBuffer(c,"master",6);
        addReplyLongLong(c,g_pserver->master_repl_offset);
        mbcount = addReplyDeferredLen(c);
        listRewind(g_pserver->slaves,&li);
        while((ln = listNext(&li))) {
            client *replica = (client*)ln->value;
            char ip[NET_IP_STR_LEN], *slaveip = replica->slave_ip;

            if (slaveip[0] == '\0') {
                if (anetPeerToString(replica->fd,ip,sizeof(ip),NULL) == -1)
                    continue;
                slaveip = ip;
            }
            if (replica->replstate != SLAVE_STATE_ONLINE) continue;
            addReplyArrayLen(c,3);
            addReplyBulkCString(c,slaveip);
            addReplyBulkLongLong(c,replica->slave_listening_port);
            addReplyBulkLongLong(c,replica->repl_ack_off+replica->reploff_skipped);
            slaves++;
        }
        setDeferredArrayLen(c,mbcount,slaves);
    } else {
        listIter li;
        listNode *ln;
        listRewind(g_pserver->masters, &li);

        while ((ln = listNext(&li)))
        {
            redisMaster *mi = (redisMaster*)listNodeValue(ln);
            const char *slavestate = NULL;
            addReplyArrayLen(c,5);
            if (g_pserver->fActiveReplica)
                addReplyBulkCBuffer(c,"active-replica",14);
            else
                addReplyBulkCBuffer(c,"slave",5);
            addReplyBulkCString(c,mi->masterhost);
            addReplyLongLong(c,mi->masterport);
            if (slaveIsInHandshakeState(mi)) {
                slavestate = "handshake";
            } else {
                switch(mi->repl_state) {
                case REPL_STATE_NONE: slavestate = "none"; break;
                case REPL_STATE_CONNECT: slavestate = "connect"; break;
                case REPL_STATE_CONNECTING: slavestate = "connecting"; break;
                case REPL_STATE_TRANSFER: slavestate = "sync"; break;
                case REPL_STATE_CONNECTED: slavestate = "connected"; break;
                default: slavestate = "unknown"; break;
                }
            }
            addReplyBulkCString(c,slavestate);
            addReplyLongLong(c,mi->master ? mi->master->reploff : -1);
        }
    }
}

/* Send a REPLCONF ACK command to the master to inform it about the current
 * processed offset. If we are not connected with a master, the command has
 * no effects. */
void replicationSendAck(redisMaster *mi) 
{
    client *c = mi->master;

    if (c != NULL) {
        c->flags |= CLIENT_MASTER_FORCE_REPLY;
        addReplyArrayLen(c,3);
        addReplyBulkCString(c,"REPLCONF");
        addReplyBulkCString(c,"ACK");
        addReplyBulkLongLong(c,c->reploff);
        c->flags &= ~CLIENT_MASTER_FORCE_REPLY;
    }
}

/* ---------------------- MASTER CACHING FOR PSYNC -------------------------- */

/* In order to implement partial synchronization we need to be able to cache
 * our master's client structure after a transient disconnection.
 * It is cached into g_pserver->cached_master and flushed away using the following
 * functions. */

/* This function is called by freeClient() in order to cache the master
 * client structure instead of destroying it. freeClient() will return
 * ASAP after this function returns, so every action needed to avoid problems
 * with a client that is really "suspended" has to be done by this function.
 *
 * The other functions that will deal with the cached master are:
 *
 * replicationDiscardCachedMaster() that will make sure to kill the client
 * as for some reason we don't want to use it in the future.
 *
 * replicationResurrectCachedMaster() that is used after a successful PSYNC
 * handshake in order to reactivate the cached master.
 */
void replicationCacheMaster(redisMaster *mi, client *c) {
    serverAssert(mi->master != NULL && mi->cached_master == NULL);
    serverLog(LL_NOTICE,"Caching the disconnected master state.");
    AssertCorrectThread(c);
    std::lock_guard<decltype(c->lock)> clientlock(c->lock);

    /* Unlink the client from the server structures. */
    unlinkClient(c);

    /* Reset the master client so that's ready to accept new commands:
     * we want to discard te non processed query buffers and non processed
     * offsets, including pending transactions, already populated arguments,
     * pending outputs to the master. */
    sdsclear(mi->master->querybuf);
    sdsclear(mi->master->pending_querybuf);
    mi->master->read_reploff = mi->master->reploff;
    if (c->flags & CLIENT_MULTI) discardTransaction(c);
    listEmpty(c->reply);
    c->sentlen = 0;
    c->sentlenAsync = 0;
    c->reply_bytes = 0;
    c->bufpos = 0;
    resetClient(c);

    /* Save the master. g_pserver->master will be set to null later by
     * replicationHandleMasterDisconnection(). */
    mi->cached_master = mi->master;

    /* Invalidate the Peer ID cache. */
    if (c->peerid) {
        sdsfree(c->peerid);
        c->peerid = NULL;
    }

    /* Caching the master happens instead of the actual freeClient() call,
     * so make sure to adjust the replication state. This function will
     * also set g_pserver->master to NULL. */
    replicationHandleMasterDisconnection(mi);
}

/* This function is called when a master is turend into a replica, in order to
 * create from scratch a cached master for the new client, that will allow
 * to PSYNC with the replica that was promoted as the new master after a
 * failover.
 *
 * Assuming this instance was previously the master instance of the new master,
 * the new master will accept its replication ID, and potentiall also the
 * current offset if no data was lost during the failover. So we use our
 * current replication ID and offset in order to synthesize a cached master. */
void replicationCacheMasterUsingMyself(redisMaster *mi) {
    /* The master client we create can be set to any DBID, because
     * the new master will start its replication stream with SELECT. */
    mi->master_initial_offset = g_pserver->master_repl_offset;
    replicationCreateMasterClient(mi, -1,-1);
    std::lock_guard<decltype(mi->master->lock)> lock(mi->master->lock);

    /* Use our own ID / offset. */
    memcpy(mi->master->replid, g_pserver->replid, sizeof(g_pserver->replid));

    /* Set as cached master. */
    unlinkClient(mi->master);
    mi->cached_master = mi->master;
    mi->master = NULL;
    serverLog(LL_NOTICE,"Before turning into a replica, using my master parameters to synthesize a cached master: I may be able to synchronize with the new master with just a partial transfer.");
}

/* Free a cached master, called when there are no longer the conditions for
 * a partial resync on reconnection. */
void replicationDiscardCachedMaster(redisMaster *mi) {
    if (mi->cached_master == NULL) return;

    serverLog(LL_NOTICE,"Discarding previously cached master state.");
    mi->cached_master->flags &= ~CLIENT_MASTER;
    if (FCorrectThread(mi->cached_master))
        freeClient(mi->cached_master);
    else
        freeClientAsync(mi->cached_master);
    mi->cached_master = NULL;
}

/* Turn the cached master into the current master, using the file descriptor
 * passed as argument as the socket for the new master.
 *
 * This function is called when successfully setup a partial resynchronization
 * so the stream of data that we'll receive will start from were this
 * master left. */
void replicationResurrectCachedMaster(redisMaster *mi, int newfd) {
    mi->master = mi->cached_master;
    mi->cached_master = NULL;
    mi->master->fd = newfd;
    mi->master->flags &= ~(CLIENT_CLOSE_AFTER_REPLY|CLIENT_CLOSE_ASAP);
    mi->master->authenticated = 1;
    mi->master->lastinteraction = g_pserver->unixtime;
    mi->repl_state = REPL_STATE_CONNECTED;
    mi->repl_down_since = 0;

    /* Normally changing the thread of a client is a BIG NONO,
        but this client was unlinked so its OK here */
    mi->master->iel = serverTL - g_pserver->rgthreadvar; // martial to this thread

    /* Re-add to the list of clients. */
    linkClient(mi->master);
    if (aeCreateFileEvent(g_pserver->rgthreadvar[mi->master->iel].el, newfd, AE_READABLE|AE_READ_THREADSAFE,
                          readQueryFromClient, mi->master)) {
        serverLog(LL_WARNING,"Error resurrecting the cached master, impossible to add the readable handler: %s", strerror(errno));
        freeClientAsync(mi->master); /* Close ASAP. */
    }

    /* We may also need to install the write handler as well if there is
     * pending data in the write buffers. */
    if (clientHasPendingReplies(mi->master)) {
        if (aeCreateFileEvent(g_pserver->rgthreadvar[mi->master->iel].el, newfd, AE_WRITABLE|AE_WRITE_THREADSAFE,
                          sendReplyToClient, mi->master)) {
            serverLog(LL_WARNING,"Error resurrecting the cached master, impossible to add the writable handler: %s", strerror(errno));
            freeClientAsync(mi->master); /* Close ASAP. */
        }
    }
}

/* ------------------------- MIN-SLAVES-TO-WRITE  --------------------------- */

/* This function counts the number of slaves with lag <= min-slaves-max-lag.
 * If the option is active, the server will prevent writes if there are not
 * enough connected slaves with the specified lag (or less). */
void refreshGoodSlavesCount(void) {
    listIter li;
    listNode *ln;
    int good = 0;

    if (!g_pserver->repl_min_slaves_to_write ||
        !g_pserver->repl_min_slaves_max_lag) return;

    listRewind(g_pserver->slaves,&li);
    while((ln = listNext(&li))) {
        client *replica = (client*)ln->value;
        time_t lag = g_pserver->unixtime - replica->repl_ack_time;

        if (replica->replstate == SLAVE_STATE_ONLINE &&
            lag <= g_pserver->repl_min_slaves_max_lag) good++;
    }
    g_pserver->repl_good_slaves_count = good;
}

/* ----------------------- REPLICATION SCRIPT CACHE --------------------------
 * The goal of this code is to keep track of scripts already sent to every
 * connected replica, in order to be able to replicate EVALSHA as it is without
 * translating it to EVAL every time it is possible.
 *
 * We use a capped collection implemented by a hash table for fast lookup
 * of scripts we can send as EVALSHA, plus a linked list that is used for
 * eviction of the oldest entry when the max number of items is reached.
 *
 * We don't care about taking a different cache for every different replica
 * since to fill the cache again is not very costly, the goal of this code
 * is to avoid that the same big script is trasmitted a big number of times
 * per second wasting bandwidth and processor speed, but it is not a problem
 * if we need to rebuild the cache from scratch from time to time, every used
 * script will need to be transmitted a single time to reappear in the cache.
 *
 * This is how the system works:
 *
 * 1) Every time a new replica connects, we flush the whole script cache.
 * 2) We only send as EVALSHA what was sent to the master as EVALSHA, without
 *    trying to convert EVAL into EVALSHA specifically for slaves.
 * 3) Every time we trasmit a script as EVAL to the slaves, we also add the
 *    corresponding SHA1 of the script into the cache as we are sure every
 *    replica knows about the script starting from now.
 * 4) On SCRIPT FLUSH command, we replicate the command to all the slaves
 *    and at the same time flush the script cache.
 * 5) When the last replica disconnects, flush the cache.
 * 6) We handle SCRIPT LOAD as well since that's how scripts are loaded
 *    in the master sometimes.
 */

/* Initialize the script cache, only called at startup. */
void replicationScriptCacheInit(void) {
    g_pserver->repl_scriptcache_size = 10000;
    g_pserver->repl_scriptcache_dict = dictCreate(&replScriptCacheDictType,NULL);
    g_pserver->repl_scriptcache_fifo = listCreate();
}

/* Empty the script cache. Should be called every time we are no longer sure
 * that every replica knows about all the scripts in our set, or when the
 * current AOF "context" is no longer aware of the script. In general we
 * should flush the cache:
 *
 * 1) Every time a new replica reconnects to this master and performs a
 *    full SYNC (PSYNC does not require flushing).
 * 2) Every time an AOF rewrite is performed.
 * 3) Every time we are left without slaves at all, and AOF is off, in order
 *    to reclaim otherwise unused memory.
 */
void replicationScriptCacheFlush(void) {
    dictEmpty(g_pserver->repl_scriptcache_dict,NULL);
    listRelease(g_pserver->repl_scriptcache_fifo);
    g_pserver->repl_scriptcache_fifo = listCreate();
}

/* Add an entry into the script cache, if we reach max number of entries the
 * oldest is removed from the list. */
void replicationScriptCacheAdd(sds sha1) {
    int retval;
    sds key = sdsdup(sha1);

    /* Evict oldest. */
    if (listLength(g_pserver->repl_scriptcache_fifo) == g_pserver->repl_scriptcache_size)
    {
        listNode *ln = listLast(g_pserver->repl_scriptcache_fifo);
        sds oldest = (sds)listNodeValue(ln);

        retval = dictDelete(g_pserver->repl_scriptcache_dict,oldest);
        serverAssert(retval == DICT_OK);
        listDelNode(g_pserver->repl_scriptcache_fifo,ln);
    }

    /* Add current. */
    retval = dictAdd(g_pserver->repl_scriptcache_dict,key,NULL);
    listAddNodeHead(g_pserver->repl_scriptcache_fifo,key);
    serverAssert(retval == DICT_OK);
}

/* Returns non-zero if the specified entry exists inside the cache, that is,
 * if all the slaves are aware of this script SHA1. */
int replicationScriptCacheExists(sds sha1) {
    return dictFind(g_pserver->repl_scriptcache_dict,sha1) != NULL;
}

/* ----------------------- SYNCHRONOUS REPLICATION --------------------------
 * Redis synchronous replication design can be summarized in points:
 *
 * - Redis masters have a global replication offset, used by PSYNC.
 * - Master increment the offset every time new commands are sent to slaves.
 * - Slaves ping back masters with the offset processed so far.
 *
 * So synchronous replication adds a new WAIT command in the form:
 *
 *   WAIT <num_replicas> <milliseconds_timeout>
 *
 * That returns the number of replicas that processed the query when
 * we finally have at least num_replicas, or when the timeout was
 * reached.
 *
 * The command is implemented in this way:
 *
 * - Every time a client processes a command, we remember the replication
 *   offset after sending that command to the slaves.
 * - When WAIT is called, we ask slaves to send an acknowledgement ASAP.
 *   The client is blocked at the same time (see blocked.c).
 * - Once we receive enough ACKs for a given offset or when the timeout
 *   is reached, the WAIT command is unblocked and the reply sent to the
 *   client.
 */

/* This just set a flag so that we broadcast a REPLCONF GETACK command
 * to all the slaves in the beforeSleep() function. Note that this way
 * we "group" all the clients that want to wait for synchronouns replication
 * in a given event loop iteration, and send a single GETACK for them all. */
void replicationRequestAckFromSlaves(void) {
    g_pserver->get_ack_from_slaves = 1;
}

/* Return the number of slaves that already acknowledged the specified
 * replication offset. */
int replicationCountAcksByOffset(long long offset) {
    listIter li;
    listNode *ln;
    int count = 0;

    listRewind(g_pserver->slaves,&li);
    while((ln = listNext(&li))) {
        client *replica = (client*)ln->value;

        if (replica->replstate != SLAVE_STATE_ONLINE) continue;
        if ((replica->repl_ack_off + replica->reploff_skipped) >= offset) count++;
    }
    return count;
}

/* WAIT for N replicas to acknowledge the processing of our latest
 * write command (and all the previous commands). */
void waitCommand(client *c) {
    mstime_t timeout;
    long numreplicas, ackreplicas;
    long long offset = c->woff;

    if (listLength(g_pserver->masters) && !g_pserver->fActiveReplica) {
        addReplyError(c,"WAIT cannot be used with replica instances. Please also note that since Redis 4.0 if a replica is configured to be writable (which is not the default) writes to replicas are just local and are not propagated.");
        return;
    }

    /* Argument parsing. */
    if (getLongFromObjectOrReply(c,c->argv[1],&numreplicas,NULL) != C_OK)
        return;
    if (getTimeoutFromObjectOrReply(c,c->argv[2],&timeout,UNIT_MILLISECONDS)
        != C_OK) return;

    /* First try without blocking at all. */
    ackreplicas = replicationCountAcksByOffset(c->woff);
    if (ackreplicas >= numreplicas || c->flags & CLIENT_MULTI) {
        addReplyLongLong(c,ackreplicas);
        return;
    }

    /* Otherwise block the client and put it into our list of clients
     * waiting for ack from slaves. */
    c->bpop.timeout = timeout;
    c->bpop.reploffset = offset;
    c->bpop.numreplicas = numreplicas;
    listAddNodeTail(g_pserver->clients_waiting_acks,c);
    blockClient(c,BLOCKED_WAIT);

    /* Make sure that the server will send an ACK request to all the slaves
     * before returning to the event loop. */
    replicationRequestAckFromSlaves();
}

/* This is called by unblockClient() to perform the blocking op type
 * specific cleanup. We just remove the client from the list of clients
 * waiting for replica acks. Never call it directly, call unblockClient()
 * instead. */
void unblockClientWaitingReplicas(client *c) {
    listNode *ln = listSearchKey(g_pserver->clients_waiting_acks,c);
    serverAssert(ln != NULL);
    listDelNode(g_pserver->clients_waiting_acks,ln);
}

/* Check if there are clients blocked in WAIT that can be unblocked since
 * we received enough ACKs from slaves. */
void processClientsWaitingReplicas(void) {
    long long last_offset = 0;
    int last_numreplicas = 0;

    listIter li;
    listNode *ln;

    listRewind(g_pserver->clients_waiting_acks,&li);
    while((ln = listNext(&li))) {
        client *c = (client*)ln->value;
        fastlock_lock(&c->lock);

        /* Every time we find a client that is satisfied for a given
         * offset and number of replicas, we remember it so the next client
         * may be unblocked without calling replicationCountAcksByOffset()
         * if the requested offset / replicas were equal or less. */
        if (last_offset && last_offset > c->bpop.reploffset &&
                           last_numreplicas > c->bpop.numreplicas)
        {
            unblockClient(c);
            addReplyLongLongAsync(c,last_numreplicas);
        } else {
            int numreplicas = replicationCountAcksByOffset(c->bpop.reploffset);

            if (numreplicas >= c->bpop.numreplicas) {
                last_offset = c->bpop.reploffset;
                last_numreplicas = numreplicas;
                unblockClient(c);
                addReplyLongLongAsync(c,numreplicas);
            }
        }
        fastlock_unlock(&c->lock);
    }
}

/* Return the replica replication offset for this instance, that is
 * the offset for which we already processed the master replication stream. */
long long replicationGetSlaveOffset(redisMaster *mi) {
    long long offset = 0;

    if (mi != NULL && mi->masterhost != NULL) {
        if (mi->master) {
            offset = mi->master->reploff;
        } else if (mi->cached_master) {
            offset = mi->cached_master->reploff;
        }
    }
    /* offset may be -1 when the master does not support it at all, however
     * this function is designed to return an offset that can express the
     * amount of data processed by the master, so we return a positive
     * integer. */
    if (offset < 0) offset = 0;
    return offset;
}

/* --------------------------- REPLICATION CRON  ---------------------------- */

/* Replication cron function, called 1 time per second. */
void replicationCron(void) {
    static long long replication_cron_loops = 0;
    serverAssert(GlobalLocksAcquired());
    listIter liMaster;
    listNode *lnMaster;
    listRewind(g_pserver->masters, &liMaster);
    while ((lnMaster = listNext(&liMaster)))
    {
        redisMaster *mi = (redisMaster*)listNodeValue(lnMaster);

        std::unique_lock<decltype(mi->master->lock)> ulock;
        if (mi->master != nullptr)
            ulock = decltype(ulock)(mi->master->lock);

        /* Non blocking connection timeout? */
        if (mi->masterhost &&
            (mi->repl_state == REPL_STATE_CONNECTING ||
            slaveIsInHandshakeState(mi)) &&
            (time(NULL)-mi->repl_transfer_lastio) > g_pserver->repl_timeout)
        {
            serverLog(LL_WARNING,"Timeout connecting to the MASTER...");
            cancelReplicationHandshake(mi);
        }

        /* Bulk transfer I/O timeout? */
        if (mi->masterhost && mi->repl_state == REPL_STATE_TRANSFER &&
            (time(NULL)-mi->repl_transfer_lastio) > g_pserver->repl_timeout)
        {
            serverLog(LL_WARNING,"Timeout receiving bulk data from MASTER... If the problem persists try to set the 'repl-timeout' parameter in keydb.conf to a larger value.");
            cancelReplicationHandshake(mi);
        }

        /* Timed out master when we are an already connected replica? */
        if (mi->masterhost && mi->master && mi->repl_state == REPL_STATE_CONNECTED &&
            (time(NULL)-mi->master->lastinteraction) > g_pserver->repl_timeout)
        {
            serverLog(LL_WARNING,"MASTER timeout: no data nor PING received...");
            if (FCorrectThread(mi->master))
                freeClient(mi->master);
            else
                freeClientAsync(mi->master);
        }

        /* Check if we should connect to a MASTER */
        if (mi->repl_state == REPL_STATE_CONNECT) {
            serverLog(LL_NOTICE,"Connecting to MASTER %s:%d",
                mi->masterhost, mi->masterport);
            if (connectWithMaster(mi) == C_OK) {
                serverLog(LL_NOTICE,"MASTER <-> REPLICA sync started");
            }
        }

        /* Send ACK to master from time to time.
        * Note that we do not send periodic acks to masters that don't
        * support PSYNC and replication offsets. */
        if (mi->masterhost && mi->master &&
            !(mi->master->flags & CLIENT_PRE_PSYNC))
            replicationSendAck(mi);
    }

    /* If we have attached slaves, PING them from time to time.
    * So slaves can implement an explicit timeout to masters, and will
    * be able to detect a link disconnection even if the TCP connection
    * will not actually go down. */
    listIter li;
    listNode *ln;
    robj *ping_argv[1];

    /* First, send PING according to ping_slave_period. */
    if ((replication_cron_loops % g_pserver->repl_ping_slave_period) == 0 &&
        listLength(g_pserver->slaves))
    {
        /* Note that we don't send the PING if the clients are paused during
         * a Redis Cluster manual failover: the PING we send will otherwise
         * alter the replication offsets of master and replica, and will no longer
         * match the one stored into 'mf_master_offset' state. */
        int manual_failover_in_progress =
            g_pserver->cluster_enabled &&
            g_pserver->cluster->mf_end &&
            clientsArePaused();

        if (!manual_failover_in_progress) {
            ping_argv[0] = createStringObject("PING",4);
            replicationFeedSlaves(g_pserver->slaves, g_pserver->replicaseldb,
                ping_argv, 1);
            decrRefCount(ping_argv[0]);
        }
    }

    /* Second, send a newline to all the slaves in pre-synchronization
    * stage, that is, slaves waiting for the master to create the RDB file.
    *
    * Also send the a newline to all the chained slaves we have, if we lost
    * connection from our master, to keep the slaves aware that their
    * master is online. This is needed since sub-slaves only receive proxied
    * data from top-level masters, so there is no explicit pinging in order
    * to avoid altering the replication offsets. This special out of band
    * pings (newlines) can be sent, they will have no effect in the offset.
    *
    * The newline will be ignored by the replica but will refresh the
    * last interaction timer preventing a timeout. In this case we ignore the
    * ping period and refresh the connection once per second since certain
    * timeouts are set at a few seconds (example: PSYNC response). */
    listRewind(g_pserver->slaves,&li);
    while((ln = listNext(&li))) {
        client *replica = (client*)ln->value;

        int is_presync =
            (replica->replstate == SLAVE_STATE_WAIT_BGSAVE_START ||
            (replica->replstate == SLAVE_STATE_WAIT_BGSAVE_END &&
            g_pserver->rdb_child_type != RDB_CHILD_TYPE_SOCKET));

        if (is_presync) {
            if (write(replica->fd, "\n", 1) == -1) {
                /* Don't worry about socket errors, it's just a ping. */
            }
        }
    }

    /* Disconnect timedout slaves. */
    if (listLength(g_pserver->slaves)) {
        listIter li;
        listNode *ln;

        listRewind(g_pserver->slaves,&li);
        while((ln = listNext(&li))) {
            client *replica = (client*)ln->value;

            if (replica->replstate != SLAVE_STATE_ONLINE) continue;
            if (replica->flags & CLIENT_PRE_PSYNC) continue;
            if ((g_pserver->unixtime - replica->repl_ack_time) > g_pserver->repl_timeout)
            {
                serverLog(LL_WARNING, "Disconnecting timedout replica: %s",
                    replicationGetSlaveName(replica));
                if (FCorrectThread(replica))
                    freeClient(replica);
                else
                    freeClientAsync(replica);
            }
        }
    }

    /* If this is a master without attached slaves and there is a replication
    * backlog active, in order to reclaim memory we can free it after some
    * (configured) time. Note that this cannot be done for slaves: slaves
    * without sub-slaves attached should still accumulate data into the
    * backlog, in order to reply to PSYNC queries if they are turned into
    * masters after a failover. */
    if (listLength(g_pserver->slaves) == 0 && g_pserver->repl_backlog_time_limit &&
        g_pserver->repl_backlog && listLength(g_pserver->masters) == 0)
    {
        time_t idle = g_pserver->unixtime - g_pserver->repl_no_slaves_since;

        if (idle > g_pserver->repl_backlog_time_limit) {
            /* When we free the backlog, we always use a new
            * replication ID and clear the ID2. This is needed
            * because when there is no backlog, the master_repl_offset
            * is not updated, but we would still retain our replication
            * ID, leading to the following problem:
            *
            * 1. We are a master instance.
            * 2. Our replica is promoted to master. It's repl-id-2 will
            *    be the same as our repl-id.
            * 3. We, yet as master, receive some updates, that will not
            *    increment the master_repl_offset.
            * 4. Later we are turned into a replica, connect to the new
            *    master that will accept our PSYNC request by second
            *    replication ID, but there will be data inconsistency
            *    because we received writes. */
            changeReplicationId();
            clearReplicationId2();
            freeReplicationBacklog();
            serverLog(LL_NOTICE,
                "Replication backlog freed after %d seconds "
                "without connected replicas.",
                (int) g_pserver->repl_backlog_time_limit);
        }
    }

    /* If AOF is disabled and we no longer have attached slaves, we can
    * free our Replication Script Cache as there is no need to propagate
    * EVALSHA at all. */
    if (listLength(g_pserver->slaves) == 0 &&
        g_pserver->aof_state == AOF_OFF &&
        listLength(g_pserver->repl_scriptcache_fifo) != 0)
    {
        replicationScriptCacheFlush();
    }

    /* Start a BGSAVE good for replication if we have slaves in
    * WAIT_BGSAVE_START state.
    *
    * In case of diskless replication, we make sure to wait the specified
    * number of seconds (according to configuration) so that other slaves
    * have the time to arrive before we start streaming. */
    if (!g_pserver->FRdbSaveInProgress() && g_pserver->aof_child_pid == -1) {
        time_t idle, max_idle = 0;
        int slaves_waiting = 0;
        int mincapa = -1;
        listNode *ln;
        listIter li;

        listRewind(g_pserver->slaves,&li);
        while((ln = listNext(&li))) {
            client *replica = (client*)ln->value;
            if (replica->replstate == SLAVE_STATE_WAIT_BGSAVE_START) {
                idle = g_pserver->unixtime - replica->lastinteraction;
                if (idle > max_idle) max_idle = idle;
                slaves_waiting++;
                mincapa = (mincapa == -1) ? replica->slave_capa :
                                            (mincapa & replica->slave_capa);
            }
        }

        if (slaves_waiting &&
            (!g_pserver->repl_diskless_sync ||
            max_idle > g_pserver->repl_diskless_sync_delay))
        {
            /* Start the BGSAVE. The called function may start a
            * BGSAVE with socket target or disk target depending on the
            * configuration and slaves capabilities. */
            startBgsaveForReplication(mincapa);
        }
    }

    propagateMasterStaleKeys();

    /* Refresh the number of slaves with lag <= min-slaves-max-lag. */
    refreshGoodSlavesCount();
    replication_cron_loops++; /* Incremented with frequency 1 HZ. */
}

int FBrokenLinkToMaster()
{
    listIter li;
    listNode *ln;
    listRewind(g_pserver->masters, &li);

    while ((ln = listNext(&li)))
    {
        redisMaster *mi = (redisMaster*)listNodeValue(ln);
        if (mi->repl_state != REPL_STATE_CONNECTED)
            return true;
    }
    return false;
}

int FActiveMaster(client *c)
{
    if (!(c->flags & CLIENT_MASTER))
        return false;

    listIter li;
    listNode *ln;
    listRewind(g_pserver->masters, &li);
    while ((ln = listNext(&li)))
    {
        redisMaster *mi = (redisMaster*)listNodeValue(ln);
        if (mi->master == c)
            return true;
    }
    return false;
}

redisMaster *MasterInfoFromClient(client *c)
{
    listIter li;
    listNode *ln;
    listRewind(g_pserver->masters, &li);
    while ((ln = listNext(&li)))
    {
        redisMaster *mi = (redisMaster*)listNodeValue(ln);
        if (mi->master == c || mi->cached_master == c)
            return mi;
    }
    return nullptr;
}

#define REPLAY_MAX_NESTING 64
class ReplicaNestState
{
public:
    bool FPush()
    {
        if (m_cnesting == REPLAY_MAX_NESTING) {
            m_fCancelled = true;
            return false;   // overflow
        }
        
        if (m_cnesting == 0)
            m_fCancelled = false;
        ++m_cnesting;
        return true;
    }

    void Pop()
    {
        --m_cnesting;
    }

    void Cancel()
    {
        m_fCancelled = true;
    }

    bool FCancelled() const
    {
        return m_fCancelled;
    }

    bool FFirst() const
    {
        return m_cnesting == 1;
    }

private:
    int m_cnesting = 0;
    bool m_fCancelled = false;
};

void replicaReplayCommand(client *c)
{
    static thread_local ReplicaNestState *s_pstate = nullptr;
    if (s_pstate == nullptr)
        s_pstate = new (MALLOC_LOCAL) ReplicaNestState;

    // the replay command contains two arguments: 
    //  1: The UUID of the source
    //  2: The raw command buffer to be replayed
    //  3: (OPTIONAL) the database ID the command should apply to
    
    if (!(c->flags & CLIENT_MASTER))
    {
        addReplyError(c, "Command must be sent from a master");
        s_pstate->Cancel();
        return;
    }

    /* First Validate Arguments */
    if (c->argc < 3)
    {
        addReplyError(c, "Invalid number of arguments");
        s_pstate->Cancel();
        return;
    }

    unsigned char uuid[UUID_BINARY_LEN];
    if (c->argv[1]->type != OBJ_STRING || sdslen((sds)ptrFromObj(c->argv[1])) != 36 
        || uuid_parse((sds)ptrFromObj(c->argv[1]), uuid) != 0)
    {
        addReplyError(c, "Expected UUID arg1");
        s_pstate->Cancel();
        return;
    }

    if (c->argv[2]->type != OBJ_STRING)
    {
        addReplyError(c, "Expected command buffer arg2");
        s_pstate->Cancel();
        return;
    }

    if (c->argc >= 4)
    {
        long long db;
        if (getLongLongFromObject(c->argv[3], &db) != C_OK || db >= cserver.dbnum || selectDb(c, (int)db) != C_OK)
        {
            addReplyError(c, "Invalid database ID");
            s_pstate->Cancel();
            return;
        }
    }

    uint64_t mvcc = 0;
    if (c->argc >= 5)
    {
        if (getUnsignedLongLongFromObject(c->argv[4], &mvcc) != C_OK)
        {
            addReplyError(c, "Invalid MVCC Timestamp");
            s_pstate->Cancel();
            return;
        }
    }

    if (FSameUuidNoNil(uuid, cserver.uuid))
    {
        addReply(c, shared.ok);
        s_pstate->Cancel();
        return; // Our own commands have come back to us.  Ignore them.
    }

    if (!s_pstate->FPush())
        return;

    // OK We've recieved a command lets execute
    client *current_clientSave = serverTL->current_client;
    client *cFake = createClient(-1, c->iel);
    cFake->lock.lock();
    cFake->authenticated = c->authenticated;
    cFake->puser = c->puser;
    cFake->querybuf = sdscatsds(cFake->querybuf,(sds)ptrFromObj(c->argv[2]));
    selectDb(cFake, c->db->id);
    auto ccmdPrev = serverTL->commandsExecuted;
    processInputBuffer(cFake, (CMD_CALL_FULL & (~CMD_CALL_PROPAGATE)));
    bool fExec = ccmdPrev != serverTL->commandsExecuted;
    cFake->lock.unlock();
    if (fExec)
    {
        addReply(c, shared.ok);
        selectDb(c, cFake->db->id);
        redisMaster *mi = MasterInfoFromClient(c);
        if (mi != nullptr)  // this should never be null but I'd prefer not to crash
        {
            mi->mvccLastSync = mvcc;
        }
    }
    else
    {
        addReplyError(c, "command did not execute");
    }
    freeClient(cFake);
    serverTL->current_client = current_clientSave;

    // call() will not propogate this for us, so we do so here
    if (!s_pstate->FCancelled() && s_pstate->FFirst())
        alsoPropagate(cserver.rreplayCommand,c->db->id,c->argv,c->argc,PROPAGATE_AOF|PROPAGATE_REPL);
    
    s_pstate->Pop();
    return;
}

void updateMasterAuth()
{
    listIter li;
    listNode *ln;

    listRewind(g_pserver->masters, &li);
    while ((ln = listNext(&li)))
    {
        redisMaster *mi = (redisMaster*)listNodeValue(ln);
        zfree(mi->masterauth); mi->masterauth = nullptr;
        zfree(mi->masteruser); mi->masteruser = nullptr;

        if (cserver.default_masterauth)
            mi->masterauth = zstrdup(cserver.default_masterauth);
        if (cserver.default_masteruser)
            mi->masteruser = zstrdup(cserver.default_masteruser);
    }
}

static void propagateMasterStaleKeys()
{
    listIter li;
    listNode *ln;
    listRewind(g_pserver->masters, &li);
    robj *rgobj[2];

    rgobj[0] = createEmbeddedStringObject("DEL", 3);

    while ((ln = listNext(&li)) != nullptr)
    {
        redisMaster *mi = (redisMaster*)listNodeValue(ln);
        if (mi->staleKeyMap != nullptr)
        {
            if (mi->master != nullptr)
            {
                for (auto &pair : *mi->staleKeyMap)
                {
                    if (pair.second.empty())
                        continue;
                    
                    client *replica = replicaFromMaster(mi);
                    if (replica == nullptr)
                        continue;

                    for (auto &spkey : pair.second)
                    {
                        rgobj[1] = spkey.get();
                        replicationFeedSlave(replica, pair.first, rgobj, 2, false);
                    }
                }
                delete mi->staleKeyMap;
                mi->staleKeyMap = nullptr;
            }
        }
    }

    decrRefCount(rgobj[0]);
}
