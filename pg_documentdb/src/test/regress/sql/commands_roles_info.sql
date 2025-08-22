SET documentdb.next_collection_id TO 1983000;
SET documentdb.next_collection_index_id TO 1983000;

SET documentdb.maxUserLimit TO 10;
\set VERBOSITY TERSE

-- Enable role CRUD operations for testing
SET documentdb.enableRoleCrud TO ON;

-- ********* Test rolesInfo with int value *********
-- Test rolesInfo with 1
SELECT documentdb_api.roles_info('{"rolesInfo":1, "$db":"admin"}');

-- Test rolesInfo with int value other than 1, which is not allowed
SELECT documentdb_api.roles_info('{"rolesInfo":0, "$db":"admin"}');

-- Test rolesInfo with showPrivileges
SELECT documentdb_api.roles_info('{"rolesInfo":1, "showPrivileges":true, "$db":"admin"}');

-- Test rolesInfo with invalid showPrivileges type
SELECT documentdb_api.roles_info('{"rolesInfo":1, "showPrivileges":1, "$db":"admin"}');

-- Test rolesInfo with showBuiltInRoles
SELECT documentdb_api.roles_info('{"rolesInfo":1, "showBuiltInRoles":true, "$db":"admin"}');

-- Test rolesInfo with invalid showBuiltInRoles type
SELECT documentdb_api.roles_info('{"rolesInfo":1, "showBuiltInRoles":1, "$db":"admin"}');

-- Test rolesInfo with unsupported field at root level
SELECT documentdb_api.roles_info('{"rolesInfo":1, "unsupportedField":"value", "$db":"admin"}');

-- Test rolesInfo with ignorable field at root level
SELECT documentdb_api.roles_info('{"rolesInfo":1, "lsid":"value", "$db":"admin"}');

-- Test rolesInfo with missing mandatory rolesInfo field
SELECT documentdb_api.roles_info('{"$db":"admin"}');

-- Test rolesInfo with null value
SELECT documentdb_api.roles_info('{"rolesInfo":null, "$db":"admin"}');

-- ********* Test rolesInfo with string value *********
-- Test rolesInfo with string value of built-in role
-- which also doesn't require showBuiltInRoles since it's only an addon flag for rolesInfo:1
SELECT documentdb_api.roles_info('{"rolesInfo":"readAnyDatabase", "showPrivileges":true, "$db":"admin"}');

-- Test rolesInfo with string value of custom role
SELECT documentdb_api.roles_info('{"rolesInfo":"admin_db_custom_role", "showPrivileges":true, "$db":"admin"}');

-- Test rolesInfo with string value of not-exist role
SELECT documentdb_api.roles_info('{"rolesInfo":"not_exist_role", "$db":"admin"}');

-- Test rolesInfo with empty string value, which is the same as specifying a not-exist role
SELECT documentdb_api.roles_info('{"rolesInfo":"", "$db":"admin"}');

-- ********* Test rolesInfo with role document *********
-- Test rolesInfo with basic role document
SELECT documentdb_api.roles_info('{"rolesInfo": {"role":"readAnyDatabase", "db":"admin"}, "$db":"admin"}');

-- Test rolesInfo with bson document with empty role or db
SELECT documentdb_api.roles_info('{"rolesInfo": {"role":"", "db":""}, "$db":"admin"}');

-- Test rolesInfo with bson document with miss db which is not allowed
SELECT documentdb_api.roles_info('{"rolesInfo": {"role":"readAnyDatabase"}, "$db":"admin"}');

-- Test rolesInfo with bson document with miss role which is not allowed
SELECT documentdb_api.roles_info('{"rolesInfo": {"db":"admin"}, "$db":"admin"}');

-- Test rolesInfo with bson document of invalid role field type
SELECT documentdb_api.roles_info('{"rolesInfo":{"role":1, "db":"admin"}, "$db":"admin"}');

-- Test rolesInfo with bson document of invalid role field type
SELECT documentdb_api.roles_info('{"rolesInfo":{"role":"readAnyDatabase", "db":1}, "$db":"admin"}');

-- Test rolesInfo with bson document with unknown field, which is not allowed
SELECT documentdb_api.roles_info('{"rolesInfo":{"role":"readAnyDatabase", "db":"admin", "unknownField":"value"}, "$db":"admin"}');

-- ********* Test rolesInfo with array *********
-- Test rolesInfo with empty array
SELECT documentdb_api.roles_info('{"rolesInfo":[], "$db":"admin"}');

-- Test rolesInfo with array of role names, including empty string
SELECT documentdb_api.roles_info('{"rolesInfo":["readAnyDatabase", "admin_db_custom_role", "not_exist_role", ""], "$db":"admin"}');

-- Test rolesInfo with array of mixed bson document and string
SELECT documentdb_api.roles_info('{"rolesInfo":[{"role":"readAnyDatabase", "db":"admin"}, "admin_db_custom_role"], "$db":"admin"}');

-- Reset settings
RESET documentdb.enableRoleCrud;
SET documentdb.enableRoleCrud TO ON;

-- Reset settings
RESET documentdb.enableRoleCrud;
