#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <dlfcn.h>
#include <getopt.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DEFAULT_ENDPOINT_ID 13
#define DEFAULT_TX_WINDOW 1
#define BUFFER_SIZE 4096

/* libcpc's host API uses opaque handles. Avoid including the secondary-side
 * sl_cpc.h here: it describes a different API and its types are not needed
 * when resolving libcpc dynamically. */
typedef int32_t cpc_status_t;
typedef struct cpc_handle cpc_handle_t;
typedef struct cpc_endpoint_handle cpc_endpoint_handle_t;
#define CPC_STATUS_OK 0

static volatile sig_atomic_t stopping = 0;
static int listen_fd = -1;
static int client_fd = -1;
static bool verbose = false;

typedef struct {
  void *so;
  cpc_status_t (*init)(cpc_handle_t **, const char *, void (*)(void *), void *);
  cpc_status_t (*deinit)(cpc_handle_t *);
  cpc_status_t (*open_endpoint)(cpc_endpoint_handle_t **, cpc_handle_t *, uint8_t, uint8_t, uint8_t);
  cpc_status_t (*close_endpoint)(cpc_endpoint_handle_t *);
  cpc_status_t (*read)(cpc_endpoint_handle_t *, void **, uint16_t *, uint32_t, uint8_t);
  cpc_status_t (*write)(cpc_endpoint_handle_t *, void *, uint16_t, uint8_t, void *);
  cpc_status_t (*free_rx_buffer)(void *);
} cpc_api_t;
static cpc_api_t api;

#define cpc_init(...) api.init(__VA_ARGS__)
#define cpc_deinit(...) api.deinit(__VA_ARGS__)
#define cpc_open_endpoint(...) api.open_endpoint(__VA_ARGS__)
#define cpc_close_endpoint(...) api.close_endpoint(__VA_ARGS__)
#define cpc_read(...) api.read(__VA_ARGS__)
#define cpc_write(...) api.write(__VA_ARGS__)
#define cpc_free_rx_buffer(...) api.free_rx_buffer(__VA_ARGS__)

typedef struct {
  cpc_endpoint_handle_t *endpoint;
  int socket_fd;
} bridge_args_t;

static void on_signal(int sig)
{
  (void)sig;
  stopping = 1;
  if (client_fd >= 0) shutdown(client_fd, SHUT_RDWR);
  if (listen_fd >= 0) shutdown(listen_fd, SHUT_RDWR);
}

static void log_msg(const char *message)
{
  fprintf(stderr, "BRIDGE: %s\n", message);
}

static int send_all(int fd, const void *data, size_t len)
{
  const unsigned char *p = data;
  while (len > 0) {
    ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return -1;
    p += (size_t)n;
    len -= (size_t)n;
  }
  return 0;
}

static void *cpc_to_socket(void *arg)
{
  bridge_args_t *a = arg;
  while (!stopping) {
    void *data = NULL;
    uint16_t length = 0;
    cpc_status_t status = cpc_read(a->endpoint, &data, &length, 0, 0);
    if (status != CPC_STATUS_OK) {
      if (!stopping) {
        fprintf(stderr, "BRIDGE: CPC read failed (status 0x%08x)\n", (unsigned)status);
      }
      break;
    }
    if (verbose) fprintf(stderr, "BRIDGE: CPC -> TCP: %u bytes\n", length);
    int rc = send_all(a->socket_fd, data, length);
    cpc_free_rx_buffer(data);
    if (rc != 0) break;
  }
  shutdown(a->socket_fd, SHUT_RDWR);
  return NULL;
}

static void *socket_to_cpc(void *arg)
{
  bridge_args_t *a = arg;
  unsigned char buffer[BUFFER_SIZE];
  while (!stopping) {
    ssize_t n = recv(a->socket_fd, buffer, sizeof(buffer), 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    if (verbose) fprintf(stderr, "BRIDGE: TCP -> CPC: %zd bytes\n", n);
    cpc_status_t status = cpc_write(a->endpoint, buffer, (uint16_t)n, 0, NULL);
    if (status != CPC_STATUS_OK) {
      fprintf(stderr, "BRIDGE: CPC write failed (status 0x%08x)\n", (unsigned)status);
      break;
    }
  }
  shutdown(a->socket_fd, SHUT_RDWR);
  return NULL;
}

static void usage(const char *program)
{
  fprintf(stderr,
          "Usage: %s -n INSTANCE -l LIBCPC_PATH -p PORT [-e ENDPOINT_ID] [-v]\n"
          "  -n, --name       CPC daemon instance name\n"
          "  -l, --library    libcpc shared-library path (for runtime lookup)\n"
          "  -p, --port       TCP listen port\n"
          "  -e, --endpoint   CPC endpoint ID (default: %d)\n"
          "  -v, --verbose    Log transfer sizes\n",
          program, DEFAULT_ENDPOINT_ID);
}

int main(int argc, char **argv)
{
  const char *instance = NULL, *library = NULL;
  unsigned long port = 0, endpoint_id = DEFAULT_ENDPOINT_ID;
  static const struct option opts[] = {
    {"name", required_argument, NULL, 'n'},
    {"library", required_argument, NULL, 'l'},
    {"port", required_argument, NULL, 'p'},
    {"endpoint", required_argument, NULL, 'e'},
    {"verbose", no_argument, NULL, 'v'},
    {"help", no_argument, NULL, 'h'},
    {NULL, 0, NULL, 0}
  };
  int opt;
  while ((opt = getopt_long(argc, argv, "n:l:p:e:vh", opts, NULL)) != -1) {
    char *end = NULL;
    switch (opt) {
      case 'n': instance = optarg; break;
      case 'l': library = optarg; break;
      case 'p':
        errno = 0; port = strtoul(optarg, &end, 10);
        if (errno || !end || *end || port == 0 || port > 65535) { usage(argv[0]); return 2; }
        break;
      case 'e':
        errno = 0; endpoint_id = strtoul(optarg, &end, 0);
        if (errno || !end || *end || endpoint_id > 255) { usage(argv[0]); return 2; }
        break;
      case 'v': verbose = true; break;
      case 'h': usage(argv[0]); return 0;
      default: usage(argv[0]); return 2;
    }
  }
  if (!instance || !library || !port || optind != argc) { usage(argv[0]); return 2; }

  api.so = dlopen(library, RTLD_NOW | RTLD_LOCAL);
  if (!api.so) { fprintf(stderr, "BRIDGE: cannot load %s: %s\n", library, dlerror()); return 1; }
#define LOAD_API(member, symbol) do { *(void **)(&api.member) = dlsym(api.so, symbol); \
  if (!api.member) { fprintf(stderr, "BRIDGE: missing %s in %s\n", symbol, library); dlclose(api.so); return 1; } } while (0)
  LOAD_API(init, "cpc_init");
  LOAD_API(deinit, "cpc_deinit");
  LOAD_API(open_endpoint, "cpc_open_endpoint");
  LOAD_API(close_endpoint, "cpc_close_endpoint");
  LOAD_API(read, "cpc_read");
  LOAD_API(write, "cpc_write");
  LOAD_API(free_rx_buffer, "cpc_free_rx_buffer");
#undef LOAD_API

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);

  listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) { perror("socket"); return 1; }
  int reuse = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons((uint16_t)port);
  if (bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(listen_fd, 1) < 0) {
    perror("bind/listen"); close(listen_fd); return 1;
  }
  log_msg("Listen OK");

  cpc_handle_t *cpc = NULL;
  cpc_status_t status = cpc_init(&cpc, instance, NULL, NULL);
  if (status != CPC_STATUS_OK) {
    fprintf(stderr, "BRIDGE: CPC Init failed (status 0x%08x)\n", (unsigned)status);
    close(listen_fd); dlclose(api.so); return 1;
  }
  log_msg("CPC Init success");

  cpc_endpoint_handle_t *endpoint = NULL;
  status = cpc_open_endpoint(&endpoint, cpc, (uint8_t)endpoint_id, DEFAULT_TX_WINDOW, 0);
  if (status != CPC_STATUS_OK) {
    fprintf(stderr, "BRIDGE: CPC endpoint %lu open failed (status 0x%08x)\n", endpoint_id, (unsigned)status);
    cpc_deinit(cpc); close(listen_fd); dlclose(api.so); return 1;
  }

  while (!stopping) {
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    int fd = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
    if (fd < 0) {
      if (errno == EINTR) continue;
      if (!stopping) perror("accept");
      break;
    }
    client_fd = fd;
    if (verbose) log_msg("TCP client connected");
    bridge_args_t args = { endpoint, fd };
    pthread_t tx, rx;
    int e1 = pthread_create(&tx, NULL, cpc_to_socket, &args);
    int e2 = e1 ? e1 : pthread_create(&rx, NULL, socket_to_cpc, &args);
    if (e1 || e2) {
      fprintf(stderr, "BRIDGE: pthread_create: %s\n", strerror(e1 ? e1 : e2));
      shutdown(fd, SHUT_RDWR);
      if (!e1) pthread_join(tx, NULL);
    } else {
      pthread_join(tx, NULL);
      pthread_join(rx, NULL);
    }
    close(fd);
    client_fd = -1;
    if (verbose && !stopping) log_msg("TCP client disconnected");
  }

  cpc_close_endpoint(endpoint);
  cpc_deinit(cpc);
  if (listen_fd >= 0) close(listen_fd);
  dlclose(api.so);
  return 0;
}
