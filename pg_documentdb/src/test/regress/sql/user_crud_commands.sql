SET documentdb.next_collection_id TO 1972800;
SET documentdb.next_collection_index_id TO 1972800;

--set Feature flag for user crud
SET documentdb.enableUserCrud TO ON;
\set VERBOSITY TERSE

show documentdb.blockedRolePrefixList;

SET documentdb.blockedRolePrefixList TO '';
SELECT documentdb_api.create_user('{"createUser":"documentdb_unblocked", "pwd":"test_password", "roles":[{"role":"readWriteAnyDatabase","db":"admin"}, {"role":"clusterAdmin","db":"admin"}]}');
SELECT documentdb_api.drop_user('{"dropUser":"documentdb_unblocked"}');
SET documentdb.blockedRolePrefixList TO 'documentdb';
SELECT documentdb_api.drop_user('{"dropUser":"documentdb_unblocked"}');

SET documentdb.blockedRolePrefixList TO 'newBlock, newBlock2';
SELECT documentdb_api.create_user('{"createUser":"newBlock_user", "pwd":"test_password", "roles":[{"role":"readWriteAnyDatabase","db":"admin"}, {"role":"clusterAdmin","db":"admin"}]}');
SELECT documentdb_api.create_user('{"createUser":"newBlock2_user", "pwd":"test_password", "roles":[{"role":"readWriteAnyDatabase","db":"admin"}, {"role":"clusterAdmin","db":"admin"}]}');

RESET documentdb.blockedRolePrefixList;

--Create a readOnly user
SELECT documentdb_api.create_user('{"createUser":"test_user", "pwd":"test_password", "roles":[{"role":"readAnyDatabase","db":"admin"}]}');

--Verify that the user is created
SELECT documentdb_api.users_info('{"usersInfo":"test_user"}');

--Create an admin user
SELECT documentdb_api.create_user('{"createUser":"test_user2", "pwd":"test_password", "roles":[{"role":"readWriteAnyDatabase","db":"admin"}, {"role":"clusterAdmin","db":"admin"}]}');

--Verify that the user is created
SELECT documentdb_api.users_info('{"usersInfo":"test_user2"}');

--Create a user with a blocked prefix
SELECT documentdb_api.create_user('{"createUser":"documentdb_user2", "pwd":"test_password", "roles":[{"role":"readWriteAnyDatabase","db":"admin"}, {"role":"clusterAdmin","db":"admin"}]}');

--Create an already existing user
SELECT documentdb_api.create_user('{"createUser":"test_user2", "pwd":"test_password", "roles":[{"role":"readWriteAnyDatabase","db":"admin"}, {"role":"clusterAdmin","db":"admin"}]}');

--Create a user with no role
SELECT documentdb_api.create_user('{"createUser":"test_user4", "pwd":"test_password", "roles":[]}');

--Create a user without specifying role parameter
SELECT documentdb_api.create_user('{"createUser":"test_user4", "pwd":"test_password"}');

--Create a user with a disallowed parameter
SELECT documentdb_api.create_user('{"createUser":"test_user", "pwd":"test_password", "roles":[{"role":"readAnyDatabase","db":"admin"}], "testParam":"This is a test" }');

--Create a user with a disallowed DB
SELECT documentdb_api.create_user('{"createUser":"test_user4", "pwd":"test_password", "roles":[{"role":"readWriteAnyDatabase","db":"Test"}, {"role":"clusterAdmin","db":"admin"}]}');

--Create a user with a disallowed role
SELECT documentdb_api.create_user('{"createUser":"test_user4", "pwd":"test_password", "roles":[{"role":"read","db":"admin"}, {"role":"clusterAdmin","db":"admin"}]}');

--Create a user with no DB
SELECT documentdb_api.create_user('{"createUser":"test_user4", "pwd":"test_password", "roles":[{"role":"readWriteAnyDatabase"}, {"role":"clusterAdmin"}]}');

-- Create a user with an empty password should fail
SELECT documentdb_api.create_user('{"createUser":"test_user_empty_pwd", "pwd":"", "roles":[{"role":"readAnyDatabase","db":"admin"}]}');

-- Create a user with password less than 8 characters and drop it
SELECT documentdb_api.create_user('{"createUser":"test_user_short_pwd", "pwd":"Short1!", "roles":[{"role":"readAnyDatabase","db":"admin"}]}');
SELECT documentdb_api.drop_user('{"dropUser":"test_user_short_pwd"}');

-- Create a user with password more than 256 characters and drop it
SELECT documentdb_api.create_user('{"createUser":"test_user_long_pwd", "pwd":"ThisIsAVeryLongPasswordThatExceedsTheTwoHundredFiftySixCharacterLimitAndThereforeShouldFailValidation1!abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", "roles":[{"role":"readAnyDatabase","db":"admin"}]}');
SELECT documentdb_api.drop_user('{"dropUser":"test_user_long_pwd"}');

--Verify that the user is created
SELECT documentdb_api.users_info('{"usersInfo":"test_user4"}');

--Create a user with no DB in just one role
SELECT documentdb_api.create_user('{"createUser":"test_user5", "pwd":"test_password", "roles":[{"role":"readWriteAnyDatabase"}, {"role":"clusterAdmin","db":"admin"}]}');

--Verify that the user is created
SELECT documentdb_api.users_info('{"usersInfo":"test_user5"}');

--Create a user with no parameters at all
SELECT documentdb_api.create_user('{}');

--Get all users
SELECT documentdb_api.users_info('{"usersInfo":1}');

--Get all users for all DBs
SELECT documentdb_api.users_info('{"forAllDBs":true}');

--Test SQL injection attack
SELECT documentdb_api.create_user('{"createUser":"test_user_injection_attack", "pwd":"; DROP TABLE users; --", "roles":[{"role":"readAnyDatabase","db":"admin"}]}');

--Verify that the user is created
SELECT documentdb_api.users_info('{"usersInfo":"test_user_injection_attack"}');

--Get all users
SELECT documentdb_api.users_info('{"usersInfo":1}');

--Drop a user
SELECT documentdb_api.drop_user('{"dropUser":"test_user5"}');

--Verify successful drop
SELECT documentdb_api.users_info('{"usersInfo":"test_user5"}');

--Drop a reserved user, should fail
SELECT documentdb_api.drop_user('{"dropUser":"documentdb_user"}');

--Drop non-existent user
SELECT documentdb_api.drop_user('{"dropUser":"nonexistent_user"}');

--Drop with disallowed parameter
SELECT documentdb_api.drop_user('{"user":"test_user"}');

--Update a user
SELECT documentdb_api.update_user('{"updateUser":"test_user", "pwd":"new_password"}');

--Update non existent user
SELECT documentdb_api.update_user('{"updateUser":"nonexistent_user", "pwd":"new_password"}');

--Update with disllowed parameter
SELECT documentdb_api.update_user('{"updateUser":"test_user", "pwd":"new_password", "roles":[{"role":"readAnyDatabase","db":"admin"}]}');

--Get non existent user
SELECT documentdb_api.users_info('{"usersInfo":"nonexistent_user"}');

--Create a readOnly user
SELECT documentdb_api.create_user('{"createUser":"readOnlyUser", "pwd":"test_password", "roles":[{"role":"readAnyDatabase","db":"admin"}]}');

--Create an admin user
SELECT documentdb_api.create_user('{"createUser":"adminUser", "pwd":"test_password", "roles":[{"role":"readWriteAnyDatabase","db":"admin"}, {"role":"clusterAdmin","db":"admin"}]}');

--Verify that we error out for external identity provider
SELECT documentdb_api.create_user('{"createUser":"bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb", "roles":[{"role":"readWriteAnyDatabase","db":"admin"}, {"role":"clusterAdmin","db":"admin"}], "customData":{"IdentityProvider" : {"type" : "ExternalProvider", "properties": {"principalType": "servicePrincipal"}}}}');

SELECT current_user as original_user \gset

-- switch to read only user
\c regression readOnlyUser

--set Feature flag for user crud
SET documentdb.enableUserCrud TO ON;

--Create without privileges
SELECT documentdb_api.create_user('{"createUser":"newUser", "pwd":"test_password", "roles":[{"role":"readAnyDatabase","db":"admin"}]}');

--Drop without privileges
SELECT documentdb_api.drop_user('{"dropUser":"test_user"}');

-- switch to admin user
\c regression adminUser

--set Feature flag for user crud
SET documentdb.enableUserCrud TO ON;

--Create without privileges
SELECT documentdb_api.create_user('{"createUser":"newUser", "pwd":"test_password", "roles":[{"role":"readAnyDatabase","db":"admin"}]}');

--Drop without privileges
SELECT documentdb_api.drop_user('{"dropUser":"test_user"}');

--set Feature flag for user crud OFF
SET documentdb.enableUserCrud TO OFF;

--All user crud commnads should fail
SELECT documentdb_api.create_user('{"createUser":"test_user", "pwd":"test_password", "roles":[{"role":"readAnyDatabase","db":"admin"}]}');
SELECT documentdb_api.users_info('{"usersInfo":1}');
SELECT documentdb_api.update_user('{"updateUser":"test_user", "pwd":"new_password"}');
SELECT documentdb_api.drop_user('{"dropUser":"test_user5"}');

-- switch back to original user
\c regression :original_user

--set Feature flag for user crud
SET documentdb.enableUserCrud TO ON;
\set VERBOSITY TERSE
 
 --set max user limit to 11
SET documentdb.maxUserLimit TO 11;

-- Keep creating users till we have 11 users
SELECT documentdb_api.create_user('{"createUser":"newUser7", "pwd":"test_password", "roles":[{"role":"readAnyDatabase","db":"admin"}]}');
SELECT documentdb_api.create_user('{"createUser":"newUser8", "pwd":"test_password", "roles":[{"role":"readAnyDatabase","db":"admin"}]}');
SELECT documentdb_api.create_user('{"createUser":"newUser9", "pwd":"test_password", "roles":[{"role":"readAnyDatabase","db":"admin"}]}');
SELECT documentdb_api.create_user('{"createUser":"newUser10", "pwd":"test_password", "roles":[{"role":"readAnyDatabase","db":"admin"}]}');
SELECT documentdb_api.create_user('{"createUser":"newUser11", "pwd":"test_password", "roles":[{"role":"readAnyDatabase","db":"admin"}]}');

-- This call should fail since we're only allowed to create 11 users
SELECT documentdb_api.create_user('{"createUser":"newUser12", "pwd":"test_password", "roles":[{"role":"readAnyDatabase","db":"admin"}]}');

-- Increase allowed users to 12
SET documentdb.maxUserLimit TO 12;

-- This should now succeed
SELECT documentdb_api.create_user('{"createUser":"newUser12", "pwd":"test_password", "roles":[{"role":"readAnyDatabase","db":"admin"}]}');

-- This should fail
SELECT documentdb_api.create_user('{"createUser":"newUser13", "pwd":"test_password", "roles":[{"role":"readAnyDatabase","db":"admin"}]}');

-- Delete a user and try to create again, this should succeed
SELECT documentdb_api.drop_user('{"dropUser":"newUser7"}');
SELECT documentdb_api.create_user('{"createUser":"newUser13", "pwd":"test_password", "roles":[{"role":"readAnyDatabase","db":"admin"}]}');

-- Delete all users created so we don't break other tests that also create users
SELECT documentdb_api.drop_user('{"dropUser":"newUser8"}');
SELECT documentdb_api.drop_user('{"dropUser":"newUser9"}');
SELECT documentdb_api.drop_user('{"dropUser":"newUser10"}');
SELECT documentdb_api.drop_user('{"dropUser":"newUser11"}');
SELECT documentdb_api.drop_user('{"dropUser":"newUser12"}');
SELECT documentdb_api.drop_user('{"dropUser":"newUser13"}');
SELECT documentdb_api.drop_user('{"dropUser":"test_user"}');
SELECT documentdb_api.drop_user('{"dropUser":"test_user2"}');
SELECT documentdb_api.drop_user('{"dropUser":"test_user4"}');
SELECT documentdb_api.drop_user('{"dropUser":"test_user_injection_attack"}');
SELECT documentdb_api.drop_user('{"dropUser":"readOnlyUser"}');
SELECT documentdb_api.drop_user('{"dropUser":"adminUser"}');

-- Reset the max user limit to 10
SET documentdb.maxUserLimit TO 10;