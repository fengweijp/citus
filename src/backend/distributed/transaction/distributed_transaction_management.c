/*-------------------------------------------------------------------------
 *
 * distributed_transaction_management.c
 *
 *   TODO: fill here
 *
 * Copyright (c) 2017, Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "miscadmin.h"

#include "funcapi.h"
#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "datatype/timestamp.h"
#include "distributed/distributed_transaction_management.h"
#include "distributed/metadata_cache.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/spin.h"
#include "storage/s_lock.h"
#include "utils/timestamp.h"


/*
 * Citus identifies a distributed transaction with a triplet consisting of
 *
 *  -  initiatorNodeIdentifier: A unique identifier of the node that initiated
 *     the distributed transaction
 *  -  transactionId: A locally unique identifier assigned for the distributed
 *     transaction on the node that initiated the distributed transaction
 *  -  timestamp: The current timestamp of distributed transaction initiation
 *
 */
typedef struct DistributedTransactionId
{
	uint64 initiatorNodeIdentifier;
	uint64 transactionId;
	TimestampTz timestamp;
} DistributedTransactionId;


/*
 * Each backend's active distributed transaction information is tracked via
 * DistributedTransactionBackendData on the shared memory.
 */
typedef struct DistributedTransactionBackendData
{
	Oid databaseId;
	DistributedTransactionId transactionId;
} DistributedTransactionBackendData;


/*
 * All backends active distributed transaction data reside in the
 * shared memory on the DistributedTransactionShmemData.
 */
typedef struct DistributedTransactionShmemData
{
	int trancheId;
#if (PG_VERSION_NUM < 100000)
	LWLockTranche lockTranche;
#else
	NamedLWLockTranche namedLockTranche;
#endif
	LWLock lock;

	pg_atomic_uint64 nextTransactionId;

	DistributedTransactionBackendData sessions[FLEXIBLE_ARRAY_MEMBER];
} DistributedTransactionShmemData;


static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static DistributedTransactionShmemData *distributedTransactionShmemData = NULL;
static DistributedTransactionBackendData *DistributedTransactionBackend = NULL;
slock_t BackendDistributedTransactionMutex = '\0';


static void DistributedTransactionManagementShmemInit(void);
static Datum GenerateDistributedTransactionIdTuple(Oid databaseId, uint64
												   initiatorNodeIdentifier, uint64
												   transactionId, TimestampTz timestamp);
static TupleDesc GenerateDistributedTransactionTupleDesc(void);
static size_t DistributedTransactionManagementShmemSize(void);


PG_FUNCTION_INFO_V1(assign_distributed_transaction_id);
PG_FUNCTION_INFO_V1(get_distributed_transaction_id);


/*
 * assign_distributed_transaction_id updates the shared memory allocated for this backend
 * and sets initiatorNodeIdentifier, transactionId, timestamp fields with the given
 * inputs. Also, the function sets the database id via MyDatabaseId variable provided
 * by "misadmmin.h".
 */
Datum
assign_distributed_transaction_id(PG_FUNCTION_ARGS)
{
	CheckCitusVersion(ERROR);

	/* TODO: Is assert OK? */
	if (!DistributedTransactionBackend)
	{
		InitializeDistributedTransactionManagementBackend();
	}

	SpinLockAcquire(&BackendDistributedTransactionMutex);

	DistributedTransactionBackend->databaseId = MyDatabaseId;

	DistributedTransactionBackend->transactionId.initiatorNodeIdentifier =
		PG_GETARG_INT64(0);
	DistributedTransactionBackend->transactionId.transactionId = PG_GETARG_INT64(1);
	DistributedTransactionBackend->transactionId.timestamp = PG_GETARG_TIMESTAMPTZ(2);

	SpinLockRelease(&BackendDistributedTransactionMutex);

	PG_RETURN_VOID();
}


/*
 * get_distributed_transaction_id returns a tuple with (databaseId, initiatorNodeIdentifier,
 * transactionId, timestamp) that exists in the shared memory.
 */
Datum
get_distributed_transaction_id(PG_FUNCTION_ARGS)
{
	Datum distributedTransactionId = 0;

	Oid databaseId = InvalidOid;
	uint64 initiatorNodeIdentifier = 0;
	uint64 transactionId = 0;
	TimestampTz timestamp = 0;

	CheckCitusVersion(ERROR);

	if (!DistributedTransactionBackend)
	{
		InitializeDistributedTransactionManagementBackend();
	}

	/* read the shmem while mutex is held */
	SpinLockAcquire(&BackendDistributedTransactionMutex);

	databaseId = DistributedTransactionBackend->databaseId;
	initiatorNodeIdentifier =
		DistributedTransactionBackend->transactionId.initiatorNodeIdentifier;
	transactionId = DistributedTransactionBackend->transactionId.transactionId;
	timestamp = DistributedTransactionBackend->transactionId.timestamp;

	SpinLockRelease(&BackendDistributedTransactionMutex);

	distributedTransactionId =
		GenerateDistributedTransactionIdTuple(databaseId, initiatorNodeIdentifier,
											  transactionId, timestamp);

	PG_RETURN_DATUM(distributedTransactionId);
}


/*
 * GenerateDistributedTransactionIdTuple returns a datum of psudo-generated heaptuple
 * for the distributed transaction id provided by the parameters.
 */
static Datum
GenerateDistributedTransactionIdTuple(Oid databaseId, uint64 initiatorNodeIdentifier,
									  uint64 transactionId, TimestampTz timestamp)
{
	TupleDesc tupleDescriptor = NULL;
	HeapTuple heapTuple = NULL;
	Datum distributedTransactionIdDatum = 0;
	int attributeCount = 4;
	Datum values[attributeCount];
	bool isNulls[attributeCount];

	/* form new shard tuple */
	memset(values, 0, sizeof(values));
	memset(isNulls, false, sizeof(isNulls));

	values[0] = ObjectIdGetDatum(databaseId);
	values[1] = UInt64GetDatum(initiatorNodeIdentifier);
	values[2] = UInt64GetDatum(transactionId);
	values[3] = TimestampTzGetDatum(timestamp);

	tupleDescriptor = GenerateDistributedTransactionTupleDesc();

	heapTuple = heap_form_tuple(tupleDescriptor, values, isNulls);
	distributedTransactionIdDatum = HeapTupleGetDatum(heapTuple);

	return distributedTransactionIdDatum;
}


/*
 * GenerateDistributedTransactionTupleDesc returns a psudo tuple descriptor
 * for distributed transaction id in the form of (databaseId,
 * initiatorNodeIdentifier, transactionId, timestamp).
 */
static TupleDesc
GenerateDistributedTransactionTupleDesc(void)
{
	TupleDesc distributedTransactionTupleDesc = NULL;
	int attributeCount = 4;
	bool hasOids = false;

	/* generate a custom tuple descriptor for distributed transaction id*/
	distributedTransactionTupleDesc = CreateTemplateTupleDesc(attributeCount, hasOids);

	TupleDescInitEntry(distributedTransactionTupleDesc, (AttrNumber) 1, "databaseId",
					   OIDOID, -1, 0);
	TupleDescInitEntry(distributedTransactionTupleDesc, (AttrNumber) 2,
					   "initiatorNodeIdentifier", INT8OID, -1, 0);
	TupleDescInitEntry(distributedTransactionTupleDesc, (AttrNumber) 3, "transactionId",
					   INT8OID, -1, 0);
	TupleDescInitEntry(distributedTransactionTupleDesc, (AttrNumber) 4, "timestamp",
					   TIMESTAMPTZOID, -1, 0);

	/* let Postgres learn about this tuple descriptor */
	BlessTupleDesc(distributedTransactionTupleDesc);

	return distributedTransactionTupleDesc;
}


/*
 * InitializeDistributedTransactionManagement requests the necessary shared memory
 * from Postgres and sets up the shared memory startup hook.
 */
void
InitializeDistributedTransactionManagement(void)
{
	/* allocate shared memory */
	RequestAddinShmemSpace(DistributedTransactionManagementShmemSize());

	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = DistributedTransactionManagementShmemInit;
}


/*
 * DistributedTransactionManagementShmemInit is the callback that is to be called on
 * shared memory startup hook. The function sets up the necessary shared memory segment
 * for the distributed transaction manager.
 */
static void
DistributedTransactionManagementShmemInit(void)
{
	bool alreadyInitialized = false;
	char *trancheName = "Distributed Transaction Management";

	/* we may update the shmem, acquire lock exclusively */
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	distributedTransactionShmemData =
		(DistributedTransactionShmemData *) ShmemInitStruct(
			"Distributed Transaction Management",
			DistributedTransactionManagementShmemSize(),
			&alreadyInitialized);

	if (!alreadyInitialized)
	{
#if (PG_VERSION_NUM < 100000)
		LWLockTranche *lockTranche = &distributedTransactionShmemData->lockTranche;
#else
		NamedLWLockTranche *namedLockTranche =
			&distributedTransactionShmemData->namedLockTranche;
#endif

		/* start by zeroing out all the memory */
		memset(distributedTransactionShmemData, 0,
			   DistributedTransactionManagementShmemSize());

		distributedTransactionShmemData->trancheId = LWLockNewTrancheId();

#if (PG_VERSION_NUM < 100000)

		/* we only need a single lock */
		lockTranche->array_base = &distributedTransactionShmemData->lock;
		lockTranche->array_stride = sizeof(LWLock);
		lockTranche->name = trancheName;

		LWLockRegisterTranche(distributedTransactionShmemData->trancheId, lockTranche);
		LWLockInitialize(&distributedTransactionShmemData->lock,
						 distributedTransactionShmemData->trancheId);
#else
		LWLockRegisterTranche(namedLockTranche->trancheId, trancheName);
		LWLockInitialize(&distributedTransactionShmemData->lock,
						 namedLockTranche->trancheId);
#endif

		/* start the distributed transaction ids from 1 */
		pg_atomic_init_u64(&distributedTransactionShmemData->nextTransactionId, 1);
	}

	LWLockRelease(AddinShmemInitLock);

	if (prev_shmem_startup_hook != NULL)
	{
		prev_shmem_startup_hook();
	}
}


/*
 * DistributedTransactionShmemSize returns the size that should be allocated
 * on the shared memory for distributed transaction management.
 */
static size_t
DistributedTransactionManagementShmemSize(void)
{
	Size size = 0;

	size = add_size(size, sizeof(DistributedTransactionShmemData));
	size = add_size(size, mul_size(sizeof(DistributedTransactionBackendData),
								   MaxBackends));

	return size;
}


/*
 *  InitializeDistributedTransactionManagementBackend is called per backend
 *  and does the required initilizations.
 */
void
InitializeDistributedTransactionManagementBackend(void)
{
	LWLockAcquire(&distributedTransactionShmemData->lock, LW_EXCLUSIVE);

	DistributedTransactionBackend =
		&distributedTransactionShmemData->sessions[MyProc->pgprocno];

	Assert(DistributedTransactionBackend);

	DistributedTransactionBackend->databaseId = 0;
	DistributedTransactionBackend->transactionId.initiatorNodeIdentifier = 0;
	DistributedTransactionBackend->transactionId.transactionId = 0;
	DistributedTransactionBackend->transactionId.timestamp = 0;

	/* we'll use mutex while accessing the backend shmem */
	SpinLockInit(&BackendDistributedTransactionMutex);

	LWLockRelease(&distributedTransactionShmemData->lock);
}


/*
 * UnSetDistributedTransactionId simply acquires the mutex and resets
 * the backend's distributed transaction data in shared memory to the
 * initial values.
 */
void
UnSetDistributedTransactionId(void)
{
	if (DistributedTransactionBackend)
	{
		SpinLockAcquire(&BackendDistributedTransactionMutex);

		DistributedTransactionBackend->databaseId = 0;
		DistributedTransactionBackend->transactionId.initiatorNodeIdentifier = 0;
		DistributedTransactionBackend->transactionId.transactionId = 0;
		DistributedTransactionBackend->transactionId.timestamp = 0;

		SpinLockRelease(&BackendDistributedTransactionMutex);
	}
}


/*
 * Will be used for testing. Uncomment once tests are added.
 *
 * static uint64
 * GetNextTransactionId()
 * {
 *  pg_atomic_uint64 transactionIdSequence =
 *      distributedTransactionShmemData->nextTransactionId;
 *  uint64 nextTransactionId = 0;
 *
 *  nextTransactionId = pg_atomic_fetch_add_u64(&transactionIdSequence, 1);
 *
 *  return nextTransactionId;
 * }
 */
