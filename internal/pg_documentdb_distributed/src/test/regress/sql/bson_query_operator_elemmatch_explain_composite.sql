
SET search_path TO documentdb_core,documentdb_api,documentdb_api_catalog,documentdb_api_internal;
SET citus.next_shard_id TO 1064000;
SET documentdb.next_collection_id TO 10640;
SET documentdb.next_collection_index_id TO 10640;

SET documentdb.enableNewCompositeIndexOpClass to on;

set enable_seqscan TO on;
set documentdb.forceUseIndexIfAvailable to on;
set documentdb.forceDisableSeqScan to off;

SELECT documentdb_api.drop_collection('comp_elmdb', 'cmp_elemmatch_ops') IS NOT NULL;
SELECT documentdb_api.create_collection('comp_elmdb', 'cmp_elemmatch_ops') IS NOT NULL;

SELECT documentdb_api_internal.create_indexes_non_concurrently('comp_elmdb',
    '{ "createIndexes": "cmp_elemmatch_ops", "indexes": [ { "key": { "score": 1 }, "name": "score_1", "enableCompositeTerm": true }, { "key": { "results": 1 }, "name": "results_1", "enableCompositeTerm": true } ] }', TRUE);
SELECT documentdb_api_internal.create_indexes_non_concurrently('comp_elmdb',
    '{ "createIndexes": "cmp_elemmatch_ops", "indexes": [ { "key": { "results.product": 1 }, "name": "results.product_1", "enableCompositeTerm": true }, { "key": { "results.safety": 1 }, "name": "results.safety_1", "enableCompositeTerm": true } ] }', TRUE);

SELECT documentdb_api.insert_one('comp_elmdb', 'cmp_elemmatch_ops', '{ "_id": 1, "score": [ 80, 90, 70 ] }');
SELECT documentdb_api.insert_one('comp_elmdb', 'cmp_elemmatch_ops', '{ "_id": 2, "score": [ 75, 86, 89 ] }');

-- pushes to the score index
EXPLAIN (COSTS OFF, ANALYZE ON, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('comp_elmdb',
    '{ "find": "cmp_elemmatch_ops", "filter": { "score": { "$elemMatch": { "$gt": 80, "$lt": 85 } } } }');

EXPLAIN (COSTS OFF, ANALYZE ON, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('comp_elmdb',
    '{ "find": "cmp_elemmatch_ops", "filter": { "score": { "$elemMatch": { "$in": [ 80, 86 ], "$gt": 81 } } } }');

EXPLAIN (COSTS OFF, ANALYZE ON, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('comp_elmdb',
    '{ "find": "cmp_elemmatch_ops", "filter": { "score": { "$elemMatch": { "$type": "number" } } } }');

EXPLAIN (COSTS OFF, ANALYZE ON, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('comp_elmdb',
    '{ "find": "cmp_elemmatch_ops", "filter": { "score": { "$elemMatch": { "$ne": 89 } } } }');

EXPLAIN (COSTS OFF, ANALYZE ON, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('comp_elmdb',
    '{ "find": "cmp_elemmatch_ops", "filter": { "score": { "$elemMatch": { "$nin": [ 89, 75, 86] } } } }');

-- now test some with nested objects
SELECT documentdb_api.insert_one('comp_elmdb', 'cmp_elemmatch_ops', '{ "_id": 3, "results": [ { "product" : "fish", "safety" : 10 }, { "product" : "sugar", "safety" : 5 } ] }');
SELECT documentdb_api.insert_one('comp_elmdb', 'cmp_elemmatch_ops', '{ "_id": 4, "results": [ { "product" : "fish", "safety" : 8 }, { "product" : "sugar", "safety" : 7 } ] }');
SELECT documentdb_api.insert_one('comp_elmdb', 'cmp_elemmatch_ops', '{ "_id": 5, "results": [ { "product" : "fish", "safety" : 7 }, { "product" : "sugar", "safety" : 8 } ] }');

EXPLAIN (COSTS OFF, ANALYZE ON, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('comp_elmdb',
    '{ "find": "cmp_elemmatch_ops", "filter": { "results": { "$elemMatch": { "product": "fish", "safety": 7 } } } }');

EXPLAIN (COSTS OFF, ANALYZE ON, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('comp_elmdb',
    '{ "find": "cmp_elemmatch_ops", "filter": { "results": { "$elemMatch": { "product": "fish" } } } }');