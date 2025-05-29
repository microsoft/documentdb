
SET search_path TO documentdb_core,documentdb_api,documentdb_api_catalog,documentdb_api_internal;
SET citus.next_shard_id TO 380000;
SET documentdb.next_collection_id TO 3800;
SET documentdb.next_collection_index_id TO 3800;


SET documentdb.enableNewCompositeIndexOpClass to on;

set enable_seqscan TO off;
set documentdb.forceUseIndexIfAvailable to on;
set documentdb.forceDisableSeqScan to on;

SELECT documentdb_api.drop_collection('comp_arrdb', 'composite_array_ops') IS NOT NULL;
SELECT documentdb_api_internal.create_indexes_non_concurrently('comp_arrdb', '{ "createIndexes": "composite_array_ops", "indexes": [ { "key": { "a": 1 }, "enableCompositeTerm": true, "name": "queryoperator_a" }] }', true) IS NOT NULL;

\i sql/bson_query_operator_array_tests_core.sql