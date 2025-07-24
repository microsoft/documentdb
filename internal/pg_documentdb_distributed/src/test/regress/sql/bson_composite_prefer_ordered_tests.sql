SET search_path TO documentdb_api,documentdb_api_catalog,documentdb_api_internal,documentdb_core;

SET citus.next_shard_id TO 685000;
SET documentdb.next_collection_id TO 68500;
SET documentdb.next_collection_index_id TO 68500;

set documentdb.enableExtendedExplainPlans to on;
SET documentdb.enableNewCompositeIndexOpClass to on;
set documentdb.enableIndexOrderbyPushdown to on;

-- if documentdb_extended_rum exists, set alternate index handler
SELECT pg_catalog.set_config('documentdb.alternate_index_handler_name', 'extended_rum', false), extname FROM pg_extension WHERE extname = 'documentdb_extended_rum';


SELECT documentdb_api.drop_collection('comp_db2', 'query_ordered_pref') IS NOT NULL;
SELECT documentdb_api.create_collection('comp_db2', 'query_ordered_pref');

SELECT documentdb_api_internal.create_indexes_non_concurrently('comp_db2', '{ "createIndexes": "query_ordered_pref", "indexes": [ { "key": { "a": 1 }, "enableCompositeTerm": true, "name": "a_1" }] }', true);

\d documentdb_data.documents_68501

SELECT COUNT(documentdb_api.insert_one('comp_db2', 'query_ordered_pref', FORMAT('{ "_id": %s, "a": %s }', i, i)::bson)) FROM generate_series(1, 10000) AS i;

ANALYZE documentdb_data.documents_68501;

set enable_bitmapscan to off;
set documentdb.forceDisableSeqScan to on;
set documentdb_rum.preferOrderedIndexScan to off;
EXPLAIN (ANALYZE ON, COSTS OFF, VERBOSE ON, BUFFERS OFF, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('comp_db2',
    '{ "find": "query_ordered_pref", "filter": { "a": { "$gt": 50 } }, "projection": { "_id": 1 }, "limit": 5 }');
EXPLAIN (ANALYZE ON, COSTS OFF, VERBOSE ON, BUFFERS OFF, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('comp_db2',
    '{ "find": "query_ordered_pref", "filter": { "a": { "$gt": 50, "$lt": 900 } }, "projection": { "_id": 1 }, "limit": 5 }');

set documentdb_rum.preferOrderedIndexScan to on;
EXPLAIN (ANALYZE ON, COSTS OFF, VERBOSE ON, BUFFERS OFF, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('comp_db2',
    '{ "find": "query_ordered_pref", "filter": { "a": { "$gt": 50 } }, "projection": { "_id": 1 }, "limit": 5 }');
EXPLAIN (ANALYZE ON, COSTS OFF, VERBOSE ON, BUFFERS OFF, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('comp_db2',
    '{ "find": "query_ordered_pref", "filter": { "a": { "$gt": 50, "$lt": 900 } }, "projection": { "_id": 1 }, "limit": 5 }');