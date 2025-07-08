SET search_path TO documentdb_core,documentdb_api,documentdb_api_catalog,documentdb_api_internal;
SET citus.next_shard_id TO 220000;
SET documentdb.next_collection_id TO 2200;
SET documentdb.next_collection_index_id TO 2200;

-- arrayFilters with aggregation pipeline
SELECT newDocument as bson_update_document FROM documentdb_api_internal.bson_update_document(
    '{"_id": 1 }','{ "": [ { "$addFields": { "fieldA.fieldB": 10 } }]}', '{}', '{ "": []}');
SELECT newDocument as bson_update_document FROM documentdb_api_internal.bson_update_document(
    '{"_id": 1 }','{ "": [ { "$addFields": { "fieldA.fieldB": 10 } }]}', '{}', NULL);
SELECT newDocument as bson_update_document FROM documentdb_api_internal.bson_update_document(
    '{"_id": 1 }','{ "": [ { "$addFields": { "fieldA.fieldB": 10 } }]}', '{}', '{ "": [ { "filterX": 30 }]}');

-- arrayFilters ignored on replace
SELECT newDocument as bson_update_document FROM documentdb_api_internal.bson_update_document(
    '{"_id": 1 }','{ "": { "fieldC": 40 } }', '{}', '{ "": [ { "filterX": 50 }]}');

-- arrayFilters with update fails - missing array filter
SELECT newDocument as bson_update_document FROM documentdb_api_internal.bson_update_document(
    '{"_id": 1 }','{ "": { "$set": { "arrayA.$[itemA]": 60 }}}', '{}', '{ "": [] }');

-- arrayFilters with update fails - invalid array filters
SELECT newDocument as bson_update_document FROM documentdb_api_internal.bson_update_document(
    '{"_id": 1 }','{ "": { "$set": { "arrayA.$[itemA]": 70 }}}', '{}', '{ "": [ 2 ] }');
SELECT newDocument as bson_update_document FROM documentdb_api_internal.bson_update_document(
    '{"_id": 1 }','{ "": { "$set": { "arrayA.$[itemA]": 70 }}}', '{}', '{ "": [ {} ] }');
SELECT newDocument as bson_update_document FROM documentdb_api_internal.bson_update_document(
    '{"_id": 1 }','{ "": { "$set": { "arrayA.$[itemA]": 70 }}}', '{}', '{ "": [ { "": 3} ] }');
SELECT newDocument as bson_update_document FROM documentdb_api_internal.bson_update_document(
    '{"_id": 1 }','{ "": { "$set": { "arrayA.$[itemA]": 70 }}}', '{}', '{ "": [ { "itemA": 4, "itemB.itemC": 5 } ] }');
SELECT newDocument as bson_update_document FROM documentdb_api_internal.bson_update_document(
    '{"_id": 1 }','{ "": { "$set": { "arrayA.$[itemA]": 70 }}}', '{}', '{ "": [ { "itemA": 6 }, { "itemA": 7 } ] }');

-- simple array update on equality
SELECT newDocument as bson_update_document FROM documentdb_api_internal.bson_update_document(
    '{"_id": 1, "numbers": [ 100, 200 ] }','{ "": { "$set": { "numbers.$[numElem]": 300 }}}', '{}', '{ "": [{ "numElem": 100 }] }');
SELECT newDocument as bson_update_document FROM documentdb_api_internal.bson_update_document(
    '{}','{ "": { "$set": { "numbers.$[numElem]": 300 }}}', '{"_id": 1, "numbers": [ 100, 200 ] }', '{ "": [{ "numElem": 100 }] }');
SELECT newDocument as bson_update_document FROM documentdb_api_internal.bson_update_document(
    '{"_id": 1 }','{ "": { "$set": { "numbers.$[numElem]": 300 }}}', '{}', '{ "": [{ "numElem": 100 }] }');

-- updates on $gte condition
SELECT newDocument as bson_update_document FROM documentdb_api_internal.bson_update_document(
    '{ "_id" : 1, "scores" : [ 150, 120, 110 ], "age": 15 }','{ "": { "$set": { "scores.$[scoreElem]": 200 }}}', '{}', '{ "": [{ "scoreElem": { "$gte": 200 } }] }');
SELECT newDocument as bson_update_document FROM documentdb_api_internal.bson_update_document(
    '{ "_id" : 3, "scores" : [ 150, 210, 200, 180, 202 ], "age": 16 }','{ "": { "$set": { "scores.$[scoreElem]": 200 }}}', '{}', '{ "": [{ "scoreElem": { "$gte": 200 } }] }');

-- nested arrayFilters.
SELECT newDocument as bson_update_document FROM documentdb_api_internal.bson_update_document(
    '{ "_id" : 3, "metrics" : [ { "value": 58, "max": 136, "avg": 66, "dev": 88}, { "value": 96, "max": 176, "avg": 99, "dev": 75}, { "value": 68, "max":168, "avg": 86, "dev": 83 } ] }',
    '{ "": { "$set": { "metrics.$[metricElem].avg": 100 }}}', '{}', '{ "": [{ "metricElem.value": { "$gte": 60 } }] }');
SELECT newDocument as bson_update_document FROM documentdb_api_internal.bson_update_document(
    '{ "_id" : 3, "metrics" : [ { "value": 58, "max": 136, "avg": 66, "dev": 88}, { "value": 96, "max": 176, "avg": 99, "dev": 75 }, { "value": 68, "max":168, "avg": 86, "dev": 83 } ] }',
    '{ "": { "$inc": { "metrics.$[metricElem].dev": -50 }}}', '{}', '{ "": [{ "metricElem.value": { "$gte": 60 }, "metricElem.dev": { "$gte": 80 } }] }');
SELECT newDocument as bson_update_document FROM documentdb_api_internal.bson_update_document(
    '{ "_id" : 3, "metrics" : [ { "value": 58, "max": 136, "avg": 66, "dev": 88}, { "value": 96, "max": 176, "avg": 99, "dev": 75 }, { "value": 68, "max":168, "avg": 86, "dev": 83 } ] }',
    '{ "": { "$inc": { "metrics.$[metricElem].dev": -50 }}}', '{}', '{ "": [{ "metricElem.value": { "$gte": 60 }, "metricElem.dev": { "$gte": 75 } }] }');

-- negation operators
SELECT newDocument as bson_update_document FROM documentdb_api_internal.bson_update_document(
    '{ "_id" : 1, "degreesList" : [ { "level": "PhD", "age": 28}, { "level": "Bachelor", "age": 22} ] }',
    '{ "": { "$set" : { "degreesList.$[deg].gradYear" : 2020 }} }', '{}', '{ "": [{ "deg.level": { "$ne": "Bachelor" } }] }');

-- multiple positional operators
SELECT newDocument as bson_update_document FROM documentdb_api_internal.bson_update_document(
    '{ "_id" : 1, "results" : [ { "type": "quiz", "answers": [ 20, 18, 15 ] }, { "type": "quiz", "answers": [ 18, 19, 16 ] }, { "type": "hw", "answers": [ 15, 14, 13 ] }, { "type": "exam", "answers": [ 35, 20, 33, 10 ] }] }',
    '{ "": { "$inc": { "results.$[typeElem].answers.$[ansScore]": 190 }} }', '{}', '{ "": [{ "typeElem.type": "quiz" }, { "ansScore": { "$gte": 18 } }] }');
SELECT newDocument as bson_update_document FROM documentdb_api_internal.bson_update_document(
    '{ "_id" : 1, "results" : [ { "type": "quiz", "answers": [ 20, 18, 15 ] }, { "type": "quiz", "answers": [ 18, 19, 16 ] }, { "type": "hw", "answers": [ 15, 14, 13 ] }, { "type": "exam", "answers": [ 35, 20, 33, 10 ] }] }',
    '{ "": { "$inc": { "results.$[].answers.$[ansScore]": 190 }} }', '{}', '{ "": [{ "ansScore": { "$gte": 18 } }] }');
SELECT newDocument as bson_update_document FROM documentdb_api_internal.bson_update_document(
    '{ "_id" : 1, "results" : [ { "type": "quiz", "answers": [ 20, 18, 15 ] }, { "type": "quiz", "answers": [ 18, 19, 16 ] }, { "type": "hw", "answers": [ 15, 14, 13 ] }, { "type": "exam", "answers": [ 35, 20, 33, 10 ] }] }',
    '{ "": { "$inc": { "results.$[typeElem].answers.$[]": 190 }} }', '{}', '{ "": [{ "typeElem.type": "quiz" }] }');

-- arrayFilters for all Update operators should recurse if for a single level nested array
-- array update operators
SELECT newDocument as bson_update_document FROM documentdb_api_internal.bson_update_document(
    '{ "_id" : 1, "matrix" : [ [0], [1] ] }',
    '{ "": { "$addToSet": { "matrix.$[row]": 2 }} }', '{}', '{ "": [{ "row": 0 }] }');
SELECT newDocument as bson_update_document FROM documentdb_api_internal.bson_update_document(
    '{ "_id" : 1, "matrix" : [ [0, 1], [1, 2] ] }',
    '{ "": { "$pop": { "matrix.$[row]": 1 }} }', '{}', '{ "": [{ "row": 0 }] }');
SELECT newDocument as bson_update_document FROM documentdb_api_internal.bson_update_document(
    '{ "_id" : 1, "matrix" : [ [0, 1], [1, 2] ] }',
    '{ "": { "$pull": { "matrix.$[row]": 1 }} }', '{}', '{ "": [{ "row": 2 }] }');
SELECT newDocument as bson_update_document FROM documentdb_api_internal.bson_update_document(
    '{ "_id" : 1, "matrix" : [ [0, 1], [1, 2] ] }',
    '{ "": { "$pull": { "matrix.$[row]": 1 }} }', '{}', '{ "": [{ "row": 2 }] }');
SELECT newDocument as bson_update_document FROM documentdb_api_internal.bson_update_document(
    '{ "_id" : 1, "matrix" : [ [0, 1], [2, 3] ] }',
    '{ "": { "$push": { "matrix.$[row]": 1 }} }', '{}', '{ "": [{ "row": 1 }] }');

-- field update operators, should be able to match but apply update based on the type requirement
SELECT newDocument as bson_update_document FROM documentdb_api_internal.bson_update_document(
    '{ "_id" : 1, "matrix" : [ [0], [1] ] }',
    '{ "": { "$inc": { "matrix.$[row]": 10 }} }', '{}', '{ "": [{ "row": 0 }] }');
SELECT newDocument as bson_update_document FROM documentdb_api_internal.bson_update_document(
    '{ "_id" : 1, "matrix" : [ [0], [1] ] }',
    '{ "": { "$min": { "matrix.$[row]": 10 }} }', '{}', '{ "": [{ "row": 0 }] }');
SELECT newDocument as bson_update_document FROM documentdb_api_internal.bson_update_document(
    '{ "_id" : 1, "matrix" : [ [0], [1] ] }',
    '{ "": { "$max": { "matrix.$[row]": 10 }} }', '{}', '{ "": [{ "row": 0 }] }');
SELECT newDocument as bson_update_document FROM documentdb_api_internal.bson_update_document(
    '{ "_id" : 1, "matrix" : [ [0], [1] ] }',
    '{ "": { "$mul": { "matrix.$[row]": 2 }} }', '{}', '{ "": [{ "row": 0 }] }');
SELECT newDocument as bson_update_document FROM documentdb_api_internal.bson_update_document(
    '{ "_id" : 1, "matrix" : [ [0], [1] ] }',
    '{ "": { "$rename": { "matrix.$[row]": "arrayA.3" }} }', '{}', '{ "": [{ "row": 0 }] }');
SELECT newDocument as bson_update_document FROM documentdb_api_internal.bson_update_document(
    '{ "_id" : 1, "matrix" : [ [0], [1] ] }',
    '{ "": { "$set": { "matrix.$[row]": "updatedValue" }} }', '{}', '{ "": [{ "row": 0 }] }');

-- bit operator
SELECT newDocument as bson_update_document FROM documentdb_api_internal.bson_update_document(
    '{ "_id" : 1, "matrix" : [ [0], [1] ] }',
    '{ "": { "$bit": { "matrix.$[row]": {"or": 5} }} }', '{}', '{ "": [{ "row": 0 }] }');

-- Check array value should also match in arrayFilters
SELECT newDocument as bson_update_document FROM documentdb_api_internal.bson_update_document(
    '{ "_id" : 1, "matrix" : [ [11,12,13], [14,15,16] ] }',
    '{ "": { "$set": { "matrix.$[row]": [21,22,23] }} }', '{}', '{ "": [{ "row": [11,12,13] }] }');
SELECT newDocument as bson_update_document FROM documentdb_api_internal.bson_update_document(
    '{ "_id" : 1, "matrix" : [ [11,12,13], [14,15,16] ] }',
    '{ "": { "$set": { "matrix.$[row]": 33 }} }', '{}', '{ "": [{ "row": {"$size": 3} }] }');
