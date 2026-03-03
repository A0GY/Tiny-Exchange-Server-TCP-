````markdown
# Tiny Exchange Server (Project 3)

Async TCP matching engine server in **C++23** using **Boost.Asio**.

- Per-connection sessions
- Line message → command → engine pipeline
- Replies with `ACK` / `FILL` / `CANCEL_OK` / `CANCEL_REJECT`
- Deterministic behaviour: all order-book mutation runs on a single **strand** (one execution context)

## Build

```bash
g++ -std=c++23 -O2 -Wall -Wextra -pedantic tiny_exchange_server_asio.cpp -o tiny_exchange -lboost_system -pthread
````

## Run

```bash
./tiny_exchange 9001
```

Optional threads arg (network IO), engine remains serialized:

```bash
./tiny_exchange 9001 2
```

## Protocol (one command per line)

Client sends:

* `NEW BUY <price> <qty>`
* `NEW SELL <price> <qty>`
* `CANCEL <order_id>`
* `PRINTBOOK`
* `QUIT`

Server replies:

* `ACK <order_id>`
* `FILL <buy_id> <sell_id> <price> <qty> <ts>` (0..N lines after ACK)
* `CANCEL_OK <order_id>` / `CANCEL_REJECT <order_id>`
* `END`

## Quick test

```bash
nc 127.0.0.1 9001
NEW SELL 100 5
NEW BUY 100 5
PRINTBOOK
QUIT
```

```
```
