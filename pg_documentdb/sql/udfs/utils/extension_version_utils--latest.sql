
CREATE OR REPLACE FUNCTION __API_SCHEMA_V2__.binary_version()
RETURNS text
LANGUAGE C
IMMUTABLE PARALLEL SAFE STRICT
AS 'MODULE_PATHNAME', __CONCAT_NAME_FUNCTION__($function$get_, __API_SCHEMA_V2__, _binary_version$function$);

CREATE OR REPLACE FUNCTION __API_SCHEMA_V2__.binary_extended_version()
RETURNS text
LANGUAGE C
IMMUTABLE PARALLEL SAFE STRICT
AS 'MODULE_PATHNAME', __CONCAT_NAME_FUNCTION__($function$get_, __API_SCHEMA_V2__, _extended_binary_version$function$);