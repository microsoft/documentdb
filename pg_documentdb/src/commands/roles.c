/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/commands/roles.c
 *
 * Implementation of role CRUD functions.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "utils/documentdb_errors.h"
#include "utils/query_utils.h"
#include "commands/commands_common.h"
#include "commands/parse_error.h"
#include "utils/feature_counter.h"
#include "metadata/metadata_cache.h"
#include "api_hooks_def.h"
#include "users.h"
#include "api_hooks.h"
#include "utils/list_utils.h"
#include "roles.h"
#include "utils/elog.h"
#include "utils/array.h"
#include "utils/hashset_utils.h"

/* GUC to enable user crud operations */
extern bool EnableRoleCrud;

PG_FUNCTION_INFO_V1(command_create_role);
PG_FUNCTION_INFO_V1(command_drop_role);
PG_FUNCTION_INFO_V1(command_roles_info);
PG_FUNCTION_INFO_V1(command_update_role);

static void ParseCreateRoleSpec(pgbson *createRoleBson, CreateRoleSpec *createRoleSpec);
static void ParseRolesInfoSpec(pgbson *rolesInfoBson, RolesInfoSpec *rolesInfoSpec);
static void ParseRoleDocument(bson_iter_t *rolesArrayIter, RolesInfoSpec *rolesInfoSpec);
static void ParseRoleDefinition(bson_iter_t *iter, RolesInfoSpec *rolesInfoSpec);

/*
 * Parses a createRole spec, executes the createRole command, and returns the result.
 */
Datum
command_create_role(PG_FUNCTION_ARGS)
{
	pgbson *createRoleSpec = PG_GETARG_PGBSON(0);

	Datum response = create_role(createRoleSpec);

	PG_RETURN_DATUM(response);
}


/*
 * Implements dropRole command.
 */
Datum
command_drop_role(PG_FUNCTION_ARGS)
{
	ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_COMMANDNOTSUPPORTED),
					errmsg("DropRole command is not supported in preview."),
					errdetail_log("DropRole command is not supported in preview.")));
}


/*
 * Implements rolesInfo command, which will be implemented in the future.
 */
Datum
command_roles_info(PG_FUNCTION_ARGS)
{
	pgbson *rolesInfoSpec = PG_GETARG_PGBSON(0);

	Datum response = roles_info(rolesInfoSpec);

	PG_RETURN_DATUM(response);
}


/*
 * Implements updateRole command, which will be implemented in the future.
 */
Datum
command_update_role(PG_FUNCTION_ARGS)
{
	ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_COMMANDNOTSUPPORTED),
					errmsg("UpdateRole command is not supported in preview."),
					errdetail_log("UpdateRole command is not supported in preview.")));
}


/*
 * create_role implements the core logic for createRole command
 */
Datum
create_role(pgbson *createRoleBson)
{
	if (!EnableRoleCrud)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_COMMANDNOTSUPPORTED),
						errmsg("The CreateRole command is currently unsupported."),
						errdetail_log(
							"The CreateRole command is currently unsupported.")));
	}

	ReportFeatureUsage(FEATURE_ROLE_CREATE);

	if (!IsMetadataCoordinator())
	{
		StringInfo createRoleQuery = makeStringInfo();
		appendStringInfo(createRoleQuery,
						 "SELECT %s.create_role(%s::%s.bson)",
						 ApiSchemaNameV2,
						 quote_literal_cstr(PgbsonToHexadecimalString(createRoleBson)),
						 CoreSchemaNameV2);
		DistributedRunCommandResult result = RunCommandOnMetadataCoordinator(
			createRoleQuery->data);

		if (!result.success)
		{
			ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_INTERNALERROR),
							errmsg(
								"Create role operation failed: %s",
								text_to_cstring(result.response)),
							errdetail_log(
								"Create role operation failed: %s",
								text_to_cstring(result.response))));
		}

		pgbson_writer finalWriter;
		PgbsonWriterInit(&finalWriter);
		PgbsonWriterAppendInt32(&finalWriter, "ok", 2, 1);
		return PointerGetDatum(PgbsonWriterGetPgbson(&finalWriter));
	}

	CreateRoleSpec createRoleSpec = { NULL, NIL };
	ParseCreateRoleSpec(createRoleBson, &createRoleSpec);

	/* Validate that at least one inherited role is specified */
	if (list_length(createRoleSpec.inheritedBuiltInRoles) == 0)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
						errmsg(
							"At least one inherited role must be specified in 'roles' array.")));
	}

	/* Create the specified role in the database */
	StringInfo createRoleInfo = makeStringInfo();
	appendStringInfo(createRoleInfo, "CREATE ROLE %s", quote_identifier(
						 createRoleSpec.roleName));

	bool readOnly = false;
	bool isNull = false;
	ExtensionExecuteQueryViaSPI(createRoleInfo->data, readOnly, SPI_OK_UTILITY, &isNull);

	/* Grant inherited roles to the new role */
	ListCell *currentRole;
	foreach(currentRole, createRoleSpec.inheritedBuiltInRoles)
	{
		const char *inheritedRole = (const char *) lfirst(currentRole);

		if (!IS_SUPPORTED_BUILTIN_ROLE(inheritedRole))
		{
			ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_ROLENOTFOUND),
							errmsg("Role '%s' not supported.",
								   inheritedRole)));
		}

		StringInfo grantRoleInfo = makeStringInfo();
		appendStringInfo(grantRoleInfo, "GRANT %s TO %s",
						 quote_identifier(inheritedRole),
						 quote_identifier(createRoleSpec.roleName));

		ExtensionExecuteQueryViaSPI(grantRoleInfo->data, readOnly, SPI_OK_UTILITY,
									&isNull);
	}

	pgbson_writer finalWriter;
	PgbsonWriterInit(&finalWriter);
	PgbsonWriterAppendInt32(&finalWriter, "ok", 2, 1);
	return PointerGetDatum(PgbsonWriterGetPgbson(&finalWriter));
}


/*
 * drop_role implements the core logic for dropRole command
 */
Datum
drop_role(pgbson *dropRoleBson)
{
	ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_COMMANDNOTSUPPORTED),
					errmsg("The DropRole command is currently unsupported."),
					errdetail_log("The DropRole command is currently unsupported.")));
}


/*
 * roles_info implements the core logic for rolesInfo command
 */
Datum
roles_info(pgbson *rolesInfoBson)
{
	if (!EnableRoleCrud)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_COMMANDNOTSUPPORTED),
						errmsg("RolesInfo command is not supported."),
						errdetail_log("RolesInfo command is not supported.")));
	}

	if (!IsMetadataCoordinator())
	{
		StringInfo rolesInfoQuery = makeStringInfo();
		appendStringInfo(rolesInfoQuery,
						 "SELECT %s.roles_info(%s::%s.bson)",
						 ApiSchemaNameV2,
						 quote_literal_cstr(PgbsonToHexadecimalString(rolesInfoBson)),
						 CoreSchemaNameV2);
		DistributedRunCommandResult result = RunCommandOnMetadataCoordinator(
			rolesInfoQuery->data);

		if (!result.success)
		{
			ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_INTERNALERROR),
							errmsg(
								"Roles info operation failed: %s",
								text_to_cstring(result.response)),
							errdetail_log(
								"Roles info operation failed: %s",
								text_to_cstring(result.response))));
		}

		pgbson_writer finalWriter;
		PgbsonWriterInit(&finalWriter);
		PgbsonWriterAppendInt32(&finalWriter, "ok", 2, 1);
		return PointerGetDatum(PgbsonWriterGetPgbson(&finalWriter));
	}

	RolesInfoSpec rolesInfoSpec = { NIL, false, false, false };
	ParseRolesInfoSpec(rolesInfoBson, &rolesInfoSpec);

	pgbson_writer finalWriter;
	PgbsonWriterInit(&finalWriter);

	pgbson_array_writer rolesArrayWriter;
	PgbsonWriterStartArray(&finalWriter, "roles", 5, &rolesArrayWriter);

	PgbsonWriterEndArray(&finalWriter, &rolesArrayWriter);
	PgbsonWriterAppendInt32(&finalWriter, "ok", 2, 1);

	return PointerGetDatum(PgbsonWriterGetPgbson(&finalWriter));
}


/*
 * ParseCreateRoleSpec parses the createRole command parameters
 */
static void
ParseCreateRoleSpec(pgbson *createRoleBson, CreateRoleSpec *createRoleSpec)
{
	bson_iter_t createRoleIter;
	PgbsonInitIterator(createRoleBson, &createRoleIter);

	while (bson_iter_next(&createRoleIter))
	{
		const char *key = bson_iter_key(&createRoleIter);

		if (strcmp(key, "createRole") == 0)
		{
			EnsureTopLevelFieldType(key, &createRoleIter, BSON_TYPE_UTF8);
			uint32_t strLength = 0;
			createRoleSpec->roleName = bson_iter_utf8(&createRoleIter, &strLength);

			if (strLength == 0)
			{
				ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
								errmsg("'createRole' cannot be empty.")));
			}

			if (IsUserNameInvalid(createRoleSpec->roleName))
			{
				ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
								errmsg("Invalid role name, use a different role name.")));
			}
		}
		else if (strcmp(key, "roles") == 0)
		{
			if (bson_iter_type(&createRoleIter) == BSON_TYPE_ARRAY)
			{
				bson_iter_t rolesArrayIter;
				bson_iter_recurse(&createRoleIter, &rolesArrayIter);

				while (bson_iter_next(&rolesArrayIter))
				{
					if (bson_iter_type(&rolesArrayIter) == BSON_TYPE_UTF8)
					{
						uint32_t roleNameLength = 0;
						const char *inheritedBuiltInRole = bson_iter_utf8(&rolesArrayIter,
																		  &roleNameLength);

						if (roleNameLength > 0)
						{
							createRoleSpec->inheritedBuiltInRoles = lappend(
								createRoleSpec->inheritedBuiltInRoles,
								pstrdup(
									inheritedBuiltInRole));
						}
					}
					else
					{
						ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
										errmsg(
											"Invalid inherited from role name provided.")));
					}
				}
			}
			else
			{
				ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
								errmsg("'roles' must be an array, not %s",
									   BsonTypeName(bson_iter_type(&createRoleIter)))));
			}
		}
		else if (IsCommonSpecIgnoredField(key))
		{
			continue;
		}
		else
		{
			ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
							errmsg("Unsupported field specified: '%s'.", key)));
		}
	}

	if (createRoleSpec->roleName == NULL)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
						errmsg("'createRole' is a required field.")));
	}
}


/*
 * ParseRolesInfoSpec parses the rolesInfo command parameters
 */
static void
ParseRolesInfoSpec(pgbson *rolesInfoBson, RolesInfoSpec *rolesInfoSpec)
{
	bson_iter_t rolesInfoIter;
	PgbsonInitIterator(rolesInfoBson, &rolesInfoIter);

	rolesInfoSpec->roleNames = NIL;
	rolesInfoSpec->showAllRoles = false;
	rolesInfoSpec->showBuiltInRoles = false;
	rolesInfoSpec->showPrivileges = false;
	bool rolesInfoFound = false;
	while (bson_iter_next(&rolesInfoIter))
	{
		const char *key = bson_iter_key(&rolesInfoIter);

		if (strcmp(key, "rolesInfo") == 0)
		{
			rolesInfoFound = true;
			if (bson_iter_type(&rolesInfoIter) == BSON_TYPE_INT32)
			{
				int32_t value = bson_iter_int32(&rolesInfoIter);
				if (value == 1)
				{
					rolesInfoSpec->showAllRoles = true;
				}
				else
				{
					ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
									errmsg(
										"'rolesInfo' must be 1, a string, a document, or an array.")));
				}
			}
			else if (bson_iter_type(&rolesInfoIter) == BSON_TYPE_ARRAY)
			{
				bson_iter_t rolesArrayIter;
				bson_iter_recurse(&rolesInfoIter, &rolesArrayIter);

				while (bson_iter_next(&rolesArrayIter))
				{
					ParseRoleDefinition(&rolesArrayIter, rolesInfoSpec);
				}
			}
			else
			{
				ParseRoleDefinition(&rolesInfoIter, rolesInfoSpec);
			}
		}
		else if (strcmp(key, "showBuiltInRoles") == 0)
		{
			if (BSON_ITER_HOLDS_BOOL(&rolesInfoIter))
			{
				rolesInfoSpec->showBuiltInRoles = bson_iter_as_bool(&rolesInfoIter);
			}
			else
			{
				ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
								errmsg(
									"'showBuiltInRoles' must be a boolean value")));
			}
		}
		else if (strcmp(key, "showPrivileges") == 0)
		{
			if (BSON_ITER_HOLDS_BOOL(&rolesInfoIter))
			{
				rolesInfoSpec->showPrivileges = bson_iter_as_bool(&rolesInfoIter);
			}
			else
			{
				ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
								errmsg(
									"'showPrivileges' must be a boolean value")));
			}
		}
		else if (IsCommonSpecIgnoredField(key))
		{
			continue;
		}
		else
		{
			ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
							errmsg("Unsupported field specified: '%s'.", key)));
		}
	}

	if (!rolesInfoFound)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
						errmsg("'rolesInfo' is a required field.")));
	}
}


/*
 * Helper function to parse a role definition (string or document)
 */
static void
ParseRoleDefinition(bson_iter_t *iter, RolesInfoSpec *rolesInfoSpec)
{
	if (bson_iter_type(iter) == BSON_TYPE_UTF8)
	{
		uint32_t roleNameLength = 0;
		const char *roleName = bson_iter_utf8(iter, &roleNameLength);

		/* If the string is empty, we will not add it to the list of roles to fetched */
		if (roleNameLength > 0)
		{
			rolesInfoSpec->roleNames = lappend(rolesInfoSpec->roleNames, pstrdup(
												   roleName));
		}
	}
	else if (bson_iter_type(iter) == BSON_TYPE_DOCUMENT)
	{
		ParseRoleDocument(iter, rolesInfoSpec);
	}
	else
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
						errmsg(
							"'rolesInfo' must be 1, a string, a document, or an array.")));
	}
}


/*
 * Helper function to parse a role document from an array element or single document
 */
static void
ParseRoleDocument(bson_iter_t *rolesArrayIter, RolesInfoSpec *rolesInfoSpec)
{
	bson_iter_t roleDocIter;
	bson_iter_recurse(rolesArrayIter, &roleDocIter);

	const char *roleName = NULL;
	uint32_t roleNameLength = 0;
	const char *dbName = NULL;
	uint32_t dbNameLength = 0;

	while (bson_iter_next(&roleDocIter))
	{
		const char *roleKey = bson_iter_key(&roleDocIter);

		if (strcmp(roleKey, "role") == 0)
		{
			if (bson_iter_type(&roleDocIter) != BSON_TYPE_UTF8)
			{
				ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
								errmsg("'role' field must be a string.")));
			}

			roleName = bson_iter_utf8(&roleDocIter, &roleNameLength);
		}

		/* db is required as part of every role document. */
		else if (strcmp(roleKey, "db") == 0)
		{
			if (bson_iter_type(&roleDocIter) != BSON_TYPE_UTF8)
			{
				ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
								errmsg("'db' field must be a string.")));
			}

			dbName = bson_iter_utf8(&roleDocIter, &dbNameLength);

			if (strcmp(dbName, "admin") != 0)
			{
				ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
								errmsg(
									"Unsupported value specified for db. Only 'admin' is allowed.")));
			}
		}
		else
		{
			ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
							errmsg("Unknown property '%s' in role document.", roleKey)));
		}
	}

	if (roleName == NULL || dbName == NULL)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
						errmsg("'role' and 'db' are required fields.")));
	}

	/* Only add role to the list if both role name and db name have valid lengths */
	if (roleNameLength > 0 && dbNameLength > 0)
	{
		rolesInfoSpec->roleNames = lappend(rolesInfoSpec->roleNames, pstrdup(roleName));
	}
}
