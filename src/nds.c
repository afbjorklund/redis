/*
 * Copyright (c) 2013 Anchor Systems Pty Ltd
 * Copyright (c) 2013 Matt Palmer <matt@hezmatt.org>
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
#include "nds.h"

#include <lmdb.h>

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/statvfs.h>

/* Ripped wholesale from mdb.c */
#define MDB_MAXKEYSIZE 511
#define MDB_MAXDATASIZE 0xffffffffUL

/* Bugger this manual typing thing */
#define NDS_TIMER_START unsigned long long start = ustime();
#define NDS_TIMER_END server.stat_nds_usec += ustime()-start;

/* Generate the name of the freezer we want, based on the database passed
 * in, and stuff the name into buf. */
static void freezer_filename(redisDb *db, char *buf) {
    snprintf(buf, REDIS_FREEZER_FILENAME_LEN-1,
             "freezer_%i",
             db->id);
}

/* Close an NDS database. */
static void nds_close(NDSDB *ndsdb) {
    serverLog(LL_DEBUG, "nds_close(ndsdb=\"%s\"), ref=%i", ndsdb->db_name, server.ndsdb.refs);

    if (!ndsdb) {
        return;
    }

    if (--(ndsdb->refs) == 0) {
        /* Everything except the environment must go! */
        if (ndsdb->txn) {
            mdb_txn_commit(ndsdb->txn);
            ndsdb->txn = NULL;
        }

        if (ndsdb->dbi != (unsigned)-1) {
            mdb_dbi_close(ndsdb->env, ndsdb->dbi);
            ndsdb->dbi = -1;
        }

        ndsdb->rdb = NULL;
        ndsdb->db_name[0] = '\0';
        ndsdb->txn_count = 0;
    }
}

/* Open the freezer.  Pass in the redis DB you wish to open, and whether you
 * want to open it for read (writer == 0) or write (writer == 1).  You'll
 * get back an `NDSDB *` that can be handed around to other nds_* functions,
 * or NULL on failure.
 */
static NDSDB *nds_open(redisDb *db, int writer) {
    int rv;

    serverLog(LL_DEBUG, "nds_open(db=%i, writer=%i), ref=%i", db->id, writer, server.ndsdb.refs);

    /* If we're re-opening, we need to make sure we're being asked for the
     * same thing */
    if (server.ndsdb.refs > 0) {
        if (server.ndsdb.writer != writer) {
            serverLog(LL_WARNING,
                     "Can't reopen active NDS environment for %s",
                     writer ? "writing" : "reading");
            return NULL;
        }
        if (server.ndsdb.rdb->id != db->id) {
            serverLog(LL_WARNING,
                     "Can't open DB %i, DB %i is already open",
                     db->id, server.ndsdb.rdb->id);
            return NULL;
        }

        /* OK, we can short-circuit things from here */
        server.ndsdb.refs++;
        return &server.ndsdb;
    }

    /* Re-open everything if we aren't open in the correct access mode.
     * We can be confident that we're not already in a transaction,
     * because of the previous refcount check */
    if (server.ndsdb.writer != writer) {
        mdb_env_close(server.ndsdb.env);
        server.ndsdb.env = NULL;
    }

    /* Now we can commence the creation of the NDSDB */

    if (!server.ndsdb.env) {
        struct stat statbuf;
        struct statvfs statvfsbuf;
        unsigned long long mapsize = 0;

        serverLog(LL_DEBUG, "initialising mdb_env");

        /* There's a bit of hinkyness in MDB.  First off, if your database
         * doesn't already exist, you need to open the database in
         * read-write mode, otherwise it doesn't get created (OK, that
         * *sorta* makes sense).  The thing that *really* fluffs my muffin,
         * though, is that you need to set a "map size" when you open the
         * database for writing, because...  mmap() yada yada.  Seems that
         * MDB doesn't handle change well.  The least-worst option for
         * deciding "how big should I make the map size" is just to set it
         * to the size of the partition we're writing the files to.  Making
         * it "insanely hueg" (like a PB or so) doesn't work well, because
         * then the crazy thing tries to mmap a PB of memory, which takes a
         * little while.
         *
         * So, we try to stat the datafile.  If that fails because the
         * datafile doesn't exist, we enable writer mode, because we'll be
         * creating the database.  Then we find out how big the partition is
         * that we're on, and set the map size to that.
         */
        if (stat("data.mdb", &statbuf) == -1) {
            if (errno == ENOENT) {
                serverLog(LL_DEBUG, "data.mdb doesn't exist; creating");
                writer = 1;
            } else {
                serverLog(LL_WARNING, "stat(data.mdb) failed: %s", strerror(errno));
                goto mdb_env_cleanup;
            }
        }

        if (statvfs(".", &statvfsbuf) == -1) {
            serverLog(LL_WARNING, "statvfs(.) failed: %s", strerror(errno));
            goto mdb_env_cleanup;
        }

        mapsize = statvfsbuf.f_blocks * statvfsbuf.f_frsize;

        /* Ensure the mapsize is a multiple of the page size, because
         * grumble grumble */
        mapsize = (mapsize / sysconf(_SC_PAGESIZE)) * sysconf(_SC_PAGESIZE);

        serverLog(LL_DEBUG, "Setting mapsize to %llu", mapsize);

        if ((rv = mdb_env_create(&server.ndsdb.env))) {
            serverLog(LL_WARNING, "mdb_env_create() failed: %s", mdb_strerror(rv));
            server.ndsdb.env = NULL;
            goto mdb_env_cleanup;
        }

        if ((rv = mdb_env_set_mapsize(server.ndsdb.env, mapsize))) {
            serverLog(LL_WARNING, "mdb_env_set_mapsize() failed: %s", mdb_strerror(rv));
        }

        if ((rv = mdb_env_set_maxdbs(server.ndsdb.env, server.dbnum))) {
            serverLog(LL_WARNING, "mdb_env_set_maxdbs() failed: %s", mdb_strerror(rv));
            goto mdb_env_cleanup;
        }

        if ((rv = mdb_env_open(server.ndsdb.env, ".", writer ? 0 : MDB_RDONLY, 0644))) {
            serverLog(LL_WARNING, "mdb_env_open() failed: %s", mdb_strerror(rv));
            goto mdb_env_cleanup;
        }

        server.ndsdb.writer = writer;

        goto success;

mdb_env_cleanup:
        if (server.ndsdb.env) {
            mdb_env_close(server.ndsdb.env);
            server.ndsdb.env = NULL;
        }
        return NULL;

success:
        serverLog(LL_DEBUG, "mdb_env initialised");
    }

    /* We'll only get here if we're not already open (if the refcount was
     * already > 0, we'd have returned waaaay back at the beginning), so we
     * can now behave like we're starting in a whole new world.  */
    server.ndsdb.rdb = db;
    server.ndsdb.txn_count = 0;
    server.ndsdb.dbi = -1;
    server.ndsdb.refs = 1;

    freezer_filename(db, server.ndsdb.db_name);

    if ((rv = mdb_txn_begin(server.ndsdb.env, NULL, writer ? 0 : MDB_RDONLY, &(server.ndsdb.txn)))) {
        serverLog(LL_WARNING, "Failed to begin a txn: %s", mdb_strerror(rv));
        server.ndsdb.txn = NULL;
        goto err_cleanup;
    }

    if ((rv = mdb_dbi_open(server.ndsdb.txn, server.ndsdb.db_name, writer ? MDB_CREATE : 0, &(server.ndsdb.dbi)))) {
        if (writer || (rv != MDB_NOTFOUND && rv != EPERM)) {
            serverLog(LL_WARNING, "Failed to open freezer DBi for DB %i: %s", db->id, mdb_strerror(rv));
            goto err_cleanup;
        }
    }

    /* Epic win! */
    goto done;

err_cleanup:
    nds_close(&server.ndsdb);

done:
    return &server.ndsdb;
}

/* Return 1 if it is *known* to be pointless to go to disk, because the key
 * isn't there.  This requires the keycache to be enabled, naturally. */
static int not_in_keycache(redisDb *db, sds key) {
    if (!server.nds_keycache) {
        return 0;
    }

    return (dictFind(db->nds_keys, key) ? 0 : 1);
}

static void cache_key(redisDb *db, sds key) {
    if (!server.nds_keycache) {
        return;
    }

    if (not_in_keycache(db, key)) {
        dictAdd(db->nds_keys, key, NULL);
    }
}

static void uncache_key(redisDb *db, sds key) {
    if (!server.nds_keycache) {
        return;
    }

    if (!not_in_keycache(db, key)) {
        dictDelete(db->nds_keys, key);
    }
}

/* Check whether a key exists in the NDS.  Give me a DB and a key, and
 * I'll give you a 1/0 to say whether it exists or not.  You'll get -1
 * if there was an error.
 */
static int nds_exists(NDSDB *db, sds key) {
    MDB_val k, v;
    int rv;

    if (not_in_keycache(db->rdb, key)) {
        return 0;
    }

    k.mv_size = sdslen(key);
    k.mv_data = key;

    if (isDirtyKey(db->rdb, key)) {
        /* If the key's dirty but you're coming here, then it isn't in
         * memory, so it clearly mustn't exist.  */
        serverLog(LL_DEBUG,
                 "nds_exists(db=%i, key=%s) => NOT_IN_MEMORY",
                 db->rdb->id, key);
        return 0;
    }

    if (sdslen(key) > MDB_MAXKEYSIZE) {
        serverLog(LL_WARNING, "Passed excessively long key to nds_exists");
        return -1;
    }

    rv = mdb_get(db->txn, db->dbi, &k, &v);

    if (rv == 0) {
        rv = 1;
    } else if (rv == MDB_NOTFOUND) {
        rv = 0;
    } else {
        serverLog(LL_WARNING, "mdb_get(%s) failed: %s", key, mdb_strerror(rv));
        rv = -1;
    }

    serverLog(LL_DEBUG, "nds_exists(db=%i, key=%s) => %i", db->rdb->id, key, rv);
    return rv;
}

/* Get a value out of the NDS.  Pass in the DB and key to get the value for,
 * and return an sds containing the value if found, or NULL on error or
 * key-not-found.  Will report errors via serverLog.  */
static sds nds_get(NDSDB *db, sds key) {
    MDB_val k, v;
    int rv;

    if (not_in_keycache(db->rdb, key)) {
        return NULL;
    }

    k.mv_size = sdslen(key);
    k.mv_data = key;

    if (isDirtyKey(db->rdb, key)) {
        /* A dirty key *must* be in memory if it still exists.  If you're
         * coming here, then the key *isn't* in memory, thus it does not
         * exist, and so I'm not going to go and get an out-of-date copy off
         * disk for you.
         */
        serverLog(LL_DEBUG,
                 "nds_get(db=%i, key=%s) => NOT_IN_MEMORY",
                 db->rdb->id, key);
        return NULL;
    }

    if (sdslen(key) > MDB_MAXKEYSIZE) {
        serverLog(LL_WARNING, "Passed excessively long key to nds_get");
        return NULL;
    }

    rv = mdb_get(db->txn, db->dbi, &k, &v);

    if (rv) {
        if (rv == MDB_NOTFOUND) {
            serverLog(LL_DEBUG, "nds_get(db=%i, key=%s) => NOTFOUND",
                     db->rdb->id, key);
        } else {
            serverLog(LL_WARNING, "mdb_get(%s) failed: %s", key, mdb_strerror(rv));
        }
        return NULL;
    }

    serverLog(LL_DEBUG, "nds_get(db=>%i, key=%s) => %lu byte value",
             db->rdb->id, key, v.mv_size);
    return sdsnewlen(v.mv_data, v.mv_size);
}

/* Set a value in the NDS.  Takes an NDSDB, a key, and a value, and makes
 * sure they get into the database (or you at least know what's going on via
 * the logs).  Returns C_ERR on failure or C_OK on success.  */
static int nds_set(NDSDB *db, sds key, sds val) {
    int rv = C_OK;
    MDB_val k, v;

    k.mv_size = sdslen(key);
    k.mv_data = key;

    v.mv_size = sdslen(val);
    v.mv_data = val;

    if (sdslen(key) > MDB_MAXKEYSIZE) {
        serverLog(LL_WARNING, "Passed excessively long key to nds_set");
        return C_ERR;
    }

    if (sdslen(val) > MDB_MAXDATASIZE) {
        serverLog(LL_WARNING, "Key %s has an excessively long value", key);
        return C_ERR;
    }

    rv = mdb_put(db->txn, db->dbi, &k, &v, 0);
    db->txn_count++;

    if (rv) {
        serverLog(LL_WARNING, "mdb_put(%s) failed: %s", key, mdb_strerror(rv));
        return C_ERR;
    }

    if (db->txn_count > 50000) {
        serverLog(LL_NOTICE, "txn full; performing intermediate txn commit");
        rv = mdb_txn_commit(db->txn);
        if (rv) {
            serverLog(LL_WARNING, "Failed to commit txn: %s", mdb_strerror(rv));
            return C_ERR;
        } else {
            db->txn_count = 0;
            mdb_dbi_close(db->env, db->dbi);
            mdb_txn_begin(db->env, NULL, 0, &(db->txn));
            mdb_dbi_open(db->txn, db->db_name, 0, &(db->dbi));
        }
    }

    serverLog(LL_DEBUG, "nds_set(db=%i, key=%s) => C_OK",
             db->rdb->id, key);
    return C_OK;
}

/* Deletion time!  Take an NDSDB and a key, and make the key go away.  Tells
 * the user about problems via the logs, and returns 1 if a key was deleted,
 * 0 if no key was deleted, and -1 if an error occured. */
static int nds_del(NDSDB *db, sds key) {
    MDB_val k;
    int rv;

    k.mv_size = sdslen(key);
    k.mv_data = key;

    rv = mdb_del(db->txn, db->dbi, &k, NULL);

    if (rv == MDB_NOTFOUND) {
        rv = 0;
    } else if (rv) {
        serverLog(LL_WARNING, "nds_del('%s') failed: %s", key, mdb_strerror(rv));
        rv = -1;
    } else {
        rv = 1;
    }

    serverLog(LL_DEBUG, "nds_del(db=%i, key=%s) => %i",
             db->rdb->id, key, rv);

    return rv;
}

/* Do the necessary bits and pieces required before a fork */
void preforkNDS(void) {
    mdb_env_close(server.ndsdb.env);
    server.ndsdb.env = NULL;
}

robj *getNDS(redisDb *db, robj *key) {
    sds val = NULL;
    rio payload;
    int type;
    robj *obj = NULL;
    NDSDB *ndsdb = nds_open(db, 0);
    NDS_TIMER_START;

    serverLog(LL_DEBUG, "Looking up %s in NDS", (char *)key->ptr);

    if (!ndsdb) {
        return NULL;
    }

    val = nds_get(ndsdb, key->ptr);

    nds_close(ndsdb);

    if (val) {
        serverLog(LL_DEBUG, "Key %s was found in NDS", (char *)key->ptr);

        /* We got one!  Thaw and return */

        rioInitWithBuffer(&payload, val);
        if (((type = rdbLoadObjectType(&payload)) == -1) ||
            ((obj  = rdbLoadObject(type,&payload,key->ptr,db->id,NULL)) == NULL))
        {
            serverLog(LL_WARNING, "Bad data format for key %s; ignoring", (char *)key->ptr);
            goto nds_cleanup;
        }

        if (obj) {
            sds copy = sdsdup(key->ptr);
            serverAssertWithInfo(NULL, key, dictAdd(db->dict, copy, obj) == C_OK);

            if (rdbLoadType(&payload) == RDB_OPCODE_EXPIRETIME_MS) {
                long long expire = rdbLoadMillisecondTime(&payload, RDB_VERSION);
                serverLog(LL_DEBUG, "Setting expiry time to %lld", expire);
                setExpire(NULL, db, key, expire);
            }
        }
    }

nds_cleanup:
    if (val) {
        sdsfree(val);
    }
    NDS_TIMER_END;
    return obj;
}

/* Return 0/1 based on a key's existence in NDS.  Doesn't bring the key into
 * memory. */
int existsNDS(redisDb *db, robj *key) {
    NDSDB *ndsdb = nds_open(db, 0);
    int rv;
    NDS_TIMER_START;

    serverLog(LL_DEBUG, "Checking for existence of %s in NDS", (char *)key->ptr);

    if (!ndsdb) {
        return -1;
    }

    rv = nds_exists(ndsdb, key->ptr);
    nds_close(ndsdb);

    NDS_TIMER_END;
    return rv;
}

/* Remove all keys from an NDS database. */
int emptyNDS(redisDb *db) {
    NDSDB *ndsdb = nds_open(db, 1);
    int rv;
    NDS_TIMER_START;

    if (!ndsdb) {
        serverLog(LL_WARNING, "Failed to open DB %i", db->id);
        return C_ERR;
    }

    if ((rv = mdb_drop(ndsdb->txn, ndsdb->dbi, 0)) != 0) {
        serverLog(LL_WARNING, "Failed to empty DB: %s", mdb_strerror(rv));
    }

    serverLog(LL_DEBUG, "emptyNDS(db=%i) => C_OK", db->id);
    nds_close(ndsdb);
    NDS_TIMER_END;
    return C_OK;
}

size_t keyCountNDS(redisDb *db) {
    NDSDB *ndsdb = nds_open(db, 0);
    int rv;
    MDB_stat stats;
    NDS_TIMER_START;

    if (!ndsdb) {
        return 0;
    }

    if ((rv = mdb_stat(ndsdb->txn, ndsdb->dbi, &stats))) {
        serverLog(LL_DEBUG, "Failed to stat: %s", mdb_strerror(rv));
        nds_close(ndsdb);
        return 0;
    }

    nds_close(ndsdb);

    serverLog(LL_DEBUG, "keyCountNDS(db=%i) => %lu",
             db->id, stats.ms_entries);
    NDS_TIMER_END;
    return stats.ms_entries;
}

/* Walk the entire keyspace of an NDS database, calling walkerCallback for
 * every key we find.  Pass in 'data' for any callback-specific state you
 * might like to deal with.
 */
int walkNDS(redisDb *db,
            int (*walkerCallback)(void *, robj *),
            void *data,
            int interrupt_rate) {
    MDB_cursor *cur = NULL;
    NDSDB *ndsdb = NULL;
    MDB_val key, val;
    int rv, counter = 0;
    NDS_TIMER_START;

    ndsdb = nds_open(db, 0);
    if (!ndsdb) {
        rv = C_ERR;
        goto cleanup;
    }

    rv = mdb_cursor_open(ndsdb->txn, ndsdb->dbi, &cur);
    if (rv) {
        if (rv == EINVAL) {
            /* EINVAL gets returned if we (amongst other things) ask to get a cursor
             * for a "sub-database" that doesn't actually exist.  This is quite the
             * pest. */
            rv = C_OK;
        } else {
            serverLog(LL_WARNING, "Failed to open MDB cursor: %s", mdb_strerror(rv));
            rv = C_ERR;
        }
        goto cleanup;
    }

    serverLog(LL_DEBUG, "Walking the NDS keyspace for DB %i", db->id);

    while ((rv = mdb_cursor_get(cur, &key, &val, MDB_NEXT)) == 0) {
        robj *kobj = createStringObject(key.mv_data, key.mv_size);

        if (kobj && walkerCallback(data, kobj) == C_ERR) {
            serverLog(LL_DEBUG, "walkNDS terminated prematurely at callback's request");
            rv = C_ERR;
            if (kobj) decrRefCount(kobj);
            goto cleanup;
        }

        if (kobj) decrRefCount(kobj);

        if (interrupt_rate > 0 && !(++counter % interrupt_rate)) {
            /* Let other clients have a sniff */
            aeProcessEvents(server.el, AE_FILE_EVENTS|AE_DONT_WAIT);
        }
    }

cleanup:
    if (cur) {
        mdb_cursor_close(cur);
    }
    nds_close(ndsdb);

    NDS_TIMER_END;
    return rv;
}

/* Clear all NDS databases */
void nukeNDSFromOrbit(void) {
    NDS_TIMER_START;
    unlink("data.mdb");
    unlink("lock.mdb");
    NDS_TIMER_END;
}

static int preloadWalker(void *data, robj *key) {
    redisDb *db = (redisDb *)data;
    sds copy = sdsdup(key->ptr);

    if (!dictFind(db->dict, copy)) {
        int retval = dictAdd(db->dict, copy, getNDS(db, key));

        serverAssertWithInfo(NULL,key,retval == C_OK);
    }

    return C_OK;
}

/* Read all keys from the NDS datastores into memory. */
void preloadNDS(void) {
    if (server.nds_preload_in_progress || server.nds_preload_complete) {
        return;
    }
    serverLog(LL_NOTICE, "Preloading all keys from NDS");
    server.nds_preload_in_progress = 1;
    for (int i = 0; i < server.dbnum; i++) {
        walkNDS(server.db+i, preloadWalker, server.db+i, 1000);
    }
    serverLog(LL_NOTICE, "NDS preload complete");
    server.nds_preload_in_progress = 0;
    server.nds_preload_complete = 1;
}

static int keycacheWalker(void *data, robj *key) {
    redisDb *db = (redisDb *)data;
    sds copy = sdsdup(key->ptr);

    int retval = dictAdd(db->nds_keys, copy, NULL);
    serverAssertWithInfo(NULL, key, retval == C_OK);

    return C_OK;
}

void notifyNDS(redisDb *db, sds key, int change_type) {
    NDS_TIMER_START;
    if (!dictFind(db->dirty_keys, key)) {
        dictAdd(db->dirty_keys, sdsdup(key), NULL);
    }

    switch (change_type) {
        case NDS_KEY_ADD:
            cache_key(db, key);
            break;
        case NDS_KEY_DEL:
        case NDS_KEY_EXPIRED:
            uncache_key(db, key);
            break;
        case NDS_KEY_CHANGE:
            /* Nothing special to do here; just avoiding a log message */
            break;
        default:
            serverLog(LL_WARNING, "notifyNDS called with unknown change_type: %i", change_type);
    }
    NDS_TIMER_END;
}

void loadNDSKeycache(void) {
    serverLog(LL_NOTICE, "Loading all keys from NDS");

    for (int i = 0; i < server.dbnum; i++) {
        walkNDS(server.db+i, keycacheWalker, server.db+i, 0);
    }
    serverLog(LL_NOTICE, "Key cache loaded");
}


int isDirtyKey(redisDb *db, sds key) {
    NDS_TIMER_START;
    if (dictFind(db->dirty_keys, key)
        || dictFind(db->flushing_keys, key)
       ) {
        return 1;
    } else {
        return 0;
    }
    NDS_TIMER_END;
}

unsigned long long dirtyKeyCount(void) {
    unsigned long long count = 0;
    NDS_TIMER_START;

    for (int i = 0; i < server.dbnum; i++) {
        count += dictSize((server.db+i)->dirty_keys);
    }

    NDS_TIMER_END;
    return count;
}

unsigned long long flushingKeyCount(void) {
    unsigned long long count = 0;
    NDS_TIMER_START;

    for (int i = 0; i < server.dbnum; i++) {
        count += dictSize((server.db+i)->flushing_keys);
    }

    NDS_TIMER_END;
    return count;
}

/* Fork and flush all the dirty keys out to disk. */
int backgroundDirtyKeysFlush(void) {
    pid_t childpid;
    NDS_TIMER_START;

    if (server.child_pid != -1) return C_ERR;

    /* Can't (shouldn't?) happen -- trying to flush while there's already a
     * non-empty set of flushing keys. */
    for (int i = 0; i < server.dbnum; i++) {
        redisDb *db = server.db+i;

        if (dictSize(db->flushing_keys) > 0) {
            serverLog(LL_WARNING, "FFFUUUUU- you can't flush when there's already keys being flushed.");
            serverLog(LL_WARNING, "This isn't supposed to be able to happen.");
            return C_ERR;
        }
    }

    server.dirty_before_bgsave = server.dirty;

    preforkNDS();

    if ((childpid = redisFork(CHILD_TYPE_NDS)) == 0) {
        int retval;

        serverLog(LL_DEBUG, "In child");

        /* Child */
        for (int j = 0; j < server.ipfd.count; j++) close(server.ipfd.fd[j]);
        if (server.sofd > 0) close(server.sofd);

        retval = flushDirtyKeys();

        exitFromChild((retval == C_OK) ? 0 : 1);
    } else {
        /* Parent */
        if (childpid == -1) {
            serverLog(LL_WARNING, "Can't save in background: fork: %s",
                     strerror(errno));
            return C_ERR;
        }

        serverLog(LL_DEBUG, "Dirty key flush started in PID %d", childpid);
        server.child_pid = childpid;
        /* Rotate the dirty keys into the flushing keys list, and use the
         * previous flushing keys list as the new dirty keys list. */
        for (int j = 0; j < server.dbnum; j++) {
            redisDb *db = server.db+j;
            dict *dTmp;
            dTmp = db->flushing_keys;
            db->flushing_keys = db->dirty_keys;
            db->dirty_keys = dTmp;
        }
        NDS_TIMER_END;
        return C_OK;
    }

    /* Can't happen */
    return C_ERR;
}

int flushDirtyKeys(void) {
    NDS_TIMER_START;

    serverLog(LL_DEBUG, "Flushing dirty keys");
    for (int j = 0; j < server.dbnum; j++) {
        redisDb *db = server.db+j;
        dictIterator *di;
        dictEntry *deKey, *deVal;
        NDSDB *ndsdb;

        serverLog(LL_DEBUG, "Flushing %lu keys for DB %i", dictSize(db->dirty_keys), j);

        if (dictSize(db->dirty_keys) == 0) continue;

        di = dictGetSafeIterator(db->dirty_keys);
        if (!di) {
            serverLog(LL_WARNING, "dictGetSafeIterator failed");
            return C_ERR;
        }

        ndsdb = nds_open(db, 1);

        if (!ndsdb) {
            return C_ERR;
        }

        while ((deKey = dictNext(di)) != NULL) {
            sds keystr = dictGetKey(deKey);
            deVal = dictFind(db->dict, keystr);

            if (sdslen(keystr) > MDB_MAXKEYSIZE) {
                serverLog(LL_NOTICE, "Attempted to flush excessively long key: %s", keystr);
                continue;
            }

            if (!deVal) {
                /* Key must have been deleted after it got dirtied.  NUKE IT! */
                serverLog(LL_DEBUG, "Deleting key '%s' from NDS", keystr);
                if (nds_del(ndsdb, keystr) == -1) {
                    serverLog(LL_WARNING, "nds_del returned error, flush failed");
                    NDS_TIMER_END;
                    return C_ERR;
                }
            } else {
                rio payload;
                robj *kobj = createStringObject(keystr, sdslen(keystr));
                robj *vobj = dictGetVal(deVal);
                long long expire = getExpire(db, kobj);

                decrRefCount(kobj);

                rioInitWithBuffer(&payload,sdsempty());
                serverAssert(rdbSaveObjectType(&payload,vobj));
                serverAssert(rdbSaveObject(&payload,vobj,kobj,db->id));
                if (expire >= 0) {
                    serverLog(LL_DEBUG, "Saving expiry time of %s (%lld)", keystr, expire);
                    serverAssert(rdbSaveType(&payload, RDB_OPCODE_EXPIRETIME_MS));
                    serverAssert(rdbSaveMillisecondTime(&payload, expire));
                }
                serverLog(LL_DEBUG, "Flushing %s (%lu serialized bytes)", keystr, sdslen(payload.io.buffer.ptr));
                if (nds_set(ndsdb, keystr, payload.io.buffer.ptr) == C_ERR) {
                    serverLog(LL_WARNING, "nds_set returned error, flush failed");
                    sdsfree(payload.io.buffer.ptr);
                    return C_ERR;
                }
                sdsfree(payload.io.buffer.ptr);
            }
        }

        nds_close(ndsdb);
    }

    serverLog(LL_DEBUG, "Flush complete");

    if (server.nds_snapshot_in_progress) {
        int rv;
        int rc;

        serverLog(LL_NOTICE, "Commencing snapshot");
        /* Woohoo!  Snapshot time! */
        if ((rc = system("rm -rf ./snapshot"))) {
            serverLog(LL_WARNING, "Snapshot removal: %s", strerror(rc));
        }
        if ((rc = system("mkdir -p ./snapshot"))) {
            serverLog(LL_WARNING, "Snapshot removal: %s", strerror(rc));
        }
        /* Corner-case alert: if we had no keys to flush in any database,
         * then nds_open() will never have been called, meaning that
         * server.mdb_env won't have been initialised since it was closed in
         * backgroundDirtyKeysFlush() before we forked.  Hence, we *may*
         * need to trigger a quick open to initialise server.mdb_env.  */
        if (!server.ndsdb.env) {
            nds_close(nds_open(server.db, 0));
        }

        if ((rv = mdb_env_copy(server.ndsdb.env, "./snapshot"))) {
            serverLog(LL_WARNING, "Snapshot failed: %s", mdb_strerror(rv));
        } else {
            serverLog(LL_NOTICE, "Snapshot completed successfully");
        }
    }

    NDS_TIMER_END;
    return C_OK;
}

void postNDSFlushCleanup(void) {
    NDS_TIMER_START;
    for (int i = 0; i < server.dbnum; i++) {
        redisDb *db = server.db+i;
        dictEmpty(db->flushing_keys, NULL);
    }
    server.lastsave = time(NULL);
    server.stat_nds_flush_success++;
    NDS_TIMER_END;
}

void backgroundNDSFlushDoneHandler(int exitcode, int bysignal) {
    NDS_TIMER_START;

    serverLog(LL_NOTICE, "NDS background save completed.  exitcode=%i, bysignal=%i", exitcode, bysignal);

    server.nds_snapshot_in_progress = 0;

    if (exitcode == 0 && bysignal == 0) {
        postNDSFlushCleanup();
        server.dirty -= server.dirty_before_bgsave;

        if (server.nds_bg_requestor) {
            addReply(server.nds_bg_requestor, shared.ok);
            server.nds_bg_requestor = NULL;
        }
    } else {
        server.stat_nds_flush_failure++;
        /* Merge the flushing keys back into the dirty keys so that they'll be
         * retried on the next flush, since we can't know for certain whether
         * they got flushed before our child died */
        for (int i = 0; i < server.dbnum; i++) {
            redisDb *db = server.db+i;
            dictIterator *di;
            dictEntry *de;

            serverLog(LL_DEBUG, "Merging %lu flushing keys back into dirty keys for DB %i", dictSize(db->flushing_keys), i);

            di = dictGetSafeIterator(db->flushing_keys);
            if (!di) {
                serverLog(LL_WARNING, "backgroundNDSFlushDoneHandler: dictGetSafeIterator failed!  This is terribad!");
                NDS_TIMER_END;
                return;
            }

            while ((de = dictNext(di)) != NULL) {
                dictAdd(db->dirty_keys, sdsdup(dictGetKey(de)), NULL);
            }

            dictEmpty(db->flushing_keys, NULL);
        }

        if (server.nds_bg_requestor) {
            if (server.nds_snapshot_in_progress) {
                addReplyError(server.nds_bg_requestor, "NDS SNAPSHOT failed in child; consult logs for details");
            } else if (server.nds_bg_requestor) {
                addReplyError(server.nds_bg_requestor, "NDS FLUSH failed in child; consult logs for details");
            }
            server.nds_bg_requestor = NULL;
        }
    }

    server.child_pid = -1;

    if (server.nds_snapshot_pending) {
        /* Trigger a snapshot job now */
        server.nds_snapshot_in_progress = server.nds_snapshot_pending;
        server.nds_snapshot_pending = 0;
        if (backgroundDirtyKeysFlush() == C_ERR && server.nds_bg_requestor) {
            addReplyError(server.nds_bg_requestor, "Delayed NDS SNAPSHOT failed; consult logs for details");
            server.nds_bg_requestor = NULL;
            NDS_TIMER_END;
            return;
        }
    }
    NDS_TIMER_END;
}

void checkNDSChildComplete(void) {
    NDS_TIMER_START;

    if (server.child_pid != -1) {
        int statloc;
        pid_t pid;

        if ((pid = wait3(&statloc,WNOHANG,NULL)) != 0) {
            int exitcode = WEXITSTATUS(statloc);
            int bysignal = 0;

            if (pid == -1) {
                serverLog(LL_WARNING, "wait3() failed: %s", strerror(errno));
            }

            if (WIFSIGNALED(statloc)) bysignal = WTERMSIG(statloc);

            if (pid > 0) {
                if (pid == server.child_pid) {
                    NDS_TIMER_END;
                    backgroundNDSFlushDoneHandler(exitcode,bysignal);
                } else {
                    serverLog(LL_WARNING,
                        "Warning, detected child with unmatched pid: %ld",
                        (long)pid);
                    NDS_TIMER_END;
                }
            }
        }
    }
}

void ndsFlushCommand(client *c) {
    if (server.nds_bg_requestor) {
        addReplyError(c, "NDS background operation already in progress");
        return;
    }

    if (server.child_pid == -1) {
        if (backgroundDirtyKeysFlush() == C_ERR) {
            addReplyError(c, "NDS FLUSH failed to start; consult logs for details");
            return;
        }
    }

    server.nds_bg_requestor = c;
}

void ndsSnapshotCommand(client *c) {
    if (server.nds_snapshot_pending || server.nds_snapshot_in_progress) {
        addReplyError(c, "NDS SNAPSHOT already in progress");
        return;
    }

    if (server.nds_bg_requestor) {
        addReplyError(c, "NDS background operation already in progress");
        return;
    }

    if (server.child_pid == -1) {
        server.nds_snapshot_in_progress = 1;
        if (backgroundDirtyKeysFlush() == C_ERR) {
            addReplyError(c, "NDS SNAPSHOT failed to start; consult logs for details");
            return;
        }
    } else {
        /* A regular (non-snapshot) NDS flush is already in progress; we'll
         * have to do our snapshot later */
        server.nds_snapshot_pending = 1;
    }

    server.nds_bg_requestor = c;
}

void ndsMemkeysCommand(client *c) {
    void *rlen = addReplyDeferredLen(c);
    dictIterator *di = dictGetSafeIterator(c->db->dict);
    dictEntry *de;
    int numkeys = 0;

    di = dictGetSafeIterator(c->db->dict);

    while ((de = dictNext(di)) != NULL) {
        sds key = dictGetKey(de);
        robj *keyobj = createStringObject(key, sdslen(key));

        addReplyBulk(c, keyobj);
        decrRefCount(keyobj);
        numkeys++;
    }
    setDeferredArrayLen(c, rlen, numkeys);
}

void ndsCommand(client *c) {
    if (!strcasecmp(c->argv[1]->ptr,"snapshot")) {
        if (c->argc != 2) goto badarity;
        serverLog(LL_NOTICE, "NDS SNAPSHOT requested");
        ndsSnapshotCommand(c);
        /* We don't want to send an OK immediately; that'll get sent when the
         * snapshot completes */
        return;
    } else if (!strcasecmp(c->argv[1]->ptr,"flush")) {
        if (c->argc != 2) goto badarity;
        serverLog(LL_NOTICE, "NDS FLUSH requested");
        ndsFlushCommand(c);
        /* We don't want to send an OK immediately; that'll get sent when the
         * flush completes */
        return;
    } else if (!strcasecmp(c->argv[1]->ptr,"clearstats")) {
        if (c->argc != 2) goto badarity;
        serverLog(LL_NOTICE, "NDS CLEARSTATS requested");
        server.stat_nds_cache_hits = 0;
        server.stat_nds_cache_misses = 0;
        server.stat_nds_usec = 0;
    } else if (!strcasecmp(c->argv[1]->ptr,"preload")) {
        if (c->argc != 2) goto badarity;
        serverLog(LL_NOTICE, "NDS PRELOAD requested");
        preloadNDS();
    } else if (!strcasecmp(c->argv[1]->ptr,"memkeys")) {
        if (c->argc != 2) goto badarity;
        serverLog(LL_NOTICE, "NDS MEMKEYS requested");
        ndsMemkeysCommand(c);
        /* We don't want to send an OK; the response gets sent by the command
         * handler. */
        return;
    } else {
        addReplyError(c,
            "NDS subcommand must be one of: SNAPSHOT FLUSH CLEARSTATS PRELOAD MEMKEYS");
        return;
    }
    addReply(c, shared.ok);
    return;

badarity:
    addReplyErrorFormat(c,"Wrong number of arguments for NDS %s",
        (char*) c->argv[1]->ptr);
}
