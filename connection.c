/*-------------------------------------------------------------------------
 *
 * connection.c
 * 		Foreign-data wrapper for remote MongoDB servers
 *
 * Portions Copyright (c) 2012-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 2004-2020, EnterpriseDB Corporation.
 * Portions Copyright (c) 2012–2014 Citus Data, Inc.
 *
 * IDENTIFICATION
 * 		connection.c
 *
 *-------------------------------------------------------------------------
 */
#include <sys/types.h>


#include "postgres.h"
#include "mongo_wrapper.h"
#include "mongo_fdw.h"

#include "access/xact.h"
#if PG_VERSION_NUM >= 130000
#include "common/hashfn.h"
#endif
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/syscache.h"

/* Length of host */
#define HOST_LEN 256

/*
 * Connection cache hash table entry
 *
 * The lookup key in this hash table is the foreign server OID plus the user
 * mapping OID.  (We use just one connection per user per foreign server,
 * so that we can ensure all scans use the same snapshot during a query.)
 */
typedef struct ConnCacheKey
{
	Oid			serverid;		/* OID of foreign server */
	Oid			userid;			/* OID of local user whose mapping we use */
} ConnCacheKey;

typedef struct ConnCacheEntry
{
	ConnCacheKey key;			/* hash key (must be first) */
	MONGO_CONN *conn;			/* connection to foreign server, or NULL */
	bool		invalidated;	/* true if reconnect is pending */
	uint32		server_hashvalue;	/* hash value of foreign server OID */
	uint32		mapping_hashvalue;  /* hash value of user mapping OID */
} ConnCacheEntry;

/*
 * Connection cache (initialized on first use)
 */
static HTAB *ConnectionHash = NULL;

static void mongo_inval_callback(Datum arg, int cacheid, uint32 hashvalue);

/*
 * mongo_get_connection:
 * 			Get a mong connection which can be used to execute queries on
 * the remote Mongo server with the user's authorization. A new connection
 * is established if we don't already have a suitable one.
 */
MONGO_CONN *
mongo_get_connection(ForeignServer *server, UserMapping *user, MongoFdwOptions *opt)
{
	bool		found;
	ConnCacheEntry *entry;
	ConnCacheKey key;

	/* First time through, initialize connection cache hashtable */
	if (ConnectionHash == NULL)
	{
		HASHCTL		ctl;

		MemSet(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(ConnCacheKey);
		ctl.entrysize = sizeof(ConnCacheEntry);
		ctl.hash = tag_hash;
		/* allocate ConnectionHash in the cache context */
		ctl.hcxt = CacheMemoryContext;
		ConnectionHash = hash_create("mongo_fdw connections", 8,
									 &ctl,
									 HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);

		/*
		 * Register some callback functions that manage connection cleanup.
		 * This should be done just once in each backend.
		 */
		CacheRegisterSyscacheCallback(FOREIGNSERVEROID,
									  mongo_inval_callback, (Datum) 0);
		CacheRegisterSyscacheCallback(USERMAPPINGOID,
									  mongo_inval_callback, (Datum) 0);
	}

	/* Create hash key for the entry.  Assume no pad bytes in key struct */
	key.serverid = server->serverid;
	key.userid = user->userid;

	/*
	 * Find or create cached entry for requested connection.
	 */
	entry = hash_search(ConnectionHash, &key, HASH_ENTER, &found);
	if (!found)
	{
		/* initialize new hashtable entry (key is already filled in) */
		entry->conn = NULL;
	}

	/* If an existing entry has invalid connection then release it */
	if (entry->conn != NULL && entry->invalidated)
	{
		elog(DEBUG3, "disconnecting mongo_fdw connection %p for option changes to take effect",
			 entry->conn);
		MongoDisconnect(entry->conn);
		entry->conn = NULL;
	}

	if (entry->conn == NULL)
	{
#if PG_VERSION_NUM < 90600
		Oid			umoid;
#endif

#ifdef META_DRIVER
		entry->conn = MongoConnect(opt->svr_address, opt->svr_port, opt->svr_database, opt->svr_username, opt->svr_password,
								   opt->authenticationDatabase, opt->replicaSet, opt->readPreference,
								   opt->ssl, opt->pem_file, opt->pem_pwd, opt->ca_file, opt->ca_dir, opt->crl_file, opt->weak_cert_validation);
#else
		entry->conn = MongoConnect(opt->svr_address, opt->svr_port, opt->svr_database, opt->svr_username, opt->svr_password);
#endif
		elog(DEBUG3, "new mongo_fdw connection %p for server \"%s:%d\"",
			 entry->conn, opt->svr_address, opt->svr_port);

		/*
		 * Once the connection is established, then set the connection
		 * invalidation flag to false, also set the server and user mapping
		 * hash values.
		 */
		entry->invalidated = false;
		entry->server_hashvalue =
			GetSysCacheHashValue1(FOREIGNSERVEROID,
								  ObjectIdGetDatum(server->serverid));
#if PG_VERSION_NUM >= 90600
		entry->mapping_hashvalue =
			GetSysCacheHashValue1(USERMAPPINGOID,
								  ObjectIdGetDatum(user->umid));
#else
		/* Pre-9.6, UserMapping doesn't store its OID, so look it up again */
		umoid = GetSysCacheOid2(USERMAPPINGUSERSERVER,
								ObjectIdGetDatum(user->userid),
								ObjectIdGetDatum(user->serverid));
		if (!OidIsValid(umoid))
		{
			/* Not found for the specific user -- try PUBLIC */
			umoid = GetSysCacheOid2(USERMAPPINGUSERSERVER,
									ObjectIdGetDatum(InvalidOid),
									ObjectIdGetDatum(user->serverid));
		}
		entry->mapping_hashvalue =
			GetSysCacheHashValue1(USERMAPPINGOID, ObjectIdGetDatum(umoid));
#endif
	}

	return entry->conn;
}

/*
 * mongo_cleanup_connection:
 * Delete all the cache entries on backend exists.
 */
void
mongo_cleanup_connection()
{
	HASH_SEQ_STATUS scan;
	ConnCacheEntry *entry;

	if (ConnectionHash == NULL)
		return;

	hash_seq_init(&scan, ConnectionHash);
	while ((entry = (ConnCacheEntry *) hash_seq_search(&scan)))
	{
		if (entry->conn == NULL)
			continue;

		elog(DEBUG3, "disconnecting mongo_fdw connection %p", entry->conn);
		MongoDisconnect(entry->conn);
		entry->conn = NULL;
	}
}

/*
 * Release connection created by calling mongo_get_connection.
 */
void
mongo_release_connection(MONGO_CONN *conn)
{
	/*
	 * We don't close the connection indvisually  here, will do all connection
	 * cleanup on the backend exit.
	 */
}

/*
 * Connection invalidation callback function for mongo.
 *
 * After a change to a pg_foreign_server or pg_user_mapping catalog entry,
 * mark connections depending on that entry as needing to be remade. This
 * implementation is similar as pgfdw_inval_callback.
 */
static void
mongo_inval_callback(Datum arg, int cacheid, uint32 hashvalue)
{
	HASH_SEQ_STATUS scan;
	ConnCacheEntry *entry;

	Assert(cacheid == FOREIGNSERVEROID || cacheid == USERMAPPINGOID);

	/* ConnectionHash must exist already, if we're registered */
	hash_seq_init(&scan, ConnectionHash);
	while ((entry = (ConnCacheEntry *) hash_seq_search(&scan)))
	{
		/* Ignore invalid entries */
		if (entry->conn == NULL)
			continue;

		/* hashvalue == 0 means a cache reset, must clear all state */
		if (hashvalue == 0 ||
			(cacheid == FOREIGNSERVEROID &&
			 entry->server_hashvalue == hashvalue) ||
			(cacheid == USERMAPPINGOID &&
			 entry->mapping_hashvalue == hashvalue))
			entry->invalidated = true;
	}
}
