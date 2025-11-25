# OS-Doc

## Overview

A simulation of a collaborative document editing system that demonstrates concurrent read/write operations on a 10×10 grid-based document. Multiple clients compete for access under a time-bounded reader-writer synchronization model.

### Key Features

- **Concurrent Access Control**: Time-bounded reader-writer locks with automatic expiration
- **IPC Communication**: POSIX Named Pipes (FIFOs) for client-server messaging
- **Shared State Management**: POSIX Shared Memory with fine-grained locking
- **Real-time Synchronization**: Millisecond-precision timestamps for lock management
- **Background Document Snapshots**: Periodic document state capture per client

### Components

- **Server**: Single authoritative process managing document state and coordinating access
- **Clients**: Multiple concurrent processes issuing read/write operations from scripted commands

---

## Table of Contents

- [System Architecture](#system-architecture)
- [Data Structures](#data-structures)
- [IPC Strategy](#ipc-strategy)
- [Synchronization Logic](#synchronization-logic)
- [Client Implementation](#client-implementation)
- [Server Implementation](#server-implementation)
- [Logging](#logging-and-observability)

---

## System Architecture

### Topology

```
        ┌─────────────────┐
        │     Server      │
        │  (Single Node)  │
        └────────┬────────┘
                 │
         ┌───────┼───────┐
         │       │       │
    ┌────▼───┐ ┌▼────┐ ┌▼────┐
    │Client 0│ │Cl 1 │ │Cl N │
    └────────┘ └─────┘ └─────┘
```

**Star Topology with Bidirectional Channels:**
- Server maintains one shared read FIFO for incoming requests
- Each client has a dedicated response FIFO
- All state resides in server-managed shared memory

### Communication Flow

```
Client                    Server                  Shared Memory
  |                         |                           |
  |--[MSG_WRITE_REQUEST]-->|                           |
  |                         |--[lock mutex]----------->|
  |                         |--[check/update state]--->|
  |                         |--[unlock mutex]--------->|
  |<--[MSG_WRITE_RESPONSE]-|                           |
  |                         |                           |
  |--[MSG_READ_REQUEST]--->|                           |
  |                         |--[lock/check/unlock]---->|
  |<--[MSG_READ_RESPONSE]--|                           |
```

---

## Data Structures

### Communication Protocol

```c
typedef enum {
  MSG_READ_REQUEST,      // Client→Server: Request to read a cell
  MSG_WRITE_REQUEST,     // Client→Server: Request to write a cell
  MSG_READ_RESPONSE,     // Server→Client: Read result
  MSG_WRITE_RESPONSE,    // Server→Client: Write acknowledgment
  MSG_SHUTDOWN,          // Admin→Server: Graceful shutdown signal
  MSG_DISCONNECT         // Client→Server: Client termination notice
} MessageType;

typedef struct {
  MessageType type;           // Operation identifier
  int client_id;              // Sender/recipient ID (0-9)
  int line;                   // Grid row (0-9)
  int word_pos;               // Grid column (0-9)
  char word[64];              // Payload (for writes/read responses)
  int duration_ms;            // Write lock duration (milliseconds)
  int success;                // Response status (1=granted, 0=denied)
  int is_special_read;        // Flag: 1=silent read for printing
} Message;
```

**Design Rationale:**
- **Fixed Size**: Struct size (~128 bytes) fits well within `PIPE_BUF` (4096 bytes on Linux), ensuring atomic read/write operations on the pipe
- **Self-Contained**: Each message carries complete context, eliminating need for session state on pipes
- **Bidirectional Protocol**: Same struct used for requests and responses, simplifying parsing

### Shared Memory Layout

```c
typedef struct {
  char word[64];                    // Cell content
  volatile int is_locked;           // Write lock status (0/1)
  volatile int reader_count;        // Active readers (currently unused in impl)
  volatile long long lock_end_time_ms;  // Absolute expiration timestamp
  volatile int locking_client_id;   // Owner of write lock (-1 if none)
} WordState;

typedef struct {
  WordState document[10][10];          // The 10×10 grid
  pthread_mutex_t mutexes[10][10];     // Per-cell mutexes
} SharedMemory;
```

**Design Rationale:**

1. **Fine-Grained Locking**: 100 independent mutexes (one per cell) maximize concurrency. Client A writing to (0,0) doesn't block Client B reading from (9,9).

2. **Volatile Semantics**: All state variables marked `volatile` to prevent compiler optimizations that cache values across function calls, ensuring memory consistency.

3. **Time-Bounded Locks**: Write locks automatically expire after `duration_ms`, preventing indefinite deadlocks if a client crashes while holding a lock.

4. **Process-Shared Mutexes**: Mutexes initialized with `PTHREAD_PROCESS_SHARED` attribute, allowing synchronization across processes (not just threads).

---

## IPC Strategy

### Named Pipes (FIFOs)

**Server FIFO**: `/tmp/osdoc_fifo_server`
- **Pattern**: Many-to-One
- **Usage**: All clients write requests here; server reads sequentially
- **Mode**: Server opens as `O_RDONLY` (initially non-blocking, then switched to blocking)
- **Trick**: Server also opens a write handle (`O_WRONLY`) to prevent EOF when the last client disconnects

**Client FIFOs**: `/tmp/osdoc_fifo_client_<ID>`
- **Pattern**: One-to-One per client
- **Usage**: Server writes responses to specific client's FIFO
- **Mode**: Server opens as `O_WRONLY` with retry logic; Client opens as `O_RDONLY` (blocking)

### FIFO Lifecycle

```
Server Startup:
  1. unlink(server_fifo)          // Clean stale file
  2. mkfifo(server_fifo, 0666)
  3. fd_read = open(O_RDONLY | O_NONBLOCK)
  4. Set fd_read to blocking mode
  5. fd_write = open(O_WRONLY)    // Keeps pipe open

Client Startup:
  1. unlink(client_fifo)
  2. mkfifo(client_fifo, 0666)
  3. Open server_fifo (O_WRONLY)  // Connects to server
  4. Open client_fifo (O_RDONLY)  // Waits for server

Server-Client Handshake:
  1. Client sends first request (e.g., READ/WRITE)
  2. Server sees client_id, attempts open(client_fifo, O_WRONLY)
     - Retry up to 5 times with 10ms delays
  3. If successful, server caches client_fd for future responses
```

**Race Condition Mitigation**: The retry loop handles the timing window where a client has sent a request but hasn't yet opened its FIFO for reading.

### Shared Memory Management

```c
// Server initialization
shm_unlink(SHM_NAME);                          // Clean previous instance
shm_fd = shm_open(SHM_NAME, O_CREAT|O_RDWR, 0666);
ftruncate(shm_fd, sizeof(SharedMemory));
shm = mmap(NULL, sizeof(SharedMemory), 
           PROT_READ|PROT_WRITE, MAP_SHARED, shm_fd, 0);

// Initialize all mutexes with process-shared attribute
pthread_mutexattr_t attr;
pthread_mutexattr_init(&attr);
pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
for (each cell):
  pthread_mutex_init(&shm->mutexes[i][j], &attr);
```

**Why Shared Memory?**
- **State Persistence**: Document survives server restarts (if `shm_unlink` isn't called)
- **Future Scalability**: Clients could map read-only views for direct reads without IPC overhead
- **Atomic Operations**: Mutex-protected updates ensure consistency

---

## Synchronization Logic

### Reader-Writer Problem with Time-Bounded Locks

This system implements a **Writer-Preferring** variant with automatic lock expiration.

**Invariants:**
1. Multiple readers OR one writer (mutual exclusion)
2. No reads while a write lock is active
3. Write locks expire after `duration_ms` (lazy cleanup)

### Write Operation Flow

```c
void handle_write(Message *msg, int client_fd) {
  WordState *ws = &shm->document[line][word_pos];
  
  pthread_mutex_lock(&shm->mutexes[line][word_pos]);
  
  // Lazy expiration: Clean up stale locks
  if (ws->is_locked && current_time >= ws->lock_end_time_ms) {
    printf("Server: Client %d UNLOCK(%d,%d)\n", 
           ws->locking_client_id, line, word_pos);
    ws->is_locked = 0;
    ws->locking_client_id = -1;
  }
  
  // Check preconditions
  if (ws->is_locked || ws->reader_count > 0) {
    response.success = 0;  // DENIED
    printf("Server: Client %d WRITE LOCK(%d,%d) DENIED\n", ...);
  } else {
    // Grant write lock
    ws->is_locked = 1;
    ws->locking_client_id = msg->client_id;
    strcpy(ws->word, msg->word);
    ws->lock_end_time_ms = current_time + msg->duration_ms;
    response.success = 1;  // GRANTED
    printf("Server: Client %d WRITE LOCK(%d,%d) GRANTED\n", ...);
  }
  
  pthread_mutex_unlock(&shm->mutexes[line][word_pos]);
  write(client_fd, &response, sizeof(Message));
}
```

**Key Points:**
- Lock expiration is **lazy**: Only checked when new requests arrive
- Write locks are **time-bounded**: Automatically released after `duration_ms`
- Clients receiving `success=1` are responsible for sleeping `duration_ms` before considering the lock released

### Read Operation Flow

```c
void handle_read(Message *msg, int client_fd) {
  WordState *ws = &shm->document[line][word_pos];
  
  pthread_mutex_lock(&shm->mutexes[line][word_pos]);
  
  release_expired_lock(ws, line, word_pos);  // Same lazy cleanup
  
  if (ws->is_locked) {
    response.success = 0;  // DENIED
    if (msg->is_special_read) {
      printf("Server: Client %d PRINT_DOC read(%d,%d) DROPPED\n", ...);
    } else {
      printf("Server: Client %d READ LOCK(%d,%d) DENIED\n", ...);
    }
  } else {
    // Grant read access
    ws->reader_count++;  // Increment (conceptual tracking)
    strcpy(response.word, ws->word);
    response.success = 1;
    
    if (!msg->is_special_read) {
      printf("Server: Client %d READ LOCK(%d,%d) GRANTED\n", ...);
    }
    
    ws->reader_count--;  // Immediate decrement (reads are instantaneous)
  }
  
  pthread_mutex_unlock(&shm->mutexes[line][word_pos]);
  write(client_fd, &response, sizeof(Message));
}
```

**Implementation Note:** In this simulation, `reader_count` is incremented and immediately decremented because reads complete instantly. In a real system, readers would hold the count until finished.

### Lock Expiration Mechanism

```c
int is_lock_expired(WordState *ws) {
  if (!ws->is_locked) return 1;
  return get_time_ms() >= ws->lock_end_time_ms;
}

void release_expired_lock(WordState *ws, int line, int col) {
  if (ws->is_locked && is_lock_expired(ws)) {
    printf("Server: Client %d UNLOCK(%d,%d)\n", 
           ws->locking_client_id, line, col);
    ws->is_locked = 0;
    ws->locking_client_id = -1;
  }
}
```

**Rationale:** Prevents deadlocks caused by:
- Client crashes while holding a lock
- Network delays in distributed scenarios
- Buggy client implementations

---

## Client Implementation

### Multi-Threaded Architecture

```
Main Thread                  Printer Thread
    |                             |
    |--[Parse input.txt]          |
    |--[WRITE (0,0) "Hello"]      |
    |--[send_and_receive()]       |
    |     (blocks on read)        |
    |<--[Response received]       |
    |--[msleep(duration)]         |--[Every 2 seconds]
    |                             |--[Scan entire 10×10 grid]
    |                             |--[is_special_read=1]
    |--[READ (0,1)]               |--[Build local document view]
    |--[SLEEP 500]                |--[Write to output_clientN.txt]
    |--[...]                      |
```

### Command Processing (Main Thread)

```c
while (fgets(line, sizeof(line), fp)) {
  sscanf(line, "C%d %s", &cmd_client_id, cmd);
  if (cmd_client_id != client_id) continue;  // Skip other clients' commands
  
  if (strcmp(cmd, "READ") == 0) {
    sscanf(line, "C%d READ %d %d", &cid, &line_num, &word_pos);
    handle_read(line_num, word_pos);
  }
  else if (strcmp(cmd, "WRITE") == 0) {
    sscanf(line, "C%d WRITE %d %d %s %d", 
           &cid, &line_num, &word_pos, word, &duration_ms);
    handle_write(line_num, word_pos, word, duration_ms);
  }
  else if (strcmp(cmd, "SLEEP") == 0) {
    sscanf(line, "C%d SLEEP %d", &cid, &sleep_time);
    msleep(sleep_time);  // Simulate thinking/network delay
  }
}
```

**Synchronous Model**: The client blocks on each operation, waiting for the server's response before proceeding. This simplifies state management but reduces concurrency.

### Document Printing (Background Thread)

```c
void *print_doc_thread_func(void *arg) {
  while (!should_exit) {
    msleep(2000);  // 2-second intervals
    
    char doc_buffer[LARGE_SIZE];
    for (int i = 0; i < GRID_SIZE; i++) {
      for (int j = 0; j < GRID_SIZE; j++) {
        Message request = {
          .type = MSG_READ_REQUEST,
          .client_id = client_id,
          .line = i,
          .word_pos = j,
          .is_special_read = 1  // Silent mode: no logging
        };
        
        send_and_receive(&request, &response);
        
        if (response.success) {
          // Add word to buffer
        } else {
          // Cell is locked, mark as "???"
        }
      }
    }
    
    write_to_file("output_client<id>.txt", doc_buffer);
  }
  return NULL;
}
```

**Purpose**: Simulates a UI refresh or auto-save feature. The `is_special_read` flag prevents these background reads from cluttering logs.

### Client Lifecycle

```
Startup:
  1. Parse command-line argument (client ID)
  2. Create client FIFO
  3. Open server FIFO (blocks until server is ready)
  4. Open client FIFO for reading
  5. Spawn printer thread
  6. Process commands from input.txt

Shutdown:
  1. Finish processing all commands
  2. Set should_exit = 1
  3. Join printer thread
  4. Send MSG_DISCONNECT to server
  5. Close FIFOs
  6. Unlink client FIFO
```

---

## Server Implementation

### Main Loop

```c
int active_clients = 0;
int clients_seen[MAX_CLIENTS] = {0};

while (1) {
  Message msg;
  read(server_fd, &msg, sizeof(Message));
  
  // Track active clients
  if (!clients_seen[msg.client_id]) {
    clients_seen[msg.client_id] = 1;
    active_clients++;
  }
  
  // Handle different message types
  switch (msg.type) {
    case MSG_DISCONNECT:
      clients_seen[msg.client_id] = 0;
      active_clients--;
      if (active_clients == 0) {
        goto shutdown;  // All clients finished
      }
      break;
      
    case MSG_SHUTDOWN:
      goto shutdown;
      
    case MSG_READ_REQUEST:
      if (client_fds[msg.client_id] < 0) {
        open_client_fifo_with_retry(msg.client_id);
      }
      handle_read(&msg, client_fds[msg.client_id]);
      break;
      
    case MSG_WRITE_REQUEST:
      if (client_fds[msg.client_id] < 0) {
        open_client_fifo_with_retry(msg.client_id);
      }
      handle_write(&msg, client_fds[msg.client_id]);
      break;
  }
}

shutdown:
  write_output();  // Dump document to output.txt
  cleanup_resources();
```

### Graceful Shutdown Logic

**Trigger Conditions:**
1. All clients send `MSG_DISCONNECT` (normal termination)
2. Explicit `MSG_SHUTDOWN` received (admin command)
3. Server FIFO read returns EOF (pipe broken)

**Shutdown Sequence:**
1. Write final document state to `output.txt`
2. Close all client FIFOs
3. Close server FIFO
4. Unlink server FIFO
5. Unmap shared memory
6. Unlink shared memory object

### Client FIFO Opening with Retry

```c
void open_client_fifo_with_retry(int client_id) {
  char client_fifo[128];
  snprintf(client_fifo, sizeof(client_fifo), 
           "/tmp/osdoc_fifo_client_%d", client_id);
  
  for (int retry = 0; retry < 5; retry++) {
    client_fds[client_id] = open(client_fifo, O_WRONLY | O_NONBLOCK);
    if (client_fds[client_id] >= 0) {
      // Switch to blocking mode
      int flags = fcntl(client_fds[client_id], F_GETFL);
      fcntl(client_fds[client_id], F_SETFL, flags & ~O_NONBLOCK);
      return;
    }
    usleep(10000);  // 10ms delay
  }
  
  fprintf(stderr, "Failed to open client FIFO after 5 retries\n");
}
```

**Why Needed?** There's a race condition:
1. Client sends first request (server sees client_id)
2. Server immediately tries to open `/tmp/osdoc_fifo_client_<id>`
3. Client might not have created its FIFO yet!

The retry loop with delays gives the client time to set up its receiving pipe.

---

## Logging and Observability

### Timestamped Output

All logs use a custom `ts_printf` macro that prepends timestamps:

```
Format: HH:MM:SS.mmm Message
Example: 14:32:18.427 Server: Client 2 WRITE LOCK(3,5) GRANTED
```

**Implementation:**
```c
#define printf(...) ts_printf(__VA_ARGS__)

static void ts_printf(const char *fmt, ...) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  // Format: HH:MM:SS.mmm
  fprintf(stdout, "%s.%03ld ", time_string, tv.tv_usec / 1000);
  vfprintf(stdout, fmt, ...);
  fflush(stdout);
}
```

### Log Patterns

**Server Logs:**
- `Server: Client X READ LOCK(Y,Z) GRANTED`
- `Server: Client X READ LOCK(Y,Z) DENIED`
- `Server: Client X WRITE LOCK(Y,Z) GRANTED`
- `Server: Client X WRITE LOCK(Y,Z) DENIED`
- `Server: Client X UNLOCK(Y,Z)` (on expiration)
- `Server: Client X PRINT_DOC read(Y,Z) DROPPED` (special read denied)

**Client Logs:**
- `Client X: Requesting READ lock for (Y,Z)`
- `Client X: READ(Y,Z) SUCCESS - Value: 'word'`
- `Client X: READ(Y,Z) DROPPED`
- `Client X: Requesting WRITE lock for (Y,Z)`
- `Client X: WRITE(Y,Z) = 'word', sleeping for Nms`
- `Client X: WRITE(Y,Z) COMPLETED`
- `Client X: WRITE(Y,Z) DROPPED`
- `Client X: Sleeping for Nms`

**Special Handling:** Background printer thread reads use `is_special_read=1` to avoid log spam during document scanning.

---

## Output Files

### `output.txt` (Server Final State)

Written by server at shutdown. Contains the authoritative final document state:

```
Format: One line per non-empty grid row, words space-separated
Example:
Hello World
The quick brown fox
```

**Generation Logic:**
```c
for (int i = 0; i < GRID_SIZE; i++) {
  for (int j = 0; j < GRID_SIZE; j++) {
    if (strlen(shm->document[i][j].word) > 0) {
      fprintf(fp, "%s ", shm->document[i][j].word);
    }
  }
  fprintf(fp, "\n");
}
```

### `output_client<id>.txt` (Client Snapshots)

Written by each client's printer thread every 2 seconds. Shows the client's view of the document at that moment.

**Locked Cells:** Represented as `???` when read is denied due to active write lock.

---

## Summary

This system demonstrates a collaborative document editor using POSIX IPC primitives. Key design decisions include:

- **Fine-grained locking** for maximum concurrency
- **Time-bounded write locks** to prevent deadlocks
- **FIFO-based request/response pattern** for simplicity
- **Lazy lock expiration** to handle client failures
- **Background document printing** to simulate UI updates

The implementation demonstrates fundamental concepts in concurrent programming, IPC, and distributed state management, providing a foundation for more sophisticated collaborative systems.

---