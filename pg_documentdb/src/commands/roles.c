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
#include <utils/elog.h>

/* GUC to enable user crud operations */
extern bool EnableRoleCrud;

PG_FUNCTION_INFO_V1(command_create_role);
PG_FUNCTION_INFO_V1(command_drop_role);
PG_FUNCTION_INFO_V1(command_roles_info);
PG_FUNCTION_INFO_V1(command_update_role);

static void ParseCreateRoleSpec(pgbson *createRoleBson, CreateRoleSpec *createRoleSpec);
static void ValidateInheritedRole(const char *roleName);

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
 * Implements dropRole command, which will be implemented in the future.
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
	ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_COMMANDNOTSUPPORTED),
					errmsg("RolesInfo command is not supported in preview."),
					errdetail_log("RolesInfo command is not supported in preview.")));
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
						errmsg("CreateRole command is not supported."),
						errdetail_log("CreateRole command is not supported.")));
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
								"Create role operation failed. Error details: Metadata coordinator %s failure. Open a support request.",
								text_to_cstring(result.response)),
							errdetail_log(
								"Create role operation failed. Error details: Metadata coordinator %s failure. Open a support request.",
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

	/* Create the role */
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

		/* Validate that the inherited role is supported */
		ValidateInheritedRole(inheritedRole);

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
					errmsg("DropRole command is not supported."),
					errdetail_log("DropRole command is not supported.")));
}


/*
 * roles_info implements the core logic for rolesInfo command
 */
Datum
roles_info(pgbson *rolesInfoBson)
{
	ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_COMMANDNOTSUPPORTED),
					errmsg("RolesInfo command is not supported."),
					errdetail_log("RolesInfo command is not supported.")));
}


/*
 * update_role implements the core logic for updateRole command
 */
Datum
update_role(pgbson *updateRoleBson)
{
	ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_COMMANDNOTSUPPORTED),
					errmsg("UpdateRole command is not supported."),
					errdetail_log("UpdateRole command is not supported.")));
}


/*
 * ValidateInheritedRole validates that the given role name is supported
 */
static void
ValidateInheritedRole(const char *roleName)
{
	if (strcmp(roleName, ApiAdminRoleV2) != 0 &&
		strcmp(roleName, ApiReadOnlyRole) != 0)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_ROLENOTFOUND),
						errmsg("Role '%s' not found or not supported.",
							   roleName)));
	}
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
						uint32_t roleStrLength = 0;
						const char *inheritedBuiltInRole = bson_iter_utf8(&rolesArrayIter,
																		  &roleStrLength);

						if (roleStrLength > 0)
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
		else if (strcmp(key, "db") == 0 || strcmp(key, "$db") == 0)
		{
			EnsureTopLevelFieldType(key, &createRoleIter, BSON_TYPE_UTF8);
			uint32_t strLength = 0;
			const char *db = bson_iter_utf8(&createRoleIter, &strLength);
			if (strcmp(db, "admin") != 0)
			{
				ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE), errmsg(
									"Unsupported value specified for db. Only 'admin' is allowed.")));
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
