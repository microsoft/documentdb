/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/configs/feature_flag_configs.c
 *
 * Initialization of GUCs that control feature flags that will eventually
 * become defaulted and simply toggle behavior.
 *-------------------------------------------------------------------------
 */

#include <postgres.h>
#include <miscadmin.h>
#include <utils/guc.h>
#include <limits.h>
#include "configs/config_initialization.h"


/*
 * SECTION: Top level feature flags
 */
#define DEFAULT_ENABLE_SCHEMA_VALIDATION false
bool EnableSchemaValidation =
	DEFAULT_ENABLE_SCHEMA_VALIDATION;

#define DEFAULT_ENABLE_BYPASSDOCUMENTVALIDATION false
bool EnableBypassDocumentValidation =
	DEFAULT_ENABLE_BYPASSDOCUMENTVALIDATION;

#define DEFAULT_ENABLE_USERNAME_PASSWORD_CONSTRAINTS true
bool EnableUsernamePasswordConstraints = DEFAULT_ENABLE_USERNAME_PASSWORD_CONSTRAINTS;

#define DEFAULT_ENABLE_USERS_INFO_PRIVILEGES true
bool EnableUsersInfoPrivileges = DEFAULT_ENABLE_USERS_INFO_PRIVILEGES;

#define DEFAULT_ENABLE_NATIVE_AUTHENTICATION true
bool IsNativeAuthEnabled = DEFAULT_ENABLE_NATIVE_AUTHENTICATION;

#define DEFAULT_ENABLE_ROLE_CRUD false
bool EnableRoleCrud = DEFAULT_ENABLE_ROLE_CRUD;

/*
 * SECTION: Vector Search flags
 */

/* GUC to enable HNSW index type and query for vector search. */
#define DEFAULT_ENABLE_VECTOR_HNSW_INDEX true
bool EnableVectorHNSWIndex = DEFAULT_ENABLE_VECTOR_HNSW_INDEX;

/* GUC to enable vector pre-filtering feature for vector search. */
#define DEFAULT_ENABLE_VECTOR_PRE_FILTER true
bool EnableVectorPreFilter = DEFAULT_ENABLE_VECTOR_PRE_FILTER;

#define DEFAULT_ENABLE_VECTOR_PRE_FILTER_V2 false
bool EnableVectorPreFilterV2 = DEFAULT_ENABLE_VECTOR_PRE_FILTER_V2;

#define DEFAULT_ENABLE_VECTOR_FORCE_INDEX_PUSHDOWN false
bool EnableVectorForceIndexPushdown = DEFAULT_ENABLE_VECTOR_FORCE_INDEX_PUSHDOWN;

/* GUC to enable vector compression for vector search. */
#define DEFAULT_ENABLE_VECTOR_COMPRESSION_HALF true
bool EnableVectorCompressionHalf = DEFAULT_ENABLE_VECTOR_COMPRESSION_HALF;

#define DEFAULT_ENABLE_VECTOR_COMPRESSION_PQ true
bool EnableVectorCompressionPQ = DEFAULT_ENABLE_VECTOR_COMPRESSION_PQ;

#define DEFAULT_ENABLE_VECTOR_CALCULATE_DEFAULT_SEARCH_PARAM true
bool EnableVectorCalculateDefaultSearchParameter =
	DEFAULT_ENABLE_VECTOR_CALCULATE_DEFAULT_SEARCH_PARAM;

/*
 * SECTION: Indexing feature flags
 */

#define DEFAULT_ENABLE_NEW_COMPOSITE_INDEX_OPCLASS true
bool EnableNewCompositeIndexOpclass = DEFAULT_ENABLE_NEW_COMPOSITE_INDEX_OPCLASS;

#define DEFAULT_USE_NEW_COMPOSITE_INDEX_OPCLASS false
bool DefaultUseCompositeOpClass = DEFAULT_USE_NEW_COMPOSITE_INDEX_OPCLASS;

#define DEFAULT_ENABLE_INDEX_ORDERBY_PUSHDOWN false
bool EnableIndexOrderbyPushdown = DEFAULT_ENABLE_INDEX_ORDERBY_PUSHDOWN;

#define DEFAULT_ENABLE_DESCENDING_COMPOSITE_INDEX true
bool EnableDescendingCompositeIndex = DEFAULT_ENABLE_DESCENDING_COMPOSITE_INDEX;

#define DEFAULT_ENABLE_COMPOSITE_UNIQUE_INDEX true
bool EnableCompositeUniqueIndex = DEFAULT_ENABLE_COMPOSITE_UNIQUE_INDEX;

/*
 * SECTION: Planner feature flags
 */
#define DEFAULT_ENABLE_NEW_OPERATOR_SELECTIVITY false
bool EnableNewOperatorSelectivityMode = DEFAULT_ENABLE_NEW_OPERATOR_SELECTIVITY;

#define DEFAULT_DISABLE_DOLLAR_FUNCTION_SELECTIVITY false
bool DisableDollarSupportFuncSelectivity = DEFAULT_DISABLE_DOLLAR_FUNCTION_SELECTIVITY;

/* Remove after v109 */

#define DEFAULT_LOOKUP_ENABLE_INNER_JOIN true
bool EnableLookupInnerJoin = DEFAULT_LOOKUP_ENABLE_INNER_JOIN;

#define DEFAULT_FORCE_BITMAP_SCAN_FOR_LOOKUP false
bool ForceBitmapScanForLookup = DEFAULT_FORCE_BITMAP_SCAN_FOR_LOOKUP;

#define DEFAULT_LOW_SELECTIVITY_FOR_LOOKUP true
bool LowSelectivityForLookup = DEFAULT_LOW_SELECTIVITY_FOR_LOOKUP;

/* Remove after v110 */
#define DEFAULT_ENABLE_RUM_INDEX_SCAN true
bool EnableRumIndexScan = DEFAULT_ENABLE_RUM_INDEX_SCAN;

#define DEFAULT_USE_NEW_ELEMMATCH_INDEX_PUSHDOWN false
bool UseNewElemMatchIndexPushdown = DEFAULT_USE_NEW_ELEMMATCH_INDEX_PUSHDOWN;

/* Can be removed after v110 (keep for a few releases for stability) */
#define DEFAULT_ENABLE_INSERT_CUSTOM_PLAN true
bool EnableInsertCustomPlan = DEFAULT_ENABLE_INSERT_CUSTOM_PLAN;

#define DEFAULT_ENABLE_INDEX_PRIORITY_ORDERING true
bool EnableIndexPriorityOrdering = DEFAULT_ENABLE_INDEX_PRIORITY_ORDERING;


/*
 * SECTION: Aggregation & Query feature flags
 */
#define DEFAULT_ENABLE_NOW_SYSTEM_VARIABLE false
bool EnableNowSystemVariable = DEFAULT_ENABLE_NOW_SYSTEM_VARIABLE;

#define DEFAULT_ENABLE_PRIMARY_KEY_CURSOR_SCAN false
bool EnablePrimaryKeyCursorScan = DEFAULT_ENABLE_PRIMARY_KEY_CURSOR_SCAN;

#define DEFAULT_USE_FILE_BASED_PERSISTED_CURSORS false
bool UseFileBasedPersistedCursors = DEFAULT_USE_FILE_BASED_PERSISTED_CURSORS;

#define DEFAULT_USE_LEGACY_NULL_EQUALITY_BEHAVIOR false
bool UseLegacyNullEqualityBehavior = DEFAULT_USE_LEGACY_NULL_EQUALITY_BEHAVIOR;

/* Remove after v108 */
#define DEFAULT_ENABLE_INDEX_HINT_SUPPORT true
bool EnableIndexHintSupport = DEFAULT_ENABLE_INDEX_HINT_SUPPORT;

/* Remove after v109 */
#define DEFAULT_USE_LEGACY_FORCE_PUSHDOWN_BEHAVIOR false
bool UseLegacyForcePushdownBehavior = DEFAULT_USE_LEGACY_FORCE_PUSHDOWN_BEHAVIOR;


/*
 * SECTION: Let support feature flags
 */
#define DEFAULT_ENABLE_LET_AND_COLLATION_FOR_QUERY_MATCH false
bool EnableLetAndCollationForQueryMatch =
	DEFAULT_ENABLE_LET_AND_COLLATION_FOR_QUERY_MATCH;

#define DEFAULT_ENABLE_VARIABLES_SUPPORT_FOR_WRITE_COMMANDS false
bool EnableVariablesSupportForWriteCommands =
	DEFAULT_ENABLE_VARIABLES_SUPPORT_FOR_WRITE_COMMANDS;


/*
 * SECTION: Collation feature flags
 */
#define DEFAULT_SKIP_FAIL_ON_COLLATION false
bool SkipFailOnCollation = DEFAULT_SKIP_FAIL_ON_COLLATION;

#define DEFAULT_ENABLE_LOOKUP_ID_JOIN_OPTIMIZATION_ON_COLLATION false
bool EnableLookupIdJoinOptimizationOnCollation =
	DEFAULT_ENABLE_LOOKUP_ID_JOIN_OPTIMIZATION_ON_COLLATION;


/*
 * SECTION: Cluster administration & DDL feature flags
 */
#define DEFAULT_RECREATE_RETRY_TABLE_ON_SHARDING false
bool RecreateRetryTableOnSharding = DEFAULT_RECREATE_RETRY_TABLE_ON_SHARDING;

#define DEFAULT_ENABLE_DATA_TABLES_WITHOUT_CREATION_TIME true
bool EnableDataTableWithoutCreationTime =
	DEFAULT_ENABLE_DATA_TABLES_WITHOUT_CREATION_TIME;

/* Remove after v108 */
#define DEFAULT_ENABLE_MULTIPLE_INDEX_BUILDS_PER_RUN true
bool EnableMultipleIndexBuildsPerRun = DEFAULT_ENABLE_MULTIPLE_INDEX_BUILDS_PER_RUN;

#define DEFAULT_ENABLE_BUCKET_AUTO_STAGE true
bool EnableBucketAutoStage = DEFAULT_ENABLE_BUCKET_AUTO_STAGE;

/* Remove after v108 */
#define DEFAULT_ENABLE_COMPACT_COMMAND true
bool EnableCompact = DEFAULT_ENABLE_COMPACT_COMMAND;

/* FEATURE FLAGS END */

void
InitializeFeatureFlagConfigurations(const char *prefix, const char *newGucPrefix)
{
	DefineCustomBoolVariable(
		psprintf("%s.enableVectorHNSWIndex", prefix),
		gettext_noop(
			"Enables support for HNSW index type and query for vector search in bson documents index."),
		NULL, &EnableVectorHNSWIndex, DEFAULT_ENABLE_VECTOR_HNSW_INDEX,
		PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable(
		psprintf("%s.enableVectorPreFilter", prefix),
		gettext_noop(
			"Enables support for vector pre-filtering feature for vector search in bson documents index."),
		NULL, &EnableVectorPreFilter, DEFAULT_ENABLE_VECTOR_PRE_FILTER,
		PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable(
		psprintf("%s.enableVectorPreFilterV2", prefix),
		gettext_noop(
			"Enables support for vector pre-filtering v2 feature for vector search in bson documents index."),
		NULL, &EnableVectorPreFilterV2, DEFAULT_ENABLE_VECTOR_PRE_FILTER_V2,
		PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable(
		psprintf("%s.enable_force_push_vector_index", prefix),
		gettext_noop(
			"Enables ensuring that vector index queries are always pushed to the vector index."),
		NULL, &EnableVectorForceIndexPushdown, DEFAULT_ENABLE_VECTOR_FORCE_INDEX_PUSHDOWN,
		PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable(
		psprintf("%s.enableVectorCompressionHalf", newGucPrefix),
		gettext_noop(
			"Enables support for vector index compression half"),
		NULL, &EnableVectorCompressionHalf, DEFAULT_ENABLE_VECTOR_COMPRESSION_HALF,
		PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable(
		psprintf("%s.enableVectorCompressionPQ", newGucPrefix),
		gettext_noop(
			"Enables support for vector index compression product quantization"),
		NULL, &EnableVectorCompressionPQ, DEFAULT_ENABLE_VECTOR_COMPRESSION_PQ,
		PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable(
		psprintf("%s.enableVectorCalculateDefaultSearchParam", newGucPrefix),
		gettext_noop(
			"Enables support for vector index default search parameter calculation"),
		NULL, &EnableVectorCalculateDefaultSearchParameter,
		DEFAULT_ENABLE_VECTOR_CALCULATE_DEFAULT_SEARCH_PARAM,
		PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable(
		psprintf("%s.enableNewSelectivityMode", newGucPrefix),
		gettext_noop(
			"Determines whether to use the new selectivity logic."),
		NULL, &EnableNewOperatorSelectivityMode,
		DEFAULT_ENABLE_NEW_OPERATOR_SELECTIVITY,
		PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable(
		psprintf("%s.disableDollarSupportFuncSelectivity", newGucPrefix),
		gettext_noop(
			"Disables the selectivity calculation for dollar support functions - override on top of enableNewSelectivityMode."),
		NULL, &DisableDollarSupportFuncSelectivity,
		DEFAULT_DISABLE_DOLLAR_FUNCTION_SELECTIVITY,
		PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable(
		psprintf("%s.enableRumIndexScan", newGucPrefix),
		gettext_noop(
			"Allow rum index scans."),
		NULL,
		&EnableRumIndexScan,
		DEFAULT_ENABLE_RUM_INDEX_SCAN,
		PGC_USERSET,
		0,
		NULL, NULL, NULL);

	DefineCustomBoolVariable(
		psprintf("%s.enableSchemaValidation", prefix),
		gettext_noop(
			"Whether or not to support schema validation."),
		NULL,
		&EnableSchemaValidation,
		DEFAULT_ENABLE_SCHEMA_VALIDATION,
		PGC_USERSET,
		0,
		NULL, NULL, NULL);

	DefineCustomBoolVariable(
		psprintf("%s.enableBypassDocumentValidation", prefix),
		gettext_noop(
			"Whether or not to support 'bypassDocumentValidation'."),
		NULL,
		&EnableBypassDocumentValidation,
		DEFAULT_ENABLE_BYPASSDOCUMENTVALIDATION,
		PGC_USERSET,
		0,
		NULL, NULL, NULL);

	DefineCustomBoolVariable(
		psprintf("%s.recreate_retry_table_on_shard", prefix),
		gettext_noop(
			"Gets whether or not to recreate a retry table to match the main table"),
		NULL, &RecreateRetryTableOnSharding, DEFAULT_RECREATE_RETRY_TABLE_ON_SHARDING,
		PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable(
		psprintf("%s.skipFailOnCollation", newGucPrefix),
		gettext_noop(
			"Determines whether we can skip failing when collation is specified but collation is not supported"),
		NULL, &SkipFailOnCollation, DEFAULT_SKIP_FAIL_ON_COLLATION,
		PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable(
		psprintf("%s.enableLookupIdJoinOptimizationOnCollation", newGucPrefix),
		gettext_noop(
			"Determines whether we can perform _id join opetimization on collation. It would be a customer input confiriming that _id does not contain collation aware data types (i.e., UTF8 and DOCUMENT)."),
		NULL, &EnableLookupIdJoinOptimizationOnCollation,
		DEFAULT_ENABLE_LOOKUP_ID_JOIN_OPTIMIZATION_ON_COLLATION,
		PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable(
		psprintf("%s.enableNowSystemVariable", newGucPrefix),
		gettext_noop(
			"Enables support for the $$NOW time system variable."),
		NULL, &EnableNowSystemVariable,
		DEFAULT_ENABLE_NOW_SYSTEM_VARIABLE,
		PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable(
		psprintf("%s.enableLetAndCollationForQueryMatch", newGucPrefix),
		gettext_noop(
			"Whether or not to enable collation and let for query match."),
		NULL, &EnableLetAndCollationForQueryMatch,
		DEFAULT_ENABLE_LET_AND_COLLATION_FOR_QUERY_MATCH,
		PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable(
		psprintf("%s.enableVariablesSupportForWriteCommands", newGucPrefix),
		gettext_noop(
			"Whether or not to enable let variables and $$NOW support for write (update, delete, findAndModify) commands. Only support for delete is available now."),
		NULL, &EnableVariablesSupportForWriteCommands,
		DEFAULT_ENABLE_VARIABLES_SUPPORT_FOR_WRITE_COMMANDS,
		PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable(
		psprintf("%s.enablePrimaryKeyCursorScan", newGucPrefix),
		gettext_noop(
			"Whether or not to enable primary key cursor scan for streaming cursors."),
		NULL, &EnablePrimaryKeyCursorScan,
		DEFAULT_ENABLE_PRIMARY_KEY_CURSOR_SCAN,
		PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable(
		psprintf("%s.enableUsernamePasswordConstraints", newGucPrefix),
		gettext_noop(
			"Determines whether username and password constraints are enabled."),
		NULL, &EnableUsernamePasswordConstraints,
		DEFAULT_ENABLE_USERNAME_PASSWORD_CONSTRAINTS,
		PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable(
		psprintf("%s.enableDataTableWithoutCreationTime", newGucPrefix),
		gettext_noop(
			"Create data table without creation_time column."),
		NULL, &EnableDataTableWithoutCreationTime,
		DEFAULT_ENABLE_DATA_TABLES_WITHOUT_CREATION_TIME,
		PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable(
		psprintf("%s.enableMultipleIndexBuildsPerRun", newGucPrefix),
		gettext_noop(
			"Whether or not to enable multiple index builds per run."),
		NULL, &EnableMultipleIndexBuildsPerRun,
		DEFAULT_ENABLE_MULTIPLE_INDEX_BUILDS_PER_RUN,
		PGC_USERSET, 0, NULL, NULL, NULL
		);

	DefineCustomBoolVariable(
		psprintf("%s.useFileBasedPersistedCursors", newGucPrefix),
		gettext_noop(
			"Whether or not to use file based persisted cursors."),
		NULL, &UseFileBasedPersistedCursors,
		DEFAULT_USE_FILE_BASED_PERSISTED_CURSORS,
		PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable(
		psprintf("%s.enableCompact", newGucPrefix),
		gettext_noop(
			"Whether or not to enable compact command."),
		NULL, &EnableCompact,
		DEFAULT_ENABLE_COMPACT_COMMAND,
		PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable(
		psprintf("%s.enableUsersInfoPrivileges", newGucPrefix),
		gettext_noop(
			"Determines whether the usersInfo command returns privileges."),
		NULL, &EnableUsersInfoPrivileges,
		DEFAULT_ENABLE_USERS_INFO_PRIVILEGES,
		PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable(
		psprintf("%s.isNativeAuthEnabled", newGucPrefix),
		gettext_noop(
			"Determines whether native authentication is enabled."),
		NULL, &IsNativeAuthEnabled,
		DEFAULT_ENABLE_NATIVE_AUTHENTICATION,
		PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable(
		psprintf("%s.useLegacyNullEqualityBehavior", newGucPrefix),
		gettext_noop(
			"Whether or not to use legacy null equality behavior."),
		NULL, &UseLegacyNullEqualityBehavior,
		DEFAULT_USE_LEGACY_NULL_EQUALITY_BEHAVIOR,
		PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable(
		psprintf("%s.useNewElemMatchIndexPushdown", newGucPrefix),
		gettext_noop(
			"Whether or not to use the new elemMatch index pushdown logic."),
		NULL, &UseNewElemMatchIndexPushdown,
		DEFAULT_USE_NEW_ELEMMATCH_INDEX_PUSHDOWN,
		PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable(
		psprintf("%s.enableLookupInnerJoin", newGucPrefix),
		gettext_noop(
			"Whether or not to enable lookup inner join."),
		NULL, &EnableLookupInnerJoin,
		DEFAULT_LOOKUP_ENABLE_INNER_JOIN,
		PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable(
		psprintf("%s.forceBitmapScanForLookup", newGucPrefix),
		gettext_noop(
			"Whether or not to force bitmap scan for lookup."),
		NULL, &ForceBitmapScanForLookup,
		DEFAULT_FORCE_BITMAP_SCAN_FOR_LOOKUP,
		PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable(
		psprintf("%s.lowSelectivityForLookup", newGucPrefix),
		gettext_noop(
			"Whether or not to use low selectivity for lookup."),
		NULL, &LowSelectivityForLookup,
		DEFAULT_LOW_SELECTIVITY_FOR_LOOKUP,
		PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable(
		psprintf("%s.enableBucketAutoStage", newGucPrefix),
		gettext_noop(
			"Whether to enable the $bucketAuto stage."),
		NULL, &EnableBucketAutoStage,
		DEFAULT_ENABLE_BUCKET_AUTO_STAGE,
		PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable(
		psprintf("%s.enableInsertCustomPlan", newGucPrefix),
		gettext_noop(
			"Whether to use custom insert plan for insert commands."),
		NULL, &EnableInsertCustomPlan,
		DEFAULT_ENABLE_INSERT_CUSTOM_PLAN,
		PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable(
		psprintf("%s.enableNewCompositeIndexOpClass", newGucPrefix),
		gettext_noop(
			"Whether to enable the new experimental composite index opclass"),
		NULL, &EnableNewCompositeIndexOpclass, DEFAULT_ENABLE_NEW_COMPOSITE_INDEX_OPCLASS,
		PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable(
		psprintf("%s.defaultUseCompositeOpClass", newGucPrefix),
		gettext_noop(
			"Whether to enable the new experimental composite index opclass for default index creates"),
		NULL, &DefaultUseCompositeOpClass, DEFAULT_USE_NEW_COMPOSITE_INDEX_OPCLASS,
		PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable(
		psprintf("%s.enableIndexOrderbyPushdown", newGucPrefix),
		gettext_noop(
			"Whether to enable the sort on the new experimental composite index opclass"),
		NULL, &EnableIndexOrderbyPushdown, DEFAULT_ENABLE_INDEX_ORDERBY_PUSHDOWN,
		PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable(
		psprintf("%s.enableDescendingCompositeIndex", newGucPrefix),
		gettext_noop(
			"Whether to enable descending composite index support"),
		NULL, &EnableDescendingCompositeIndex, DEFAULT_ENABLE_DESCENDING_COMPOSITE_INDEX,
		PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable(
		psprintf("%s.enableCompositeUniqueIndex", newGucPrefix),
		gettext_noop(
			"Whether to enable composite unique index support"),
		NULL, &EnableCompositeUniqueIndex, DEFAULT_ENABLE_COMPOSITE_UNIQUE_INDEX,
		PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable(
		psprintf("%s.enableIndexHintSupport", newGucPrefix),
		gettext_noop(
			"Whether to enable index hint support for index pushdown."),
		NULL, &EnableIndexHintSupport, DEFAULT_ENABLE_INDEX_HINT_SUPPORT,
		PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable(
		psprintf("%s.useLegacyForcePushdownBehavior", newGucPrefix),
		gettext_noop(
			"Whether to use legacy force index pushdown behavior."),
		NULL, &UseLegacyForcePushdownBehavior, DEFAULT_USE_LEGACY_FORCE_PUSHDOWN_BEHAVIOR,
		PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable(
		psprintf("%s.enableRoleCrud", newGucPrefix),
		gettext_noop(
			"Enables role crud through the data plane."),
		NULL, &EnableRoleCrud, DEFAULT_ENABLE_ROLE_CRUD,
		PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable(
		psprintf("%s.enableIndexPriorityOrdering", newGucPrefix),
		gettext_noop(
			"Whether to reorder the indexlist at the planner level based on priority of indexes."),
		NULL, &EnableIndexPriorityOrdering, DEFAULT_ENABLE_INDEX_PRIORITY_ORDERING,
		PGC_USERSET, 0, NULL, NULL, NULL);
}
