/*-------------------------------------------------------------------------
 *
 * citus_tools.c
 *	  UDF to run multi shard/worker queries
 *
 * This file contains functions to run commands on other worker/shards.
 *
 * Copyright (c) Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "distributed/argutils.h"
#include "distributed/connection_management.h"
#include "distributed/metadata_cache.h"
#include "distributed/multi_client_executor.h"
#include "distributed/multi_server_executor.h"
#include "distributed/remote_commands.h"
#include "distributed/listutils.h"
#include "distributed/lock_graph.h"
#include "distributed/tuplestore.h"
#include "distributed/version_compat.h"
#include "distributed/worker_protocol.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "libpq-fe.h"
#include "miscadmin.h"
#include "utils/builtins.h"

/* simple query to run on workers to check connectivity */
#define CONNECTIVITY_CHECK_QUERY "SELECT 1"
#define CONNECTIVITY_CHECK_COLUMNS 5

PG_FUNCTION_INFO_V1(citus_check_connection_to_node);
PG_FUNCTION_INFO_V1(citus_check_cluster_node_health);
PG_FUNCTION_INFO_V1(master_run_on_worker);

static bool CheckConnectionToNode(char *nodeName, uint32 nodePort);
static int ParseCommandParameters(FunctionCallInfo fcinfo, StringInfo **nodeNameArray,
								  int **nodePortsArray, StringInfo **commandStringArray,
								  bool *parallel);
static void ExecuteCommandsInParallelAndStoreResults(StringInfo *nodeNameArray,
													 int *nodePortArray,
													 StringInfo *commandStringArray,
													 bool *statusArray,
													 StringInfo *resultStringArray,
													 int commandCount);
static bool GetConnectionStatusAndResult(MultiConnection *connection, bool *resultStatus,
										 StringInfo queryResultString);
static bool EvaluateQueryResult(MultiConnection *connection, PGresult *queryResult,
								StringInfo queryResultString);
static void StoreErrorMessage(MultiConnection *connection, StringInfo queryResultString);
static void ExecuteCommandsAndStoreResults(StringInfo *nodeNameArray,
										   int *nodePortArray,
										   StringInfo *commandStringArray,
										   bool *statusArray,
										   StringInfo *resultStringArray,
										   int commandCount);
static bool ExecuteRemoteQueryOrCommand(char *nodeName, uint32 nodePort,
										char *queryString, StringInfo queryResult);
static Tuplestorestate * CreateTupleStore(TupleDesc tupleDescriptor,
										  StringInfo *nodeNameArray, int *nodePortArray,
										  bool *statusArray,
										  StringInfo *resultArray, int commandCount);
static void StoreAllConnectivityChecks(Tuplestorestate *tupleStore,
									   TupleDesc tupleDescriptor);
static char * GetConnectivityCheckCommand(const char *nodeName, const uint32 nodePort);


/*
 * citus_check_connection_to_node sends a simple query from a worker node to another
 * node, and returns success status.
 */
Datum
citus_check_connection_to_node(PG_FUNCTION_ARGS)
{
	char *nodeName = PG_GETARG_TEXT_TO_CSTRING(0);
	uint32 nodePort = PG_GETARG_UINT32(1);

	bool success = CheckConnectionToNode(nodeName, nodePort);
	PG_RETURN_BOOL(success);
}


/*
 * CheckConnectionToNode sends a simple query to a node and returns success status
 */
static bool
CheckConnectionToNode(char *nodeName, uint32 nodePort)
{
	int connectionFlags = 0;
	MultiConnection *connection = GetNodeConnection(connectionFlags, nodeName, nodePort);
	int responseStatus = ExecuteOptionalRemoteCommand(connection,
													  CONNECTIVITY_CHECK_QUERY, NULL);

	return responseStatus == RESPONSE_OKAY;
}


/*
 * citus_check_cluster_node_health UDF performs connectivity checks from all the nodes to
 * all the nodes, and report success status
 */
Datum
citus_check_cluster_node_health(PG_FUNCTION_ARGS)
{
	CheckCitusVersion(ERROR);

	TupleDesc tupleDescriptor = NULL;
	Tuplestorestate *tupleStore = SetupTuplestore(fcinfo, &tupleDescriptor);

	StoreAllConnectivityChecks(tupleStore, tupleDescriptor);

	PG_RETURN_VOID();
}


/*
 * GetConnectivityCheckCommand returns the command to check connections to a node
 */
static char *
GetConnectivityCheckCommand(const char *nodeName, const uint32 nodePort)
{
	StringInfo connectivityCheckCommand = makeStringInfo();
	appendStringInfo(connectivityCheckCommand,
					 "SELECT citus_check_connection_to_node('%s', %d)",
					 nodeName, nodePort);

	return connectivityCheckCommand->data;
}


/*
 * StoreAllConnectivityChecks performs connectivity checks from all the nodes to all the
 * nodes, and report success status.
 *
 * Algorithm is:
 * for sourceNode in activeReadableNodeList:
 *   c = connectToNode(sourceNode)
 *   for targetNode in activeReadableNodeList:
 *     result = c.execute("SELECT citus_check_connection_to_node(targetNode.name, targetNode.port")
 *     emit sourceNode.name, sourceNode.port, targetNode.name, targetNode.port, result
 *
 * -- result -> true  -> connection attempt from source to target succeeded
 * -- result -> false -> connection attempt from source to target failed
 * -- result -> NULL  -> connection attempt from the current node to source node failed
 */
static void
StoreAllConnectivityChecks(Tuplestorestate *tupleStore, TupleDesc tupleDescriptor)
{
	Datum values[CONNECTIVITY_CHECK_COLUMNS];
	bool isNulls[CONNECTIVITY_CHECK_COLUMNS];

	/*
	 * Get all the readable node list so that we will check connectivity to followers in
	 * the cluster as well.
	 */
	List *workerNodeList = ActiveReadableNodeList();

	/* we want to check for connectivity in a deterministic order */
	workerNodeList = SortList(workerNodeList, CompareWorkerNodes);

	/*
	 * We iterate over the workerNodeList twice, for source and target worker nodes. This
	 * operation is safe for foreach_ptr macro, as long as we use different variables for
	 * each iteration.
	 */
	WorkerNode *sourceWorkerNode = NULL;
	foreach_ptr(sourceWorkerNode, workerNodeList)
	{
		const char *sourceNodeName = sourceWorkerNode->workerName;
		const int sourceNodePort = sourceWorkerNode->workerPort;
		int32 connectionFlags = 0;

		/* open a connection to the source node using the synchronous api */
		MultiConnection *connectionToSourceNode =
			GetNodeConnection(connectionFlags, sourceNodeName, sourceNodePort);

		/* the second iteration over workerNodeList for the target worker nodes. */
		WorkerNode *targetWorkerNode = NULL;
		foreach_ptr(targetWorkerNode, workerNodeList)
		{
			const char *targetNodeName = targetWorkerNode->workerName;
			const int targetNodePort = targetWorkerNode->workerPort;

			char *connectivityCheckCommandToTargetNode =
				GetConnectivityCheckCommand(targetNodeName, targetNodePort);

			PGresult *result = NULL;
			int executionResult =
				ExecuteOptionalRemoteCommand(connectionToSourceNode,
											 connectivityCheckCommandToTargetNode,
											 &result);

			/* get ready for the next tuple */
			memset(values, 0, sizeof(values));
			memset(isNulls, false, sizeof(isNulls));

			values[0] = PointerGetDatum(cstring_to_text(sourceNodeName));
			values[1] = Int32GetDatum(sourceNodePort);
			values[2] = PointerGetDatum(cstring_to_text(targetNodeName));
			values[3] = Int32GetDatum(targetNodePort);

			/*
			 * If we could not send the query or the result was not ok, set success field
			 * to NULL. This may indicate connection errors to a worker node, however that
			 * node can potentially connect to other nodes.
			 *
			 * Therefore, we mark the success as NULL to indicate that the connectivity
			 * status is unknown.
			 */
			if (executionResult != RESPONSE_OKAY)
			{
				isNulls[4] = true;
			}
			else
			{
				int rowIndex = 0;
				int columnIndex = 0;
				values[4] = BoolGetDatum(ParseBoolField(result, rowIndex, columnIndex));
			}

			tuplestore_putvalues(tupleStore, tupleDescriptor, values, isNulls);

			PQclear(result);
			ForgetResults(connectionToSourceNode);
		}
	}
}


/*
 * master_run_on_worker executes queries/commands to run on specified worker and
 * returns success status and query/command result. Expected input is 3 arrays
 * containing node names, node ports, and query strings, and boolean flag to specify
 * parallel execution. The function then returns node_name, node_port, success,
 * result tuples upon completion of the query. The same user credentials are used
 * to connect to remote nodes.
 */
Datum
master_run_on_worker(PG_FUNCTION_ARGS)
{
	CheckCitusVersion(ERROR);

	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	bool parallelExecution = false;
	StringInfo *nodeNameArray = NULL;
	int *nodePortArray = NULL;
	StringInfo *commandStringArray = NULL;

	/* check to see if caller supports us returning a tuplestore */
	if (!rsinfo || !(rsinfo->allowedModes & SFRM_Materialize))
	{
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("materialize mode required, but it is not "
						"allowed in this context")));
	}

	int commandCount = ParseCommandParameters(fcinfo, &nodeNameArray, &nodePortArray,
											  &commandStringArray, &parallelExecution);

	MemoryContext per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	MemoryContext oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* get the requested return tuple description */
	TupleDesc tupleDescriptor = CreateTupleDescCopy(rsinfo->expectedDesc);

	/*
	 * Check to make sure we have correct tuple descriptor
	 */
	if (tupleDescriptor->natts != 4 ||
		TupleDescAttr(tupleDescriptor, 0)->atttypid != TEXTOID ||
		TupleDescAttr(tupleDescriptor, 1)->atttypid != INT4OID ||
		TupleDescAttr(tupleDescriptor, 2)->atttypid != BOOLOID ||
		TupleDescAttr(tupleDescriptor, 3)->atttypid != TEXTOID)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_DEFINITION),
				 errmsg("query-specified return tuple and "
						"function return type are not compatible")));
	}

	/*
	 * prepare storage for status and result values.
	 * commandCount is based on user input however, it is the length of list
	 * instead of a user given integer, hence this should be safe here in terms
	 * of memory allocation.
	 */
	bool *statusArray = palloc0(commandCount * sizeof(bool));
	StringInfo *resultArray = palloc0(commandCount * sizeof(StringInfo));
	for (int commandIndex = 0; commandIndex < commandCount; commandIndex++)
	{
		resultArray[commandIndex] = makeStringInfo();
	}

	if (parallelExecution)
	{
		ExecuteCommandsInParallelAndStoreResults(nodeNameArray, nodePortArray,
												 commandStringArray,
												 statusArray, resultArray, commandCount);
	}
	else
	{
		ExecuteCommandsAndStoreResults(nodeNameArray, nodePortArray, commandStringArray,
									   statusArray, resultArray, commandCount);
	}

	/* let the caller know we're sending back a tuplestore */
	rsinfo->returnMode = SFRM_Materialize;
	Tuplestorestate *tupleStore = CreateTupleStore(tupleDescriptor,
												   nodeNameArray, nodePortArray,
												   statusArray,
												   resultArray, commandCount);
	rsinfo->setResult = tupleStore;
	rsinfo->setDesc = tupleDescriptor;

	MemoryContextSwitchTo(oldcontext);

	PG_RETURN_VOID();
}


/* ParseCommandParameters reads call parameters and fills in data structures */
static int
ParseCommandParameters(FunctionCallInfo fcinfo, StringInfo **nodeNameArray,
					   int **nodePortsArray, StringInfo **commandStringArray,
					   bool *parallel)
{
	ArrayType *nodeNameArrayObject = PG_GETARG_ARRAYTYPE_P(0);
	ArrayType *nodePortArrayObject = PG_GETARG_ARRAYTYPE_P(1);
	ArrayType *commandStringArrayObject = PG_GETARG_ARRAYTYPE_P(2);
	bool parallelExecution = PG_GETARG_BOOL(3);
	int nodeNameCount = ArrayObjectCount(nodeNameArrayObject);
	int nodePortCount = ArrayObjectCount(nodePortArrayObject);
	int commandStringCount = ArrayObjectCount(commandStringArrayObject);
	Datum *nodeNameDatumArray = DeconstructArrayObject(nodeNameArrayObject);
	Datum *nodePortDatumArray = DeconstructArrayObject(nodePortArrayObject);
	Datum *commandStringDatumArray = DeconstructArrayObject(commandStringArrayObject);

	if (nodeNameCount != nodePortCount || nodeNameCount != commandStringCount)
	{
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("expected same number of node name, port, and query string")));
	}

	StringInfo *nodeNames = palloc0(nodeNameCount * sizeof(StringInfo));
	int *nodePorts = palloc0(nodeNameCount * sizeof(int));
	StringInfo *commandStrings = palloc0(nodeNameCount * sizeof(StringInfo));

	for (int index = 0; index < nodeNameCount; index++)
	{
		text *nodeNameText = DatumGetTextP(nodeNameDatumArray[index]);
		char *nodeName = text_to_cstring(nodeNameText);
		int32 nodePort = DatumGetInt32(nodePortDatumArray[index]);
		text *commandText = DatumGetTextP(commandStringDatumArray[index]);
		char *commandString = text_to_cstring(commandText);

		nodeNames[index] = makeStringInfo();
		commandStrings[index] = makeStringInfo();

		appendStringInfo(nodeNames[index], "%s", nodeName);
		nodePorts[index] = nodePort;
		appendStringInfo(commandStrings[index], "%s", commandString);
	}

	*nodeNameArray = nodeNames;
	*nodePortsArray = nodePorts;
	*commandStringArray = commandStrings;
	*parallel = parallelExecution;

	return nodeNameCount;
}


/*
 * ExecuteCommandsInParallelAndStoreResults connects to each node specified in
 * nodeNameArray and nodePortArray, and executes command in commandStringArray
 * in parallel fashion. Execution success status and result is reported for
 * each command in statusArray and resultStringArray. Each array contains
 * commandCount items.
 */
static void
ExecuteCommandsInParallelAndStoreResults(StringInfo *nodeNameArray, int *nodePortArray,
										 StringInfo *commandStringArray,
										 bool *statusArray, StringInfo *resultStringArray,
										 int commandCount)
{
	MultiConnection **connectionArray =
		palloc0(commandCount * sizeof(MultiConnection *));
	int finishedCount = 0;

	/* start connections asynchronously */
	for (int commandIndex = 0; commandIndex < commandCount; commandIndex++)
	{
		char *nodeName = nodeNameArray[commandIndex]->data;
		int nodePort = nodePortArray[commandIndex];
		int connectionFlags = FORCE_NEW_CONNECTION;
		connectionArray[commandIndex] =
			StartNodeConnection(connectionFlags, nodeName, nodePort);
	}

	/* establish connections */
	for (int commandIndex = 0; commandIndex < commandCount; commandIndex++)
	{
		MultiConnection *connection = connectionArray[commandIndex];
		StringInfo queryResultString = resultStringArray[commandIndex];
		char *nodeName = nodeNameArray[commandIndex]->data;
		int nodePort = nodePortArray[commandIndex];

		FinishConnectionEstablishment(connection);

		if (PQstatus(connection->pgConn) != CONNECTION_OK)
		{
			appendStringInfo(queryResultString, "failed to connect to %s:%d", nodeName,
							 (int) nodePort);
			statusArray[commandIndex] = false;
			connectionArray[commandIndex] = NULL;
			finishedCount++;
		}
		else
		{
			statusArray[commandIndex] = true;
		}
	}

	/* send queries at once */
	for (int commandIndex = 0; commandIndex < commandCount; commandIndex++)
	{
		MultiConnection *connection = connectionArray[commandIndex];
		char *queryString = commandStringArray[commandIndex]->data;
		StringInfo queryResultString = resultStringArray[commandIndex];

		/*
		 * If we don't have a connection, nothing to send, error string should already
		 * been filled.
		 */
		if (connection == NULL)
		{
			continue;
		}

		int querySent = SendRemoteCommand(connection, queryString);
		if (querySent == 0)
		{
			StoreErrorMessage(connection, queryResultString);
			statusArray[commandIndex] = false;
			CloseConnection(connection);
			connectionArray[commandIndex] = NULL;
			finishedCount++;
		}
	}

	/* check for query results */
	while (finishedCount < commandCount)
	{
		for (int commandIndex = 0; commandIndex < commandCount; commandIndex++)
		{
			MultiConnection *connection = connectionArray[commandIndex];
			StringInfo queryResultString = resultStringArray[commandIndex];
			bool success = false;

			if (connection == NULL)
			{
				continue;
			}

			bool queryFinished = GetConnectionStatusAndResult(connection, &success,
															  queryResultString);

			if (queryFinished)
			{
				finishedCount++;
				statusArray[commandIndex] = success;
				connectionArray[commandIndex] = NULL;
				CloseConnection(connection);
			}
		}

		CHECK_FOR_INTERRUPTS();

		if (finishedCount < commandCount)
		{
			long sleepIntervalPerCycle = RemoteTaskCheckInterval * 1000L;
			pg_usleep(sleepIntervalPerCycle);
		}
	}

	pfree(connectionArray);
}


/*
 * GetConnectionStatusAndResult checks the active connection and returns true if
 * query execution is finished (either success or fail).
 * Query success/fail in resultStatus, and query result in queryResultString are
 * reported upon completion of the query.
 */
static bool
GetConnectionStatusAndResult(MultiConnection *connection, bool *resultStatus,
							 StringInfo queryResultString)
{
	bool finished = true;
	ConnStatusType connectionStatus = PQstatus(connection->pgConn);

	*resultStatus = false;
	resetStringInfo(queryResultString);

	if (connectionStatus == CONNECTION_BAD)
	{
		appendStringInfo(queryResultString, "connection lost");
		return finished;
	}

	int consumeInput = PQconsumeInput(connection->pgConn);
	if (consumeInput == 0)
	{
		appendStringInfo(queryResultString, "query result unavailable");
		return finished;
	}

	/* check later if busy */
	if (PQisBusy(connection->pgConn) != 0)
	{
		finished = false;
		return finished;
	}

	/* query result is available at this point */
	PGresult *queryResult = PQgetResult(connection->pgConn);
	bool success = EvaluateQueryResult(connection, queryResult, queryResultString);
	PQclear(queryResult);

	*resultStatus = success;
	finished = true;
	return true;
}


/*
 * EvaluateQueryResult gets the query result from connection and returns
 * true if the query is executed successfully, false otherwise. A query result
 * or an error message is returned in queryResultString. The function requires
 * that the query returns a single column/single row result. It returns an
 * error otherwise.
 */
static bool
EvaluateQueryResult(MultiConnection *connection, PGresult *queryResult,
					StringInfo queryResultString)
{
	bool success = false;

	ExecStatusType resultStatus = PQresultStatus(queryResult);
	if (resultStatus == PGRES_COMMAND_OK)
	{
		char *commandStatus = PQcmdStatus(queryResult);
		appendStringInfo(queryResultString, "%s", commandStatus);
		success = true;
	}
	else if (resultStatus == PGRES_TUPLES_OK)
	{
		int ntuples = PQntuples(queryResult);
		int nfields = PQnfields(queryResult);

		/* error if query returns more than 1 rows, or more than 1 fields */
		if (nfields != 1)
		{
			appendStringInfo(queryResultString,
							 "expected a single column in query target");
		}
		else if (ntuples > 1)
		{
			appendStringInfo(queryResultString,
							 "expected a single row in query result");
		}
		else
		{
			int row = 0;
			int column = 0;
			if (!PQgetisnull(queryResult, row, column))
			{
				char *queryResultValue = PQgetvalue(queryResult, row, column);
				appendStringInfo(queryResultString, "%s", queryResultValue);
			}
			success = true;
		}
	}
	else
	{
		StoreErrorMessage(connection, queryResultString);
	}

	return success;
}


/*
 * StoreErrorMessage gets the error message from connection and stores it
 * in queryResultString. It should be called only when error is present
 * otherwise it would return a default error message.
 */
static void
StoreErrorMessage(MultiConnection *connection, StringInfo queryResultString)
{
	char *errorMessage = PQerrorMessage(connection->pgConn);
	if (errorMessage != NULL)
	{
		/* copy the error message to a writable memory */
		errorMessage = pnstrdup(errorMessage, strlen(errorMessage));

		char *firstNewlineIndex = strchr(errorMessage, '\n');

		/* trim the error message at the line break */
		if (firstNewlineIndex != NULL)
		{
			*firstNewlineIndex = '\0';
		}
	}
	else
	{
		/* put a default error message if no error message is reported */
		errorMessage = "An error occurred while running the query";
	}

	appendStringInfo(queryResultString, "%s", errorMessage);
}


/*
 * ExecuteCommandsAndStoreResults connects to each node specified in
 * nodeNameArray and nodePortArray, and executes command in commandStringArray
 * in sequential order. Execution success status and result is reported for
 * each command in statusArray and resultStringArray. Each array contains
 * commandCount items.
 */
static void
ExecuteCommandsAndStoreResults(StringInfo *nodeNameArray, int *nodePortArray,
							   StringInfo *commandStringArray, bool *statusArray,
							   StringInfo *resultStringArray, int commandCount)
{
	for (int commandIndex = 0; commandIndex < commandCount; commandIndex++)
	{
		char *nodeName = nodeNameArray[commandIndex]->data;
		int32 nodePort = nodePortArray[commandIndex];
		char *queryString = commandStringArray[commandIndex]->data;
		StringInfo queryResultString = resultStringArray[commandIndex];

		bool success = ExecuteRemoteQueryOrCommand(nodeName, nodePort, queryString,
												   queryResultString);

		statusArray[commandIndex] = success;

		CHECK_FOR_INTERRUPTS();
	}
}


/*
 * ExecuteRemoteQueryOrCommand executes a query at specified remote node using
 * the calling user's credentials. The function returns the query status
 * (success/failure), and query result. The query is expected to return a single
 * target containing zero or one rows.
 */
static bool
ExecuteRemoteQueryOrCommand(char *nodeName, uint32 nodePort, char *queryString,
							StringInfo queryResultString)
{
	int connectionFlags = FORCE_NEW_CONNECTION;
	MultiConnection *connection =
		GetNodeConnection(connectionFlags, nodeName, nodePort);
	bool raiseInterrupts = true;

	if (PQstatus(connection->pgConn) != CONNECTION_OK)
	{
		appendStringInfo(queryResultString, "failed to connect to %s:%d", nodeName,
						 (int) nodePort);
		return false;
	}

	if (!SendRemoteCommand(connection, queryString))
	{
		appendStringInfo(queryResultString, "failed to send query to %s:%d", nodeName,
						 (int) nodePort);
		return false;
	}

	PGresult *queryResult = GetRemoteCommandResult(connection, raiseInterrupts);
	bool success = EvaluateQueryResult(connection, queryResult, queryResultString);

	PQclear(queryResult);

	/* close the connection */
	CloseConnection(connection);

	return success;
}


/* CreateTupleStore prepares result tuples from individual query results */
static Tuplestorestate *
CreateTupleStore(TupleDesc tupleDescriptor,
				 StringInfo *nodeNameArray, int *nodePortArray, bool *statusArray,
				 StringInfo *resultArray, int commandCount)
{
	Tuplestorestate *tupleStore = tuplestore_begin_heap(true, false, work_mem);
	bool nulls[4] = { false, false, false, false };

	for (int commandIndex = 0; commandIndex < commandCount; commandIndex++)
	{
		Datum values[4];
		StringInfo nodeNameString = nodeNameArray[commandIndex];
		StringInfo resultString = resultArray[commandIndex];
		text *nodeNameText = cstring_to_text_with_len(nodeNameString->data,
													  nodeNameString->len);
		text *resultText = cstring_to_text_with_len(resultString->data,
													resultString->len);

		values[0] = PointerGetDatum(nodeNameText);
		values[1] = Int32GetDatum(nodePortArray[commandIndex]);
		values[2] = BoolGetDatum(statusArray[commandIndex]);
		values[3] = PointerGetDatum(resultText);

		HeapTuple tuple = heap_form_tuple(tupleDescriptor, values, nulls);
		tuplestore_puttuple(tupleStore, tuple);

		heap_freetuple(tuple);
		pfree(nodeNameText);
		pfree(resultText);
	}
	return tupleStore;
}
