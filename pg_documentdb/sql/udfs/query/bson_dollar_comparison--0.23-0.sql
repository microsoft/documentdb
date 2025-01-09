CREATE OR REPLACE FUNCTION __API_CATALOG_SCHEMA__.dollar_support(internal)
 RETURNS internal
 LANGUAGE c
 IMMUTABLE PARALLEL SAFE STRICT
AS 'MODULE_PATHNAME', $$dollar_support$$;

CREATE OR REPLACE FUNCTION __API_CATALOG_SCHEMA__.bson_dollar_eq(__CORE_SCHEMA__.bson, __CORE_SCHEMA__.bson)
 RETURNS bool
 LANGUAGE c
 IMMUTABLE PARALLEL SAFE STRICT
 SUPPORT __API_CATALOG_SCHEMA__.dollar_support
AS 'MODULE_PATHNAME', $function$bson_dollar_eq$function$;

CREATE OR REPLACE FUNCTION __API_CATALOG_SCHEMA__.bson_dollar_lt(__CORE_SCHEMA__.bson, __CORE_SCHEMA__.bson)
 RETURNS bool
 LANGUAGE c
 IMMUTABLE PARALLEL SAFE STRICT
 SUPPORT __API_CATALOG_SCHEMA__.dollar_support
AS 'MODULE_PATHNAME', $function$bson_dollar_lt$function$;

CREATE OR REPLACE FUNCTION __API_CATALOG_SCHEMA__.bson_dollar_lte(__CORE_SCHEMA__.bson, __CORE_SCHEMA__.bson)
 RETURNS bool
 LANGUAGE c
 IMMUTABLE PARALLEL SAFE STRICT
 SUPPORT __API_CATALOG_SCHEMA__.dollar_support
AS 'MODULE_PATHNAME', $function$bson_dollar_lte$function$;

CREATE OR REPLACE FUNCTION __API_CATALOG_SCHEMA__.bson_dollar_gt(__CORE_SCHEMA__.bson, __CORE_SCHEMA__.bson)
 RETURNS bool
 LANGUAGE c
 IMMUTABLE PARALLEL SAFE STRICT
 SUPPORT __API_CATALOG_SCHEMA__.dollar_support
AS 'MODULE_PATHNAME', $function$bson_dollar_gt$function$;

CREATE OR REPLACE FUNCTION __API_CATALOG_SCHEMA__.bson_dollar_gte(__CORE_SCHEMA__.bson, __CORE_SCHEMA__.bson)
 RETURNS bool
 LANGUAGE c
 IMMUTABLE PARALLEL SAFE STRICT
 SUPPORT __API_CATALOG_SCHEMA__.dollar_support
AS 'MODULE_PATHNAME', $function$bson_dollar_gte$function$;

CREATE OR REPLACE FUNCTION __API_CATALOG_SCHEMA__.bson_dollar_in(__CORE_SCHEMA__.bson, __CORE_SCHEMA__.bson)
 RETURNS bool
 LANGUAGE c
 IMMUTABLE PARALLEL SAFE STRICT
 SUPPORT __API_CATALOG_SCHEMA__.dollar_support
AS 'MODULE_PATHNAME', $function$bson_dollar_in$function$;

CREATE OR REPLACE FUNCTION __API_CATALOG_SCHEMA__.bson_dollar_ne(__CORE_SCHEMA__.bson, __CORE_SCHEMA__.bson)
 RETURNS bool
 LANGUAGE c
 IMMUTABLE PARALLEL SAFE STRICT
 SUPPORT __API_CATALOG_SCHEMA__.dollar_support
AS 'MODULE_PATHNAME', $function$bson_dollar_ne$function$;

CREATE OR REPLACE FUNCTION __API_CATALOG_SCHEMA__.bson_dollar_nin(__CORE_SCHEMA__.bson, __CORE_SCHEMA__.bson)
 RETURNS bool
 LANGUAGE c
 IMMUTABLE PARALLEL SAFE STRICT
 SUPPORT __API_CATALOG_SCHEMA__.dollar_support
AS 'MODULE_PATHNAME', $function$bson_dollar_nin$function$;

CREATE OR REPLACE FUNCTION __API_SCHEMA_INTERNAL_V2__.bson_dollar_range(__CORE_SCHEMA__.bson, __CORE_SCHEMA__.bson)
 RETURNS bool
 LANGUAGE c
 IMMUTABLE PARALLEL SAFE STRICT
 SUPPORT __API_CATALOG_SCHEMA__.dollar_support
AS 'MODULE_PATHNAME', $function$bson_dollar_range$function$;