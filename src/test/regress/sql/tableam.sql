SET citus.shard_replication_factor to 1;
SET citus.next_shard_id TO 60000;
SET citus.next_placement_id TO 60000;
SET citus.shard_count TO 4;

create schema test_tableam;
set search_path to test_tableam;

SELECT public.run_command_on_coordinator_and_workers($Q$
	SET citus.enable_ddl_propagation TO off;
	CREATE FUNCTION fake_am_handler(internal)
	RETURNS table_am_handler
	AS 'citus'
	LANGUAGE C;
	CREATE ACCESS METHOD fake_am TYPE TABLE HANDLER fake_am_handler;
$Q$);

-- Since Citus assumes access methods are part of the extension, make fake_am
-- owned manually to be able to pass checks on Citus while distributing tables.
ALTER EXTENSION citus ADD ACCESS METHOD fake_am;

--
-- Hash distributed table using a non-default table access method
--

create table test_hash_dist(id int, val int) using fake_am;
insert into test_hash_dist values (1, 1);

select create_distributed_table('test_hash_dist','id', colocate_with := 'none');

select * from test_hash_dist;
insert into test_hash_dist values (1, 1);

-- we should error on following, since this AM is append only
SET client_min_messages TO ERROR;
delete from test_hash_dist where id=1;
update test_hash_dist set val=2 where id=2;
RESET client_min_messages;

-- ddl events should include "USING fake_am"
SELECT * FROM master_get_table_ddl_events('test_hash_dist');

--
-- Reference table using a non-default table access method
--

create table test_ref(a int) using fake_am;
insert into test_ref values (1);

select create_reference_table('test_ref');

select * from test_ref;
insert into test_ref values (1);

-- we should error on following, since this AM is append only
SET client_min_messages TO ERROR;
delete from test_ref;
update test_ref set a=2;
RESET client_min_messages;

-- ddl events should include "USING fake_am"
SELECT * FROM master_get_table_ddl_events('test_ref');

--
-- Range partitioned table using a non-default table access method
--

CREATE TABLE test_range_dist(id int, val int) using fake_am;

SELECT create_distributed_table('test_range_dist', 'id', 'range');

CALL public.create_range_partitioned_shards('test_range_dist', '{"0","25"}','{"24","49"}');

select * from test_range_dist;
insert into test_range_dist values (1, 1);
COPY test_range_dist FROM PROGRAM 'echo 0, 0 && echo 1, -1 && echo 2, 4 && echo 3, 9' WITH CSV;
COPY test_range_dist FROM PROGRAM 'echo 25, 16 && echo 26, 1 && echo 27, 4 && echo 7, 9' WITH CSV;


-- ddl events should include "USING fake_am"
SELECT * FROM master_get_table_ddl_events('test_range_dist');

--
-- Test copy_copy_shard_placement with a fake_am table
--

select a.shardid, a.nodeport
FROM pg_dist_shard b, pg_dist_shard_placement a
WHERE a.shardid=b.shardid AND logicalrelid = 'test_hash_dist'::regclass::oid
ORDER BY a.shardid, nodeport;

-- Change repmodel to allow copy_copy_shard_placement
UPDATE pg_dist_partition SET repmodel='c' WHERE logicalrelid = 'test_hash_dist'::regclass;

SELECT citus_copy_shard_placement(
           get_shard_id_for_distribution_column('test_hash_dist', '1'),
           'localhost', :worker_1_port,
           'localhost', :worker_2_port,
		   transfer_mode := 'block_writes');

select a.shardid, a.nodeport
FROM pg_dist_shard b, pg_dist_shard_placement a
WHERE a.shardid=b.shardid AND logicalrelid = 'test_hash_dist'::regclass::oid
ORDER BY a.shardid, nodeport;

-- verify that data was copied correctly

\c - - - :worker_1_port
select * from test_tableam.test_hash_dist_60000 ORDER BY id;

\c - - - :worker_2_port
select * from test_tableam.test_hash_dist_60000 ORDER BY id;

\c - - - :master_port

set search_path to test_tableam;

--
-- Test that partitioned tables work correctly with a fake_am table
--

-- parent using default am, one of children using fake_am
CREATE TABLE test_partitioned(id int, p int, val int)
PARTITION BY RANGE (p);

CREATE TABLE test_partitioned_p1 PARTITION OF test_partitioned
	FOR VALUES FROM (1) TO (10);
CREATE TABLE test_partitioned_p2 PARTITION OF test_partitioned
	FOR VALUES FROM (11) TO (20) USING fake_am;

INSERT INTO test_partitioned VALUES (1, 5, -1), (2, 15, -2);

SELECT create_distributed_table('test_partitioned', 'id');

INSERT INTO test_partitioned VALUES (3, 6, -6), (4, 16, -4);

SELECT count(*) FROM test_partitioned;

DROP TABLE test_partitioned;

\set VERBOSITY terse
ALTER EXTENSION citus DROP ACCESS METHOD fake_am;
drop schema test_tableam cascade;
