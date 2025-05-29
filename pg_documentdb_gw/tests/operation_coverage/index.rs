use bson::{doc, Document};
use mongodb::IndexModel;

mod common;

#[tokio::test]
async fn create_index() {
    let db = common::initialize_with_db("create_index").await;

    let result = db
        .collection::<Document>("test")
        .create_index(IndexModel::builder().keys(doc! {"a":1}).build())
        .await
        .unwrap();
    assert_eq!(result.index_name, "a_1");

    let indexes = db
        .collection::<Document>("test")
        .list_index_names()
        .await
        .unwrap();
    assert_eq!(indexes.len(), 2);
}

#[tokio::test]
async fn create_list_drop_index() {
    let db = common::initialize_with_db("drop_indexes").await;

    let coll = db.collection("test");
    coll.insert_one(doc! {"a": 1}).await.unwrap();

    db.collection::<Document>("test")
        .create_index(IndexModel::builder().keys(doc! {"a":1}).build())
        .await
        .unwrap();

    db.collection::<Document>("test")
        .drop_indexes()
        .await
        .unwrap();

    coll.drop().await.unwrap();
}
