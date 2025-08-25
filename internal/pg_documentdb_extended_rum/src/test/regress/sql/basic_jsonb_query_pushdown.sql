set search_path to documentdb_extended_rum_jsonb_ops,public;

CREATE TABLE public.jsonb_basic_query (val jsonb);

INSERT INTO public.jsonb_basic_query VALUES ('{ "a": { "b": 1, "c": 3 }}'), ('{ "a": { "b": 2, "c": 4 }}');

-- can query as runtime can use the new jsonpath operator
SELECT * FROM public.jsonb_basic_query WHERE val @@@ '$.a.b == 2';
SELECT * FROM public.jsonb_basic_query WHERE val @@@ '$.a.b == 1';

-- it's a seqscan that executes it
EXPLAIN (COSTS OFF) SELECT * FROM public.jsonb_basic_query WHERE val @@@ '$.a.b == 2';

-- now create an extended_rum index on the jsonb
set client_min_messages to DEBUG1;
CREATE INDEX my_idx_ab ON public.jsonb_basic_query USING documentdb_extended_rum(val jsonb_path_extended_rum_ops(pathspec='$.a.b',wildcard=true));
CREATE INDEX my_idx_ac ON public.jsonb_basic_query USING documentdb_extended_rum(val jsonb_path_extended_rum_ops(pathspec='$.a.c'));
reset client_min_messages;

-- now each query gets pushed to the appropriate index.
set enable_seqscan to off;
EXPLAIN (COSTS OFF) SELECT * FROM public.jsonb_basic_query WHERE val @@@ '$.a.b == 2';
EXPLAIN (COSTS OFF) SELECT * FROM public.jsonb_basic_query WHERE val @@@ '$.a.b.c == 2';
EXPLAIN (COSTS OFF) SELECT * FROM public.jsonb_basic_query WHERE val @@@ '$.a.c == 2';

-- but this one can't be pushed (since a.c is not a wildcard)
EXPLAIN (COSTS OFF) SELECT * FROM public.jsonb_basic_query WHERE val @@@ '$.a.c.d == 2';

SELECT * FROM public.jsonb_basic_query WHERE val @@@ '$.a.b == 2';
SELECT * FROM public.jsonb_basic_query WHERE val @@@ '$.a.b.c == 2';
SELECT * FROM public.jsonb_basic_query WHERE val @@@ '$.a.c == 2';
SELECT * FROM public.jsonb_basic_query WHERE val @@@ '$.a.c == 4';

SELECT * FROM public.jsonb_basic_query WHERE val @@@ '$.a.c > 3';
SELECT * FROM public.jsonb_basic_query WHERE val @@@ '$.a.c > 4';
SELECT * FROM public.jsonb_basic_query WHERE val @@@ '$.a.c < 2';
SELECT * FROM public.jsonb_basic_query WHERE val @@@ '$.a.c < 5';