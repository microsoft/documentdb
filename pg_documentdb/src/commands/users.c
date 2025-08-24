/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/commands/users.c
 *
 * Implementation of user CRUD functions.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "fmgr.h"
#include "utils/documentdb_errors.h"
#include "utils/query_utils.h"
#include "utils/documentdb_errors.h"
#include "commands/commands_common.h"
#include "commands/parse_error.h"
#include "utils/feature_counter.h"
#include "libpq/scram.h"
#include "metadata/metadata_cache.h"
#include <common/saslprep.h>
#include <common/scram-common.h>
#include "api_hooks_def.h"
#include "users.h"
#include "roles.h"
#include "api_hooks.h"
#include "utils/hashset_utils.h"
#include "miscadmin.h"
#include "utils/list_utils.h"
#include "utils/string_view.h"

#define SCRAM_MAX_SALT_LEN 64

/* --------------------------------------------------------- */
/* Type definitions */
/* --------------------------------------------------------- */

/*
 * UserPrivilege stores a single user privilege and its actions.
 */
typedef struct
{
	const char *db;
	const char *collection;
	bool isCluster;
	size_t numActions;
	const StringView *actions;
} UserPrivilege;

/*
 * ConsolidateUserPrivilege consolidates the actions of a user privilege.
 */
typedef struct
{
	const char *db;
	const char *collection;
	bool isCluster;
	HTAB *actions;
} ConsolidatedUserPrivilege;

/*
 * Hash entry structure for user roles.
 */
typedef struct UserRoleHashEntry
{
	char *user;
	HTAB *roles;
	bool isExternal;
} UserRoleHashEntry;

/* GUC to enable user crud operations */
extern bool EnableUserCrud;

/* GUC that controls the default salt length*/
extern int ScramDefaultSaltLen;

/* GUC that controls the max number of users allowed*/
extern int MaxUserLimit;

/* GUC that controls the blocked role prefix list*/
extern char *BlockedRolePrefixList;

/* GUC that controls whether we use username/password validation*/
extern bool EnableUsernamePasswordConstraints;

/* GUC that controls whether the usersInfo command returns privileges*/
extern bool EnableUsersInfoPrivileges;

/* GUC that controls whether native authentication is enabled*/
extern bool IsNativeAuthEnabled;

PG_FUNCTION_INFO_V1(documentdb_extension_create_user);
PG_FUNCTION_INFO_V1(documentdb_extension_drop_user);
PG_FUNCTION_INFO_V1(documentdb_extension_update_user);
PG_FUNCTION_INFO_V1(documentdb_extension_get_users);
PG_FUNCTION_INFO_V1(command_connection_status);

static void ParseCreateUserSpec(pgbson *createUserSpec, CreateUserSpec *spec);
static char * ParseDropUserSpec(pgbson *dropSpec);
static void ParseUpdateUserSpec(pgbson *updateSpec, UpdateUserSpec *spec);
static Datum UpdateNativeUser(UpdateUserSpec *spec);
static void ParseGetUserSpec(pgbson *getSpec, GetUserSpec *spec);
static void CreateNativeUser(const CreateUserSpec *createUserSpec);
static void DropNativeUser(const char *dropUser);
static void ParseUsersInfoDocument(const bson_value_t *usersInfoBson, GetUserSpec *spec);
static char * PrehashPassword(const char *password);
static bool IsCallingUserExternal(void);
static bool IsPasswordInvalid(const char *username, const char *password);
static void WriteSinglePrivilegeDocument(const ConsolidatedUserPrivilege *privilege,
										 pgbson_array_writer *privilegesArrayWriter);

static void ConsolidatePrivileges(List **consolidatedPrivileges,
								  const UserPrivilege *sourcePrivileges,
								  size_t sourcePrivilegeCount);
static void ConsolidatePrivilege(List **consolidatedPrivileges,
								 const UserPrivilege *sourcePrivilege);
static void ConsolidatePrivilegesForRole(const char *roleName,
										 List **consolidatedPrivileges);
static bool ComparePrivileges(const ConsolidatedUserPrivilege *privilege1,
							  const UserPrivilege *privilege2);
static void DeepFreePrivileges(List *consolidatedPrivileges);
static void WriteSingleRolePrivileges(const char *roleName,
									  pgbson_array_writer *privilegesArrayWriter);
static void WriteMultipleRolePrivileges(HTAB *rolesTable,
										pgbson_array_writer *privilegesArrayWriter);
static void WritePrivilegeListToArray(List *consolidatedPrivileges,
									  pgbson_array_writer *privilegesArrayWriter);
static bool ParseConnectionStatusSpec(pgbson *connectionStatusSpec);
static Datum GetSingleUserInfo(const char *userName, bool returnDocuments);
static Datum GetAllUsersInfo(void);
static void WriteSingleUserDocument(UserRoleHashEntry *userEntry, bool showPrivileges,
									pgbson_array_writer *userArrayWriter);
static void WriteRoles(const char *parentRole,
					   pgbson_array_writer *roleArrayWriter);
static void WriteMultipleRoles(HTAB *rolesTable, pgbson_array_writer *roleArrayWriter);
static HTAB * CreateUserEntryHashSet(void);
static HTAB * BuildUserRoleEntryTable(Datum *userDatums, int userCount);
static uint32 UserHashEntryHashFunc(const void *obj, size_t objsize);
static int UserHashEntryCompareFunc(const void *obj1, const void *obj2,
									Size objsize);
static void FreeUserRoleEntryTable(HTAB *userRolesTable);
static const char * GetAllUsersQuery(void);
static const char * GetSingleUserQuery(void);
static const char * GetSingleUserNameQuery(void);
static const char * GetAllUsersQuery(void);
const char *GetAllUsersQueryString = NULL;
const char *GetSingleUserQueryString = NULL;
const char *GetSingleUserNameQueryString = NULL;

/*
 * Static definitions for user privileges and roles
 * These are used to define the privileges associated with each role
 */
static const UserPrivilege readOnlyPrivileges[] = {
	{
		.db = "",
		.collection = "",
		.isCluster = false,
		.numActions = 7,
		.actions = (const StringView[]) {
			{ .string = "changeStream", .length = 12 },
			{ .string = "collStats", .length = 9 },
			{ .string = "dbStats", .length = 7 },
			{ .string = "find", .length = 4 },
			{ .string = "killCursors", .length = 11 },
			{ .string = "listCollections", .length = 15 },
			{ .string = "listIndexes", .length = 11 }
		}
	},
	{
		.db = "",
		.collection = "",
		.isCluster = true,
		.numActions = 1,
		.actions = (const StringView[]) {
			{ .string = "listDatabases", .length = 13 }
		}
	}
};

static const UserPrivilege readWritePrivileges[] = {
	{
		.db = "",
		.collection = "",
		.isCluster = false,
		.numActions = 14,
		.actions = (const StringView[]) {
			{ .string = "changeStream", .length = 12 },
			{ .string = "collStats", .length = 9 },
			{ .string = "createCollection", .length = 16 },
			{ .string = "createIndex", .length = 11 },
			{ .string = "dbStats", .length = 7 },
			{ .string = "dropCollection", .length = 14 },
			{ .string = "dropIndex", .length = 9 },
			{ .string = "find", .length = 4 },
			{ .string = "insert", .length = 6 },
			{ .string = "killCursors", .length = 11 },
			{ .string = "listCollections", .length = 15 },
			{ .string = "listIndexes", .length = 11 },
			{ .string = "remove", .length = 6 },
			{ .string = "update", .length = 6 }
		}
	},
	{
		.db = "",
		.collection = "",
		.isCluster = true,
		.numActions = 1,
		.actions = (const StringView[]) {
			{ .string = "listDatabases", .length = 13 }
		}
	}
};


static const UserPrivilege dbAdminPrivileges[] = {
	{
		.db = "admin",
		.collection = "",
		.isCluster = false,
		.numActions = 15,
		.actions = (const StringView[]) {
			{ .string = "analyze", .length = 7 },
			{ .string = "bypassDocumentValidation", .length = 24 },
			{ .string = "collMod", .length = 7 },
			{ .string = "collStats", .length = 9 },
			{ .string = "compact", .length = 7 },
			{ .string = "createCollection", .length = 16 },
			{ .string = "createIndex", .length = 11 },
			{ .string = "dbStats", .length = 7 },
			{ .string = "dropCollection", .length = 14 },
			{ .string = "dropDatabase", .length = 12 },
			{ .string = "dropIndex", .length = 9 },
			{ .string = "listCollections", .length = 15 },
			{ .string = "listIndexes", .length = 11 },
			{ .string = "reIndex", .length = 7 },
			{ .string = "validate", .length = 8 }
		}
	}
};

static const UserPrivilege userAdminPrivileges[] = {
	{
		.db = "admin",
		.collection = "",
		.isCluster = false,
		.numActions = 8,
		.actions = (const StringView[]) {
			{ .string = "createRole", .length = 11 },
			{ .string = "createUser", .length = 11 },
			{ .string = "dropRole", .length = 8 },
			{ .string = "dropUser", .length = 8 },
			{ .string = "grantRole", .length = 9 },
			{ .string = "revokeRole", .length = 10 },
			{ .string = "viewRole", .length = 8 },
			{ .string = "viewUser", .length = 8 }
		}
	}
};

static const UserPrivilege clusterMonitorPrivileges[] = {
	{
		.db = "",
		.collection = "",
		.isCluster = true,
		.numActions = 11,
		.actions = (const StringView[]) {
			{ .string = "connPoolStats", .length = 13 },
			{ .string = "getDefaultRWConcern", .length = 19 },
			{ .string = "getCmdLineOpts", .length = 14 },
			{ .string = "getLog", .length = 6 },
			{ .string = "getParameter", .length = 12 },
			{ .string = "getShardMap", .length = 11 },
			{ .string = "hostInfo", .length = 8 },
			{ .string = "listDatabases", .length = 13 },
			{ .string = "listSessions", .length = 12 },
			{ .string = "listShards", .length = 10 },
			{ .string = "serverStatus", .length = 12 }
		}
	},
	{
		.db = "",
		.collection = "",
		.isCluster = false,
		.numActions = 5,
		.actions = (const StringView[]) {
			{ .string = "collStats", .length = 9 },
			{ .string = "dbStats", .length = 7 },
			{ .string = "getDatabaseVersion", .length = 18 },
			{ .string = "getShardVersion", .length = 15 },
			{ .string = "indexStats", .length = 10 }
		}
	}
};

static const UserPrivilege clusterManagerPrivileges[] = {
	{
		.db = "",
		.collection = "",
		.isCluster = true,
		.numActions = 6,
		.actions = (const StringView[]) {
			{ .string = "getClusterParameter", .length = 19 },
			{ .string = "getDefaultRWConcern", .length = 19 },
			{ .string = "listSessions", .length = 12 },
			{ .string = "listShards", .length = 10 },
			{ .string = "setChangeStreamState", .length = 20 },
			{ .string = "getChangeStreamState", .length = 20 }
		}
	},
	{
		.db = "",
		.collection = "",
		.isCluster = false,
		.numActions = 5,
		.actions = (const StringView[]) {
			{ .string = "analyzeShardKey", .length = 15 },
			{ .string = "enableSharding", .length = 14 },
			{ .string = "reshardCollection", .length = 17 },
			{ .string = "splitVector", .length = 11 },
			{ .string = "unshardCollection", .length = 17 }
		}
	},
};

static const UserPrivilege hostManagerPrivileges[] = {
	{
		.db = "",
		.collection = "",
		.isCluster = true,
		.numActions = 5,
		.actions = (const StringView[]) {
			{ .string = "compact", .length = 7 },
			{ .string = "dropConnections", .length = 15 },
			{ .string = "killAnyCursor", .length = 13 },
			{ .string = "killAnySession", .length = 14 },
			{ .string = "killop", .length = 6 }
		}
	},
	{
		.db = "",
		.collection = "",
		.isCluster = false,
		.numActions = 1,
		.actions = (const StringView[]) {
			{ .string = "killCursors", .length = 11 }
		}
	}
};

static const UserPrivilege dropDatabasePrivileges[] = {
	{
		.db = "",
		.collection = "",
		.isCluster = false,
		.numActions = 1,
		.actions = (const StringView[]) {
			{ .string = "dropDatabase", .length = 12 }
		}
	}
};

/*
 * Parses a connectionStatus spec, executes the connectionStatus command, and returns the result.
 */
Datum
command_connection_status(PG_FUNCTION_ARGS)
{
	pgbson *connectionStatusSpec = PG_GETARG_PGBSON(0);

	Datum response = connection_status(connectionStatusSpec);

	PG_RETURN_DATUM(response);
}


/*
 * documentdb_extension_create_user implements the
 * core logic to create a user
 */
Datum
documentdb_extension_create_user(PG_FUNCTION_ARGS)
{
	if (!EnableUserCrud)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_COMMANDNOTSUPPORTED),
						errmsg("CreateUser command is not supported."),
						errdetail_log("CreateUser command is not supported.")));
	}

	if (PG_ARGISNULL(0))
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
						errmsg(
							"'createUser', 'pwd' and 'roles' fields must be specified.")));
	}

	if (!IsMetadataCoordinator())
	{
		StringInfo createUserQuery = makeStringInfo();
		appendStringInfo(createUserQuery,
						 "SELECT %s.create_user(%s::%s.bson)",
						 ApiSchemaNameV2,
						 quote_literal_cstr(PgbsonToHexadecimalString(PG_GETARG_PGBSON(
																		  0))),
						 CoreSchemaNameV2);
		DistributedRunCommandResult result = RunCommandOnMetadataCoordinator(
			createUserQuery->data);

		if (!result.success)
		{
			ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_INTERNALERROR),
							errmsg(
								"Internal error creating user in metadata coordinator %s",
								text_to_cstring(result.response)),
							errdetail_log(
								"Internal error creating user in metadata coordinator via distributed call %s",
								text_to_cstring(result.response))));
		}

		pgbson_writer finalWriter;
		PgbsonWriterInit(&finalWriter);
		PgbsonWriterAppendInt32(&finalWriter, "ok", 2, 1);
		PG_RETURN_POINTER(PgbsonWriterGetPgbson(&finalWriter));
	}

	/*Verify that we have not yet hit the limit of users allowed */
	const char *cmdStr = FormatSqlQuery(
		"SELECT COUNT(*) "
		"FROM pg_roles parent "
		"JOIN pg_auth_members am ON parent.oid = am.roleid "
		"JOIN pg_roles child ON am.member = child.oid "
		"WHERE child.rolcanlogin = true "
		"  AND parent.rolname IN ('%s', '%s') "
		"  AND child.rolname NOT IN ('%s', '%s', '%s');",
		ApiAdminRoleV2, ApiReadOnlyRole,
		ApiAdminRoleV2, ApiReadOnlyRole, ApiBgWorkerRole);

	bool readOnly = true;
	bool isNull = false;
	Datum userCountDatum = ExtensionExecuteQueryViaSPI(cmdStr, readOnly,
													   SPI_OK_SELECT, &isNull);
	int userCount = 0;

	if (!isNull)
	{
		userCount = DatumGetInt32(userCountDatum);
	}
	else
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_INTERNALERROR),
						errmsg("Failed to get current user count.")));
	}

	if (userCount >= MaxUserLimit)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_USERCOUNTLIMITEXCEEDED),
						errmsg("Exceeded the limit of %d user roles.", MaxUserLimit)));
	}

	pgbson *createUserBson = PG_GETARG_PGBSON(0);
	CreateUserSpec createUserSpec = { 0 };
	ParseCreateUserSpec(createUserBson, &createUserSpec);

	if (createUserSpec.has_identity_provider)
	{
		if (!CreateUserWithExternalIdentityProvider(createUserSpec.createUser,
													createUserSpec.pgRole,
													createUserSpec.identityProviderData))
		{
			pgbson_writer finalWriter;
			PgbsonWriterInit(&finalWriter);
			PgbsonWriterAppendInt32(&finalWriter, "ok", 2, 0);
			PgbsonWriterAppendUtf8(&finalWriter, "errmsg", 6,
								   "External identity providers are currently unsupported");
			PgbsonWriterAppendInt32(&finalWriter, "code", 4, 115);
			PgbsonWriterAppendUtf8(&finalWriter, "codeName", 8,
								   "CommandNotSupported");
			PG_RETURN_POINTER(PgbsonWriterGetPgbson(&finalWriter));
		}
	}
	else
	{
		CreateNativeUser(&createUserSpec);
	}

	/* Grant pgRole to user created */
	readOnly = false;
	const char *queryGrant = psprintf("GRANT %s TO %s",
									  quote_identifier(createUserSpec.pgRole),
									  quote_identifier(createUserSpec.createUser));

	ExtensionExecuteQueryViaSPI(queryGrant, readOnly, SPI_OK_UTILITY, &isNull);

	if (strcmp(createUserSpec.pgRole, ApiReadOnlyRole) == 0)
	{
		/* This is needed to grant ApiReadOnlyRole */
		/* read access to all new and existing collections */
		StringInfo grantReadOnlyPermissions = makeStringInfo();
		resetStringInfo(grantReadOnlyPermissions);
		appendStringInfo(grantReadOnlyPermissions,
						 "GRANT pg_read_all_data TO %s",
						 quote_identifier(createUserSpec.createUser));
		readOnly = false;
		isNull = false;
		ExtensionExecuteQueryViaSPI(grantReadOnlyPermissions->data, readOnly,
									SPI_OK_UTILITY,
									&isNull);
	}

	pgbson_writer finalWriter;
	PgbsonWriterInit(&finalWriter);
	PgbsonWriterAppendInt32(&finalWriter, "ok", 2, 1);
	PG_RETURN_POINTER(PgbsonWriterGetPgbson(&finalWriter));
}


/*
 * ParseCreateUserSpec parses the wire
 * protocol message createUser() which creates a user
 */
static void
ParseCreateUserSpec(pgbson *createSpec, CreateUserSpec *spec)
{
	bson_iter_t createIter;
	PgbsonInitIterator(createSpec, &createIter);

	bool has_user = false;
	bool has_pwd = false;
	bool has_roles = false;

	while (bson_iter_next(&createIter))
	{
		const char *key = bson_iter_key(&createIter);
		if (strcmp(key, "createUser") == 0)
		{
			EnsureTopLevelFieldType(key, &createIter, BSON_TYPE_UTF8);
			uint32_t strLength = 0;
			spec->createUser = bson_iter_utf8(&createIter, &strLength);
			if (strLength == 0)
			{
				ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
								errmsg(
									"'createUser' is a required field.")));
			}

			if (IsUserNameInvalid(spec->createUser))
			{
				ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
								errmsg("Invalid username, use a different username.")));
			}

			has_user = true;
		}
		else if (strcmp(key, "pwd") == 0)
		{
			EnsureTopLevelFieldType(key, &createIter, BSON_TYPE_UTF8);
			uint32_t strLength = 0;
			spec->pwd = bson_iter_utf8(&createIter, &strLength);
			if (strLength == 0)
			{
				ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
								errmsg(
									"The password field must not be left empty.")));
			}

			has_pwd = true;
		}
		else if (strcmp(key, "roles") == 0)
		{
			if (!BSON_ITER_HOLDS_ARRAY(&createIter))
			{
				ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
								errmsg(
									"'roles' field must be an array")));
			}

			spec->roles = *bson_iter_value(&createIter);

			if (IsBsonValueEmptyDocument(&spec->roles))
			{
				ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
								errmsg(
									"'roles' is a required field.")));
			}

			/* Check if it's in the right format */
			spec->pgRole = ValidateAndObtainUserRole(&spec->roles);
			has_roles = true;
		}
		else if (strcmp(key, "customData") == 0)
		{
			const bson_value_t *customDataDocument = bson_iter_value(&createIter);
			if (customDataDocument->value_type != BSON_TYPE_DOCUMENT)
			{
				ereport(ERROR,
						(errcode(ERRCODE_DOCUMENTDB_BADVALUE),
						 errmsg(
							 "The 'customData' parameter is required to be provided as a BSON document.")));
			}

			if (!IsBsonValueEmptyDocument(customDataDocument))
			{
				bson_iter_t customDataIterator;
				BsonValueInitIterator(customDataDocument, &customDataIterator);
				while (bson_iter_next(&customDataIterator))
				{
					const char *customDataKey = bson_iter_key(&customDataIterator);

					if (strcmp(customDataKey, "IdentityProvider") == 0)
					{
						spec->identityProviderData = *bson_iter_value(
							&customDataIterator);
						spec->has_identity_provider = true;
					}
					else
					{
						ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
										errmsg(
											"The specified field in the custom data is not supported: '%s'.",
											customDataKey)));
					}
				}
			}
		}
		else if (!IsCommonSpecIgnoredField(key))
		{
			ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
							errmsg("Unsupported field specified : '%s'.", key)));
		}
	}

	if (spec->has_identity_provider)
	{
		if (!has_user || !has_roles)
		{
			ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE), errmsg(
								"'createUser' and 'roles' are required fields.")));
		}

		if (has_pwd)
		{
			ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE), errmsg(
								"Password is not allowed when using an external identity provider.")));
		}
	}
	else
	{
		if (!has_user || !has_roles || !has_pwd)
		{
			ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE), errmsg(
								"'createUser', 'roles' and 'pwd' are required fields.")));
		}

		if (IsPasswordInvalid(spec->createUser, spec->pwd))
		{
			ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
							errmsg("Invalid password, use a different password.")));
		}
	}
}


/*
 *  At the moment we only allow ApiAdminRole and ApiReadOnlyRole
 *  1. ApiAdminRole corresponds to
 *      roles: [
 *          { role: "clusterAdmin", db: "admin" },
 *          { role: "readWriteAnyDatabase", db: "admin" }
 *      ]
 *
 *  2. ApiReadOnlyRole corresponds to
 *      roles: [
 *          { role: "readAnyDatabase", db: "admin" }
 *      ]
 *
 *  Reject all other combinations.
 */
char *
ValidateAndObtainUserRole(const bson_value_t *rolesDocument)
{
	bson_iter_t rolesIterator;
	BsonValueInitIterator(rolesDocument, &rolesIterator);
	int userRoles = 0;

	while (bson_iter_next(&rolesIterator))
	{
		bson_iter_t roleIterator;

		BsonValueInitIterator(bson_iter_value(&rolesIterator), &roleIterator);
		while (bson_iter_next(&roleIterator))
		{
			const char *key = bson_iter_key(&roleIterator);

			if (strcmp(key, "role") == 0)
			{
				EnsureTopLevelFieldType(key, &roleIterator, BSON_TYPE_UTF8);
				uint32_t strLength = 0;
				const char *role = bson_iter_utf8(&roleIterator, &strLength);
				if (strcmp(role, "readAnyDatabase") == 0)
				{
					/*This would indicate the ApiReadOnlyRole provided the db is "admin" */
					userRoles |= DocumentDB_Role_Read_AnyDatabase;
				}
				else if (strcmp(role, "readWriteAnyDatabase") == 0)
				{
					/*This would indicate the ApiAdminRole provided the db is "admin" and there is another role "clusterAdmin" */
					userRoles |= DocumentDB_Role_ReadWrite_AnyDatabase;
				}
				else if (strcmp(role, "clusterAdmin") == 0)
				{
					/*This would indicate the ApiAdminRole provided the db is "admin" and there is another role "readWriteAnyDatabase" */
					userRoles |= DocumentDB_Role_Cluster_Admin;
				}
				else
				{
					ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_ROLENOTFOUND),
									errmsg(
										"The specified value for the role is invalid: '%s'.",
										role),
									errdetail_log(
										"The specified value for the role is invalid: '%s'.",
										role)));
				}
			}
			else if (strcmp(key, "db") == 0 || strcmp(key, "$db") == 0)
			{
				EnsureTopLevelFieldType(key, &roleIterator, BSON_TYPE_UTF8);
				uint32_t strLength = 0;
				const char *db = bson_iter_utf8(&roleIterator, &strLength);
				if (strcmp(db, "admin") != 0)
				{
					ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE), errmsg(
										"Unsupported value specified for db. Only 'admin' is allowed.")));
				}
			}
			else
			{
				ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
								errmsg("Unsupported field specified: '%s'.",
									   key),
								errdetail_log(
									"Unsupported field specified: '%s'.",
									key)));
			}
		}
	}

	if ((userRoles & DocumentDB_Role_ReadWrite_AnyDatabase) != 0 &&
		(userRoles & DocumentDB_Role_Cluster_Admin) != 0)
	{
		return ApiAdminRoleV2;
	}

	if ((userRoles & DocumentDB_Role_Read_AnyDatabase) != 0)
	{
		return ApiReadOnlyRole;
	}

	ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_ROLENOTFOUND),
					errmsg(
						"Roles specified are invalid. Only [{role: \"readAnyDatabase\", db: \"admin\"}] or [{role: \"clusterAdmin\", db: \"admin\"}, {role: \"readWriteAnyDatabase\", db: \"admin\"}] are allowed."),
					errdetail_log(
						"Roles specified are invalid. Only [{role: \"readAnyDatabase\", db: \"admin\"}] or [{role: \"clusterAdmin\", db: \"admin\"}, {role: \"readWriteAnyDatabase\", db: \"admin\"}] are allowed.")));
}


/*
 * CreateNativeUser creates a native PostgreSQL role for the user
 */
static void
CreateNativeUser(const CreateUserSpec *createUserSpec)
{
	/*Verify that native authentication is enabled*/
	if (!IsNativeAuthEnabled)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_COMMANDNOTSUPPORTED),
						errmsg(
							"Native authentication is not enabled. Enable native authentication on this cluster to perform native user management operations.")));
	}

	ReportFeatureUsage(FEATURE_USER_CREATE);

	/*Verify that the calling user is also native*/
	if (IsCallingUserExternal())
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_INSUFFICIENTPRIVILEGE),
						errmsg(
							"Only native users can create other native users. Authenticate as a built-in native administrative user to perform native user management operations.")));
	}

	StringInfo createUserInfo = makeStringInfo();
	appendStringInfo(createUserInfo,
					 "CREATE ROLE %s WITH LOGIN PASSWORD %s;",
					 quote_identifier(createUserSpec->createUser),
					 quote_literal_cstr(PrehashPassword(createUserSpec->pwd)));

	bool readOnly = false;
	bool isNull = false;
	ExtensionExecuteQueryViaSPI(createUserInfo->data, readOnly, SPI_OK_UTILITY,
								&isNull);
}


/*
 * documentdb_extension_drop_user implements the
 * core logic to drop a user
 */
Datum
documentdb_extension_drop_user(PG_FUNCTION_ARGS)
{
	if (!EnableUserCrud)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_COMMANDNOTSUPPORTED),
						errmsg("DropUser command is not supported."),
						errdetail_log("DropUser command is not supported.")));
	}

	if (PG_ARGISNULL(0))
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
						errmsg("The field 'dropUser' is mandatory.")));
	}

	if (!IsMetadataCoordinator())
	{
		StringInfo dropUserQuery = makeStringInfo();
		appendStringInfo(dropUserQuery,
						 "SELECT %s.drop_user(%s::%s.bson)",
						 ApiSchemaNameV2,
						 quote_literal_cstr(PgbsonToHexadecimalString(PG_GETARG_PGBSON(
																		  0))),
						 CoreSchemaNameV2);
		DistributedRunCommandResult result = RunCommandOnMetadataCoordinator(
			dropUserQuery->data);

		if (!result.success)
		{
			ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_INTERNALERROR),
							errmsg(
								"Internal error dropping user in metadata coordinator %s",
								text_to_cstring(result.response)),
							errdetail_log(
								"Internal error dropping user in metadata coordinator via distributed call %s",
								text_to_cstring(result.response))));
		}

		pgbson_writer finalWriter;
		PgbsonWriterInit(&finalWriter);
		PgbsonWriterAppendInt32(&finalWriter, "ok", 2, 1);
		PG_RETURN_POINTER(PgbsonWriterGetPgbson(&finalWriter));
	}

	pgbson *dropUserSpec = PG_GETARG_PGBSON(0);
	char *dropUser = ParseDropUserSpec(dropUserSpec);

	if (IsUserExternal(dropUser))
	{
		if (!DropUserWithExternalIdentityProvider(dropUser))
		{
			pgbson_writer finalWriter;
			PgbsonWriterInit(&finalWriter);
			PgbsonWriterAppendInt32(&finalWriter, "ok", 2, 0);
			PgbsonWriterAppendUtf8(&finalWriter, "errmsg", 6,
								   "External identity providers are currently unsupported");
			PgbsonWriterAppendInt32(&finalWriter, "code", 4, 115);
			PgbsonWriterAppendUtf8(&finalWriter, "codeName", 8,
								   "CommandNotSupported");
			PG_RETURN_POINTER(PgbsonWriterGetPgbson(&finalWriter));
		}
	}
	else
	{
		DropNativeUser(dropUser);
	}

	pgbson_writer finalWriter;
	PgbsonWriterInit(&finalWriter);
	PgbsonWriterAppendInt32(&finalWriter, "ok", 2, 1);
	PG_RETURN_POINTER(PgbsonWriterGetPgbson(&finalWriter));
}


/*
 * ParseDropUserSpec parses the wire
 * protocol message dropUser() which drops a user
 */
static char *
ParseDropUserSpec(pgbson *dropSpec)
{
	bson_iter_t dropIter;
	PgbsonInitIterator(dropSpec, &dropIter);

	char *dropUser = NULL;
	while (bson_iter_next(&dropIter))
	{
		const char *key = bson_iter_key(&dropIter);
		if (strcmp(key, "dropUser") == 0)
		{
			EnsureTopLevelFieldType(key, &dropIter, BSON_TYPE_UTF8);
			uint32_t strLength = 0;
			dropUser = (char *) bson_iter_utf8(&dropIter, &strLength);
			if (strLength == 0)
			{
				ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
								errmsg(
									"The field 'dropUser' is mandatory.")));
			}

			if (IsUserNameInvalid(dropUser))
			{
				ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
								errmsg("Invalid username.")));
			}
		}
		else if (strcmp(key, "lsid") == 0 || strcmp(key, "$db") == 0)
		{
			continue;
		}
		else
		{
			ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
							errmsg("Unsupported field specified: '%s'.", key)));
		}
	}

	if (dropUser == NULL)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
						errmsg("The field 'dropUser' is mandatory.")));
	}

	return dropUser;
}


/*
 * DropNativeUser drops a native PostgreSQL role for the user
 */
static void
DropNativeUser(const char *dropUser)
{
	/*Verify that native authentication is enabled*/
	if (!IsNativeAuthEnabled)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_COMMANDNOTSUPPORTED),
						errmsg(
							"Native authentication is not enabled. Enable native authentication on this cluster to perform native user management operations.")));
	}

	ReportFeatureUsage(FEATURE_USER_DROP);

	/*Verify that the calling user is also native*/
	if (IsCallingUserExternal())
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_INSUFFICIENTPRIVILEGE),
						errmsg(
							"Only native users can create other native users. Authenticate as a built-in native administrative user to perform native user management operations.")));
	}

	StringInfo dropUserInfo = makeStringInfo();
	appendStringInfo(dropUserInfo, "DROP ROLE %s;", quote_identifier(dropUser));

	bool readOnly = false;
	bool isNull = false;
	ExtensionExecuteQueryViaSPI(dropUserInfo->data, readOnly, SPI_OK_UTILITY,
								&isNull);
}


/*
 * documentdb_extension_update_user implements the core logic to update a user.
 * In Mongo community edition a user with userAdmin privileges or root privileges can change
 * other users passwords. In postgres a superuser can change any users password.
 * A user with CreateRole privileges can change pwds of roles they created. Given
 * that ApiAdminRole has neither create role nor superuser privileges in our case
 * a user can only change their own pwd and no one elses.
 */
Datum
documentdb_extension_update_user(PG_FUNCTION_ARGS)
{
	if (!EnableUserCrud)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_COMMANDNOTSUPPORTED),
						errmsg("The UpdateUser command is currently unsupported."),
						errdetail_log(
							"The UpdateUser command is currently unsupported.")));
	}

	if (PG_ARGISNULL(0))
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
						errmsg("'updateUser' and 'pwd' are required fields.")));
	}

	if (!IsMetadataCoordinator())
	{
		StringInfo updateUserQuery = makeStringInfo();
		appendStringInfo(updateUserQuery,
						 "SELECT %s.update_user(%s::%s.bson)",
						 ApiSchemaNameV2,
						 quote_literal_cstr(PgbsonToHexadecimalString(PG_GETARG_PGBSON(
																		  0))),
						 CoreSchemaNameV2);
		DistributedRunCommandResult result = RunCommandOnMetadataCoordinator(
			updateUserQuery->data);

		if (!result.success)
		{
			ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_INTERNALERROR),
							errmsg(
								"Internal error updating user in metadata coordinator %s",
								text_to_cstring(result.response)),
							errdetail_log(
								"Internal error updating user in metadata coordinator via distributed call %s",
								text_to_cstring(result.response))));
		}

		pgbson_writer finalWriter;
		PgbsonWriterInit(&finalWriter);
		PgbsonWriterAppendInt32(&finalWriter, "ok", 2, 1);
		PG_RETURN_POINTER(PgbsonWriterGetPgbson(&finalWriter));
	}

	pgbson *updateUserSpec = PG_GETARG_PGBSON(0);
	UpdateUserSpec spec = { 0 };
	ParseUpdateUserSpec(updateUserSpec, &spec);

	if (IsUserExternal(spec.updateUser))
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_COMMANDNOTSUPPORTED),
						errmsg(
							"UpdateUser command is not supported for a non-native user.")));
	}
	else
	{
		return UpdateNativeUser(&spec);
	}
}


/*
 * ParseUpdateUserSpec parses the wire
 * protocol message updateUser() which drops a user
 */
static void
ParseUpdateUserSpec(pgbson *updateSpec, UpdateUserSpec *spec)
{
	bson_iter_t updateIter;
	PgbsonInitIterator(updateSpec, &updateIter);

	bool has_user = false;

	while (bson_iter_next(&updateIter))
	{
		const char *key = bson_iter_key(&updateIter);
		if (strcmp(key, "updateUser") == 0)
		{
			EnsureTopLevelFieldType(key, &updateIter, BSON_TYPE_UTF8);
			uint32_t strLength = 0;
			spec->updateUser = bson_iter_utf8(&updateIter, &strLength);
			if (strLength == 0)
			{
				ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
								errmsg(
									"'updateUser' is a required field.")));
			}

			has_user = true;
		}
		else if (strcmp(key, "pwd") == 0)
		{
			EnsureTopLevelFieldType(key, &updateIter, BSON_TYPE_UTF8);
			uint32_t strLength = 0;
			spec->pwd = bson_iter_utf8(&updateIter, &strLength);
		}
		else if (strcmp(key, "lsid") == 0 || strcmp(key, "$db") == 0)
		{
			continue;
		}
		else if (strcmp(key, "roles") == 0)
		{
			ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
							errmsg("Updating roles is not supported.")));
		}
		else
		{
			ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
							errmsg("Unsupported field specified : '%s'.", key)));
		}
	}

	if (!has_user)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
						errmsg("'updateUser' is a required field.")));
	}
}


/*
 * Update native user
 */
static Datum
UpdateNativeUser(UpdateUserSpec *spec)
{
	/*Verify that native authentication is enabled*/
	if (!IsNativeAuthEnabled)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_COMMANDNOTSUPPORTED),
						errmsg(
							"Native authentication is not enabled. Enable native authentication on this cluster to perform native user management operations.")));
	}

	ReportFeatureUsage(FEATURE_USER_UPDATE);

	/*Verify that the calling user is also native*/
	if (IsCallingUserExternal())
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_INSUFFICIENTPRIVILEGE),
						errmsg(
							"Only native users can create other native users. Authenticate as a built-in native administrative user to perform native user management operations.")));
	}

	if (spec->pwd == NULL || spec->pwd[0] == '\0')
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
						errmsg("The password field must not be left empty.")));
	}

	/* Verify password meets complexity requirements */
	if (IsPasswordInvalid(spec->updateUser, spec->pwd))
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
						errmsg("Invalid password, use a different password.")));
	}

	StringInfo updateUserInfo = makeStringInfo();
	appendStringInfo(updateUserInfo, "ALTER USER %s WITH PASSWORD %s;", quote_identifier(
						 spec->updateUser), quote_literal_cstr(PrehashPassword(
																   spec->pwd)));

	bool readOnly = false;
	bool isNull = false;
	ExtensionExecuteQueryViaSPI(updateUserInfo->data, readOnly, SPI_OK_UTILITY,
								&isNull);

	pgbson_writer finalWriter;
	PgbsonWriterInit(&finalWriter);
	PgbsonWriterAppendInt32(&finalWriter, "ok", 2, 1);
	PG_RETURN_POINTER(PgbsonWriterGetPgbson(&finalWriter));
}


/*
 * documentdb_extension_get_users implements the
 * core logic to get user info
 */
Datum
documentdb_extension_get_users(PG_FUNCTION_ARGS)
{
	if (!EnableUserCrud)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_COMMANDNOTSUPPORTED),
						errmsg("UsersInfo command is not supported."),
						errdetail_log("UsersInfo command is not supported.")));
	}

	ReportFeatureUsage(FEATURE_USER_GET);

	if (PG_ARGISNULL(0))
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
						errmsg("'usersInfo' or 'forAllDBs' must be provided.")));
	}

	GetUserSpec userSpec = { 0 };
	ParseGetUserSpec(PG_GETARG_PGBSON(0), &userSpec);
	const char *userName = userSpec.user.length > 0 ? userSpec.user.string : NULL;
	const bool showPrivileges = userSpec.showPrivileges;
	Datum userInfoDatum;

	if (userName == NULL)
	{
		userInfoDatum = GetAllUsersInfo();
	}
	else
	{
		bool returnDocuments = true;
		userInfoDatum = GetSingleUserInfo(userName, returnDocuments);
	}

	pgbson_writer finalWriter;
	PgbsonWriterInit(&finalWriter);

	if (userInfoDatum == (Datum) 0)
	{
		PgbsonWriterAppendInt32(&finalWriter, "ok", 2, 1);
		pgbson *result = PgbsonWriterGetPgbson(&finalWriter);
		PG_RETURN_POINTER(result);
	}

	ArrayType *userArray = DatumGetArrayTypeP(userInfoDatum);
	Datum *userDatums;
	bool *userIsNullMarker;
	int userCount;

	bool arrayByVal = false;
	int elementLength = -1;
	Oid arrayElementType = ARR_ELEMTYPE(userArray);
	deconstruct_array(userArray,
					  arrayElementType, elementLength, arrayByVal,
					  TYPALIGN_INT, &userDatums, &userIsNullMarker,
					  &userCount);

	HTAB *userRolesTable = BuildUserRoleEntryTable(userDatums, userCount);

	pgbson_array_writer userArrayWriter;
	PgbsonWriterStartArray(&finalWriter, "users", 5, &userArrayWriter);

	HASH_SEQ_STATUS userStatus;
	UserRoleHashEntry *userEntry;

	hash_seq_init(&userStatus, userRolesTable);
	while ((userEntry = hash_seq_search(&userStatus)) != NULL)
	{
		WriteSingleUserDocument(userEntry, showPrivileges, &userArrayWriter);
	}
	PgbsonWriterEndArray(&finalWriter, &userArrayWriter);
	PgbsonWriterAppendInt32(&finalWriter, "ok", 2, 1);
	pgbson *result = PgbsonWriterGetPgbson(&finalWriter);

	FreeUserRoleEntryTable(userRolesTable);

	PG_RETURN_POINTER(result);
}


/*
 * ParseGetUserSpec parses the wire
 * protocol message getUser() which gets user info
 */
static void
ParseGetUserSpec(pgbson *getSpec, GetUserSpec *spec)
{
	bson_iter_t getIter;
	PgbsonInitIterator(getSpec, &getIter);

	spec->user = (StringView) {
		0
	};
	spec->showPrivileges = false;
	bool requiredFieldFound = false;
	while (bson_iter_next(&getIter))
	{
		const char *key = bson_iter_key(&getIter);
		if (strcmp(key, "usersInfo") == 0)
		{
			requiredFieldFound = true;
			if (bson_iter_type(&getIter) == BSON_TYPE_INT32)
			{
				if (bson_iter_as_int64(&getIter) != 1)
				{
					ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
									errmsg("Unsupported value for 'usersInfo' field.")));
				}
			}
			else if (bson_iter_type(&getIter) == BSON_TYPE_UTF8)
			{
				uint32_t strLength = 0;
				const char *userString = bson_iter_utf8(&getIter, &strLength);
				spec->user = (StringView) {
					.string = userString,
					.length = strLength
				};
			}
			else if (BSON_ITER_HOLDS_DOCUMENT(&getIter))
			{
				const bson_value_t usersInfoBson = *bson_iter_value(&getIter);
				ParseUsersInfoDocument(&usersInfoBson, spec);
			}
			else
			{
				ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
								errmsg("Unsupported value specified for 'usersInfo'.")));
			}
		}
		else if (strcmp(key, "forAllDBs") == 0)
		{
			requiredFieldFound = true;
			if (bson_iter_type(&getIter) == BSON_TYPE_BOOL)
			{
				if (bson_iter_as_bool(&getIter) != true)
				{
					ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
									errmsg(
										"Unsupported value specified for 'forAllDBs'.")));
				}
			}
			else
			{
				ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
								errmsg("Unsupported value specified for 'forAllDBs'")));
			}
		}
		else if (strcmp(key, "getUser") == 0)
		{
			requiredFieldFound = true;
			EnsureTopLevelFieldType(key, &getIter, BSON_TYPE_UTF8);
			uint32_t strLength = 0;
			const char *userString = bson_iter_utf8(&getIter, &strLength);
			spec->user = (StringView) {
				.string = userString,
				.length = strLength
			};
		}
		else if (strcmp(key, "showPrivileges") == 0)
		{
			if (BSON_ITER_HOLDS_BOOL(&getIter))
			{
				spec->showPrivileges = bson_iter_as_bool(&getIter);
			}
			else
			{
				ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
								errmsg(
									"'showPrivileges' must be a boolean value")));
			}
		}
		else if (strcmp(key, "lsid") == 0 || strcmp(key, "$db") == 0)
		{
			continue;
		}
		else
		{
			ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
							errmsg("Unsupported field specified: '%s'.", key)));
		}
	}

	if (!requiredFieldFound)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE), errmsg(
							"'usersInfo' or 'forAllDBs' must be provided.")));
	}
}


/*
 * connection_status implements the
 * core logic for connectionStatus command
 */
Datum
connection_status(pgbson *showPrivilegesSpec)
{
	ReportFeatureUsage(FEATURE_CONNECTION_STATUS);

	bool showPrivileges = false;
	if (showPrivilegesSpec != NULL)
	{
		showPrivileges = ParseConnectionStatusSpec(showPrivilegesSpec);
	}

	bool noError = true;
	const char *currentUser = GetUserNameFromId(GetUserId(), noError);

	bool returnDocuments = false;
	Datum userInfoDatum = GetSingleUserInfo(currentUser, returnDocuments);

	const char *parentRole = text_to_cstring(DatumGetTextP(userInfoDatum));
	if (parentRole == NULL)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_INTERNALERROR),
						errmsg(
							"Unable to locate appropriate role for the specified user")));
	}

	/*
	 * Example output structure:
	 * {
	 *   authInfo: {
	 *     authenticatedUsers: [ { user: ..., db: ... } ], // always 1 element
	 *     authenticatedUserRoles: [ { role: ..., db: ... }, ... ],
	 *     authenticatedUserPrivileges: [ { privilege }, ... ] // if showPrivileges
	 *   },
	 *   ok: 1
	 * }
	 *
	 * privilege: { resource: { db:, collection: }, actions: [...] }
	 */
	pgbson_writer finalWriter;
	PgbsonWriterInit(&finalWriter);
	pgbson_writer authInfoWriter;
	PgbsonWriterStartDocument(&finalWriter, "authInfo", 8,
							  &authInfoWriter);

	pgbson_array_writer usersArrayWriter;
	PgbsonWriterStartArray(&authInfoWriter, "authenticatedUsers", 18, &usersArrayWriter);
	pgbson_writer userWriter;
	PgbsonArrayWriterStartDocument(&usersArrayWriter, &userWriter);
	PgbsonWriterAppendUtf8(&userWriter, "user", 4, currentUser);
	PgbsonWriterAppendUtf8(&userWriter, "db", 2, "admin");
	PgbsonArrayWriterEndDocument(&usersArrayWriter, &userWriter);
	PgbsonWriterEndArray(&authInfoWriter, &usersArrayWriter);

	pgbson_array_writer roleArrayWriter;
	PgbsonWriterStartArray(&authInfoWriter, "authenticatedUserRoles", 22,
						   &roleArrayWriter);
	WriteRoles(parentRole, &roleArrayWriter);
	PgbsonWriterEndArray(&authInfoWriter, &roleArrayWriter);

	if (showPrivileges)
	{
		pgbson_array_writer privilegesArrayWriter;
		PgbsonWriterStartArray(&authInfoWriter, "authenticatedUserPrivileges", 27,
							   &privilegesArrayWriter);
		WriteSingleRolePrivileges(parentRole, &privilegesArrayWriter);
		PgbsonWriterEndArray(&authInfoWriter, &privilegesArrayWriter);
	}

	PgbsonWriterEndDocument(&finalWriter, &authInfoWriter);

	PgbsonWriterAppendInt32(&finalWriter, "ok", 2, 1);
	pgbson *result = PgbsonWriterGetPgbson(&finalWriter);
	return PointerGetDatum(result);
}


/*
 * ParseConnectionStatusSpec parses the connectionStatus command parameters
 * validates the parameters and returns the boolean flag of whether to show privileges.
 */
static bool
ParseConnectionStatusSpec(pgbson *connectionStatusSpec)
{
	bson_iter_t connectionIter;
	PgbsonInitIterator(connectionStatusSpec, &connectionIter);

	bool showPrivileges = false;
	bool requiredFieldFound = false;
	while (bson_iter_next(&connectionIter))
	{
		const char *key = bson_iter_key(&connectionIter);

		if (strcmp(key, "connectionStatus") == 0)
		{
			requiredFieldFound = true;
			if (bson_iter_type(&connectionIter) == BSON_TYPE_INT32)
			{
				if (bson_iter_as_int64(&connectionIter) != 1)
				{
					ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
									errmsg(
										"Unsupported value for 'connectionStatus' field.")));
				}
			}
			else
			{
				ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
								errmsg(
									"'connectionStatus' must be an integer value")));
			}
		}
		else if (strcmp(key, "showPrivileges") == 0)
		{
			if (BSON_ITER_HOLDS_BOOL(&connectionIter))
			{
				showPrivileges = bson_iter_as_bool(&connectionIter);
			}
			else
			{
				ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
								errmsg("'showPrivileges' must be a boolean value")));
			}
		}
		else if (strcmp(key, "lsid") == 0 || strcmp(key, "$db") == 0)
		{
			continue;
		}
		else
		{
			ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
							errmsg("Unsupported field specified: '%s'.", key)));
		}
	}

	if (!requiredFieldFound)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE), errmsg(
							"'connectionStatus' must be provided.")));
	}

	return showPrivileges;
}


/*
 * This method is mostly copied from pg_be_scram_build_secret in PG. The only substantial change
 * is that we use a default salt length of 28 as opposed to 16 used by PG. This is to ensure
 * compatiblity with drivers that expect a salt length of 28.
 */
static char *
PrehashPassword(const char *password)
{
	char *prep_password;
	pg_saslprep_rc rc;
	char saltbuf[SCRAM_MAX_SALT_LEN];
	char *result;
	const char *errstr = NULL;

	/*
	 * Validate that the default salt length is not greater than the max salt length allowed
	 */
	if (ScramDefaultSaltLen > SCRAM_MAX_SALT_LEN)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
						errmsg("Salt length value is invalid.")));
	}

	/*
	 * Normalize the password with SASLprep.  If that doesn't work, because
	 * the password isn't valid UTF-8 or contains prohibited characters, just
	 * proceed with the original password.  (See comments at top of file.)
	 */
	rc = pg_saslprep(password, &prep_password);
	if (rc == SASLPREP_SUCCESS)
	{
		password = (const char *) prep_password;
	}

	/* Generate random salt */
	if (!pg_strong_random(saltbuf, ScramDefaultSaltLen))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("Could not generate random salt.")));
	}

#if PG_VERSION_NUM >= 160000  /* PostgreSQL 16.0 or higher */
	result = scram_build_secret(PG_SHA256, SCRAM_SHA_256_KEY_LEN,
								saltbuf, ScramDefaultSaltLen,
								scram_sha_256_iterations, password,
								&errstr);
#else
	result = scram_build_secret(saltbuf, ScramDefaultSaltLen,
								SCRAM_DEFAULT_ITERATIONS, password,
								&errstr);
#endif

	if (prep_password)
	{
		pfree(prep_password);
	}

	return result;
}


/*
 * Method calls the IsUsernameValid hook to validate the username.
 * This validation logic must be in sync with control plane username validation.
 */
bool
IsUserNameInvalid(const char *userName)
{
	/* Split the blocked role prefix list */
	char *blockedRolePrefixList = pstrdup(BlockedRolePrefixList);
	bool containsBlockedPrefix = false;
	char *token = strtok(blockedRolePrefixList, ",");
	while (token != NULL)
	{
		if (strncmp(userName, token, strlen(token)) == 0)
		{
			containsBlockedPrefix = true;
			break;
		}
		token = strtok(NULL, ",");
	}
	pfree(blockedRolePrefixList);


	bool is_valid = !containsBlockedPrefix;
	if (EnableUsernamePasswordConstraints)
	{
		is_valid = IsUsernameValid(userName) && is_valid;
	}
	return !is_valid;
}


/*
 * Method calls the IsPasswordValid hook to validate the password.
 * This validation logic must be in sync with control plane password validation.
 */
static bool
IsPasswordInvalid(const char *username, const char *password)
{
	bool is_valid = true;
	if (EnableUsernamePasswordConstraints)
	{
		is_valid = IsPasswordValid(username, password);
	}
	return !is_valid;
}


/*
 * Verify that the calling user is native
 */
static bool
IsCallingUserExternal()
{
	const char *currentUser = GetUserNameFromId(GetUserId(), true);
	return IsUserExternal(currentUser);
}


/*
 * WriteSingleUserDocument creates and writes a BSON document for a single user
 * to the provided array writer.
 */
static void
WriteSingleUserDocument(UserRoleHashEntry *userEntry, bool showPrivileges,
						pgbson_array_writer *userArrayWriter)
{
	pgbson_writer userWriter;
	PgbsonWriterInit(&userWriter);

	PgbsonWriterAppendUtf8(&userWriter, "_id", 3, psprintf(
							   "admin.%s",
							   userEntry->user));
	PgbsonWriterAppendUtf8(&userWriter, "userId", 6,
						   psprintf("admin.%s", userEntry->user));
	PgbsonWriterAppendUtf8(&userWriter, "user", 4, userEntry->user);
	PgbsonWriterAppendUtf8(&userWriter, "db", 2, "admin");

	pgbson_array_writer roleArrayWriter;
	PgbsonWriterStartArray(&userWriter, "roles", 5,
						   &roleArrayWriter);
	WriteMultipleRoles(userEntry->roles, &roleArrayWriter);
	PgbsonWriterEndArray(&userWriter, &roleArrayWriter);

	if (EnableUsersInfoPrivileges && showPrivileges && userEntry->roles != NULL)
	{
		pgbson_array_writer privilegesArrayWriter;
		PgbsonWriterStartArray(&userWriter, "privileges", 10,
							   &privilegesArrayWriter);
		WriteMultipleRolePrivileges(userEntry->roles, &privilegesArrayWriter);
		PgbsonWriterEndArray(&userWriter, &privilegesArrayWriter);
	}

	if (userEntry->isExternal)
	{
		PgbsonWriterAppendDocument(&userWriter, "customData", 10,
								   GetUserInfoFromExternalIdentityProvider(
									   userEntry->user));
	}

	PgbsonArrayWriterWriteDocument(userArrayWriter, PgbsonWriterGetPgbson(
									   &userWriter));
}


/*
 * Consolidates privileges for all roles in the provided HTAB and
 * writes them to the provided BSON array writer.
 * The rolesTable should contain StringView entries representing role names.
 */
static void
WriteMultipleRolePrivileges(HTAB *rolesTable, pgbson_array_writer *privilegesArrayWriter)
{
	if (rolesTable == NULL)
	{
		return;
	}

	List *consolidatedPrivileges = NIL;
	HASH_SEQ_STATUS status;
	StringView *roleEntry;

	hash_seq_init(&status, rolesTable);
	while ((roleEntry = hash_seq_search(&status)) != NULL)
	{
		/* Convert StringView to null-terminated string */
		char *roleName = palloc(roleEntry->length + 1);
		memcpy(roleName, roleEntry->string, roleEntry->length);
		roleName[roleEntry->length] = '\0';

		ConsolidatePrivilegesForRole(roleName, &consolidatedPrivileges);

		pfree(roleName);
	}

	WritePrivilegeListToArray(consolidatedPrivileges, privilegesArrayWriter);
	DeepFreePrivileges(consolidatedPrivileges);
}


/*
 * Consolidates privileges for a role and
 * writes them to the provided BSON array writer.
 */
static void
WriteSingleRolePrivileges(const char *roleName,
						  pgbson_array_writer *privilegesArrayWriter)
{
	if (roleName == NULL)
	{
		ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
						errmsg("Role name cannot be NULL.")));
	}

	List *consolidatedPrivileges = NIL;

	ConsolidatePrivilegesForRole(roleName, &consolidatedPrivileges);

	WritePrivilegeListToArray(consolidatedPrivileges, privilegesArrayWriter);
	DeepFreePrivileges(consolidatedPrivileges);
}


/*
 * Consolidates privileges for a given role name into the provided list.
 * Ignores unknown roles silently.
 */
static void
ConsolidatePrivilegesForRole(const char *roleName, List **consolidatedPrivileges)
{
	if (roleName == NULL || consolidatedPrivileges == NULL)
	{
		return;
	}

	size_t sourcePrivilegeCount;

	if (strcmp(roleName, ApiReadOnlyRole) == 0)
	{
		sourcePrivilegeCount = sizeof(readOnlyPrivileges) / sizeof(readOnlyPrivileges[0]);
		ConsolidatePrivileges(consolidatedPrivileges, readOnlyPrivileges,
							  sourcePrivilegeCount);
	}
	else if (strcmp(roleName, ApiReadWriteRole) == 0)
	{
		sourcePrivilegeCount = sizeof(readWritePrivileges) /
							   sizeof(readWritePrivileges[0]);
		ConsolidatePrivileges(consolidatedPrivileges, readWritePrivileges,
							  sourcePrivilegeCount);
	}
	else if (strcmp(roleName, ApiAdminRoleV2) == 0)
	{
		sourcePrivilegeCount = sizeof(readWritePrivileges) /
							   sizeof(readWritePrivileges[0]);
		ConsolidatePrivileges(consolidatedPrivileges, readWritePrivileges,
							  sourcePrivilegeCount);

		sourcePrivilegeCount = sizeof(clusterManagerPrivileges) /
							   sizeof(clusterManagerPrivileges[0]);
		ConsolidatePrivileges(consolidatedPrivileges, clusterManagerPrivileges,
							  sourcePrivilegeCount);

		sourcePrivilegeCount = sizeof(clusterMonitorPrivileges) /
							   sizeof(clusterMonitorPrivileges[0]);
		ConsolidatePrivileges(consolidatedPrivileges, clusterMonitorPrivileges,
							  sourcePrivilegeCount);

		sourcePrivilegeCount = sizeof(hostManagerPrivileges) /
							   sizeof(hostManagerPrivileges[0]);
		ConsolidatePrivileges(consolidatedPrivileges, hostManagerPrivileges,
							  sourcePrivilegeCount);

		sourcePrivilegeCount = sizeof(dropDatabasePrivileges) /
							   sizeof(dropDatabasePrivileges[0]);
		ConsolidatePrivileges(consolidatedPrivileges, dropDatabasePrivileges,
							  sourcePrivilegeCount);
	}
	else if (strcmp(roleName, ApiUserAdminRole) == 0)
	{
		sourcePrivilegeCount = sizeof(userAdminPrivileges) /
							   sizeof(userAdminPrivileges[0]);
		ConsolidatePrivileges(consolidatedPrivileges, userAdminPrivileges,
							  sourcePrivilegeCount);
	}
	else if (strcmp(roleName, ApiRootRole) == 0)
	{
		sourcePrivilegeCount = sizeof(readWritePrivileges) /
							   sizeof(readWritePrivileges[0]);
		ConsolidatePrivileges(consolidatedPrivileges, readWritePrivileges,
							  sourcePrivilegeCount);

		sourcePrivilegeCount = sizeof(dbAdminPrivileges) /
							   sizeof(dbAdminPrivileges[0]);
		ConsolidatePrivileges(consolidatedPrivileges, dbAdminPrivileges,
							  sourcePrivilegeCount);

		sourcePrivilegeCount = sizeof(userAdminPrivileges) /
							   sizeof(userAdminPrivileges[0]);
		ConsolidatePrivileges(consolidatedPrivileges, userAdminPrivileges,
							  sourcePrivilegeCount);

		sourcePrivilegeCount = sizeof(clusterMonitorPrivileges) /
							   sizeof(clusterMonitorPrivileges[0]);
		ConsolidatePrivileges(consolidatedPrivileges, clusterMonitorPrivileges,
							  sourcePrivilegeCount);

		sourcePrivilegeCount = sizeof(clusterManagerPrivileges) /
							   sizeof(clusterManagerPrivileges[0]);
		ConsolidatePrivileges(consolidatedPrivileges, clusterManagerPrivileges,
							  sourcePrivilegeCount);

		sourcePrivilegeCount = sizeof(hostManagerPrivileges) /
							   sizeof(hostManagerPrivileges[0]);
		ConsolidatePrivileges(consolidatedPrivileges, hostManagerPrivileges,
							  sourcePrivilegeCount);

		sourcePrivilegeCount = sizeof(dropDatabasePrivileges) /
							   sizeof(dropDatabasePrivileges[0]);
		ConsolidatePrivileges(consolidatedPrivileges, dropDatabasePrivileges,
							  sourcePrivilegeCount);
	}

	/* Unknown roles are silently ignored */
}


/*
 * Takes a list of source privileges and consolidates them into the
 * provided list of consolidated privileges, merging any duplicate privileges and combining their actions.
 */
static void
ConsolidatePrivileges(List **consolidatedPrivileges,
					  const UserPrivilege *sourcePrivileges,
					  size_t sourcePrivilegeCount)
{
	if (sourcePrivileges == NULL)
	{
		return;
	}

	for (size_t i = 0; i < sourcePrivilegeCount; i++)
	{
		ConsolidatePrivilege(consolidatedPrivileges, &sourcePrivileges[i]);
	}
}


/*
 * Consolidates a single source privilege into the list of consolidated privileges.
 * If a privilege with the same resource target already exists, its actions are merged with the source privilege.
 * Otherwise, a new privilege is created and added to the list.
 */
static void
ConsolidatePrivilege(List **consolidatedPrivileges, const UserPrivilege *sourcePrivilege)
{
	if (sourcePrivilege == NULL || sourcePrivilege->numActions == 0)
	{
		return;
	}

	ListCell *privilege;
	ConsolidatedUserPrivilege *existingPrivilege = NULL;

	foreach(privilege, *consolidatedPrivileges)
	{
		ConsolidatedUserPrivilege *currentPrivilege =
			(ConsolidatedUserPrivilege *) lfirst(privilege);

		if (ComparePrivileges(currentPrivilege, sourcePrivilege))
		{
			existingPrivilege = currentPrivilege;
			break;
		}
	}

	if (existingPrivilege != NULL)
	{
		for (size_t i = 0; i < sourcePrivilege->numActions; i++)
		{
			bool actionFound;

			/* The consolidated privilege does not free the actual char* in the action HTAB;
			 * therefore it is safe to pass actions[i]. */
			hash_search(existingPrivilege->actions,
						&sourcePrivilege->actions[i], HASH_ENTER,
						&actionFound);
		}
	}
	else
	{
		ConsolidatedUserPrivilege *newPrivilege = palloc0(
			sizeof(ConsolidatedUserPrivilege));
		newPrivilege->isCluster = sourcePrivilege->isCluster;
		newPrivilege->db = pstrdup(sourcePrivilege->db);
		newPrivilege->collection = pstrdup(sourcePrivilege->collection);
		newPrivilege->actions = CreateStringViewHashSet();

		for (size_t i = 0; i < sourcePrivilege->numActions; i++)
		{
			bool actionFound;

			hash_search(newPrivilege->actions,
						&sourcePrivilege->actions[i],
						HASH_ENTER, &actionFound);
		}

		*consolidatedPrivileges = lappend(*consolidatedPrivileges, newPrivilege);
	}
}


/*
 * Checks if two privileges have the same resource (same cluster status and db/collection).
 */
static bool
ComparePrivileges(const ConsolidatedUserPrivilege *privilege1,
				  const UserPrivilege *privilege2)
{
	if (privilege1->isCluster != privilege2->isCluster)
	{
		return false;
	}

	if (privilege1->isCluster)
	{
		return true;
	}

	return (strcmp(privilege1->db, privilege2->db) == 0 &&
			strcmp(privilege1->collection, privilege2->collection) == 0);
}


/*
 * Helper function to write a single privilege document.
 */
static void
WriteSinglePrivilegeDocument(const ConsolidatedUserPrivilege *privilege,
							 pgbson_array_writer *privilegesArrayWriter)
{
	pgbson_writer privilegeWriter;
	PgbsonArrayWriterStartDocument(privilegesArrayWriter, &privilegeWriter);

	pgbson_writer resourceWriter;
	PgbsonWriterStartDocument(&privilegeWriter, "resource", 8,
							  &resourceWriter);
	if (privilege->isCluster)
	{
		PgbsonWriterAppendBool(&resourceWriter, "cluster", 7,
							   true);
	}
	else
	{
		PgbsonWriterAppendUtf8(&resourceWriter, "db", 2,
							   privilege->db);
		PgbsonWriterAppendUtf8(&resourceWriter, "collection", 10,
							   privilege->collection);
	}
	PgbsonWriterEndDocument(&privilegeWriter, &resourceWriter);

	pgbson_array_writer actionsArrayWriter;
	PgbsonWriterStartArray(&privilegeWriter, "actions", 7,
						   &actionsArrayWriter);

	if (privilege->actions != NULL)
	{
		HASH_SEQ_STATUS status;
		StringView *privilegeEntry;
		List *actionList = NIL;

		hash_seq_init(&status, privilege->actions);
		while ((privilegeEntry = hash_seq_search(&status)) != NULL)
		{
			char *actionString = palloc(privilegeEntry->length + 1);
			memcpy(actionString, privilegeEntry->string, privilegeEntry->length);
			actionString[privilegeEntry->length] = '\0';
			actionList = lappend(actionList, actionString);
		}

		if (actionList != NIL)
		{
			SortStringList(actionList);
			ListCell *cell;
			foreach(cell, actionList)
			{
				PgbsonArrayWriterWriteUtf8(&actionsArrayWriter, (const char *) lfirst(
											   cell));
			}
			list_free_deep(actionList);
		}
	}

	PgbsonWriterEndArray(&privilegeWriter, &actionsArrayWriter);

	PgbsonArrayWriterEndDocument(privilegesArrayWriter, &privilegeWriter);
}


/*
 * Writes the consolidated privileges list to a BSON array.
 */
static void
WritePrivilegeListToArray(List *consolidatedPrivileges,
						  pgbson_array_writer *privilegesArrayWriter)
{
	ListCell *privilege;
	foreach(privilege, consolidatedPrivileges)
	{
		ConsolidatedUserPrivilege *currentPrivilege =
			(ConsolidatedUserPrivilege *) lfirst(privilege);
		WriteSinglePrivilegeDocument(currentPrivilege, privilegesArrayWriter);
	}
}


/*
 * Frees all memory allocated for the consolidated privileges list,
 * including strings and hash table entries.
 */
static void
DeepFreePrivileges(List *consolidatedPrivileges)
{
	if (consolidatedPrivileges == NIL)
	{
		return;
	}

	ListCell *privilege;
	foreach(privilege, consolidatedPrivileges)
	{
		ConsolidatedUserPrivilege *currentPrivilege =
			(ConsolidatedUserPrivilege *) lfirst(privilege);

		if (currentPrivilege->db)
		{
			pfree((char *) currentPrivilege->db);
		}

		if (currentPrivilege->collection)
		{
			pfree((char *) currentPrivilege->collection);
		}

		if (currentPrivilege->actions)
		{
			hash_destroy(currentPrivilege->actions);
		}
	}

	list_free_deep(consolidatedPrivileges);
}


/*
 * ParseUsersInfoDocument extracts and processes the fields of the BSON document
 * for the usersInfo command.
 */
static void
ParseUsersInfoDocument(const bson_value_t *usersInfoBson, GetUserSpec *spec)
{
	bson_iter_t iter;
	BsonValueInitIterator(usersInfoBson, &iter);

	while (bson_iter_next(&iter))
	{
		const char *bsonDocKey = bson_iter_key(&iter);
		if (strcmp(bsonDocKey, "db") == 0 && BSON_ITER_HOLDS_UTF8(&iter))
		{
			uint32_t strLength;
			const char *db = bson_iter_utf8(&iter, &strLength);
			if (strcmp(db, "admin") != 0)
			{
				ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
								errmsg(
									"Unsupported value specified for 'db' field. Only 'admin' is allowed."),
								errdetail_log(
									"Unsupported value specified for 'db' field. Only 'admin' is allowed.")));
			}
		}
		else if (strcmp(bsonDocKey, "user") == 0 && BSON_ITER_HOLDS_UTF8(
					 &iter))
		{
			uint32_t strLength;
			const char *userString = bson_iter_utf8(&iter, &strLength);
			spec->user = (StringView) {
				.string = userString,
				.length = strLength
			};
		}
	}
}


/*
 * GetAllUsersInfo queries and returns all users information, including their id, name, and roles.
 * Returns the user info datum containing the query result.
 */
static Datum
GetAllUsersInfo(void)
{
	const char *cmdStr = GetAllUsersQuery();

	bool readOnly = true;
	bool isNull = false;
	return ExtensionExecuteQueryViaSPI(cmdStr, readOnly, SPI_OK_SELECT,
									   &isNull);
}


/*
 * GetSingleUserInfo queries and processes user role information for a given user.
 * Returns the user info datum containing the query result.
 */
static Datum
GetSingleUserInfo(const char *userName, bool returnDocuments)
{
	if (userName == NULL)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_INTERNALERROR),
						errmsg("Username is null")));
	}

	const char *cmdStr;

	if (returnDocuments)
	{
		cmdStr = GetSingleUserQuery();
	}
	else
	{
		cmdStr = GetSingleUserNameQuery();
	}

	int argCount = 1;
	Oid argTypes[1];
	Datum argValues[1];

	argTypes[0] = TEXTOID;
	argValues[0] = CStringGetTextDatum(userName);

	bool readOnly = true;
	bool isNull = false;
	return ExtensionExecuteQueryWithArgsViaSPI(cmdStr, argCount,
											   argTypes, argValues, NULL,
											   readOnly, SPI_OK_SELECT,
											   &isNull);
}


/*
 * GetAllUsersQuery returns the SQL query string to fetch all users information, including their id, name, and roles.
 * Returns the user info datum containing the query result.
 */
static const char *
GetAllUsersQuery()
{
	if (GetAllUsersQueryString == NULL)
	{
		return FormatSqlQuery(
			"WITH r AS ("
			"  SELECT child.rolname::text AS child_role, "
			"         parent.rolname::text AS parent_role "
			"  FROM pg_roles parent "
			"  JOIN pg_auth_members am ON parent.oid = am.roleid "
			"  JOIN pg_roles child ON am.member = child.oid "
			"  WHERE child.rolcanlogin = true"
			") "
			"SELECT ARRAY_AGG(%s.row_get_bson(r) ORDER BY child_role) "
			"FROM r;",
			CoreSchemaName);
	}

	return GetAllUsersQueryString;
}


/*
 * GetSingleUserQuery returns the SQL query string to fetch a user's information, including their id, name, and roles.
 * Returns the user info datum containing the query result.
 */
static const char *
GetSingleUserQuery()
{
	if (GetSingleUserQueryString == NULL)
	{
		return FormatSqlQuery(
			"WITH r AS ("
			"  SELECT child.rolname::text AS child_role, "
			"         parent.rolname::text AS parent_role "
			"  FROM pg_roles parent "
			"  JOIN pg_auth_members am ON parent.oid = am.roleid "
			"  JOIN pg_roles child ON am.member = child.oid "
			"  WHERE child.rolcanlogin = true "
			"    AND child.rolname = $1"
			") "
			"SELECT ARRAY_AGG(%s.row_get_bson(r) ORDER BY r.parent_role) "
			"FROM r;",
			CoreSchemaName);
	}

	return GetSingleUserQueryString;
}


/*
 * GetSingleUserNameQuery returns the SQL query string to fetch a user's name.
 * Returns the user info datum containing the query result.
 */
static const char *
GetSingleUserNameQuery()
{
	if (GetSingleUserNameQueryString == NULL)
	{
		return FormatSqlQuery(
			"SELECT parent.rolname::text "
			"FROM pg_roles parent "
			"JOIN pg_auth_members am ON parent.oid = am.roleid "
			"JOIN pg_roles child ON am.member = child.oid "
			"WHERE child.rolcanlogin = true "
			"  AND child.rolname = $1 "
			"ORDER BY parent.rolname;");
	}

	return GetSingleUserNameQueryString;
}


/*
 * WriteMultipleRoles iterates through the roles HTAB and writes each role to the provided BSON array writer.
 * This is used to write roles for usersInfo and connectionStatus commands.
 */
static void
WriteMultipleRoles(HTAB *rolesTable, pgbson_array_writer *roleArrayWriter)
{
	if (rolesTable == NULL)
	{
		return;
	}

	HASH_SEQ_STATUS status;
	StringView *roleEntry;
	hash_seq_init(&status, rolesTable);
	while ((roleEntry = hash_seq_search(&status)) != NULL)
	{
		WriteRoles(roleEntry->string, roleArrayWriter);
	}
}


/*
 * WriteRoles writes role information to a BSON array writer based on the parent role.
 * This consolidates the role mapping logic used by both usersInfo and connectionStatus commands.
 */
static void
WriteRoles(const char *parentRole, pgbson_array_writer *roleArrayWriter)
{
	if (parentRole == NULL)
	{
		return;
	}

	pgbson_writer roleWriter;
	PgbsonWriterInit(&roleWriter);
	if (strcmp(parentRole, ApiReadOnlyRole) == 0)
	{
		PgbsonWriterAppendUtf8(&roleWriter, "role", 4, "readAnyDatabase");
		PgbsonWriterAppendUtf8(&roleWriter, "db", 2, "admin");
		PgbsonArrayWriterWriteDocument(roleArrayWriter,
									   PgbsonWriterGetPgbson(
										   &roleWriter));
	}
	else if (strcmp(parentRole, ApiReadWriteRole) == 0)
	{
		PgbsonWriterAppendUtf8(&roleWriter, "role", 4,
							   "readWriteAnyDatabase");
		PgbsonWriterAppendUtf8(&roleWriter, "db", 2, "admin");
		PgbsonArrayWriterWriteDocument(roleArrayWriter,
									   PgbsonWriterGetPgbson(
										   &roleWriter));
	}
	else if (strcmp(parentRole, ApiAdminRoleV2) == 0)
	{
		PgbsonWriterAppendUtf8(&roleWriter, "role", 4,
							   "readWriteAnyDatabase");
		PgbsonWriterAppendUtf8(&roleWriter, "db", 2, "admin");
		PgbsonArrayWriterWriteDocument(roleArrayWriter,
									   PgbsonWriterGetPgbson(
										   &roleWriter));
		PgbsonWriterInit(&roleWriter);
		PgbsonWriterAppendUtf8(&roleWriter, "role", 4,
							   "clusterAdmin");
		PgbsonWriterAppendUtf8(&roleWriter, "db", 2, "admin");
		PgbsonArrayWriterWriteDocument(roleArrayWriter,
									   PgbsonWriterGetPgbson(
										   &roleWriter));
	}
	else if (strcmp(parentRole, ApiUserAdminRole) == 0)
	{
		PgbsonWriterAppendUtf8(&roleWriter, "role", 4,
							   "userAdminAnyDatabase");
		PgbsonWriterAppendUtf8(&roleWriter, "db", 2, "admin");
		PgbsonArrayWriterWriteDocument(roleArrayWriter,
									   PgbsonWriterGetPgbson(
										   &roleWriter));
	}
	else if (strcmp(parentRole, ApiRootRole) == 0)
	{
		PgbsonWriterAppendUtf8(&roleWriter, "role", 4,
							   "root");
		PgbsonWriterAppendUtf8(&roleWriter, "db", 2, "admin");
		PgbsonArrayWriterWriteDocument(roleArrayWriter,
									   PgbsonWriterGetPgbson(
										   &roleWriter));
	}
	else
	{
		return;
	}
}


/*
 * BuildUserRoleEntryTable creates and populates a hash table with user role information
 * from the provided user data array.
 */
static HTAB *
BuildUserRoleEntryTable(Datum *userDatums, int userCount)
{
	HTAB *userRolesTable = CreateUserEntryHashSet();

	for (int i = 0; i < userCount; i++)
	{
		/* Convert Datum to a bson_t object */
		pgbson *bson_doc = DatumGetPgBson(userDatums[i]);
		bson_iter_t getIter;
		PgbsonInitIterator(bson_doc, &getIter);

		const char *user = NULL;

		/* Initialize iterator */
		if (bson_iter_find(&getIter, "child_role"))
		{
			if (BSON_ITER_HOLDS_UTF8(&getIter))
			{
				user = bson_iter_utf8(&getIter, NULL);
				bool userFound = false;
				UserRoleHashEntry searchEntry = {
					.user = (char *) user,
				};

				hash_search(userRolesTable,
							&searchEntry,
							HASH_FIND,
							&userFound);

				if (!userFound)
				{
					UserRoleHashEntry newEntry = {
						.user = pstrdup(user),
						.roles = NULL,
						.isExternal = IsUserExternal(user)
					};

					bool entryCreated = false;
					hash_search(userRolesTable, &newEntry, HASH_ENTER, &entryCreated);
				}
			}
		}
		if (bson_iter_find(&getIter, "parent_role"))
		{
			if (BSON_ITER_HOLDS_UTF8(&getIter))
			{
				const char *parentRole = bson_iter_utf8(&getIter, NULL);

				if (!IS_SUPPORTED_BUILTIN_ROLE(parentRole))
				{
					continue;
				}

				UserRoleHashEntry userSearchEntry = {
					.user = (char *) user,
				};

				bool userFound = false;
				UserRoleHashEntry *userEntry = hash_search(userRolesTable,
														   &userSearchEntry,
														   HASH_FIND,
														   &userFound);

				if (userFound && userEntry != NULL)
				{
					if (userEntry->roles == NULL)
					{
						userEntry->roles = CreateStringViewHashSet();
					}

					StringView roleStringView = {
						.string = (char *) parentRole,
						.length = strlen(parentRole)
					};

					bool roleAdded = false;
					hash_search(userEntry->roles, &roleStringView, HASH_ENTER,
								&roleAdded);
				}
			}
		}
	}

	return userRolesTable;
}


/*
 * Creates a hash table that maps strings to HTAB pointers.
 */
static HTAB *
CreateUserEntryHashSet()
{
	HASHCTL hashInfo = CreateExtensionHashCTL(
		sizeof(UserRoleHashEntry),
		sizeof(UserRoleHashEntry),
		UserHashEntryCompareFunc,
		UserHashEntryHashFunc
		);
	return hash_create("User Entry Hash Table", 32, &hashInfo,
					   DefaultExtensionHashFlags);
}


/*
 * UserHashEntryHashFunc is the (HASHCTL.hash) callback used to hash a UserRoleHashEntry
 */
static uint32
UserHashEntryHashFunc(const void *obj, size_t objsize)
{
	const UserRoleHashEntry *hashEntry = obj;
	return hash_bytes((const unsigned char *) hashEntry->user, strlen(hashEntry->user));
}


/*
 * UserHashEntryCompareFunc is the (HASHCTL.match) callback used to determine if two string keys are same.
 * Returns 0 if those two string keys are same, non-zero otherwise.
 */
static int
UserHashEntryCompareFunc(const void *obj1, const void *obj2, Size objsize)
{
	const UserRoleHashEntry *hashEntry1 = obj1;
	const UserRoleHashEntry *hashEntry2 = obj2;

	return strcmp(hashEntry1->user, hashEntry2->user);
}


/*
 * FreeUserRoleEntryTable cleans up the user roles hash table and all nested role hash tables.
 */
static void
FreeUserRoleEntryTable(HTAB *userRolesTable)
{
	if (userRolesTable != NULL)
	{
		HASH_SEQ_STATUS status;
		UserRoleHashEntry *userRoleEntry;
		hash_seq_init(&status, userRolesTable);
		while ((userRoleEntry = hash_seq_search(&status)) != NULL)
		{
			if (userRoleEntry->roles != NULL)
			{
				hash_destroy(userRoleEntry->roles);
			}
		}

		hash_destroy(userRolesTable);
	}
}
