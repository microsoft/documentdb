/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * include/roles.h
 *
 * Role CRUD functions.
 *
 *-------------------------------------------------------------------------
 */

#ifndef EXTENSION_ROLES_H
#define EXTENSION_ROLES_H

#include "postgres.h"
#include "utils/string_view.h"

/* Macro to check if a role is a supported built-in role */
#define IS_SUPPORTED_BUILTIN_ROLE(roleName) \
	(strcmp((roleName), ApiAdminRoleV2) == 0 || \
	 strcmp((roleName), ApiReadOnlyRole) == 0 || \
	 strcmp((roleName), ApiReadWriteRole) == 0 || \
	 strcmp((roleName), ApiRootRole) == 0 || \
	 strcmp((roleName), ApiUserAdminRole) == 0)

typedef struct
{
	const char *roleName;
	List *inheritedBuiltInRoles;
} CreateRoleSpec;

typedef struct
{
	List *roleNames;
	bool showAllRoles;
	bool showBuiltInRoles;
	bool showPrivileges;
} RolesInfoSpec;

/* Method to create a role */
Datum create_role(pgbson *createRoleBson);

/* Method to drop a role */
Datum drop_role(pgbson *dropRoleBson);

/* Method to get roles information */
Datum roles_info(pgbson *rolesInfoBson);

/* Method to update a role */
Datum update_role(pgbson *updateRoleBson);

#endif
