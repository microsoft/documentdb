
CREATE FUNCTION documentdb_extended_rum_jsonb_ops_internal.jsonb_rum_ops_extract_value(jsonb, internal)
 RETURNS internal
 LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME', $function$jsonb_rum_ops_extract_value$function$;

CREATE OR REPLACE FUNCTION documentdb_extended_rum_jsonb_ops_internal.jsonb_rum_ops_extract_query(jsonb, internal, int2, internal, internal, internal, internal)
 RETURNS internal
 LANGUAGE c
 PARALLEL SAFE STABLE
AS 'MODULE_PATHNAME', $function$jsonb_rum_ops_extract_query$function$;

CREATE OR REPLACE FUNCTION documentdb_extended_rum_jsonb_ops_internal.jsonb_rum_ops_consistent(internal, int2, anyelement, int4, internal, internal)
 RETURNS boolean
 LANGUAGE c
 IMMUTABLE PARALLEL SAFE STRICT
AS 'MODULE_PATHNAME', $function$jsonb_rum_ops_consistent$function$;

CREATE OR REPLACE FUNCTION documentdb_extended_rum_jsonb_ops_internal.jsonb_rum_ops_compare_partial(jsonb, jsonb, int2, internal)
 RETURNS int4
 LANGUAGE c
 IMMUTABLE PARALLEL SAFE STRICT
 AS 'MODULE_PATHNAME', $function$jsonb_rum_ops_compare_partial$function$;

CREATE OR REPLACE FUNCTION documentdb_extended_rum_jsonb_ops_internal.jsonb_rum_ops_options(internal)
 RETURNS void
 LANGUAGE c
 IMMUTABLE PARALLEL SAFE STRICT
AS 'MODULE_PATHNAME', $function$jsonb_rum_ops_options$function$;