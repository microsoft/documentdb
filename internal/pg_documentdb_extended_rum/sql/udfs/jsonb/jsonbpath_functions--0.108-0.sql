
CREATE FUNCTION documentdb_extended_rum_jsonb_ops_internal.jsonb_path_support(internal)
 RETURNS internal
 LANGUAGE c
 IMMUTABLE PARALLEL SAFE STRICT
AS 'MODULE_PATHNAME', $$jsonb_path_support$$;

CREATE OR REPLACE FUNCTION documentdb_extended_rum_jsonb_ops.jsonb_path_match_opr(jsonb, jsonpath)
 RETURNS boolean
 LANGUAGE C
 SUPPORT documentdb_extended_rum_jsonb_ops_internal.jsonb_path_support
 IMMUTABLE PARALLEL SAFE STRICT
AS 'MODULE_PATHNAME', $function$jsonb_path_match_docdb_rum$function$;
