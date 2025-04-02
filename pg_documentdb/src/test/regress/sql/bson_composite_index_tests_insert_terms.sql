SET search_path TO documentdb_api,documentdb_core,documentdb_api_catalog;

SET documentdb.next_collection_id TO 5600;
SET documentdb.next_collection_index_id TO 5600;


CREATE FUNCTION documentdb_test_helpers.gin_bson_get_composite_path_generated_terms(documentdb_core.bson, text, int4, bool)
    RETURNS SETOF documentdb_core.bson LANGUAGE C IMMUTABLE PARALLEL SAFE STRICT AS '$libdir/pg_documentdb',
$$gin_bson_get_composite_path_generated_terms$$;

-- test scenarios of term generation for composite path
SELECT * FROM documentdb_test_helpers.gin_bson_get_composite_path_generated_terms('{ "a": 1, "b": 2 }', '[ "a", "b" ]', 2000, false);
SELECT * FROM documentdb_test_helpers.gin_bson_get_composite_path_generated_terms('{ "a": [ 1, 2, 3 ], "b": 2 }', '[ "a", "b" ]', 2000, false);
SELECT * FROM documentdb_test_helpers.gin_bson_get_composite_path_generated_terms('{ "a": 1, "b": [ true, false ] }', '[ "a", "b" ]', 2000, false);
SELECT * FROM documentdb_test_helpers.gin_bson_get_composite_path_generated_terms('{ "a": [ 1, 2, 3 ], "b": [ true, false ] }', '[ "a", "b" ]', 2000, false);

-- test when one doesn't exist
SELECT * FROM documentdb_test_helpers.gin_bson_get_composite_path_generated_terms('{ "b": [ true, false ] }', '[ "a", "b" ]', 2000, false);
SELECT * FROM documentdb_test_helpers.gin_bson_get_composite_path_generated_terms('{ "a": [ 1, 2, 3 ] }', '[ "a", "b" ]', 2000, false);

-- test when one gets truncated (a has 29 letters, truncation limit is 50 /2 so 25 per path)
SELECT * FROM documentdb_test_helpers.gin_bson_get_composite_path_generated_terms('{ "a": "aaaaaaaaaaaaaaaaaaaaaaaaaaaa", "b": 1 }', '[ "a", "b" ]', 50, true);

-- create a table and insert some data.
set documentdb.enableNewCompositeIndexOpClass to on;

SELECT documentdb_api_internal.create_indexes_non_concurrently(
    'comp_db', '{ "createIndexes": "comp_collection", "indexes": [ { "name": "comp_index", "key": { "a": 1, "b": 1 } } ] }');

\d documentdb_data.documents_5601

SELECT documentdb_api.insert_one('comp_db', 'comp_collection', '{ "a": 1, "b": true }');
SELECT documentdb_api.insert_one('comp_db', 'comp_collection', '{ "a": [ 1, 2 ], "b": true }');
SELECT documentdb_api.insert_one('comp_db', 'comp_collection', '{ "a": 1, "b": [ true, false ] }');
SELECT documentdb_api.insert_one('comp_db', 'comp_collection', '{ "a": [ 1, 2 ], "b": [ true, false ] }');