/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * tests/text_search_tests.rs
 *
 *-------------------------------------------------------------------------
 */

use bson::doc;
use documentdb_gateway::error::ErrorCode;

mod common;

#[tokio::test]
async fn text_query_should_fail_no_index() {
    let db = common::initialize_with_db("text_query_should_fail_no_index").await;
    db.create_collection("coll").await.unwrap();
    let collection = db.collection::<bson::Document>("coll");
    let filter = doc! { "$text": { "$search": "some search string" } };
    let result = collection.find(filter).await;

    match result {
        Err(e) => {
            if let mongodb::error::ErrorKind::Command(ref command_error) = *e.kind {
                let code_name = &command_error.code_name;
                assert_eq!(
                    code_name, "IndexNotFound",
                    "Expected codeName to be 'IndexNotFound', got: {}",
                    code_name
                );

                let code = &command_error.code;
                let expected_code = ErrorCode::IndexNotFound as i32;
                assert_eq!(
                    *code, expected_code,
                    "Expected code to be {}, got: {}",
                    expected_code, code
                );

                let error_message = &command_error.message;
                assert!(
                    error_message == "text index required for $text query",
                    "Expected error to be 'text index required for $text query', got: {}",
                    error_message
                );
            } else {
                panic!("Expected Command error kind");
            }
        }
        Ok(_) => panic!("Expected error but got success"),
    }
}
