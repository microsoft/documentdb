
/* Insert with a.b being an object with various types*/
set search_path to helio_core,helio_api,helio_api_catalog,pg_catalog;
SET citus.next_shard_id TO 390000;
SET helio_api.next_collection_id TO 3900;
SET helio_api.next_collection_index_id TO 3900;

SELECT helio_api.insert_one('db','querydollartesterrors', '{"_id": 1, "a" : { "b" : 0 }}', NULL);
SELECT helio_api.insert_one('db','querydollartesterrors', '{"_id": 2, "a" : { "b" : 1 }}', NULL);
SELECT helio_api.insert_one('db','querydollartesterrors', '{"_id": 3, "a" : { "b" : 2.0 }}', NULL);
SELECT helio_api.insert_one('db','querydollartesterrors', '{"_id": 4, "a" : { "b" : "someString" }}', NULL);
SELECT helio_api.insert_one('db','querydollartesterrors', '{"_id": 5, "a" : { "b" : true }}', NULL);

/* insert some documents with a.{some other paths} */
SELECT helio_api.insert_one('db','querydollartesterrors', '{"_id": 6, "a" : 1}', NULL);
SELECT helio_api.insert_one('db','querydollartesterrors', '{"_id": 7, "a" : true}', NULL);
SELECT helio_api.insert_one('db','querydollartesterrors', '{"_id": 8, "a" : [0, 1, 2]}', NULL);
SELECT helio_api.insert_one('db','querydollartesterrors', '{"_id": 9, "a" : { "c": 1 }}', NULL);

/* insert paths with nested objects arrays */
SELECT helio_api.insert_one('db','querydollartesterrors', '{"_id": 10, "a" : { "b" : [ 0, 1, 2 ] } }', NULL);
SELECT helio_api.insert_one('db','querydollartesterrors', '{"_id": 11, "a" : [ { "b": 0 }, { "b": 1 }, { "b": 3.0 }] }', NULL);
SELECT helio_api.insert_one('db','querydollartesterrors', '{"_id": 12, "a" : [ { "b": [-1, 1, 2] }, { "b": [0, 1, 2] }, { "b": [0, 1, 7] }] }', NULL);
SELECT helio_api.insert_one('db','querydollartesterrors', '{"_id": 13, "a" : [ { "b": [[-1, 1, 2]] }, { "b": [[0, 1, 2]] }, { "b": [[0, 1, 7]] }] }', NULL);
SELECT helio_api.insert_one('db','querydollartesterrors', '{"_id": 14, "a" : { "b" : [ { "1" : [1, 2, 3 ] } ] } }', NULL);

-- Negative test for $in/$nin
SELECT document-> '_id' FROM helio_api.collection('db', 'querydollartesterrors') WHERE document @@ '{ "h.b": { "$in" : [{"$elemMatch": {"b": 1}}]  }}';
SELECT document-> '_id' FROM helio_api.collection('db', 'querydollartesterrors') WHERE document @@ '{ "h.b": { "$nin" : [{"$elemMatch": {"b": 1}}]  }}';