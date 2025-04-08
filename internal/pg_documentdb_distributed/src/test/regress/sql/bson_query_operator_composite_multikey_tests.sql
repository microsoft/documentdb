
SET search_path TO documentdb_core,documentdb_api,documentdb_api_catalog,documentdb_api_internal;
SET citus.next_shard_id TO 2510000;
SET documentdb.next_collection_id TO 25100;
SET documentdb.next_collection_index_id TO 25100;

-- First create a collection and composite index.

SET documentdb.enableNewCompositeIndexOpClass to on;
SELECT documentdb_api.drop_collection('comp_db', 'qocomp') IS NOT NULL;
SELECT documentdb_api.create_collection('comp_db', 'qocomp');

SELECT documentdb_api_internal.create_indexes_non_concurrently('comp_db', '{ "createIndexes": "qocomp", "indexes": [ { "key": { "a": 1, "c": 1 }, "enableCompositeTerm": true, "name": "a_c" }] }', true);
SELECT documentdb_api_internal.create_indexes_non_concurrently('comp_db', '{ "createIndexes": "qocomp", "indexes": [ { "key": { "b": 1, "d": 1 }, "enableCompositeTerm": true, "name": "b_d" }] }', true);

WITH c0 AS (SELECT i, j FROM generate_series(1, 5) i, generate_series(1, 5) j),
c1 AS (SELECT documentdb_api.insert_one('comp_db', 'qocomp', FORMAT('{ "_id": %s, "a": %s, "b": [ %s, "true" ], "c": %s, "d": [ %s, %s ] }', (i * 5 + j), i, j, i, j, i)::bson) FROM c0)
SELECT COUNT(*) FROM c1;

\d documentdb_data.documents_25101

-- now a_c is single key and b_d is multi-key
SELECT COUNT(*) FROM documentdb_api.collection('comp_db', 'qocomp');

SELECT document FROM documentdb_api_catalog.bson_aggregation_find('comp_db', '{ "find": "qocomp", "filter": { "a": { "$gt": 2, "$lt": 5 } } }');

BEGIN;
SET local documentdb.enableNewCompositeIndexOpClass to on;

-- should use less buffers than the query below.
EXPLAIN (COSTS OFF, SUMMARY OFF, TIMING OFF, BUFFERS ON, ANALYZE ON)
    SELECT document FROM documentdb_api_catalog.bson_aggregation_find('comp_db', '{ "find": "qocomp", "filter": { "a": { "$gt": 2, "$lt": 5 } } }');

-- Should use way more buffers in the index_scan
EXPLAIN (COSTS OFF, SUMMARY OFF, TIMING OFF, BUFFERS ON, ANALYZE ON)
    SELECT document FROM documentdb_api_catalog.bson_aggregation_find('comp_db', '{ "find": "qocomp", "filter": { "b": { "$gt": 2, "$lt": 5 } } }');

ROLLBACK;