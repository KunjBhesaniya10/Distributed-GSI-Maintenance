Project Title: Distributed Global Secondary Index Maintenance for Sharded PostgreSQL

Team Members:
1. Kunj Bhesaniya (23B0995)
2. Dhruvraj Merchant (23B1041)
3. Dipen Sojitra (23B0913)
4. Harshil Solanki (23B1016)

Project Goals:
When a table is sharded across PostgreSQL servers by its primary key, a query on any other column (for example, WHERE email = ?) must be sent to every shard. A uniqueness constraint on such a column cannot be enforced at all, since each shard only sees its own rows. Distributed databases such as Spanner, CockroachDB and DynamoDB solve this with global secondary indexes (GSIs): indexes partitioned by the index key, independently of the base table. PostgreSQL has nothing comparable, and neither does the Citus sharding extension.

Our goal is to implement a GSI for a sharded PostgreSQL cluster. A GSI lookup will go to a single index node and then a single base shard instead of all shards. The central challenge is maintenance: every write updates the base row on one node and its index entry on another, so the two must be kept consistent across nodes. We will implement and compare two maintenance strategies:
- Synchronous: the base row and index entry are updated atomically using two-phase commit, so the index is always consistent and can enforce global uniqueness, at the cost of slower writes.
- Asynchronous: index updates are applied from a change stream after the base write commits, giving faster writes with a bounded, measured staleness.

Last semester we built a single-node index over append-only Parquet files. This project addresses a different problem: keeping a partitioned index consistent across nodes under concurrent updates and deletes, and in the presence of node failures.

Implementation Plan:
1. Cluster setup: 3-4 PostgreSQL instances. The base table on a coordinator is hash-partitioned by primary key, with partitions as postgres_fdw foreign tables on the data nodes. PostgreSQL's partition routing already handles base-table reads and writes.
2. Index layout: on each node, GSI entries of the form (index key -> primary key, shard), partitioned by hash of the index key and stored in a B-tree.
3. Synchronous maintenance: an executor hook on the coordinator intercepts INSERT, UPDATE and DELETE on indexed tables, sends the matching index change to the owning index node, and commits both nodes with two-phase commit (PREPARE TRANSACTION). A resolver process completes or rolls back in-doubt transactions after a crash. The index node also enforces global uniqueness.
4. Asynchronous maintenance: a logical decoding output plugin on each data node streams base-table changes to a worker that applies them to the index nodes, tracking how far behind the index is.
5. Query routing: planner integration, through a custom scan or postgres_fdw path, so that equality and range predicates on the indexed column use the GSI instead of broadcasting to all shards.
6. Online index build: building a GSI on an existing table while writes continue, by backfilling existing rows and then catching up on changes made during the backfill.
7. Evaluation:
   - Write latency and throughput with no index, synchronous GSI and asynchronous GSI.
   - Lookup latency with the GSI versus broadcast as the number of shards grows.
   - Index lag in asynchronous mode.
   - Fault-injection tests that crash nodes mid-transaction, then verify that the index and base table remain consistent.

References:
1. W. Tan, S. Tata, Y. Tang, L. Fong. Diff-Index: Differentiated Index in Distributed Log-Structured Data Stores. EDBT, 2014.
2. I. Rae et al. Online, Asynchronous Schema Change in F1. VLDB, 2013.
3. J. C. Corbett et al. Spanner: Google's Globally-Distributed Database. OSDI, 2012.