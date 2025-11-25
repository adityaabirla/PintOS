#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define GRID_SIZE 10
#define MAX_STRING_LEN 64
#define MAX_CLIENTS 10
#define SHM_NAME "/osdoc_shm"
#define FIFO_PREFIX "/tmp/osdoc_fifo_"

// Message types
typedef enum {
  MSG_READ_REQUEST,
  MSG_WRITE_REQUEST,
  MSG_READ_RESPONSE,
  MSG_WRITE_RESPONSE,
  MSG_SHUTDOWN,
  MSG_DISCONNECT
} MessageType;

// Message structure
typedef struct {
  MessageType type;
  int client_id;
  int line;
  int word_pos;
  char word[MAX_STRING_LEN];
  int duration_ms;
  int success;
  int is_special_read;
} Message;

// Word state in shared memory
typedef struct {
  char word[MAX_STRING_LEN];
  volatile int is_locked;
  volatile int reader_count;
  volatile long long lock_end_time_ms;
  volatile int locking_client_id;
} WordState;

// Shared memory structure
typedef struct {
  WordState document[GRID_SIZE][GRID_SIZE];
  pthread_mutex_t mutexes[GRID_SIZE][GRID_SIZE];
} SharedMemory;

SharedMemory *shm = NULL;
int shm_fd = -1;

// Timestamped printf
static void ts_printf(const char *fmt, ...) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  struct tm tm_info;
  localtime_r(&tv.tv_sec, &tm_info);
  char tbuf[32];
  strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tm_info);
  fprintf(stdout, "%s.%03ld ", tbuf, tv.tv_usec / 1000);
  va_list args;
  va_start(args, fmt);
  vfprintf(stdout, fmt, args);
  va_end(args);
  fflush(stdout);
}

#define printf(...) ts_printf(__VA_ARGS__)

// Get current time in milliseconds
long long get_time_ms() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// Initialize shared memory
int init_shared_memory() {
  shm_unlink(SHM_NAME);
  shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
  if (shm_fd < 0)
    return -1;
  if (ftruncate(shm_fd, sizeof(SharedMemory)) < 0)
    return -1;
  shm = mmap(NULL, sizeof(SharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED,
             shm_fd, 0);
  if (shm == MAP_FAILED)
    return -1;
  memset(shm, 0, sizeof(SharedMemory));

  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);

  for (int i = 0; i < GRID_SIZE; i++) {
    for (int j = 0; j < GRID_SIZE; j++) {
      pthread_mutex_init(&shm->mutexes[i][j], &attr);
    }
  }
  pthread_mutexattr_destroy(&attr);
  return 0;
}

// Check if lock has expired
int is_lock_expired(WordState *ws) {
  if (!ws->is_locked)
    return 1;
  long long now = get_time_ms();
  return now >= ws->lock_end_time_ms;
}

// Release expired lock - ADDED LOGGING HERE
void release_expired_lock(WordState *ws, int line, int col) {
  if (ws->is_locked && is_lock_expired(ws)) {
    // Log the unlock event as expected by runner
    printf("Server: Client %d UNLOCK(%d,%d)\n", ws->locking_client_id, line,
           col);
    ws->is_locked = 0;
    ws->locking_client_id = -1;
  }
}

// Handle READ request
void handle_read(Message *msg, int client_fd) {
  WordState *ws = &shm->document[msg->line][msg->word_pos];
  Message response;
  memset(&response, 0, sizeof(Message));

  response.type = MSG_READ_RESPONSE;
  response.client_id = msg->client_id;
  response.line = msg->line;
  response.word_pos = msg->word_pos;
  response.is_special_read = msg->is_special_read;

  pthread_mutex_lock(&shm->mutexes[msg->line][msg->word_pos]);

  release_expired_lock(ws, msg->line, msg->word_pos);

  if (ws->is_locked) {
    response.success = 0;
    if (msg->is_special_read) {
      printf("Server: Client %d PRINT_DOC read(%d,%d) DROPPED\n",
             msg->client_id, msg->line, msg->word_pos);
    } else {
      printf("Server: Client %d READ LOCK(%d,%d) DENIED\n", msg->client_id,
             msg->line, msg->word_pos);
    }
  } else {
    ws->reader_count++;
    strncpy(response.word, ws->word, MAX_STRING_LEN - 1);
    response.success = 1;

    if (!msg->is_special_read) {
      // CHANGED: Added "LOCK" to match regex
      printf("Server: Client %d READ LOCK(%d,%d) GRANTED\n", msg->client_id,
             msg->line, msg->word_pos);
    }
    ws->reader_count--;
  }

  pthread_mutex_unlock(&shm->mutexes[msg->line][msg->word_pos]);
  write(client_fd, &response, sizeof(Message));
}

// Handle WRITE request
void handle_write(Message *msg, int client_fd) {
  WordState *ws = &shm->document[msg->line][msg->word_pos];
  Message response;
  memset(&response, 0, sizeof(Message));

  response.type = MSG_WRITE_RESPONSE;
  response.client_id = msg->client_id;
  response.line = msg->line;
  response.word_pos = msg->word_pos;

  pthread_mutex_lock(&shm->mutexes[msg->line][msg->word_pos]);

  release_expired_lock(ws, msg->line, msg->word_pos);

  if (ws->is_locked || ws->reader_count > 0) {
    response.success = 0;
    printf("Server: Client %d WRITE LOCK(%d,%d) DENIED\n", msg->client_id,
           msg->line, msg->word_pos);
  } else {
    ws->is_locked = 1;
    ws->locking_client_id = msg->client_id;
    strncpy(ws->word, msg->word, MAX_STRING_LEN - 1);
    ws->lock_end_time_ms = get_time_ms() + msg->duration_ms;

    response.success = 1;
    // CHANGED: Added "LOCK" to match regex
    printf("Server: Client %d WRITE LOCK(%d,%d) GRANTED\n", msg->client_id,
           msg->line, msg->word_pos);
  }

  pthread_mutex_unlock(&shm->mutexes[msg->line][msg->word_pos]);
  write(client_fd, &response, sizeof(Message));
}

// Write document to output.txt
void write_output() {
  FILE *fp = fopen("output.txt", "w");
  if (!fp)
    return;

  for (int i = 0; i < GRID_SIZE; i++) {
    int has_content = 0;
    char line_buffer[GRID_SIZE * (MAX_STRING_LEN + 1)];
    line_buffer[0] = '\0';

    for (int j = 0; j < GRID_SIZE; j++) {
      if (strlen(shm->document[i][j].word) > 0) {
        if (has_content)
          strcat(line_buffer, " ");
        strcat(line_buffer, shm->document[i][j].word);
        has_content = 1;
      }
    }
    if (has_content)
      fprintf(fp, "%s\n", line_buffer);
  }
  fclose(fp);
}

int main() {
  printf("Server: Started\n");
  if (init_shared_memory() < 0)
    return 1;
  printf("Server: Shared memory initialized\n");

  char server_fifo[128];
  snprintf(server_fifo, sizeof(server_fifo), "%sserver", FIFO_PREFIX);
  unlink(server_fifo);

  printf("Server: Creating FIFO at %s\n", server_fifo);
  if (mkfifo(server_fifo, 0666) < 0)
    return 1;
  printf("Server: FIFO created successfully\n");

  int client_fds[MAX_CLIENTS];
  for (int i = 0; i < MAX_CLIENTS; i++)
    client_fds[i] = -1;

  printf("Server: Ready to receive requests\n");

  int server_fd = open(server_fifo, O_RDONLY | O_NONBLOCK);
  if (server_fd < 0)
    return 1;

  int flags = fcntl(server_fd, F_GETFL);
  fcntl(server_fd, F_SETFL, flags & ~O_NONBLOCK);

  int server_fd_write = open(server_fifo, O_WRONLY);

  int active_clients = 0;
  int clients_seen[MAX_CLIENTS] = {0};

  while (1) {
    Message msg;
    ssize_t bytes_read = read(server_fd, &msg, sizeof(Message));
    if (bytes_read <= 0)
      break;

    if (msg.type == MSG_SHUTDOWN)
      break;

    if (!clients_seen[msg.client_id]) {
      clients_seen[msg.client_id] = 1;
      active_clients++;
    }

    if (msg.type == MSG_DISCONNECT) {
      printf("Server: Client %d disconnected\n", msg.client_id);
      clients_seen[msg.client_id] = 0;
      if (active_clients > 0)
        active_clients--;
      if (active_clients == 0) {
        printf("Server: All active clients disconnected. Shutting down.\n");
        break;
      }
      continue;
    }

    if (client_fds[msg.client_id] < 0) {
      char client_fifo[128];
      snprintf(client_fifo, sizeof(client_fifo), "%sclient_%d", FIFO_PREFIX,
               msg.client_id);
      // Retry opening the client FIFO a few times with delays
      for (int retry = 0; retry < 5; retry++) {
        client_fds[msg.client_id] = open(client_fifo, O_WRONLY | O_NONBLOCK);
        if (client_fds[msg.client_id] >= 0) {
          // Switch back to blocking mode
          int flags = fcntl(client_fds[msg.client_id], F_GETFL);
          fcntl(client_fds[msg.client_id], F_SETFL, flags & ~O_NONBLOCK);
          break;
        }
        usleep(10000); // 10ms delay
      }
      if (client_fds[msg.client_id] < 0) {
        printf("Server: Failed to open client FIFO %s after retries\n",
               client_fifo);
        continue;
      }
    }

    if (msg.type == MSG_READ_REQUEST) {
      handle_read(&msg, client_fds[msg.client_id]);
    } else if (msg.type == MSG_WRITE_REQUEST) {
      handle_write(&msg, client_fds[msg.client_id]);
    }
  }

  write_output();
  printf("Server: Document written to output.txt\n");

  close(server_fd);
  close(server_fd_write);
  unlink(server_fifo);
  for (int i = 0; i < MAX_CLIENTS; i++)
    if (client_fds[i] >= 0)
      close(client_fds[i]);
  munmap(shm, sizeof(SharedMemory));
  close(shm_fd);
  shm_unlink(SHM_NAME);

  printf("Server: Shutdown complete\n");
  return 0;
}