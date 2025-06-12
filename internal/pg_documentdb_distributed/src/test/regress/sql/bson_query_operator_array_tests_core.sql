
-- some documents with mixed types
SELECT documentdb_api.insert_one('comp_arrdb', 'composite_array_ops', '{ "_id": 1, "a": 1 }');
SELECT documentdb_api.insert_one('comp_arrdb', 'composite_array_ops', '{ "_id": 2, "a": -500 }');
SELECT documentdb_api.insert_one('comp_arrdb', 'composite_array_ops', '{ "_id": 3, "a": { "$numberLong": "1000" } }');
SELECT documentdb_api.insert_one('comp_arrdb', 'composite_array_ops', '{ "_id": 4, "a": true }');
SELECT documentdb_api.insert_one('comp_arrdb', 'composite_array_ops', '{ "_id": 5, "a": "some string" }');
SELECT documentdb_api.insert_one('comp_arrdb', 'composite_array_ops', '{ "_id": 6, "a": { "b": 1 } }');
SELECT documentdb_api.insert_one('comp_arrdb', 'composite_array_ops', '{ "_id": 7, "a": { "$date": {"$numberLong": "123456"} } }');

-- now insert some documents with arrays with those terms
SELECT documentdb_api.insert_one('comp_arrdb', 'composite_array_ops', '{ "_id": 100, "a": [ 1, "some other string", true ] }');
SELECT documentdb_api.insert_one('comp_arrdb', 'composite_array_ops', '{ "_id": 101, "a": [ true, -500, { "b": 1 }, 4, 5, 6 ] }');
SELECT documentdb_api.insert_one('comp_arrdb', 'composite_array_ops', '{ "_id": 102, "a": [ true, -500, { "b": 1 }, 1, 10, { "$date": {"$numberLong": "123456"} } ] }');

-- now insert some documents with arrays of arrays of those terms
SELECT documentdb_api.insert_one('comp_arrdb', 'composite_array_ops', '{ "_id": 200, "a": [ 1, [ true, "some string" ] ] }');
SELECT documentdb_api.insert_one('comp_arrdb', 'composite_array_ops', '{ "_id": 201, "a": [ true, -500, { "b": 1 }, [ 1, "some other string", true ] ] }');
SELECT documentdb_api.insert_one('comp_arrdb', 'composite_array_ops', '{ "_id": 202, "a": [ [ true, -500, { "b": 1 }, 4, 5, 6 ] ] }');

-- insert empty arrays
SELECT documentdb_api.insert_one('comp_arrdb', 'composite_array_ops', '{ "_id": 300, "a": [ ] }');
SELECT documentdb_api.insert_one('comp_arrdb', 'composite_array_ops', '{ "_id": 301, "a": [ [], "stringValue" ] }');

SELECT document FROM bson_aggregation_find('comp_arrdb', '{ "find": "composite_array_ops", "filter": { "a": { "$eq": [ 1, "some other string", true ] } } }');

SELECT document FROM bson_aggregation_find('comp_arrdb', '{ "find": "composite_array_ops", "filter": { "a": { "$gt": [ 1, "some other string", true ] } } }');

SELECT document FROM bson_aggregation_find('comp_arrdb', '{ "find": "composite_array_ops", "filter": { "a": { "$gte": [ 1, "some other string", true ] } } }');

SELECT document FROM bson_aggregation_find('comp_arrdb', '{ "find": "composite_array_ops", "filter": { "a": { "$gt": [ true, -500, { "b": 1 }, 1, 10, { "$date": {"$numberLong": "123456"} } ] } } }');

SELECT document FROM bson_aggregation_find('comp_arrdb', '{ "find": "composite_array_ops", "filter": { "a": { "$gte": [ true, -500, { "b": 1 }, 1, 10, { "$date": {"$numberLong": "123456"} } ] } } }');

SELECT document FROM bson_aggregation_find('comp_arrdb', '{ "find": "composite_array_ops", "filter": { "a": { "$lt": [ false, -500, { "b": 1 }, 1, 10, { "$date": {"$numberLong": "123456"} } ] } } }');

SELECT document FROM bson_aggregation_find('comp_arrdb', '{ "find": "composite_array_ops", "filter": { "a": { "$lte": [ false, -500, { "b": 1 }, 1, 10, { "$date": {"$numberLong": "123456"} } ] } } }');

SELECT document FROM bson_aggregation_find('comp_arrdb', '{ "find": "composite_array_ops", "filter": { "a": { "$eq": [ ] } } }');

SELECT document FROM bson_aggregation_find('comp_arrdb', '{ "find": "composite_array_ops", "filter": { "a": { "$in": [ [ 1, "some other string", true ], [ 1, [ true, "some string" ] ] ]} } }');

SELECT document FROM bson_aggregation_find('comp_arrdb', '{ "find": "composite_array_ops", "filter": { "a": { "$in": [ [ 1, "some other string", true ], [ ] ]} } }');

SELECT document FROM bson_aggregation_find('comp_arrdb', '{ "find": "composite_array_ops", "filter": { "a": { "$size": 3 } } }');

SELECT document FROM bson_aggregation_find('comp_arrdb', '{ "find": "composite_array_ops", "filter": { "a": { "$size": 2 } } }');

SELECT document FROM bson_aggregation_find('comp_arrdb', '{ "find": "composite_array_ops", "filter": { "a": { "$size": 0 } } }');

SELECT document FROM bson_aggregation_find('comp_arrdb', '{ "find": "composite_array_ops", "filter": { "a": { "$all": [ 1, true ] } } }');

SELECT document FROM bson_aggregation_find('comp_arrdb', '{ "find": "composite_array_ops", "filter": { "a": { "$all": [ 1 ] } } }');

SELECT document FROM bson_aggregation_find('comp_arrdb', '{ "find": "composite_array_ops", "filter": { "a": { "$all": [ { "$elemMatch": { "$gt": 0 } }, { "$elemMatch": { "b": 1 } } ] } } }');

SELECT document FROM bson_aggregation_find('comp_arrdb', '{ "find": "composite_array_ops", "filter": { "a": { "$all": [ { "$elemMatch": { "$gt": 4, "$lt": 6 } }, { "$elemMatch": { "b": 1 } } ] } } }');

SELECT document FROM bson_aggregation_find('comp_arrdb', '{ "find": "composite_array_ops", "filter": { "a": { "$elemMatch": { "$gt": 4, "$lt": 6 } } } }');

SELECT document FROM bson_aggregation_find('comp_arrdb', '{ "find": "composite_array_ops", "filter": { "a": { "$elemMatch": { "b": { "$gt": 0 } } } } }');