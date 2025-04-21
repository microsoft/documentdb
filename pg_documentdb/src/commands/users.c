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
#include "api_hooks.h"
#include "miscadmin.h"

#define SCRAM_MAX_SALT_LEN 64

/* GUC to enable user crud operations */
extern bool EnableUserCrud;

/* GUC that controls the default salt length*/
extern int ScramDefaultSaltLen;

/* GUC that controls the max number of users allowed*/
extern int MaxUserLimit;

/* GUC that controls the blocked role prefix list*/
extern char *BlockedRolePrefixList;

PG_FUNCTION_INFO_V1(documentdb_extension_create_user);
PG_FUNCTION_INFO_V1(documentdb_extension_drop_user);
PG_FUNCTION_INFO_V1(documentdb_extension_update_user);
PG_FUNCTION_INFO_V1(documentdb_extension_get_users);

static CreateUserSpec * ParseCreateUserSpec(pgbson *createUserSpec);
static char * ParseDropUserSpec(pgbson *dropSpec);
static UpdateUserSpec * ParseUpdateUserSpec(pgbson *updateSpec);
static Datum UpdateNativeUser(UpdateUserSpec *spec);
static char * ParseGetUserSpec(pgbson *getSpec);
static char * PrehashPassword(const char *password);
static bool IsCallingUserExternal(void);

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
		"SELECT COUNT(*) FROM pg_roles parent JOIN pg_auth_members am ON parent.oid = am.roleid JOIN pg_roles child " \
		"ON am.member = child.oid WHERE child.rolcanlogin = true AND parent.rolname IN ('%s', '%s') " \
		"AND child.rolname NOT IN ('%s', '%s');", ApiAdminRoleV2, ApiReadOnlyRole,
		ApiAdminRoleV2, ApiReadOnlyRole);

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
	CreateUserSpec *createUserSpec = ParseCreateUserSpec(createUserBson);

	if (createUserSpec->has_identity_provider)
	{
		if (!CreateUserWithExternalIdentityProvider(createUserSpec->createUser,
													createUserSpec->pgRole,
													createUserSpec->identityProviderData))
		{
			pgbson_writer finalWriter;
			PgbsonWriterInit(&finalWriter);
			PgbsonWriterAppendInt32(&finalWriter, "ok", 2, 0);
			PgbsonWriterAppendUtf8(&finalWriter, "errmsg", strlen("errmsg"),
								   "External identity providers are not supported");
			PgbsonWriterAppendInt32(&finalWriter, "code", strlen("code"), 115);
			PgbsonWriterAppendUtf8(&finalWriter, "codeName", strlen("codeName"),
								   "CommandNotSupported");
			PG_RETURN_POINTER(PgbsonWriterGetPgbson(&finalWriter));
		}
	}
	else
	{
		/*Verify that the calling user is a native user */
		if (IsCallingUserExternal())
		{
			ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_INSUFFICIENTPRIVILEGE),
							errmsg(
								"Only native users can create other native users.")));
		}

		ReportFeatureUsage(FEATURE_USER_CREATE);

		StringInfo createUserInfo = makeStringInfo();
		appendStringInfo(createUserInfo,
						 "CREATE ROLE %s WITH LOGIN PASSWORD '%s';",
						 quote_identifier(createUserSpec->createUser),
						 PrehashPassword(createUserSpec->pwd));

		readOnly = false;
		isNull = false;
		ExtensionExecuteQueryViaSPI(createUserInfo->data, readOnly, SPI_OK_UTILITY,
									&isNull);
	}

	/* Grant pgRole to user created */
	readOnly = false;
	const char *queryGrant = psprintf("GRANT %s TO %s",
									  quote_identifier(createUserSpec->pgRole),
									  quote_identifier(createUserSpec->createUser));

	ExtensionExecuteQueryViaSPI(queryGrant, readOnly, SPI_OK_UTILITY, &isNull);

	if (strcmp(createUserSpec->pgRole, ApiReadOnlyRole) == 0)
	{
		/* This is needed to grant ApiReadOnlyRole */
		/* read access to all new and existing collections */
		StringInfo grantReadOnlyPermissions = makeStringInfo();
		resetStringInfo(grantReadOnlyPermissions);
		appendStringInfo(grantReadOnlyPermissions,
						 "GRANT pg_read_all_data TO %s",
						 quote_identifier(createUserSpec->createUser));
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
 * protocol message createUser() which creates a mongo user
 */
static CreateUserSpec *
ParseCreateUserSpec(pgbson *createSpec)
{
	bson_iter_t createIter;
	PgbsonInitIterator(createSpec, &createIter);

	CreateUserSpec *spec = palloc0(sizeof(CreateUserSpec));

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
									"Password cannot be empty.")));
			}

			has_pwd = true;
		}
		else if (strcmp(key, "roles") == 0)
		{
			spec->roles = *bson_iter_value(&createIter);

			if (IsBsonValueEmptyDocument(&spec->roles))
			{
				ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
								errmsg(
									"'roles' is a required field.")));
			}

			/* Validate that it is of the right format */
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
						 errmsg("'customData' must be a bson document.")));
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
											"Unsupported field specified in custom data: '%s'.",
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
	else if (!has_user || !has_roles || !has_pwd)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE), errmsg(
							"'createUser', 'roles' and 'pwd' are required fields.")));
	}

	return spec;
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
									errmsg("Invalid value specified for role: '%s'.",
										   role),
									errdetail_log(
										"Invalid value specified for role: '%s'.",
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
						errmsg("'dropUser' is a required field.")));
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
			PgbsonWriterAppendUtf8(&finalWriter, "errmsg", strlen("errmsg"),
								   "External identity providers are not supported");
			PgbsonWriterAppendInt32(&finalWriter, "code", strlen("code"), 115);
			PgbsonWriterAppendUtf8(&finalWriter, "codeName", strlen("codeName"),
								   "CommandNotSupported");
			PG_RETURN_POINTER(PgbsonWriterGetPgbson(&finalWriter));
		}
	}
	else
	{
		/*Verify that the calling user is also native*/
		if (IsCallingUserExternal())
		{
			ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_INSUFFICIENTPRIVILEGE),
							errmsg(
								"Only native users can drop other native users.")));
		}

		ReportFeatureUsage(FEATURE_USER_DROP);

		StringInfo dropUserInfo = makeStringInfo();
		appendStringInfo(dropUserInfo, "DROP ROLE %s;", quote_identifier(dropUser));

		bool readOnly = false;
		bool isNull = false;
		ExtensionExecuteQueryViaSPI(dropUserInfo->data, readOnly, SPI_OK_UTILITY,
									&isNull);
	}

	pgbson_writer finalWriter;
	PgbsonWriterInit(&finalWriter);
	PgbsonWriterAppendInt32(&finalWriter, "ok", 2, 1);
	PG_RETURN_POINTER(PgbsonWriterGetPgbson(&finalWriter));
}


/*
 * ParseDropUserSpec parses the wire
 * protocol message dropUser() which drops a mongo user
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
									"'dropUser' is a required field.")));
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
						errmsg("'dropUser' is a required field.")));
	}

	return dropUser;
}


/*
 * documentdb_extension_update_user implements the core logic to update a user.
 * In MongoDB a user with userAdmin privileges or root privileges can change
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
						errmsg("UpdateUser command is not supported."),
						errdetail_log("UpdateUser command is not supported.")));
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
	UpdateUserSpec *spec = ParseUpdateUserSpec(updateUserSpec);

	if (IsUserExternal(spec->updateUser))
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_COMMANDNOTSUPPORTED),
						errmsg(
							"UpdateUser command is not supported for a non native user.")));
	}
	else
	{
		return UpdateNativeUser(spec);
	}
}


/*
 * ParseUpdateUserSpec parses the wire
 * protocol message updateUser() which drops a mongo user
 */
static UpdateUserSpec *
ParseUpdateUserSpec(pgbson *updateSpec)
{
	bson_iter_t updateIter;
	PgbsonInitIterator(updateSpec, &updateIter);

	UpdateUserSpec *spec = palloc0(sizeof(UpdateUserSpec));

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

	if (has_user)
	{
		return spec;
	}

	ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
					errmsg("'updateUser' is a required field.")));
}


/*
 * Update native user
 */
static Datum
UpdateNativeUser(UpdateUserSpec *spec)
{
	ReportFeatureUsage(FEATURE_USER_UPDATE);

	/*Verify that the calling user is also native*/
	if (IsCallingUserExternal())
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_INSUFFICIENTPRIVILEGE),
						errmsg("Only native users can update other native users.")));
	}

	if (spec->pwd == NULL || spec->pwd[0] == '\0')
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
						errmsg("Password cannot be empty.")));
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

	char *userName = ParseGetUserSpec(PG_GETARG_PGBSON(0));
	const char *cmdStr = NULL;
	Datum userInfoDatum;

	if (userName == NULL)
	{
		cmdStr = FormatSqlQuery(
			"WITH r AS (SELECT child.rolname::text AS child_role, parent.rolname::text AS parent_role FROM pg_roles parent JOIN pg_auth_members am ON parent.oid = am.roleid JOIN " \
			"pg_roles child ON am.member = child.oid WHERE child.rolcanlogin = true AND parent.rolname IN ('%s', '%s') AND child.rolname NOT IN " \
			"('%s', '%s')) SELECT ARRAY_AGG(%s.row_get_bson(r)) FROM r;",
			ApiAdminRoleV2, ApiReadOnlyRole, ApiAdminRoleV2, ApiReadOnlyRole,
			CoreSchemaName);
		bool readOnly = true;
		bool isNull = false;
		userInfoDatum = ExtensionExecuteQueryViaSPI(cmdStr, readOnly,
													SPI_OK_SELECT, &isNull);
	}
	else
	{
		cmdStr = FormatSqlQuery(
			"WITH r AS (SELECT child.rolname::text AS child_role, parent.rolname::text AS parent_role FROM pg_roles parent JOIN pg_auth_members am ON parent.oid = am.roleid JOIN " \
			"pg_roles child ON am.member = child.oid WHERE child.rolcanlogin = true AND parent.rolname IN ('%s', '%s') AND child.rolname = $1) SELECT " \
			"ARRAY_AGG(%s.row_get_bson(r)) FROM r;", ApiAdminRoleV2, ApiReadOnlyRole,
			CoreSchemaName);
		int argCount = 1;
		Oid argTypes[1];
		Datum argValues[1];

		argTypes[0] = TEXTOID;
		argValues[0] = CStringGetTextDatum(userName);

		bool readOnly = true;
		bool isNull = false;
		userInfoDatum = ExtensionExecuteQueryWithArgsViaSPI(cmdStr, argCount,
															argTypes, argValues, NULL,
															readOnly, SPI_OK_SELECT,
															&isNull);
	}

	pgbson_writer finalWriter;
	PgbsonWriterInit(&finalWriter);

	if (userInfoDatum == (Datum) 0)
	{
		PgbsonWriterAppendInt32(&finalWriter, "ok", 2, 1);
		pgbson *result = PgbsonWriterGetPgbson(&finalWriter);
		PG_RETURN_POINTER(result);
	}

	ArrayType *val_array = DatumGetArrayTypeP(userInfoDatum);
	Datum *val_datums;
	bool *val_is_null_marker;
	int val_count;

	bool arrayByVal = false;
	int elementLength = -1;
	Oid arrayElementType = ARR_ELEMTYPE(val_array);
	deconstruct_array(val_array,
					  arrayElementType, elementLength, arrayByVal,
					  TYPALIGN_INT, &val_datums, &val_is_null_marker,
					  &val_count);

	if (val_count > 0)
	{
		pgbson_array_writer userArrayWriter;
		PgbsonWriterStartArray(&finalWriter, "users", strlen("users"), &userArrayWriter);
		for (int i = 0; i < val_count; i++)
		{
			pgbson_writer userWriter;
			PgbsonWriterInit(&userWriter);

			/* Convert Datum to a bson_t object */
			pgbson *bson_doc = (pgbson *) DatumGetPointer(val_datums[i]);
			bson_iter_t getIter;
			PgbsonInitIterator(bson_doc, &getIter);

			bool isUserExternal = false;
			const char *user = NULL;

			/* Initialize iterator */
			if (bson_iter_find(&getIter, "child_role"))
			{
				if (BSON_ITER_HOLDS_UTF8(&getIter))
				{
					user = bson_iter_utf8(&getIter, NULL);
					PgbsonWriterAppendUtf8(&userWriter, "_id", strlen("_id"), psprintf(
											   "admin.%s",
											   user));
					PgbsonWriterAppendUtf8(&userWriter, "userId", strlen("userId"),
										   psprintf("admin.%s", user));
					PgbsonWriterAppendUtf8(&userWriter, "user", strlen("user"), user);
					PgbsonWriterAppendUtf8(&userWriter, "db", strlen("db"), "admin");
					isUserExternal = IsUserExternal(user);
				}
			}
			if (bson_iter_find(&getIter, "parent_role"))
			{
				if (BSON_ITER_HOLDS_UTF8(&getIter))
				{
					const char *parentRole = bson_iter_utf8(&getIter, NULL);
					if (strcmp(parentRole, ApiReadOnlyRole) == 0)
					{
						pgbson_array_writer roleArrayWriter;
						PgbsonWriterStartArray(&userWriter, "roles", strlen("roles"),
											   &roleArrayWriter);
						pgbson_writer roleWriter;
						PgbsonWriterInit(&roleWriter);
						PgbsonWriterAppendUtf8(&roleWriter, "role", strlen("role"),
											   "readAnyDatabase");
						PgbsonWriterAppendUtf8(&roleWriter, "db", strlen("db"), "admin");
						PgbsonArrayWriterWriteDocument(&roleArrayWriter,
													   PgbsonWriterGetPgbson(
														   &roleWriter));
						PgbsonWriterEndArray(&userWriter, &roleArrayWriter);
					}
					else
					{
						pgbson_array_writer roleArrayWriter;
						PgbsonWriterStartArray(&userWriter, "roles", strlen("roles"),
											   &roleArrayWriter);
						pgbson_writer roleWriter;
						PgbsonWriterInit(&roleWriter);
						PgbsonWriterAppendUtf8(&roleWriter, "role", strlen("role"),
											   "readWriteAnyDatabase");
						PgbsonWriterAppendUtf8(&roleWriter, "db", strlen("db"), "admin");
						PgbsonArrayWriterWriteDocument(&roleArrayWriter,
													   PgbsonWriterGetPgbson(
														   &roleWriter));
						PgbsonWriterInit(&roleWriter);
						PgbsonWriterAppendUtf8(&roleWriter, "role", strlen("role"),
											   "clusterAdmin");
						PgbsonWriterAppendUtf8(&roleWriter, "db", strlen("db"), "admin");
						PgbsonArrayWriterWriteDocument(&roleArrayWriter,
													   PgbsonWriterGetPgbson(
														   &roleWriter));
						PgbsonWriterEndArray(&userWriter, &roleArrayWriter);
					}
				}
			}

			if (isUserExternal)
			{
				PgbsonWriterAppendDocument(&userWriter, "customData", 10,
										   GetUserInfoFromExternalIdentityProvider(user));
			}

			PgbsonArrayWriterWriteDocument(&userArrayWriter, PgbsonWriterGetPgbson(
											   &userWriter));
		}

		PgbsonWriterEndArray(&finalWriter, &userArrayWriter);
	}

	PgbsonWriterAppendInt32(&finalWriter, "ok", 2, 1);
	pgbson *result = PgbsonWriterGetPgbson(&finalWriter);
	PG_RETURN_POINTER(result);
}


static char *
ParseGetUserSpec(pgbson *getSpec)
{
	bson_iter_t getIter;
	PgbsonInitIterator(getSpec, &getIter);

	while (bson_iter_next(&getIter))
	{
		const char *key = bson_iter_key(&getIter);
		if (strcmp(key, "usersInfo") == 0)
		{
			if (bson_iter_type(&getIter) == BSON_TYPE_INT32)
			{
				if (bson_iter_as_int64(&getIter) == 1)
				{
					return NULL;
				}
				else
				{
					ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
									errmsg("Unsupported value for 'usersInfo' field.")));
				}
			}
			else if (bson_iter_type(&getIter) == BSON_TYPE_UTF8)
			{
				uint32_t strLength = 0;
				return (char *) bson_iter_utf8(&getIter, &strLength);
			}
			else if (bson_iter_type(&getIter) == BSON_TYPE_DOCUMENT)
			{
				const bson_value_t usersInfoBson = *bson_iter_value(&getIter);
				bson_iter_t iter;
				BsonValueInitIterator(&usersInfoBson, &iter);

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
						return (char *) bson_iter_utf8(&getIter, &strLength);
					}
				}
			}
			else
			{
				ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
								errmsg("Unsupported value specified for 'usersInfo'.")));
			}
		}
		else if (strcmp(key, "forAllDBs") == 0)
		{
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

			return NULL;
		}
		else if (strcmp(key, "getUser") == 0)
		{
			EnsureTopLevelFieldType(key, &getIter, BSON_TYPE_UTF8);
			uint32_t strLength = 0;
			return (char *) bson_iter_utf8(&getIter, &strLength);
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

	ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE), errmsg(
						"'usersInfo' or 'forAllDBs' must be provided.")));
}


/*
 * This method is mostly copied from pg_be_scram_build_secret in PG. The only substantial change
 * is that we use a default salt length of 28 as opposed to 16 used by PG. This is to ensure
 * compatiblity with compass, legacy mongo shell, c drivers, php drivers.
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
						errmsg("Invalid value for salt length.")));
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
				 errmsg("could not generate random salt.")));
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


bool
IsUserNameInvalid(const char *userName)
{
	/* Split the blocked role prefix list */
	char *copyBlockList = pstrdup(BlockedRolePrefixList);
	char *token = strtok(copyBlockList, ",");
	while (token != NULL)
	{
		if (strncmp(userName, token, strlen(token)) == 0)
		{
			pfree(copyBlockList);
			return true;
		}
		token = strtok(NULL, ",");
	}
	pfree(copyBlockList);
	return false;
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
