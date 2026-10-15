# redis-clone

A small Redis-like server written from scratch in C and C++.

> Built while working through ["Build Your Own Redis"](https://build-your-own.org) by James Smith. The goal was to understand how a server like Redis works, from the first byte on the wire to the last entry in a hashtable, not to replace Redis.

## Features

**Networking & I/O**
- Single-threaded, non-blocking event loop serving many clients via `poll()`
- Custom growable buffer with O(1) consume from the front
- Length-prefixed binary request protocol, TLV responses

**Data structures**
- Custom hashtable with chaining and progressive rehashing (no freeze on resize)
- Custom AVL tree with rank queries (subtree sizes)
- Sorted sets: one `ZNode` indexed by name (hashtable) and by score (tree)
- Key TTLs backed by a min-heap

**Timers & concurrency**
- Idle connection timeout (5 minutes of inactivity, configurable via `k_idle_timeout_ms` in `event_server.cpp`)
- Thread pool scaffolding (4 workers, mutex + condition variable queue). Currently only runs a demo task; not yet used for real work.

## Architecture

```
client ──► poll() ──► read into Conn.incoming
                             │
                       try_one_request()
                             │
                        parse_req()
                             ▼
                        do_request()
          ┌───────────┬──────────┬───────────┐
          ▼           ▼          ▼           ▼
      hashtable    AVL tree   TTL heap   (thread pool)
      (get/set)    (zsets)    (expire)    demo only
                             │
                             ▼
              Conn.outgoing ──► write() ──► client

timers: idle-connection list + TTL heap decide poll()'s timeout
```

## Commands

| Group | Command |
|---|---|
| Key-value | `set key value`, `get key`, `del key` |
| Sorted sets | `zadd key score name`, `zscore key name`, `zrem key name`, `zquery key score name offset limit` |
| TTL | `expire key ms`, `ttl key`, `persist key` |

## Build and run

Needs `g++` and pthreads.

```
g++ -Wall -Wextra -O2 -g event_server.cpp -o server
g++ -Wall -Wextra -O2 -g simple_client.cpp -o client
```

Terminal 1:

```
./event_server
```

Terminal 2:

```
./client
```

The client reads one command per line from stdin and prints the tagged response. Press Ctrl+D to disconnect.

```
$ ./client
set name bipin
(nil)
get name
bipin
zadd scores 100 alice
(int) 1
zadd scores 90 bob
(int) 1
zquery scores 0 a 0 10
(array of 4)
  [0] str: bob
  [1] str: 90
  [2] str: alice
  [3] str: 100
```

## Protocol

Request: a list of strings, wrapped in an outer 4-byte length.

```
[total_len][nstr][len1][str1][len2][str2]...
```

Response (TLV):

```
nil:    [0]
err:    [1][4-byte code][4-byte msg_len][msg]
str:    [2][4-byte len][bytes]
int:    [3][8-byte int64]
array:  [5][4-byte count][elements...]
```

Integers and lengths are sent in native byte order (little-endian on x86/ARM). This works when both sides run on the same architecture, which is fine for a learning project. A real wire protocol would fix the byte order explicitly.

## Design notes

- **Nothing blocks the event loop.** Every socket is non-blocking and `poll()` is the only place the thread sleeps, so one thread serves many clients.
- **Intrusive nodes.** The hashtable and AVL nodes are embedded in the data. That means one allocation per entry, and in a sorted set one object lives in two indexes at once. `container_of` converts a node pointer back to its struct.
- **TTL heap back-references.** Each entry stores its `heap_idx`, and the heap updates it whenever an item moves, so the entry always knows where it sits.
- **Progressive rehashing.** Each operation migrates a few entries from the old table to the new one, so growing the table never causes a latency spike.
- **Producer-consumer thread pool.** Workers sleep on a condition variable until a task is queued, then wake up and run it. The queue is protected by a mutex; tasks run outside the lock so workers parallelize.

## About the client

`simple_client.cpp` is a test client, not a real Redis client. It reads commands from stdin, splits on whitespace, sends each line as a request, and prints the response.

Any client that speaks the protocol above works. The protocol is simple enough to implement in a few dozen lines in any language.

## Known limitations

- Binary protocol, not RESP, so `redis-cli` won't work
- No persistence, replication, clustering, auth or TLS
- Only strings and sorted sets (no lists, sets, hashes, streams)
- `poll()` instead of `epoll`
- The thread pool is not yet used for real work
- No tests or benchmarks yet
- The test client splits on whitespace only (no quoted strings)

## Credit

Based on ["Build Your Own Redis"](https://build-your-own.org) by James Smith.
