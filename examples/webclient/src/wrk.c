// Copyright (C) 2012 - Will Glozer.  All rights reserved.

#include "wrk.h"

#include "hdr_histogram.h"
#include "main.h"
#include "script.h"
#include "stats.h"

// Max recordable latency of 1 day
#define MAX_LATENCY 24L * 60 * 60 * 1000000
// Local IP address to bind Machnet
#define DEFAULT_LOCAL_IP "10.10.1.2"

// Static variables for Machnet fd alias
static int machnet_fd_alias = 1;
static pthread_mutex_t machnet_fd_mutex;

static struct config {
  uint64_t threads;
  uint64_t connections;
  uint64_t duration;
  uint64_t timeout;
  uint64_t pipeline;
  uint64_t rate;
  uint64_t delay_ms;
  bool latency;
  bool u_latency;
  bool dynamic;
  bool record_all_responses;
  char *local_ip;  // Local IP address to bind Machnet
  char *host;      // Remote IP address
  uint16_t port;   // Port number
  char *script;
  SSL_CTX *ctx;
} cfg;

static struct {
  stats *requests;
  pthread_mutex_t mutex;
} statistics;

static struct sock sock = {.connect = sock_connect,
                           .close = sock_close,
                           .read = sock_read,
                           .write = sock_write,
                           .readable = sock_readable};

static struct http_parser_settings parser_settings = {.on_message_complete =
                                                          response_complete};

static volatile sig_atomic_t stop = 0;

static void handler(int sig) { stop = 1; }

static void usage() {
  printf(
      "Usage: wrk <options> <url>                            \n"
      "  Options:                                            \n"
      "    -c, --connections <N>  Connections to keep open   \n"
      "    -d, --duration    <T>  Duration of test           \n"
      "    -t, --threads     <N>  Number of threads to use   \n"
      "    -l, --local-ip    <IP> Local IP address to bind   \n"
      "                                                      \n"
      "    -s, --script      <S>  Load Lua script file       \n"
      "    -H, --header      <H>  Add header to request      \n"
      "    -L  --latency          Print latency statistics   \n"
      "    -U  --u_latency        Print uncorrected latency statistics\n"
      "        --timeout     <T>  Socket/request timeout     \n"
      "    -B, --batch_latency    Measure latency of whole   \n"
      "                           batches of pipelined ops   \n"
      "                           (as opposed to each op)    \n"
      "    -v, --version          Print version details      \n"
      "    -R, --rate        <T>  work rate (throughput)     \n"
      "                           in requests/sec (total)    \n"
      "                           [Required Parameter]       \n"
      "                                                      \n"
      "                                                      \n"
      "  Numeric arguments may include a SI unit (1k, 1M, 1G)\n"
      "  Time arguments may include a time unit (2s, 2m, 2h)\n");
}

int main(int argc, char **argv) {
  char *url, **headers = zmalloc(argc * sizeof(char *));
  struct http_parser_url parts = {};
  lua_State *L;

  if (parse_args(&cfg, &url, &parts, headers, argc, argv)) {
    usage();
    exit(1);
  }

  char *schema = copy_url_part(url, &parts, UF_SCHEMA);
  char *host = copy_url_part(url, &parts, UF_HOST);
  char *port = copy_url_part(url, &parts, UF_PORT);
  char *service = port ? port : schema;

  if (!strncmp("https", schema, 5)) {
    fprintf(stderr, "[ERROR] HTTPS is not supported\n");
    exit(1);
  }

  cfg.host = host;
  cfg.port = atoi(port);

  // Initialize Machnet
  if (machnet_init() != 0) {
    fprintf(stderr, "[ERROR] Failed to initialize Machnet\n");
    exit(1);
  }

  signal(SIGPIPE, SIG_IGN);
  signal(SIGINT, SIG_IGN);

  pthread_mutex_init(&statistics.mutex, NULL);
  statistics.requests = stats_alloc(10);
  thread *threads = zcalloc(cfg.threads * sizeof(thread));

  hdr_init(1, MAX_LATENCY, 3, &(statistics.requests->histogram));

  L = script_create(cfg.script, url, headers);
  if (!script_resolve(L, host, service)) {
    char *msg = strerror(errno);
    fprintf(stderr, "unable to connect to %s:%s %s\n", host, service, msg);
    exit(1);
  }

  uint64_t connections = cfg.connections / cfg.threads;
  double throughput = (double)cfg.rate / cfg.threads;
  uint64_t stop_at = time_us() + (cfg.duration * 1000000);

  for (uint64_t i = 0; i < cfg.threads; i++) {
    thread *t = &threads[i];
    t->loop = aeCreateEventLoop(10 + cfg.connections * 3);
    t->connections = connections;
    t->throughput = throughput;
    t->stop_at = stop_at;

    t->L = script_create(cfg.script, url, headers);
    script_init(L, t, argc - optind, &argv[optind]);

    if (i == 0) {
      cfg.pipeline = script_verify_request(t->L);
      cfg.dynamic = !script_is_static(t->L);
      if (script_want_response(t->L)) {
        parser_settings.on_header_field = header_field;
        parser_settings.on_header_value = header_value;
        parser_settings.on_body = response_body;
      }
    }

    if (!t->loop || pthread_create(&t->thread, NULL, &thread_main, t)) {
      char *msg = strerror(errno);
      fprintf(stderr, "unable to create thread %" PRIu64 ": %s\n", i, msg);
      exit(2);
    }
  }

  struct sigaction sa = {
      .sa_handler = handler,
      .sa_flags = 0,
  };
  sigfillset(&sa.sa_mask);
  sigaction(SIGINT, &sa, NULL);

  char *time = format_time_s(cfg.duration);
  printf("Running %s test @ %s\n", time, url);
  printf("  %" PRIu64 " threads and %" PRIu64 " connections\n", cfg.threads,
         cfg.connections);

  uint64_t start = time_us();
  uint64_t complete = 0;
  uint64_t bytes = 0;
  errors errors = {0};

  struct hdr_histogram *latency_histogram;
  hdr_init(1, MAX_LATENCY, 3, &latency_histogram);
  struct hdr_histogram *u_latency_histogram;
  hdr_init(1, MAX_LATENCY, 3, &u_latency_histogram);

  for (uint64_t i = 0; i < cfg.threads; i++) {
    thread *t = &threads[i];
    pthread_join(t->thread, NULL);
  }

  uint64_t runtime_us = time_us() - start;

  for (uint64_t i = 0; i < cfg.threads; i++) {
    thread *t = &threads[i];

    // Ensure the thread has completed before accessing its data
    pthread_join(t->thread, NULL);
    complete += t->complete;
    bytes += t->bytes;

    errors.connect += t->errors.connect;
    errors.read += t->errors.read;
    errors.write += t->errors.write;
    errors.timeout += t->errors.timeout;
    errors.status += t->errors.status;

    hdr_add(latency_histogram, t->latency_histogram);
    hdr_add(u_latency_histogram, t->u_latency_histogram);
  }

  long double runtime_s = runtime_us / 1000000.0;
  long double req_per_s = complete / (long double)cfg.duration;
  long double bytes_per_s = bytes / (long double)cfg.duration;

  stats *latency_stats = stats_alloc(10);
  latency_stats->min = hdr_min(latency_histogram);
  latency_stats->max = hdr_max(latency_histogram);
  latency_stats->histogram = latency_histogram;

  print_stats_header();
  print_stats("Latency", latency_stats, format_time_us_only);
  print_stats("Req/Sec", statistics.requests, format_metric);
  //    if (cfg.latency) print_stats_latency(latency_stats);

  if (cfg.latency) {
    print_hdr_latency(latency_histogram, "Recorded Latency");
    printf("----------------------------------------------------------\n");
  }

  if (cfg.u_latency) {
    printf("\n");
    print_hdr_latency(u_latency_histogram,
                      "Uncorrected Latency (measured without taking delayed "
                      "starts into account)");
    printf("----------------------------------------------------------\n");
  }

  char *runtime_msg = format_time_us(runtime_us);

  printf("  %" PRIu64 " requests in %s, %sB read\n", complete, runtime_msg,
         format_binary(bytes));
  if (errors.connect || errors.read || errors.write || errors.timeout) {
    printf("  Socket errors: connect %d, read %d, write %d, timeout %d\n",
           errors.connect, errors.read, errors.write, errors.timeout);
  }

  if (errors.status) {
    printf("  Non-2xx or 3xx responses: %d\n", errors.status);
  }

  printf("Requests/sec: %9.2Lf\n", req_per_s);
  printf("Transfer/sec: %10sB\n", format_binary(bytes_per_s));

  if (script_has_done(L)) {
    script_summary(L, runtime_us, complete, bytes);
    script_errors(L, &errors);
    script_done(L, latency_stats, statistics.requests);
  }

  return 0;
}

void *thread_main(void *arg) {
  thread *thread = arg;
  aeEventLoop *loop = thread->loop;

  thread->cs = zcalloc(thread->connections * sizeof(connection));
  tinymt64_init(&thread->rand, time_us());
  hdr_init(1, MAX_LATENCY, 3, &thread->latency_histogram);
  hdr_init(1, MAX_LATENCY, 3, &thread->u_latency_histogram);

  char *request = NULL;
  size_t length = 0;

  if (!cfg.dynamic) {
    script_request(thread->L, &request, &length);
  }

  double throughput = (thread->throughput / 1000000.0) / thread->connections;

  connection *c = thread->cs;

  // Schedule initial connections
  for (uint64_t i = 0; i < thread->connections; i++, c++) {
    c->thread = thread;
    c->ssl = cfg.ctx ? SSL_new(cfg.ctx) : NULL;
    c->request = request;
    c->length = length;
    c->throughput = throughput;
    c->catch_up_throughput = throughput * 2;
    c->complete = 0;
    c->caught_up = true;
    // Stagger connects 5 msec apart within thread (convert to microseconds)
    aeCreateTimeEvent(loop, i * 5000, delayed_initial_connect, c, NULL);
  }

  uint64_t calibrate_delay =
      (CALIBRATE_DELAY_MS + (thread->connections * 5)) * 1000;
  uint64_t timeout_delay =
      (TIMEOUT_INTERVAL_MS + (thread->connections * 5)) * 1000;
  aeCreateTimeEvent(loop, calibrate_delay, calibrate, thread, NULL);
  aeCreateTimeEvent(loop, timeout_delay, check_timeouts, thread, NULL);

  thread->start = time_us();
  aeMain(thread->loop);
  aeDeleteEventLoop(loop);
  zfree(thread->cs);

  return NULL;
}

static int connect_socket(thread *thread, connection *c) {
#ifdef MACHNET_DEBUG
  fprintf(stderr,
          "[DEBUG] connect_socket: fd=%d, thread=%p, "
          "c->thread_start=%lu\n",
          c->fd, c->thread, c->thread_start);
#endif
  struct aeEventLoop *loop = thread->loop;
  c->latest_connect = time_us();

  int flags = AE_WRITABLE;
  if (aeCreateFileEvent(loop, c->fd, flags, socket_connected, c) == AE_OK) {
    c->parser.data = c;
    return c->fd;
  } else {
    thread->errors.connect++;
    return -1;
  }
}

static int reconnect_socket(thread *thread, connection *c) {
#ifdef MACHNET_DEBUG
  printf("[DEBUG] Reconnecting socket...\n");
#endif
  aeDeleteFileEvent(thread->loop, c->fd, AE_WRITABLE | AE_READABLE);
  sock.close(c);
  return connect_socket(thread, c);
}

static int delayed_initial_connect(aeEventLoop *loop, long long id,
                                   void *data) {
  connection *c = data;

  pthread_mutex_lock(&machnet_fd_mutex);
  c->fd = machnet_fd_alias;
  machnet_fd_alias++;
  pthread_mutex_unlock(&machnet_fd_mutex);

  c->thread_start = time_us();
#ifdef MACHNET_DEBUG
  fprintf(stderr,
          "[DEBUG] delayed_initial_connect: fd=%d, thread=%p, "
          "c->thread_start=%lu\n",
          c->fd, c->thread, c->thread_start);
#endif
  connect_socket(c->thread, c);
  return AE_NOMORE;
}

static int calibrate(aeEventLoop *loop, long long id, void *data) {
  thread *thread = data;

  long double mean = hdr_mean(thread->latency_histogram);
  long double latency =
      hdr_value_at_percentile(thread->latency_histogram, 90.0) / 1000.0L;
  long double interval = MAX(latency * 2, 10);

  if (mean == 0) return CALIBRATE_DELAY_MS * 1000;  // Convert to microseconds

  thread->mean = (uint64_t)mean;
  hdr_reset(thread->latency_histogram);
  hdr_reset(thread->u_latency_histogram);

  thread->start = time_us();
  thread->interval = interval;
  thread->requests = 0;

  printf(
      "  Thread calibration: mean lat.: %.3fms, rate sampling interval: %dms\n",
      (thread->mean) / 1000.0, thread->interval);

  aeCreateTimeEvent(loop, thread->interval * 1000, sample_rate, thread,
                    NULL);  // Convert to microseconds

  return AE_NOMORE;
}

static int check_timeouts(aeEventLoop *loop, long long id, void *data) {
  thread *thread = data;
  connection *c = thread->cs;
  uint64_t now = time_us();

  uint64_t maxAge = now - (cfg.timeout * 1000);

  for (uint64_t i = 0; i < thread->connections; i++, c++) {
    if (maxAge > c->start) {
      thread->errors.timeout++;
    }
  }

  if (stop || now >= thread->stop_at) {
    aeStop(loop);
  }

  return TIMEOUT_INTERVAL_MS * 1000;  // Convert to microseconds
}

static int sample_rate(aeEventLoop *loop, long long id, void *data) {
  thread *thread = data;

  uint64_t elapsed_ms = (time_us() - thread->start) / 1000;
  uint64_t requests = (thread->requests / (double)elapsed_ms) * 1000;

  pthread_mutex_lock(&statistics.mutex);
  stats_record(statistics.requests, requests);
  pthread_mutex_unlock(&statistics.mutex);

  thread->requests = 0;
  thread->start = time_us();

  return thread->interval * 1000;  // Convert to microseconds
}

static int header_field(http_parser *parser, const char *at, size_t len) {
  connection *c = parser->data;
  if (c->state == VALUE) {
    *c->headers.cursor++ = '\0';
    c->state = FIELD;
  }
  buffer_append(&c->headers, at, len);
  return 0;
}

static int header_value(http_parser *parser, const char *at, size_t len) {
  connection *c = parser->data;
  if (c->state == FIELD) {
    *c->headers.cursor++ = '\0';
    c->state = VALUE;
  }
  buffer_append(&c->headers, at, len);
  return 0;
}

static int response_body(http_parser *parser, const char *at, size_t len) {
  connection *c = parser->data;
  buffer_append(&c->body, at, len);
  return 0;
}

static uint64_t usec_to_next_send(connection *c) {
  uint64_t now = time_us();
  uint64_t next_start_time = c->thread_start + (c->complete / c->throughput);
  bool send_now = true;

  if (next_start_time > now) {
    // We are on pace. Indicate caught_up and don't send now.
    c->caught_up = true;
    send_now = false;
  } else {
    // We are behind
    if (c->caught_up) {
      // This is the first fall-behind since we were last caught up
      c->caught_up = false;
      c->catch_up_start_time = now;
      c->complete_at_catch_up_start = c->complete;
    }

    // Figure out if it's time to send, per catch up throughput:
    uint64_t complete_since_catch_up_start =
        c->complete - c->complete_at_catch_up_start;

    next_start_time = c->catch_up_start_time +
                      (complete_since_catch_up_start / c->catch_up_throughput);

    if (next_start_time > now) {
      // Not yet time to send, even at catch-up throughout:
      send_now = false;
    }
  }

  if (send_now) {
    c->latest_should_send_time = now;
    c->latest_expected_start = next_start_time;
  }

#ifdef MACHNET_DEBUG
  fprintf(stderr,
          "[DEBUG] usec_to_next_send: fd=%d, thread=%p, send_now=%d, "
          "next_start_time=%lu, now=%lu next_start_time-now=%ld, "
          "c->thread_start=%lu, c->complete=%lu, c->throughput=%f\n",
          c->fd, c->thread, send_now, next_start_time, now,
          next_start_time - now, c->thread_start, c->complete, c->throughput);
#endif

  return send_now ? 0 : (next_start_time - now);
}

static int delay_request(aeEventLoop *loop, long long id, void *data) {
  connection *c = data;
  uint64_t time_usec_to_wait = usec_to_next_send(c);
#ifdef MACHNET_DEBUG
  fprintf(stderr,
          "[DEBUG] delay_request: fd=%d, thread=%p, time_usec_to_wait=%lu\n",
          c->fd, c->thread, time_usec_to_wait);
#endif
  if (time_usec_to_wait) {
    return time_usec_to_wait;
  }
  aeCreateFileEvent(c->thread->loop, c->fd, AE_WRITABLE, socket_writeable, c);
  return AE_NOMORE;
}

static int response_complete(http_parser *parser) {
  connection *c = parser->data;
  thread *thread = c->thread;
  uint64_t now = time_us();
  int status = parser->status_code;

  thread->complete++;
  thread->requests++;

  if (status > 399) {
    thread->errors.status++;
  }

  if (c->headers.buffer) {
    *c->headers.cursor++ = '\0';
    script_response(thread->L, status, &c->headers, &c->body);
    c->state = FIELD;
  }

  if (now >= thread->stop_at) {
    aeStop(thread->loop);
    goto done;
  }

  // Count all responses (including pipelined ones:)
  c->complete++;

  // Note that expected start time is computed based on the completed
  // response count seen at the beginning of the last request batch sent.
  // A single request batch send may contain multiple requests, and
  // result in multiple responses. If we incorrectly calculated expect
  // start time based on the completion count of these individual pipelined
  // requests we can easily end up "gifting" them time and seeing
  // negative latencies.
#ifdef MACHNET_DEBUG
  fprintf(stderr,
          "[DEBUG] response_complete: fd=%d, thread=%p, now=%lu, "
          "c->thread_start=%lu, c->complete_at_last_batch_start=%lu, "
          "c->throughput=%f\n",
          c->fd, c->thread, now, c->thread_start,
          c->complete_at_last_batch_start, c->throughput);
#endif

  uint64_t expected_latency_start =
      c->thread_start + (c->complete_at_last_batch_start / c->throughput);
  int64_t expected_latency_timing = now - expected_latency_start;

  if (expected_latency_timing < 0) {
    printf("\n\n ---------- \n\n");
    printf("We are about to crash and die (recoridng a negative #)");
    printf("This wil never ever ever happen...");
    printf(
        "But when it does. The following information will help in debugging");
    printf("response_complete:\n");
    // printf("  expected_latency_timing = %lld\n", expected_latency_timing);
    printf("  expected_latency_timing = %ld\n", expected_latency_timing);
    printf("  now = %ld\n", now);
    printf("  expected_latency_start = %ld\n", expected_latency_start);
    printf("  c->thread_start = %ld\n", c->thread_start);
    printf("  c->complete = %ld\n", c->complete);
    printf("  throughput = %g\n", c->throughput);
    printf("  latest_should_send_time = %ld\n", c->latest_should_send_time);
    printf("  latest_expected_start = %ld\n", c->latest_expected_start);
    printf("  latest_connect = %ld\n", c->latest_connect);
    printf("  latest_write = %ld\n", c->latest_write);

    expected_latency_start = c->thread_start + ((c->complete) / c->throughput);
    printf("  next expected_latency_start = %lu\n", expected_latency_start);
  }

  c->latest_should_send_time = 0;
  c->latest_expected_start = 0;

  if (--c->pending == 0) {
    c->has_pending = false;
    aeCreateFileEvent(thread->loop, c->fd, AE_WRITABLE, socket_writeable, c);
  }

  // Record if needed, either last in batch or all, depending in cfg:
  if (cfg.record_all_responses || !c->has_pending) {
    hdr_record_value(thread->latency_histogram, expected_latency_timing);

    uint64_t actual_latency_timing = now - c->actual_latency_start;
    hdr_record_value(thread->u_latency_histogram, actual_latency_timing);
#ifdef MACHNET_DEBUG
    fprintf(stderr,
            "[DEBUG] response_complete: index=%lu, now=%lu, "
            "expected_latency_start=%lu, "
            "actual_latency_timing=%lu vs expected_latency_timing=%lu\n",
            c->index, now, expected_latency_start, actual_latency_timing,
            expected_latency_timing);
#endif
  }

  if (!http_should_keep_alive(parser)) {
    reconnect_socket(thread, c);
    goto done;
  }

  http_parser_init(parser, HTTP_RESPONSE);

done:
  return 0;
}

static void socket_connected(aeEventLoop *loop, int fd, void *data, int mask) {
  connection *c = data;
#ifdef MACHNET_DEBUG
  fprintf(stderr,
          "[DEBUG] socket_connected event firead at thread %p for fd %d and "
          "thread start time is %lu\n",
          c->thread, fd, c->thread_start);
#endif
  switch (sock.connect(c, cfg.local_ip, cfg.host, cfg.port)) {
    case OK:
      break;
    case ERROR:
      goto error;
    case RETRY:
      return;
  }

  http_parser_init(&c->parser, HTTP_RESPONSE);
  c->written = 0;
#ifdef MACHNET_DEBUG
  c->index = 0;
#endif

  c->thread_start = time_us();
  c->thread->stop_at = time_us() + cfg.duration * 1000000;
  aeCreateFileEvent(c->thread->loop, fd, AE_WRITABLE, socket_writeable, c);

  return;

error:
  c->thread->errors.connect++;
  reconnect_socket(c->thread, c);
}

static void socket_writeable(aeEventLoop *loop, int fd, void *data, int mask) {
  connection *c = data;
  thread *thread = c->thread;

  if (!c->written) {
    uint64_t time_usec_to_wait = usec_to_next_send(c);
    if (time_usec_to_wait) {
      // Not yet time to send. Delay:
      aeDeleteFileEvent(loop, fd, AE_WRITABLE);
      aeCreateTimeEvent(thread->loop, time_usec_to_wait, delay_request, c,
                        NULL);
#ifdef MACHNET_DEBUG
      fprintf(stderr,
              "[DEBUG] socket_writeable will wait and not send index=%lu: "
              " at time %lu, fd=%d, thread=%p, time_usec_to_wait=%lu\n",
              c->index, time_us(), fd, thread, time_usec_to_wait);
#endif
      return;
    }
    c->latest_write = time_us();
  }

  if (!c->written && cfg.dynamic) {
    script_request(thread->L, &c->request, &c->length);
  }

  char *buf = c->request + c->written;
  size_t len = c->length - c->written;
  size_t n;

  if (!c->written) {
    c->start = time_us();
    if (!c->has_pending) {
      c->actual_latency_start = c->start;
      c->complete_at_last_batch_start = c->complete;
      c->has_pending = true;
    }
    c->pending = cfg.pipeline;
  }

  switch (sock.write(c, buf, len, &n)) {
    case OK:
      break;
    case ERROR:
      goto error;
    case RETRY:
      return;
  }

  c->written += n;

#ifdef MACHNET_DEBUG
  fprintf(stderr,
          "[DEBUG] socket_writeable sent index=%lu at time=%lu: fd=%d, "
          "thread=%p, written=%lu, length=%lu, latest_should_send_time=%lu, "
          "latest_expected_start=%lu\n",
          c->index, time_us(), c->fd, c->thread, c->written, c->length,
          c->latest_should_send_time, c->latest_expected_start);
#endif
  if (c->written == c->length) {
    c->written = 0;
  }

  aeCreateFileEvent(c->thread->loop, c->fd, AE_READABLE, socket_readable, c);
  aeDeleteFileEvent(c->thread->loop, c->fd, AE_WRITABLE);
  return;

error:
  thread->errors.write++;
  reconnect_socket(thread, c);
}

static void socket_readable(aeEventLoop *loop, int fd, void *data, int mask) {
  connection *c = data;
  size_t n;

  status read_status = sock.read(c, &n);
  if (read_status == ERROR) {
    goto error;
  }

  while (read_status == RETRY) {
    read_status = sock.read(c, &n);
    if (read_status == ERROR) {
      goto error;
    }
  }

#ifdef MACHNET_DEBUG
  fprintf(stderr,
          "[DEBUG] socket_readable read index=%lu at time=%lu: fd=%d, "
          "thread=%p, n=%lu\n",
          c->index, time_us(), fd, c->thread, n);
#endif
  if (http_parser_execute(&c->parser, &parser_settings, c->buf, n) != n)
    goto error;
  c->thread->bytes += n;

  // Re-register write event after reading to allow sending next request
  aeCreateFileEvent(c->thread->loop, fd, AE_WRITABLE, socket_writeable, c);
  aeDeleteFileEvent(c->thread->loop, fd, AE_READABLE);
#ifdef MACHNET_DEBUG
  c->index++;
#endif
  return;

error:
  c->thread->errors.read++;
  reconnect_socket(c->thread, c);
}

static uint64_t time_us() {
  struct timeval t;
  gettimeofday(&t, NULL);
  return (t.tv_sec * 1000000) + t.tv_usec;
}

static char *copy_url_part(char *url, struct http_parser_url *parts,
                           enum http_parser_url_fields field) {
  char *part = NULL;

  if (parts->field_set & (1 << field)) {
    uint16_t off = parts->field_data[field].off;
    uint16_t len = parts->field_data[field].len;
    part = zcalloc(len + 1 * sizeof(char));
    memcpy(part, &url[off], len);
  }

  return part;
}

static struct option longopts[] = {
    {"connections", required_argument, NULL, 'c'},
    {"duration", required_argument, NULL, 'd'},
    {"threads", required_argument, NULL, 't'},
    {"local-ip", required_argument, NULL, 'l'},
    {"script", required_argument, NULL, 's'},
    {"header", required_argument, NULL, 'H'},
    {"latency", no_argument, NULL, 'L'},
    {"u_latency", no_argument, NULL, 'U'},
    {"batch_latency", no_argument, NULL, 'B'},
    {"timeout", required_argument, NULL, 'T'},
    {"help", no_argument, NULL, 'h'},
    {"version", no_argument, NULL, 'v'},
    {"rate", required_argument, NULL, 'R'},
    {NULL, 0, NULL, 0}};

static int parse_args(struct config *cfg, char **url,
                      struct http_parser_url *parts, char **headers, int argc,
                      char **argv) {
  char c, **header = headers;

  memset(cfg, 0, sizeof(struct config));
  cfg->threads = 2;
  cfg->connections = 10;
  cfg->duration = 10;
  cfg->timeout = SOCKET_TIMEOUT_MS;
  cfg->rate = 0;
  cfg->record_all_responses = true;
  cfg->local_ip = DEFAULT_LOCAL_IP;

  while ((c = getopt_long(argc, argv, "t:c:d:l:s:H:T:R:LUBrv?", longopts,
                          NULL)) != -1) {
    switch (c) {
      case 't':
        if (scan_metric(optarg, &cfg->threads)) return -1;
        break;
      case 'c':
        if (scan_metric(optarg, &cfg->connections)) return -1;
        break;
      case 'd':
        if (scan_time(optarg, &cfg->duration)) return -1;
        break;
      case 'l':
        cfg->local_ip = optarg;
        break;
      case 's':
        cfg->script = optarg;
        break;
      case 'H':
        *header++ = optarg;
        break;
      case 'L':
        cfg->latency = true;
        break;
      case 'B':
        cfg->record_all_responses = false;
        break;
      case 'U':
        cfg->latency = true;
        cfg->u_latency = true;
        break;
      case 'T':
        if (scan_time(optarg, &cfg->timeout)) return -1;
        cfg->timeout *= 1000;
        break;
      case 'R':
        if (scan_metric(optarg, &cfg->rate)) return -1;
        break;
      case 'v':
        printf("wrk %s [%s] ", VERSION, aeGetApiName());
        printf("Copyright (C) 2012 Will Glozer\n");
        break;
      case 'h':
      case '?':
      case ':':
        return -1;
      default:
        return -1;
    }
  }

  if (optind == argc || !cfg->threads || !cfg->duration) return -1;

  if (!script_parse_url(argv[optind], parts)) {
    fprintf(stderr, "invalid URL: %s\n", argv[optind]);
    return -1;
  }

  if (!cfg->connections || cfg->connections < cfg->threads) {
    fprintf(stderr, "number of connections must be >= threads\n");
    return -1;
  }

  if (cfg->rate == 0) {
    fprintf(stderr,
            "Throughput MUST be specified with the --rate or -R option\n");
    return -1;
  }

  *url = argv[optind];
  *header = NULL;

  return 0;
}

static void print_stats_header() {
  printf("  Thread Stats%6s%11s%8s%12s\n", "Avg", "Stdev", "Max", "+/- Stdev");
}

static void print_units(long double n, char *(*fmt)(long double), int width) {
  char *msg = fmt(n);
  int len = strlen(msg), pad = 2;

  if (isalpha(msg[len - 1])) pad--;
  if (isalpha(msg[len - 2])) pad--;
  width -= pad;

  printf("%*.*s%.*s", width, width, msg, pad, "  ");

  free(msg);
}

static void print_stats(char *name, stats *stats, char *(*fmt)(long double)) {
  uint64_t max = stats->max;
  long double mean = stats_summarize(stats);
  long double stdev = stats_stdev(stats, mean);

  printf("    %-10s", name);
  print_units(mean, fmt, 8);
  print_units(stdev, fmt, 10);
  print_units(max, fmt, 9);
  printf("%8.2Lf%%\n", stats_within_stdev(stats, mean, stdev, 1));
}

static void print_hdr_latency(struct hdr_histogram *histogram,
                              const char *description) {
  long double percentiles[] = {50.0, 75.0,  90.0,   99.0,
                               99.9, 99.99, 99.999, 100.0};
  printf("  Latency Distribution (HdrHistogram - %s)\n", description);
  for (size_t i = 0; i < sizeof(percentiles) / sizeof(long double); i++) {
    long double p = percentiles[i];
    int64_t n = hdr_value_at_percentile(histogram, p);
    printf("%7.3Lf%%", p);
    print_units(n, format_time_us_only, 10);
    printf("\n");
  }
  printf("\n%s\n", "  Detailed Percentile spectrum:");
  hdr_percentiles_print(histogram, stdout, 5, 1.0, CLASSIC);
}

static void print_stats_latency(stats *stats) {
  long double percentiles[] = {50.0, 75.0,  90.0,   99.0,
                               99.9, 99.99, 99.999, 100.0};
  printf("  Latency Distribution\n");
  for (size_t i = 0; i < sizeof(percentiles) / sizeof(long double); i++) {
    long double p = percentiles[i];
    uint64_t n = stats_percentile(stats, p);
    printf("%7.3Lf%%", p);
    print_units(n, format_time_us_only, 10);
    printf("\n");
  }
}