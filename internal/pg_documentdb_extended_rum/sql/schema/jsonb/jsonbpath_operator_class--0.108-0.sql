CREATE OPERATOR CLASS documentdb_extended_rum_jsonb_ops.jsonb_path_extended_rum_ops
    DEFAULT FOR TYPE jsonb using documentdb_extended_rum AS
        OPERATOR        1       documentdb_extended_rum_jsonb_ops_internal.#@@(jsonb, jsonpath),
        FUNCTION        1       jsonb_cmp(jsonb, jsonb),
        FUNCTION        2       documentdb_extended_rum_jsonb_ops_internal.jsonb_rum_ops_extract_value(jsonb, internal),
        FUNCTION        3       documentdb_extended_rum_jsonb_ops_internal.jsonb_rum_ops_extract_query(jsonb, internal, int2, internal, internal, internal, internal),
        FUNCTION        4       documentdb_extended_rum_jsonb_ops_internal.jsonb_rum_ops_consistent(internal, int2, anyelement, int4, internal, internal),
        FUNCTION        5       documentdb_extended_rum_jsonb_ops_internal.jsonb_rum_ops_compare_partial(jsonb, jsonb, int2, internal),
        FUNCTION        11       (jsonb) documentdb_extended_rum_jsonb_ops_internal.jsonb_rum_ops_options(internal),
    STORAGE         jsonb;


-- ALTER OPERATOR FAMILY documentdb_extended_rum_jsonb_ops_internal.jsonb_path_extended_rum_ops USING documentdb_extended_rum ADD OPERATOR 7 documentdb_extended_rum_jsonb_ops_internal.|<>(jsonb, jsonpath) FOR ORDER BY jsonb_ops;
-- ALTER OPERATOR FAMILY documentdb_extended_rum_jsonb_ops_internal.jsonb_path_extended_rum_ops USING documentdb_extended_rum ADD FUNCTION 8 (jsonb)documentdb_extended_rum_jsonb_ops_internal.jsonb_ordering_transform(jsonb, jsonpath, int2, internal);
