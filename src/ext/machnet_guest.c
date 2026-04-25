/**
 * @file  machnet_guest.c
 * @brief Guest-side Machnet library for Firecracker VMs.
 *
 * Replaces machnet_init()/machnet_attach() with VirtIO config space discovery
 * + /dev/mem mapping. The Firecracker VMM maps the Machnet SHM into the guest
 * physical address space (above 4 GB, in the past_mmio64 region) and exposes
 * its guest-physical address and size via the VirtIO config space of a device
 * with device ID 64.
 *
 * All data-path and control-plane functions (send/recv/connect/listen) are
 * identical to the host-side machnet.c because they only operate on the
 * channel pointer — the SHM layout is the same on both sides.
 *
 * Usage inside the VM:
 *   LD_PRELOAD=/usr/local/lib/libmachnet_guest.so ./msg_gen ...
 *
 * Kernel boot requirement:
 *   Add "iomem=relaxed" to boot_args in vm_config.json so that /dev/mem can
 *   map the high-address SHM region without CONFIG_STRICT_DEVMEM refusing it.
 */

#include "machnet.h"
#include "machnet_ctrl.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* Kept for ABI compatibility with code that references g_ctrl_socket. */
int g_ctrl_socket = -1;

/* VirtIO device ID assigned to the Machnet device in Firecracker (= 64). */
#define MACHNET_VIRTIO_DEVICE_ID 64

/* VirtIO MMIO register offsets */
#define VIRTIO_MMIO_MAGIC_VALUE  0x000
#define VIRTIO_MMIO_DEVICE_ID    0x008
#define VIRTIO_MMIO_CONFIG       0x100
#define VIRTIO_MMIO_MAGIC        0x74726976u  /* "virt" */

/* Mirrors ConfigSpace { start: u64, size: u64 } in device.rs. */
typedef struct {
    uint64_t start;
    uint64_t size;
} MachnetGuestConfigSpace;

static void *g_channel  = NULL;
static int   g_mem_fd   = -1;

#define MIN(a, b)           \
  ({                        \
    __typeof__(a) _a = (a); \
    __typeof__(b) _b = (b); \
    _a < _b ? _a : _b;      \
  })

/* -------------------------------------------------------------------------
 * Internal helpers (identical to machnet.c)
 * ---------------------------------------------------------------------- */

static uint32_t msg_id_counter;

static inline MachnetRingSlot_t *_machnet_buffers_alloc(
    MachnetChannelCtx_t *ctx, uint32_t cnt) {
  MachnetRingSlot_t *buffer_indices = __machnet_channel_buffer_index_table(ctx);

  if (cnt > NUM_CACHED_BUFS) {
    uint32_t ret =
        __machnet_channel_buf_alloc_bulk(ctx, cnt, buffer_indices, NULL);
    if (ret != cnt) return NULL;
    return buffer_indices;
  }

  uint32_t index = 0;
  while (index < cnt) {
    if (unlikely(ctx->app_buffer_cache.count == 0)) {
      ctx->app_buffer_cache.count += __machnet_channel_buf_alloc_bulk(
          ctx, NUM_CACHED_BUFS, ctx->app_buffer_cache.indices, NULL);
      if (unlikely(ctx->app_buffer_cache.count == 0)) goto fail;
    }
    buffer_indices[index++] =
        ctx->app_buffer_cache.indices[--ctx->app_buffer_cache.count];
  }
  return buffer_indices;

fail:
  for (uint32_t i = 0; i < index; i++)
    ctx->app_buffer_cache.indices[ctx->app_buffer_cache.count++] =
        buffer_indices[i];
  return NULL;
}

static inline void _machnet_buffers_release(MachnetChannelCtx_t *ctx,
                                            uint32_t cnt,
                                            MachnetRingSlot_t *buffer_indices) {
  uint32_t index = 0;
  while (index < cnt) {
    uint32_t retries = 5;
    while (unlikely(ctx->app_buffer_cache.count == NUM_CACHED_BUFS)) {
      uint32_t elements_to_free = ctx->app_buffer_cache.count / 2;
      MachnetRingSlot_t *indices_to_free =
          ctx->app_buffer_cache.indices + (NUM_CACHED_BUFS - elements_to_free);
      ctx->app_buffer_cache.count -= __machnet_channel_buf_free_bulk(
          ctx, elements_to_free, indices_to_free);
      if (unlikely(retries-- == 0 &&
                   ctx->app_buffer_cache.count == NUM_CACHED_BUFS)) {
        fprintf(stderr, "ERROR: Failed to free buffers to global pool.\n");
        abort();
      }
    }
    ctx->app_buffer_cache.indices[ctx->app_buffer_cache.count++] =
        buffer_indices[index++];
  }
}

/* -------------------------------------------------------------------------
 * VirtIO config space discovery
 * ---------------------------------------------------------------------- */

/* Scan /proc/iomem for virtio-mmio.N regions, mmap each one via /dev/mem,
 * check the DeviceID register, and read config space from offset 0x100. */
static int find_machnet_virtio_config(MachnetGuestConfigSpace *cs) {
  int mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
  if (mem_fd < 0) {
    perror("machnet_guest: open /dev/mem");
    return -1;
  }

  FILE *f = fopen("/proc/iomem", "r");
  if (!f) {
    perror("machnet_guest: open /proc/iomem");
    close(mem_fd);
    return -1;
  }

  char line[256];
  int found = 0;
  while (fgets(line, sizeof(line), f)) {
    /* Only top-level lines (no leading whitespace) with virtio-mmio.N */
    if (line[0] == ' ' || line[0] == '\t') continue;
    if (!strstr(line, "virtio-mmio.")) continue;

    unsigned long start;
    if (sscanf(line, "%lx-", &start) != 1) continue;

    volatile uint32_t *mmio = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE,
                                   MAP_SHARED, mem_fd, (off_t)start);
    if (mmio == MAP_FAILED) continue;

    uint32_t magic  = mmio[VIRTIO_MMIO_MAGIC_VALUE / 4];
    uint32_t dev_id = mmio[VIRTIO_MMIO_DEVICE_ID / 4];

    if (magic == VIRTIO_MMIO_MAGIC && dev_id == MACHNET_VIRTIO_DEVICE_ID) {
      volatile uint8_t *config = (volatile uint8_t *)mmio + VIRTIO_MMIO_CONFIG;
      memcpy(cs, (const void *)config, sizeof(*cs));
      munmap((void *)mmio, 0x1000);
      found = 1;
      break;
    }

    munmap((void *)mmio, 0x1000);
  }

  fclose(f);
  close(mem_fd);

  if (!found) {
    fprintf(stderr, "machnet_guest: Machnet VirtIO device (ID %d) not found in /proc/iomem\n",
            MACHNET_VIRTIO_DEVICE_ID);
    return -1;
  }
  return 0;
}

/* -------------------------------------------------------------------------
 * Guest-specific public API: init / attach / bind
 * ---------------------------------------------------------------------- */

int machnet_init() {
  /* Nothing to do in the guest — no controller socket needed. */
  return 0;
}

MachnetChannelCtx_t *machnet_bind(int shm_fd, size_t *channel_size) {
  /* Not used in the guest path; kept for ABI completeness. */
  (void)shm_fd;
  if (channel_size) *channel_size = 0;
  return NULL;
}

void *machnet_attach() { return machnet_attach_raw(NULL); }

void *machnet_attach_raw(int *shm_fd) {
  if (shm_fd) *shm_fd = -1;

  if (g_channel) return g_channel;

  MachnetGuestConfigSpace cs;
  if (find_machnet_virtio_config(&cs) != 0) return NULL;

  if (cs.start == 0 || cs.size == 0) {
    fprintf(stderr,
            "machnet_guest: VirtIO config has zero start/size "
            "(start=0x%lx size=0x%lx) — VM may not have finished activating "
            "the device yet\n",
            cs.start, cs.size);
    return NULL;
  }

  /* The SHM is already a normal KVM memory region at cs.start in guest
   * physical space. Map it via /dev/mem (requires iomem=relaxed or root
   * with CONFIG_STRICT_DEVMEM=n). */
  g_mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
  if (g_mem_fd < 0) {
    perror("machnet_guest: open /dev/mem");
    return NULL;
  }

  void *ptr = mmap(NULL, (size_t)cs.size, PROT_READ | PROT_WRITE,
                   MAP_SHARED, g_mem_fd, (off_t)cs.start);
  if (ptr == MAP_FAILED) {
    perror("machnet_guest: mmap /dev/mem");
    close(g_mem_fd);
    g_mem_fd = -1;
    return NULL;
  }

  g_channel = ptr;
  return g_channel;
}

/* -------------------------------------------------------------------------
 * Data-path and control-plane functions (identical to host machnet.c)
 * ---------------------------------------------------------------------- */

int machnet_connect(void *channel_ctx, const char *src_ip, const char *dst_ip,
                    uint16_t dst_port, MachnetFlow_t *flow) {
  assert(flow != NULL);
  MachnetChannelCtx_t *ctx = channel_ctx;

  if (inet_addr(src_ip) == INADDR_NONE || inet_addr(dst_ip) == INADDR_ANY) {
    fprintf(stderr,
            "machnet_connect: Invalid source (%s) or destination (%s) IP\n",
            src_ip, dst_ip);
    return -1;
  }

  MachnetCtrlQueueEntry_t req;
  memset(&req, 0, sizeof(req));
  req.id = ctx->ctrl_ctx.req_id++;
  req.opcode = MACHNET_CTRL_OP_CREATE_FLOW;
  req.flow_info.src_ip = ntohl(inet_addr(src_ip));
  req.flow_info.dst_ip = ntohl(inet_addr(dst_ip));
  req.flow_info.dst_port = dst_port;

  if (__machnet_channel_ctrl_sq_enqueue(ctx, 1, &req) != 1) {
    fprintf(stderr, "machnet_connect: failed to enqueue control request\n");
    return -1;
  }

  MachnetCtrlQueueEntry_t resp;
  memset(&resp, 0, sizeof(resp));
  int max_tries = 10;
  uint32_t ret = 0;
  do {
    ret = __machnet_channel_ctrl_cq_dequeue(ctx, 1, &resp);
    if (ret != 0) break;
    sleep(1);
  } while (max_tries-- > 0);

  if (ret == 0 || resp.id != req.id) {
    fprintf(stderr, "machnet_connect: invalid/missing control response\n");
    return -1;
  }
  if (resp.status != MACHNET_CTRL_STATUS_OK) {
    fprintf(stderr, "machnet_connect: control plane returned failure\n");
    return -1;
  }

  *flow = resp.flow_info;
  return 0;
}

int machnet_listen(void *channel_ctx, const char *local_ip,
                   uint16_t local_port) {
  assert(channel_ctx != NULL);
  MachnetChannelCtx_t *ctx = channel_ctx;

  if (inet_addr(local_ip) == INADDR_NONE) {
    fprintf(stderr, "machnet_listen: invalid IP address: %s\n", local_ip);
    return -EINVAL;
  }

  MachnetCtrlQueueEntry_t req;
  memset(&req, 0, sizeof(req));
  req.id = ctx->ctrl_ctx.req_id++;
  req.opcode = MACHNET_CTRL_OP_LISTEN;
  req.listener_info.ip = ntohl(inet_addr(local_ip));
  req.listener_info.port = local_port;

  if (__machnet_channel_ctrl_sq_enqueue(ctx, 1, &req) != 1) {
    fprintf(stderr, "machnet_listen: failed to enqueue control request\n");
    return -1;
  }

  MachnetCtrlQueueEntry_t resp;
  memset(&resp, 0, sizeof(resp));
  int max_tries = 10;
  uint32_t ret = 0;
  do {
    ret = __machnet_channel_ctrl_cq_dequeue(ctx, 1, &resp);
    if (ret != 0) break;
    sleep(1);
  } while (max_tries-- > 0);

  if (ret == 0 || resp.id != req.id) {
    fprintf(stderr, "machnet_listen: invalid/missing control response\n");
    return -1;
  }
  if (resp.status != MACHNET_CTRL_STATUS_OK) {
    fprintf(stderr, "machnet_listen: control plane returned failure\n");
    return -1;
  }
  return 0;
}

int machnet_send(const void *channel_ctx, MachnetFlow_t flow, const void *buf,
                 size_t len) {
  struct MachnetIovec iov = {.base = (void *)buf, .len = len};
  struct MachnetMsgHdr msghdr = {
      .flags = 0,
      .msg_size = len,
      .flow_info = flow,
      .msg_iov = &iov,
      .msg_iovlen = 1,
  };
  return machnet_sendmsg(channel_ctx, &msghdr);
}

int machnet_sendmsg(const void *channel_ctx, const MachnetMsgHdr_t *msghdr) {
  assert(channel_ctx != NULL);
  assert(msghdr != NULL);
  MachnetChannelCtx_t *ctx = (MachnetChannelCtx_t *)channel_ctx;

  if (unlikely(msghdr->msg_size > MACHNET_MSG_MAX_LEN || msghdr->msg_size == 0))
    return -1;

  const uint32_t kMsgBufPayloadMax = ctx->data_ctx.buf_mss;
  const uint32_t buffers_nr =
      (msghdr->msg_size + kMsgBufPayloadMax - 1) / kMsgBufPayloadMax;
  MachnetRingSlot_t *buf_index_table = _machnet_buffers_alloc(ctx, buffers_nr);
  if (buf_index_table == NULL) return -1;

  uint32_t buffer_cur_index = 0;
  uint32_t total_bytes_copied = 0;
  uint32_t new_buffer = 1;

  for (size_t iov_index = 0; iov_index < msghdr->msg_iovlen; iov_index++) {
    const MachnetIovec_t *segment_desc = &msghdr->msg_iov[iov_index];
    assert(segment_desc != NULL);

    uchar_t *seg_data = (uchar_t *)segment_desc->base;
    uint32_t seg_bytes = segment_desc->len;

    while (seg_bytes) {
      MachnetMsgBuf_t *buffer =
          __machnet_channel_buf(ctx, buf_index_table[buffer_cur_index]);
      if (unlikely(buffer->magic != MACHNET_MSGBUF_MAGIC)) abort();
      if (new_buffer) { __machnet_channel_buf_init(buffer); new_buffer = 0; }

      uint32_t nbytes_to_copy =
          MIN(seg_bytes, __machnet_channel_buf_tailroom(buffer));
      uchar_t *buf_data = __machnet_channel_buf_append(buffer, nbytes_to_copy);
      memcpy(buf_data, seg_data, nbytes_to_copy);
      buffer->flags |= MACHNET_MSGBUF_FLAGS_SG;

      seg_data += nbytes_to_copy;
      seg_bytes -= nbytes_to_copy;
      total_bytes_copied += nbytes_to_copy;

      if ((__machnet_channel_buf_tailroom(buffer) == 0) && seg_bytes) {
        buffer_cur_index++;
        new_buffer = 1;
        assert(buffer_cur_index < buffers_nr);
        buffer->next = buf_index_table[buffer_cur_index];
      }
    }
  }

  assert(total_bytes_copied == msghdr->msg_size);
  if (unlikely(total_bytes_copied != msghdr->msg_size)) abort();

  MachnetMsgBuf_t *last =
      __machnet_channel_buf(ctx, buf_index_table[buffers_nr - 1]);
  last->flags |= MACHNET_MSGBUF_FLAGS_FIN;
  last->flags &= ~(MACHNET_MSGBUF_FLAGS_SG);

  MachnetMsgBuf_t *first = __machnet_channel_buf(ctx, buf_index_table[0]);
  first->flags |= MACHNET_MSGBUF_FLAGS_SYN;
  first->flags |= (msghdr->flags & MACHNET_MSGBUF_NOTIFY_DELIVERY);
  first->flow = msghdr->flow_info;
  first->msg_len = msghdr->msg_size;
  first->last = buf_index_table[buffers_nr - 1];

  if (__machnet_channel_app_ring_enqueue(ctx, 1, buf_index_table) != 1)
    return -1;

  return 0;
}

int machnet_sendmmsg(const void *channel_ctx,
                     const MachnetMsgHdr_t *msghdr_iovec, int vlen) {
  int msg_sent = 0;
  for (int i = 0; i < vlen; i++) {
    if (machnet_sendmsg(channel_ctx, &msghdr_iovec[i]) != 0) return msg_sent;
    msg_sent++;
  }
  return msg_sent;
}

ssize_t machnet_recv(const void *channel_ctx, void *buf, size_t len,
                     MachnetFlow_t *flow) {
  MachnetIovec_t iov = {.base = buf, .len = len};
  MachnetMsgHdr_t msghdr = {.msg_iov = &iov, .msg_iovlen = 1};
  const int ret = machnet_recvmsg(channel_ctx, &msghdr);
  if (ret <= 0) return ret;
  *flow = msghdr.flow_info;
  return msghdr.msg_size;
}

int machnet_recvmsg(const void *channel_ctx, MachnetMsgHdr_t *msghdr) {
  assert(channel_ctx != NULL);
  assert(msghdr != NULL);
  MachnetChannelCtx_t *ctx = (MachnetChannelCtx_t *)channel_ctx;

  const uint32_t kBufferBatchSize = 16;
  MachnetRingSlot_t buffer_index;
  if (__machnet_channel_machnet_ring_dequeue(ctx, 1, &buffer_index) != 1)
    return 0;

  MachnetMsgBuf_t *buffer = __machnet_channel_buf(ctx, buffer_index);
  MachnetFlow_t flow_info = buffer->flow;
  uint32_t buf_data_ofs = 0;
  size_t iov_index = 0;
  uint32_t seg_data_ofs = 0;
  uint32_t total_bytes_copied = 0;
  MachnetRingSlot_t buffer_indices[kBufferBatchSize];
  uint32_t buffer_indices_index = 0;

  while (buffer != NULL &&
         __machnet_channel_buf_data_len(buffer) > buf_data_ofs) {
    if (unlikely(iov_index >= msghdr->msg_iovlen)) goto fail;

    uchar_t *buf_data = __machnet_channel_buf_data_ofs(buffer, buf_data_ofs);
    const size_t seg_len = msghdr->msg_iov[iov_index].len;
    if (unlikely(seg_len == 0)) { iov_index++; continue; }

    assert(msghdr->msg_iov[iov_index].base != NULL);
    uchar_t *seg_data =
        (uchar_t *)msghdr->msg_iov[iov_index].base + seg_data_ofs;

    uint32_t remaining_bytes_in_buf =
        __machnet_channel_buf_data_len(buffer) - buf_data_ofs;
    uint32_t remaining_space_in_seg = seg_len - seg_data_ofs;
    uint32_t nbytes_to_copy =
        MIN(remaining_space_in_seg, remaining_bytes_in_buf);
    memcpy(seg_data, buf_data, nbytes_to_copy);
    buf_data_ofs += nbytes_to_copy;
    seg_data_ofs += nbytes_to_copy;
    total_bytes_copied += nbytes_to_copy;

    if (buf_data_ofs == __machnet_channel_buf_data_len(buffer)) {
      buffer_indices[buffer_indices_index++] = buffer_index;
      if (buffer->flags & MACHNET_MSGBUF_FLAGS_SG) {
        buffer_index = buffer->next;
        buffer = __machnet_channel_buf(ctx, buffer_index);
        buf_data_ofs = 0;
      }
      if (buffer_indices_index == kBufferBatchSize) {
        _machnet_buffers_release(ctx, buffer_indices_index, buffer_indices);
        buffer_indices_index = 0;
      }
    }
    if (seg_data_ofs == seg_len) { iov_index++; seg_data_ofs = 0; }
  }

  msghdr->msg_size = total_bytes_copied;
  msghdr->flow_info = flow_info;
  _machnet_buffers_release(ctx, buffer_indices_index, buffer_indices);
  return 1;

fail:
  while (buffer != NULL) {
    buffer_indices[buffer_indices_index++] = buffer_index;
    if (buffer->flags & MACHNET_MSGBUF_FLAGS_SG) {
      buffer_index = buffer->next;
      buffer = __machnet_channel_buf(ctx, buffer_index);
    } else {
      buffer = NULL;
    }
    if (buffer == NULL || buffer_indices_index == kBufferBatchSize) {
      _machnet_buffers_release(ctx, buffer_indices_index, buffer_indices);
      buffer_indices_index = 0;
    }
  }
  return -1;
}

void machnet_detach(const MachnetChannelCtx_t *ctx) { (void)ctx; }
