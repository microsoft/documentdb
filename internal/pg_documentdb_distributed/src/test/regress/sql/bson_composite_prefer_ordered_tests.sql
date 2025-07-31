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


-- test ordered scan in the presence of deletes
set documentdb.enableExtendedExplainPlans to off;
reset documentdb.forceDisableSeqScan;
SELECT documentdb_api_internal.create_indexes_non_concurrently('comp_db2', '{ "createIndexes": "ordered_delete", "indexes": [ { "key": { "a": 1 }, "name": "a_1", "enableOrderedIndex": true } ] }');
SELECT COUNT(documentdb_api.insert_one('comp_db2', 'ordered_delete', FORMAT('{ "_id": %s, "a": %s }', i, i % 50)::bson)) FROM generate_series(1, 100) i;

ANALYZE documentdb_data.documents_68502;

set documentdb.forceDisableSeqScan to on;
EXPLAIN (ANALYZE ON, COSTS OFF, VERBOSE ON, BUFFERS OFF, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('comp_db2', '{ "find": "ordered_delete", "filter": { "a": { "$lt": 4 } } }');

-- now delete everthing
reset documentdb.forceDisableSeqScan;
DELETE FROM documentdb_data.documents_68502;

-- vacuum the table
VACUUM documentdb_data.documents_68502;

-- query the data
set documentdb.forceDisableSeqScan to on;
EXPLAIN (ANALYZE ON, COSTS OFF, VERBOSE ON, BUFFERS OFF, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('comp_db2', '{ "find": "ordered_delete", "filter": { "a": { "$lt": 4 } } }');

-- now try with a posting tree at the end.
reset documentdb.forceDisableSeqScan;
CALL documentdb_api.drop_indexes('comp_db2', '{ "dropIndexes": "ordered_delete", "index": "a_1" }');
SELECT documentdb_api_internal.create_indexes_non_concurrently('comp_db2', '{ "createIndexes": "ordered_delete", "indexes": [ { "key": { "a": 1 }, "name": "a_1", "enableOrderedIndex": true } ] }', TRUE);
SELECT COUNT(documentdb_api.insert_one('comp_db2', 'ordered_delete', FORMAT('{ "_id": %s, "a": 1 }', i)::bson)) FROM generate_series(1, 10000) i;

ANALYZE documentdb_data.documents_68502;

set documentdb.forceDisableSeqScan to on;
EXPLAIN (ANALYZE ON, COSTS OFF, VERBOSE ON, BUFFERS OFF, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('comp_db2', '{ "find": "ordered_delete", "filter": { "a": { "$lt": 4 } } }');

-- now delete everthing
reset documentdb.forceDisableSeqScan;
DELETE FROM documentdb_data.documents_68502;

-- vacuum the table
VACUUM documentdb_data.documents_68502;

-- query the data
set documentdb.forceDisableSeqScan to on;
EXPLAIN (ANALYZE ON, COSTS OFF, VERBOSE ON, BUFFERS OFF, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('comp_db2', '{ "find": "ordered_delete", "filter": { "a": { "$lt": 4 } } }');

