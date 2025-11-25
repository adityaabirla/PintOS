#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define GRID_SIZE 10
#define MAX_STRING_LEN 64
#define FIFO_PREFIX "/tmp/osdoc_fifo_"

typedef enum {
  MSG_READ_REQUEST,
  MSG_WRITE_REQUEST,
  MSG_READ_RESPONSE,
  MSG_WRITE_RESPONSE,
  MSG_SHUTDOWN,
  MSG_DISCONNECT
} MessageType;

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

int client_id_global;
int server_fd = -1;
int client_fd = -1;
pthread_t print_doc_thread;
volatile int should_exit = 0;
pthread_mutex_t comm_mutex = PTHREAD_MUTEX_INITIALIZER;

static long long get_time_ms() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec * 1000LL + tv.tv_usec / 1000;
}

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

void msleep(int milliseconds) {
  struct timespec ts;
  ts.tv_sec = milliseconds / 1000;
  ts.tv_nsec = (milliseconds % 1000) * 1000000;
  nanosleep(&ts, NULL);
}

void print_doc(const char *doc) {
  char filename[64];
  snprintf(filename, sizeof(filename), "output_client%d.txt", client_id_global);
  FILE *fp = fopen(filename, "w");
  if (fp) {
    fprintf(fp, "%s", doc);
    fclose(fp);
  }
}

int send_and_receive(Message *request, Message *response) {
 
  memset(response, 0, sizeof(Message));

  if (write(server_fd, request, sizeof(Message)) < 0) {
    return -1;
  }

  
  usleep(1000); 

  ssize_t bytes = read(client_fd, response, sizeof(Message));
  if (bytes != sizeof(Message)) {
    return -1;
  }

  return 0;
}

void handle_read(int line, int word_pos) {
  Message request, response;
  memset(&request, 0, sizeof(Message));
  request.type = MSG_READ_REQUEST;
  request.client_id = client_id_global;
  request.line = line;
  request.word_pos = word_pos;

  
  printf("Client %d: Requesting READ lock for (%d,%d)\n", client_id_global,
         line, word_pos);

  if (send_and_receive(&request, &response) == 0) {
    if (response.success) {
      printf("Client %d: READ(%d,%d) SUCCESS - Value: '%s'\n", client_id_global,
             line, word_pos, response.word);
    } else {
      printf("Client %d: READ(%d,%d) DROPPED\n", client_id_global, line,
             word_pos);
    }
  }
}

void handle_write(int line, int word_pos, const char *word, int duration_ms) {
  Message request, response;
  memset(&request, 0, sizeof(Message));
  request.type = MSG_WRITE_REQUEST;
  request.client_id = client_id_global;
  request.line = line;
  request.word_pos = word_pos;
  strncpy(request.word, word, MAX_STRING_LEN - 1);
  request.duration_ms = duration_ms;


  printf("Client %d: Requesting WRITE lock for (%d,%d)\n", client_id_global,
         line, word_pos);

  if (send_and_receive(&request, &response) == 0) {
    if (response.success) {
      printf("Client %d: WRITE(%d,%d) = '%s', sleeping for %dms\n",
             client_id_global, line, word_pos, word, duration_ms);
      msleep(duration_ms);
      printf("Client %d: WRITE(%d,%d) COMPLETED\n", client_id_global, line,
             word_pos);
    } else {
      printf("Client %d: WRITE(%d,%d) DROPPED\n", client_id_global, line,
             word_pos);
    }
  }
}

void get_document_state(char *doc_buffer, size_t buffer_size) {
  doc_buffer[0] = '\0';
  for (int i = 0; i < GRID_SIZE; i++) {
    char line_words[GRID_SIZE][MAX_STRING_LEN];
    int word_count = 0;
    int last_content_pos = -1;


    for (int j = 0; j < GRID_SIZE; j++) {
      Message request, response;
      memset(&request, 0, sizeof(Message));
      request.type = MSG_READ_REQUEST;
      request.client_id = client_id_global;
      request.line = i;
      request.word_pos = j;
      request.is_special_read = 1;

      if (send_and_receive(&request, &response) == 0) {
        if (response.success && strlen(response.word) > 0) {
          strcpy(line_words[j], response.word);
          last_content_pos = j;
        } else if (!response.success) {
          strcpy(line_words[j], "???");
          last_content_pos = j;
        } else {
          strcpy(line_words[j], "");
        }
      } else {
        strcpy(line_words[j], "");
      }
    }

    
    if (last_content_pos >= 0) {
      int first = 1;
      for (int j = 0; j <= last_content_pos; j++) {
        if (strlen(line_words[j]) > 0) {
          if (!first)
            strcat(doc_buffer, " ");
          strcat(doc_buffer, line_words[j]);
          first = 0;
        }
      }
      strcat(doc_buffer, "\n");
    }
  }
}

void *print_doc_thread_func(void *arg) {
  (void)arg;
  while (!should_exit) {
    msleep(2000);
    if (should_exit)
      break;
    char doc_buffer[GRID_SIZE * GRID_SIZE * (MAX_STRING_LEN + 1)];
    get_document_state(doc_buffer, sizeof(doc_buffer));
    print_doc(doc_buffer);
  }
  return NULL;
}

int main(int argc, char *argv[]) {
  if (argc != 2)
    return 1;
  int client_id = atoi(argv[1]);
  client_id_global = client_id;

  printf("Client %d: Starting\n", client_id);


  char client_fifo[128];
  snprintf(client_fifo, sizeof(client_fifo), "%sclient_%d", FIFO_PREFIX,
           client_id);
  unlink(client_fifo);

  // Also clean up server FIFO in case it's stale
  char server_fifo[128];
  snprintf(server_fifo, sizeof(server_fifo), "%sserver", FIFO_PREFIX);
  // Don't unlink server fifo as server creates it
  if (mkfifo(client_fifo, 0666) < 0)
    return 1;

  server_fd = open(server_fifo, O_WRONLY);
  if (server_fd < 0)
    return 1;

  client_fd = open(client_fifo, O_RDONLY | O_NONBLOCK);
  if (client_fd < 0) {
    printf("Client %d: Failed to open client FIFO\n", client_id);
    return 1;
  }
  int flags = fcntl(client_fd, F_GETFL);
  fcntl(client_fd, F_SETFL, flags & ~O_NONBLOCK);

  msleep(200);
  pthread_create(&print_doc_thread, NULL, print_doc_thread_func, NULL);

  FILE *fp = fopen("input.txt", "r");
  if (!fp) {
    should_exit = 1;
    pthread_join(print_doc_thread, NULL);
    return 1;
  }

  char line[256];
  while (fgets(line, sizeof(line), fp)) {
    int cmd_client_id;
    char cmd[16];
    if (sscanf(line, "C%d %s", &cmd_client_id, cmd) < 2)
      continue;
    if (cmd_client_id != client_id)
      continue;

    if (strcmp(cmd, "READ") == 0) {
      int line_num, word_pos;
      if (sscanf(line, "C%d READ %d %d", &cmd_client_id, &line_num,
                 &word_pos) == 3) {
        handle_read(line_num, word_pos);
      }
    } else if (strcmp(cmd, "WRITE") == 0) {
      int line_num, word_pos, duration_ms;
      char word[MAX_STRING_LEN];
      if (sscanf(line, "C%d WRITE %d %d %s %d", &cmd_client_id, &line_num,
                 &word_pos, word, &duration_ms) == 5) {
        handle_write(line_num, word_pos, word, duration_ms);
      }
    } else if (strcmp(cmd, "SLEEP") == 0) {
      int sleep_time;
      if (sscanf(line, "C%d SLEEP %d", &cmd_client_id, &sleep_time) == 2) {
        printf("Client %d: Sleeping for %dms\n", client_id, sleep_time);
        msleep(sleep_time);
      }
    }
  }

  fclose(fp);
  msleep(500);
  should_exit = 1;
  pthread_join(print_doc_thread, NULL);

  Message msg;
  memset(&msg, 0, sizeof(Message));
  msg.type = MSG_DISCONNECT;
  msg.client_id = client_id;
  write(server_fd, &msg, sizeof(Message));
  printf("Client %d: Sent disconnect message\n", client_id);

  close(server_fd);
  close(client_fd);
  unlink(client_fifo);
  printf("Client %d: Exiting\n", client_id);
  return 0;
}