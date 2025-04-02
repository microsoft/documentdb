CREATE OPERATOR CLASS __API_SCHEMA_INTERNAL_V2__.bson_rum_composite_path_ops
    FOR TYPE __CORE_SCHEMA__.bson using __EXTENSION_OBJECT__(_rum) AS
        OPERATOR        1       __API_CATALOG_SCHEMA__.@= (__CORE_SCHEMA__.bson, __CORE_SCHEMA__.bson),
        OPERATOR        2       __API_CATALOG_SCHEMA__.@> (__CORE_SCHEMA__.bson, __CORE_SCHEMA__.bson),
        OPERATOR        3       __API_CATALOG_SCHEMA__.@>= (__CORE_SCHEMA__.bson, __CORE_SCHEMA__.bson),
        OPERATOR        4       __API_CATALOG_SCHEMA__.@< (__CORE_SCHEMA__.bson, __CORE_SCHEMA__.bson),
        OPERATOR        5       __API_CATALOG_SCHEMA__.@<= (__CORE_SCHEMA__.bson, __CORE_SCHEMA__.bson),
        FUNCTION        1       __API_CATALOG_SCHEMA__.gin_bson_compare(bytea, bytea),
        FUNCTION        2       __API_SCHEMA_INTERNAL_V2__.gin_bson_composite_path_extract_value(__CORE_SCHEMA__.bson, internal),
        FUNCTION        3       __API_SCHEMA_INTERNAL_V2__.gin_bson_composite_path_extract_query(__CORE_SCHEMA__.bson, internal, int2, internal, internal, internal, internal),
        FUNCTION        4       __API_SCHEMA_INTERNAL_V2__.gin_bson_composite_path_consistent(internal, int2, anyelement, int4, internal, internal),
        FUNCTION        5       __API_SCHEMA_INTERNAL_V2__.gin_bson_composite_path_compare_partial(bytea, bytea, int2, internal),
        FUNCTION        11       (__CORE_SCHEMA__.bson) __API_SCHEMA_INTERNAL_V2__.gin_bson_composite_path_options(internal),
    STORAGE         bytea;