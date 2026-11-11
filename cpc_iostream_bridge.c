/***************************************************************************//**
 * @file
 * @brief cpc_iostream_bridge.c
 *******************************************************************************
 * # License
 * <b>Copyright 2026 Silicon Laboratories Inc. www.silabs.com</b>
 *******************************************************************************
 *
 * SPDX-License-Identifier: Zlib
 *
 * The licensor of this software is Silicon Laboratories Inc.
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 *
 ******************************************************************************/

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
#include <sys/types.h>
#include "sl_cpc.h"

#define DEFAULT_ENDPOINT_ID 13
#define DEFAULT_TX_WINDOW 1
#define BUFFER_SIZE 4096

/* libcpc's host API uses opaque handles. */
typedef int32_t cpc_status_t;
//typedef struct cpc_handle cpc_handle_t;
//typedef struct cpc_endpoint_handle cpc_endpoint_t;
#define CPC_STATUS_OK 0

static volatile sig_atomic_t stopping = 0;
static int listen_fd = -1;
static int client_fd = -1;
static bool verbose = false;


typedef struct {
  cpc_endpoint_t endpoint;
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


/**
 * Sends all bytes from a buffer over a POSIX socket.
 * 
 * @param socket_fd The active socket file descriptor.
 * @param buffer    Pointer to the data to be sent.
 * @param length    Pointer to the total number of bytes to send. 
 *                  On return, it holds the number of bytes actually sent.
 * @return          0 on success, -1 on failure (with errno set).
 */
int send_all(int socket_fd, const void *buffer, ssize_t *length) {
    size_t total_sent = 0;        // How many bytes we've sent so far
    size_t bytes_left = *length;  // How many bytes we have left to send
    ssize_t n;

    const char *ptr = (const char *)buffer;

    while (total_sent < *length) {
        // MSG_NOSIGNAL prevents the process from crashing with SIGPIPE 
        // if the remote side closed the connection.
        n = send(socket_fd, ptr + total_sent, bytes_left, MSG_NOSIGNAL);
        
        if (n == -1) {
            // If interrupted by a signal, try again
            if (errno == EINTR) {
                continue;
            }
            // For non-blocking sockets: buffer is full, try again later
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // In a real application, you should use select/poll/epoll here 
                // to wait until the socket becomes writable again.
                continue; 
            }
            // A hard error occurred (e.g., EPIPE, ECONNRESET)
            break;
        }
        
        total_sent += n;
        bytes_left -= n;
    }

    *length = total_sent; // Set the actual number of bytes sent for the caller

    return (n == -1) ? -1 : 0; // Return -1 on error, 0 on success
}


static void *cpc_to_socket(void *arg)
{
  ssize_t nb_bytes;
  bridge_args_t *a = arg;
  while (!stopping) {
    uint8_t cpc_tx_buf[SL_CPC_READ_MINIMUM_SIZE];
    nb_bytes = cpc_read_endpoint(a->endpoint, cpc_tx_buf, SL_CPC_READ_MINIMUM_SIZE, 
	CPC_ENDPOINT_READ_FLAG_NONE);
    if (verbose && nb_bytes > 0) {
      fprintf(stderr, "BRIDGE: CPC -> TCP: %u bytes\n", nb_bytes);
    }
    if (nb_bytes > 0) {
      int rc = send_all(a->socket_fd, cpc_tx_buf, &nb_bytes);
    }
    //if (rc != 0) break;
  }
  shutdown(a->socket_fd, SHUT_RDWR);
  return NULL;
}

static void *socket_to_cpc(void *arg)
{
  bridge_args_t *a = arg;
  ssize_t nb_bytes;
  unsigned char buffer[BUFFER_SIZE];
  while (!stopping) {
    ssize_t n = recv(a->socket_fd, buffer, sizeof(buffer), 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    if (verbose) fprintf(stderr, "BRIDGE: TCP -> CPC: %zd bytes\n", n);
    nb_bytes = cpc_write_endpoint(a->endpoint, buffer, (uint16_t)n, CPC_ENDPOINT_WRITE_FLAG_NONE);
    if (verbose) fprintf(stderr, "BRIDGE: TCP -> CPC: %u bytes\n", nb_bytes);
  }
  shutdown(a->socket_fd, SHUT_RDWR);
  return NULL;
}

static void usage(const char *program)
{
  fprintf(stderr,
          "Usage: %s -n INSTANCE -l LIBCPC_PATH -p PORT [-e ENDPOINT_ID] [-v]\n"
          "  -n, --name       CPC daemon instance name\n"
          "  -p, --port       TCP listen port\n"
          "  -e, --endpoint   CPC endpoint ID (default: %d)\n"
          "  -v, --verbose    Log transfer sizes\n",
          program, DEFAULT_ENDPOINT_ID);
}

int main(int argc, char **argv)
{
  const char *instance = NULL;
  unsigned long port = 0, endpoint_id = DEFAULT_ENDPOINT_ID;
  static const struct option opts[] = {
    {"name", required_argument, NULL, 'n'},
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
  if (!instance || !port || optind != argc) { usage(argv[0]); return 2; }
 
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

  cpc_handle_t cpc;
  int status;
  int fd;
 status = cpc_init(&cpc, instance, false, NULL );
  if (status != CPC_STATUS_OK) {
    fprintf(stderr, "BRIDGE: CPC Init failed (status 0x%08x)\n", (unsigned)status);
    close(listen_fd); return 1;
  }
  log_msg("CPC Init success");

  cpc_endpoint_t endpoint;
  fd = cpc_open_endpoint(cpc, &endpoint,(uint8_t)endpoint_id, DEFAULT_TX_WINDOW);
  if (fd < CPC_STATUS_OK) {
    fprintf(stderr, "BRIDGE: CPC endpoint %lu open failed (status 0x%08x)\n", endpoint_id, (unsigned)status);
    close(listen_fd); return 1;
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

  cpc_close_endpoint(&endpoint);
  if (listen_fd >= 0) close(listen_fd);
  return 0;
}
