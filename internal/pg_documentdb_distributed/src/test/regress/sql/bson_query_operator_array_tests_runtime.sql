
SET search_path TO documentdb_core,documentdb_api,documentdb_api_catalog,documentdb_api_internal;
SET citus.next_shard_id TO 661000;
SET documentdb.next_collection_id TO 6610;
SET documentdb.next_collection_index_id TO 6610;


SET documentdb.enableNewCompositeIndexOpClass to on;

set enable_seqscan TO on;
set documentdb.forceUseIndexIfAvailable to on;
set documentdb.forceDisableSeqScan to off;

SELECT documentdb_api.drop_collection('comp_arrdb', 'composite_array_ops') IS NOT NULL;
SELECT documentdb_api.create_collection('comp_arrdb', 'composite_array_ops') IS NOT NULL;

\i sql/bson_query_operator_array_tests_core.sql