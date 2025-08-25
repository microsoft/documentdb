
CREATE OPERATOR documentdb_extended_rum_jsonb_ops.@@@ (
    LEFTARG = jsonb,
    RIGHTARG = jsonpath,
    PROCEDURE = documentdb_extended_rum_jsonb_ops.jsonb_path_match_opr
);

-- Same operator as @@@ but pushed to the index
CREATE OPERATOR documentdb_extended_rum_jsonb_ops_internal.#@@ (
    LEFTARG = jsonb,
    RIGHTARG = jsonpath,
    PROCEDURE = documentdb_extended_rum_jsonb_ops.jsonb_path_match_opr
);
