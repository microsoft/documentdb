#!/bin/bash  
  
# Start the OSS server  
./scripts/start_oss_server.sh 
  
# Start psql and connect to the database  
psql -p 9712 -d postgres  