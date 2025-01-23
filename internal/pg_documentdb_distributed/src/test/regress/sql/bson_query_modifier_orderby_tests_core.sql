
SET search_path TO helio_core,helio_api,helio_api_catalog,helio_api_internal;
SET citus.next_shard_id TO 418960000;
SET helio_api.next_collection_id TO 4189600;
SET helio_api.next_collection_index_id TO 4189600;

/* insert paths with nested objects arrays */
SELECT helio_api.insert_one('db','bsonorderby', '{"_id": 9, "a" : { "b" : 1 } }', NULL);
SELECT helio_api.insert_one('db','bsonorderby', '{"_id": 10, "a" : { "b" : [ 0, 1, 2 ] } }', NULL);
SELECT helio_api.insert_one('db','bsonorderby', '{"_id": 11, "a" : [ { "b": 0 }, { "b": 1 }, { "b": 3.0 }] }', NULL);
SELECT helio_api.insert_one('db','bsonorderby', '{"_id": 12, "a" : [ { "b": [-1, 1, 2] }, { "b": [0, 1, 2] }, { "b": [0, 1, 7] }] }', NULL);
SELECT helio_api.insert_one('db','bsonorderby', '{"_id": 13, "a" : [ { "b": [[-1, 1, 2]] }, { "b": [[0, 1, 2]] }, { "b": [[0, 1, 7]] }] }', NULL);



SELECT object_id, document FROM helio_api.collection('db', 'bsonorderby') ORDER BY bson_orderby(document, '{ "a.b": 1 }') ASC;

SELECT object_id, document FROM helio_api.collection('db', 'bsonorderby') ORDER BY bson_orderby(document, '{ "a.b": -1 }') DESC;

SELECT object_id, document FROM helio_api.collection('db', 'bsonorderby') ORDER BY bson_orderby(document, '{ "a.b.0": -1 }') DESC;

SELECT object_id, document FROM helio_api.collection('db', 'bsonorderby') ORDER BY bson_orderby(document, '{ "a.b.1": 1 }') ASC;

SELECT object_id, document FROM helio_api.collection('db', 'bsonorderby') ORDER BY bson_orderby(document, '{ "a.b": 1 }') ASC, bson_orderby(document, '{ "a.b.0": 1 }') ASC;

SELECT object_id, document FROM helio_api.collection('db', 'bsonorderby') ORDER BY bson_orderby(document, '{ "a.b": 1 }') ASC, bson_orderby(document, '{ "a.b.0": -1 }') DESC;


SELECT object_id, document FROM helio_api.collection('db', 'bsonorderby') WHERE document @@ '{ "a.b": { "$gt": 0 } }' ORDER BY bson_orderby(document, '{ "a.b": 1 }');

SELECT object_id, document FROM helio_api.collection('db', 'bsonorderby') WHERE document @@ '{ "a.b.0": { "$gte": 0 } }' ORDER BY bson_orderby(document, '{ "a.b": 1 }');

SELECT object_id, document FROM helio_api.collection('db', 'bsonorderby') WHERE document @@ '{ "a.b.0": { "$gte": 0 } }' ORDER BY bson_orderby(document, '{ "a.b": 1 }'), bson_orderby(document, '{ "a.b.0": 1 }');

SELECT object_id, document FROM helio_api.collection('db', 'bsonorderby') WHERE document @@ '{ "a.b.0": { "$gte": 0 } }' ORDER BY bson_orderby(document, '{ "a.b": 1 }'), bson_orderby(document, '{ "a.b.1": 1 }');

SELECT object_id, document FROM helio_api.collection('db', 'bsonorderby') WHERE document @@ '{ "a.b": { "$gte": 0 } }' ORDER BY bson_orderby(document, '{ "a.b": 1 }'), bson_orderby(document, '{ "a.b.1": 1 }');

SELECT object_id, document FROM helio_api.collection('db', 'bsonorderby') WHERE document @@ '{ "a.b": { "$in": [ 0, 1, 2, 3 ] } }' ORDER BY bson_orderby(document, '{ "a.b": 1 }');


PREPARE q1(bson) AS SELECT object_id, document FROM helio_api.collection('db', 'bsonorderby') ORDER BY bson_orderby(document, $1);
PREPARE q1desc(bson) AS SELECT object_id, document FROM helio_api.collection('db', 'bsonorderby') ORDER BY bson_orderby(document, $1) DESC;

EXECUTE q1('{ "a.b.0": 1 }');
EXECUTE q1desc('{ "a.b.0": -1 }');
EXECUTE q1('{ "a.b.0": 1 }');
EXECUTE q1desc('{ "a.b.0": -1 }');
EXECUTE q1('{ "a.b.0": 1 }');
EXECUTE q1desc('{ "a.b.0": -1 }');
EXECUTE q1('{ "a.b.0": 1 }');

-- now insert items that are sorted "After" arrays (e.g. boolean)
SELECT helio_api.insert_one('db','bsonorderby', '{"_id": 14, "a" : [ { "b": [ true, false ] }, { "b": [ true ] } ] }', NULL);
SELECT helio_api.insert_one('db','bsonorderby', '{"_id": 15, "a" : { "b": [ [ true, false], [ false, true ] ] } }', NULL);
SELECT helio_api.insert_one('db','bsonorderby', '{"_id": 16, "a" : { "b": true } }', NULL);

-- doesn't consider the array itself but considers nested arrays.
SELECT object_id, document FROM helio_api.collection('db', 'bsonorderby') ORDER BY bson_orderby(document, '{ "a.b": 1 }');
SELECT object_id, document FROM helio_api.collection('db', 'bsonorderby') ORDER BY bson_orderby(document, '{ "a.b": -1 }') DESC;


-- sort order across types (see sorta.js).
SELECT helio_api.insert_one('db', 'sortordertests', '{ "_id": 0, "a": { "$minKey": 1 } }');
SELECT helio_api.insert_one('db', 'sortordertests', '{ "_id": 3, "a": null }');
SELECT helio_api.insert_one('db', 'sortordertests', '{ "_id": 1, "a": [] }');
SELECT helio_api.insert_one('db', 'sortordertests', '{ "_id": 7, "a": [ 2 ] }');
SELECT helio_api.insert_one('db', 'sortordertests', '{ "_id": 4 }');
SELECT helio_api.insert_one('db', 'sortordertests', '{ "_id": 5, "a": null }');
SELECT helio_api.insert_one('db', 'sortordertests', '{ "_id": 2, "a": [] }');
SELECT helio_api.insert_one('db', 'sortordertests', '{ "_id": 6, "a": 1 }');
SELECT helio_api.insert_one('db', 'sortordertests', '{ "_id": 8, "a": { "$maxKey": 1 } }');

SELECT object_id, document FROM helio_api.collection('db', 'sortordertests') ORDER BY bson_orderby(document, '{ "a": 1 }'), object_id;


SELECT bson_orderby('{ "b": 1 }', '{ "b": -1 }') = '{ "b": 1 }';
SELECT bson_orderby('{ "b": 1 }', '{ "b": 1 }') = '{ "b": 1 }';
SELECT bson_orderby('{ "b": { "c" : 1 } }', '{ "b": -1 }') = '{ "b": { "c": 1 } }';
SELECT bson_orderby('{ "b": { "c" : 1 } }', '{ "b": 1 }') = '{ "b": { "c": 1 } }';

SELECT bson_orderby('{ "b": [ { "c" : 1 } ] }', '{ "b": -1 }') = '{ "b" : { "c": 1 } }';
SELECT bson_orderby('{ "b": [ { "c" : 1 } ] }', '{ "b": 1 }') = '{ "b" : { "c": 1 } }';

SELECT bson_orderby('{ "b": [ { "c" : 1 }, { "c": 2 } ] }', '{ "b": -1 }') = '{ "b" : { "c": 2 } }';
SELECT bson_orderby('{ "b": [ { "c" : 1 }, { "c": 2 } ] }', '{ "b": 1 }') = '{ "b" : { "c": 1 } }';

SELECT bson_orderby('{ "b": [ 1, 2, 3 ] }', '{ "b": -1 }') = '{ "b": 3 }';
SELECT bson_orderby('{ "b": [ 1, 2, 3 ] }', '{ "b": 1 }') = '{ "b": 1 }';

SELECT bson_orderby('{ "b": [ true, false, false ] }', '{ "b": -1 }') = '{ "b": true }';
SELECT bson_orderby('{ "b": [  true, false, false ] }', '{ "b": 1 }') = '{ "b": false }';

SELECT bson_orderby('{ "b": [ [1], [2], [3] ] }', '{ "b": -1 }') = '{ "b": [3] }';
SELECT bson_orderby('{ "b": [ [1], [2], [3] ] }', '{ "b": 1 }') = '{ "b": [1] }';

SELECT bson_orderby('{ "b": [ 1, true, "someString" ] }', '{ "b": -1 }') = '{ "b": true }';
SELECT bson_orderby('{ "b": [ 1, true, "someString" ] }', '{ "b": 1 }') = '{ "b": 1 }';

SELECT bson_orderby('{ "b": [ 1, { "c": 2 }, "someString" ] }', '{ "b.c": -1 }') = '{ "b.c": 2 }';
