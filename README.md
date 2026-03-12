# Dynamic Worker Orchestration Demo

This project demonstrates a simple master/worker execution model where workers can be started and stopped while a computation is already running.

The original goal was to support:
- a master running an iterative computation,
- workers joining progressively during execution,
- work being redistributed at the beginning of each iteration based on the workers currently connected.

## Why this version does not use MPI for dynamic workers

The first experiments used MPI dynamic process features such as `MPI_Comm_connect`, `MPI_Comm_accept`, and `MPI_Comm_spawn`.

That approach can work with some MPI implementations, but in practice it proved unreliable across environments, especially with Intel MPI in this setup. Since the requirement is to be able to add workers freely, at any time, and in an implementation-independent way, the current version uses a plain TCP connection between the master and workers.

This keeps the behavior simple and portable:
- the master is a normal process,
- each worker is a normal process,
- workers connect to the master over localhost,
- workers can be launched at any time,
- workers can be stopped cleanly on request.

## High-level architecture

There are two binaries:
- [orchestrator.cc](orchestrator.cc): the master process
- [worker.cc](worker.cc): the worker process

And four helper scripts:
- [build.sh](build.sh): compile both binaries
- [run_orch.sh](run_orch.sh): start the master
- [add_worker.sh](add_worker.sh): request and launch one or more workers
- [kill_worker.sh](kill_worker.sh): request that one or more workers be stopped

### Runtime model

The master runs an iterative loop.

At the beginning of each iteration it:
1. accepts newly connected workers,
2. reads pending stop requests,
3. stops as many workers as requested,
4. reads pending add requests,
5. accepts any newly launched workers that connected,
6. distributes the current iteration workload across all active workers.

If no worker is connected, the master computes the whole iteration locally.

## Files created at runtime

The system uses a few files and directories in the project root.

### Endpoint file

The master writes its TCP endpoint to [mpi_port.txt](mpi_port.txt).

Despite the file name, this is no longer an MPI port. It now contains:
- the host,
- the TCP port,
used by workers to connect to the master.

The current implementation uses localhost only:
- host: `127.0.0.1`
- port: dynamically chosen by the master

### Add request queue

Directory: `worker_requests`

When [add_worker.sh](add_worker.sh) is called, it creates one ticket file per requested worker inside `worker_requests/`.

Those tickets are used as a simple queue. The master scans the directory at the beginning of the next iteration and removes the tickets it has consumed.

### Stop request queue

Directory: `worker_kill_requests`

When [kill_worker.sh](kill_worker.sh) is called, it creates one ticket file per worker that should be stopped.

At the beginning of the next iteration, the master consumes those tickets and stops the corresponding number of currently connected workers.

### Worker logs

Directory: `worker_logs`

Each worker launched by [add_worker.sh](add_worker.sh) is started with `nohup`, and its stdout/stderr is redirected to a dedicated log file in `worker_logs/`.

This is useful because workers run in the background.

## Communication protocol

The protocol between master and workers is intentionally minimal.

### Commands

Defined in [orchestrator.cc](orchestrator.cc#L17-L25) and [worker.cc](worker.cc#L13-L18):
- `1` = work command
- `2` = stop command

### Work payload

The master sends a `WorkPayload` struct containing:
- `iteration`: current iteration index
- `begin`: first work item index for that worker
- `count`: number of items assigned to that worker

See:
- [orchestrator.cc](orchestrator.cc#L27-L32)
- [worker.cc](worker.cc#L16-L21)

### Result

A worker computes a single `int64` partial result and sends it back to the master.

## Detailed master behavior

The master implementation is in [orchestrator.cc](orchestrator.cc).

### 1. Startup

At startup, the master:
- removes old runtime artifacts,
- recreates the request directories,
- opens a TCP server socket on `127.0.0.1` with an ephemeral port,
- writes the selected endpoint to [mpi_port.txt](mpi_port.txt).

Relevant functions:
- `initialize_request_directory()`: [orchestrator.cc](orchestrator.cc#L160-L166)
- `create_server_socket()`: [orchestrator.cc](orchestrator.cc#L91-L138)
- `write_endpoint_file()`: [orchestrator.cc](orchestrator.cc#L140-L144)

### 2. Accepting workers

The master accepts incoming worker connections with a non-blocking listening socket.

This means:
- if no worker is waiting, the master continues immediately,
- if one or more workers connected already, they are added to the active worker list.

Relevant code:
- `accept_pending_workers()`: [orchestrator.cc](orchestrator.cc#L175-L188)

Each connected worker gets an internal master-side ID.

### 3. Processing stop requests

The master scans `worker_kill_requests/` and, for each ticket, stops one currently active worker.

Current policy:
- workers are stopped from the end of the internal vector,
- in practice this means the most recently tracked worker is removed first.

Relevant code:
- stop request scan: [orchestrator.cc](orchestrator.cc#L248-L265)
- actual stop: `stop_one_worker()`: [orchestrator.cc](orchestrator.cc#L190-L207)

A stop request sends a `STOP` command, closes the socket, and removes the worker from the active list.

### 4. Processing add requests

The master scans `worker_requests/` and removes the tickets it finds.

The actual worker processes are launched by the script, not by the master. The tickets simply indicate that new workers are expected to appear.

After consuming the tickets, the master calls `accept_pending_workers()` again to register workers that connected during this phase.

Relevant code:
- add request scan: [orchestrator.cc](orchestrator.cc#L267-L282)

### 5. Distributing work

If there are active workers, the master splits the total work of the current iteration across them.

The split strategy is simple:
- `base_chunk = total_work_items / number_of_workers`
- the first `remainder = total_work_items % number_of_workers` workers receive one extra item

Relevant code:
- work distribution: [orchestrator.cc](orchestrator.cc#L291-L322)

### 6. Collecting results

The master then waits for one partial result from each active worker.

If a worker disappears unexpectedly during send or receive:
- the master removes that worker from the active list,
- execution continues with the remaining workers.

Relevant code:
- send failure handling: [orchestrator.cc](orchestrator.cc#L304-L317)
- receive failure handling: [orchestrator.cc](orchestrator.cc#L324-L335)

### 7. Fallback local execution

If no worker is connected, the master computes the whole iteration itself.

Relevant code:
- fallback path: [orchestrator.cc](orchestrator.cc#L286-L290)

## Detailed worker behavior

The worker implementation is in [worker.cc](worker.cc).

### 1. Discovering the master

A worker starts by reading [mpi_port.txt](mpi_port.txt), which contains:
- host
- port

Relevant code:
- `read_endpoint()`: [worker.cc](worker.cc#L49-L65)

### 2. Connecting to the master

The worker opens a TCP socket and connects to the master endpoint.

Relevant code:
- `connect_to_master()`: [worker.cc](worker.cc#L67-L87)

On success, it logs something like:
- `[worker pid=12345] connected`

### 3. Waiting for commands

A worker then enters an infinite loop:
- receive a command,
- if command is `STOP`, exit cleanly,
- if command is `WORK`, receive the payload, compute the partial result, send it back.

Relevant code:
- worker main loop: [worker.cc](worker.cc#L135-L176)

## The example computation

This repository currently uses a simple artificial computation as a demonstration.

For each work item `value` in a chunk, the contribution is:

$$
(value + 1) \times (iteration + 1)
$$

The partial result is the sum over the assigned range.

This same logic exists:
- locally in the master: [orchestrator.cc](orchestrator.cc#L62-L71)
- in the worker: [worker.cc](worker.cc#L89-L98)

This is only a placeholder for a real application workload.

## How to build

Use:

- [build.sh](build.sh)

The build script:
- looks for `c++` or `g++`,
- compiles both binaries with C++17,
- produces `orchestrator` and `worker`.

Relevant lines:
- [build.sh](build.sh#L1-L13)

## How to run

### 1. Build

```sh
./build.sh
```

### 2. Start the master

```sh
./run_orch.sh
```

Or with custom parameters:

```sh
./run_orch.sh <iterations> <work_items_per_iteration> <pause_ms>
```

Examples:

```sh
./run_orch.sh 30 120 2000
./run_orch.sh 100 1000000 500
```

The current defaults are defined in [run_orch.sh](run_orch.sh#L1-L9).

### 3. Add workers while the master is running

```sh
./add_worker.sh 2
```

This will:
- create 2 add-request tickets,
- launch 2 background `worker` processes,
- store their logs in `worker_logs/`.

Relevant script:
- [add_worker.sh](add_worker.sh#L1-L32)

### 4. Stop workers while the master is running

```sh
./kill_worker.sh 1
```

This will:
- create 1 stop-request ticket,
- the master will stop one connected worker at the beginning of the next iteration.

Relevant script:
- [kill_worker.sh](kill_worker.sh#L1-L21)

## Example scenario

Start the master:

```sh
./run_orch.sh 20 120 2000
```

After a few iterations, add two workers:

```sh
./add_worker.sh 2
```

Later, remove one worker:

```sh
./kill_worker.sh 1
```

Typical master output:

```text
Endpoint: 127.0.0.1:33069
[master] iteration 0, active workers=0
[master] no workers, local computation -> ...
[master] 2 worker request(s) pending
[master] worker #1 connected, total active=1
[master] worker #2 connected, total active=2
[master] task sent to worker #1 : begin=0, count=60
[master] task sent to worker #2 : begin=60, count=60
```

Worker log example:

```text
[worker pid=12345] connected
[worker pid=12345] iteration 5, begin=0, count=60, result=...
[worker pid=12345] stop requested
```

Note: the current implementation logs from the C++ code are mostly in French, while this document is in English.

## Important semantics

### Workers are integrated at iteration boundaries

A newly launched worker is not inserted in the middle of an already running iteration.

It is discovered and used only when the master reaches the beginning of the next iteration.

The same rule applies to stop requests.

### Add requests do not themselves create the connection

The ticket file and the worker process are two separate mechanisms:
- the ticket says: “the user requested new workers”
- the actual `worker` process must still connect to the master

In practice, [add_worker.sh](add_worker.sh) performs both actions.

### The system is local-only by default

The endpoint is bound to `127.0.0.1`, so workers must run on the same machine as the master.

If remote workers are needed, the host binding and scripts would need to be adapted.

## Failure handling

The current implementation is intentionally simple but includes a few protections:
- if a worker disconnects, the master removes it,
- if a send fails, the worker is dropped,
- if a receive fails, the worker is dropped,
- if no workers exist, the master still progresses locally.

That said, this is still a demo, not a production fault-tolerant scheduler.

## Limitations

Current limitations include:
- workers are only accepted at iteration boundaries,
- stop requests remove workers in reverse connection order,
- there is no authentication,
- communication is plain local TCP,
- the protocol is binary and versionless,
- the endpoint file name is still [mpi_port.txt](mpi_port.txt) for historical reasons,
- worker logs are not rotated automatically.

## Possible future improvements

Some natural next steps would be:
- accept workers continuously instead of only between iterations,
- support remote hosts,
- replace ticket directories with a control socket or RPC API,
- let the user target specific workers to stop,
- add heartbeat messages,
- make the work protocol extensible,
- replace the demo computation with the real application kernel,
- rename [mpi_port.txt](mpi_port.txt) to something clearer such as `master_endpoint.txt`.

## Repository contents

Main files:
- [orchestrator.cc](orchestrator.cc)
- [worker.cc](worker.cc)
- [build.sh](build.sh)
- [run_orch.sh](run_orch.sh)
- [add_worker.sh](add_worker.sh)
- [kill_worker.sh](kill_worker.sh)

Runtime-generated files/directories:
- [mpi_port.txt](mpi_port.txt)
- `worker_requests/`
- `worker_kill_requests/`
- `worker_logs/`

## Summary

This project now implements a simple dynamic worker system with the following properties:
- the master starts alone,
- workers can be launched later,
- any number of workers can be added over time,
- workers can be stopped on demand,
- work is redistributed at each iteration according to the active workers,
- the design no longer depends on MPI dynamic process support.
