#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "queue.h"
#include "logging.h"
#include "network.h"
#include "stream_mgr.h"
#include "viddec.h"

static int pin_to_core(int core);

void* stream_mgr(void* ptr) {
  int ret = 0;
  char logstr[128];

  thread_ctx* ctx = (thread_ctx*)ptr;
  uint8_t* enc_frame_buf = malloc(ENCODED_FRAME_BUF_SIZE);
  if (!enc_frame_buf) {
    log(ERROR, "Failed to allocate encoded frame buffer in a thread");
    goto cleanup;
  }

  ret = pin_to_core(ctx->core);
  if (ret) {
    goto cleanup;
  }

  queue timestamp_queue;
  ret = init_queue(
    &timestamp_queue,
    sizeof(uint64_t),
    32 // initial capacity
  );
  if (ret) {
    goto cleanup;
  }

  decoder viddec;
  ret = init_decoder(
    &viddec,
    DECODED_FRAME_WIDTH,
    DECODED_FRAME_HEIGHT
  );
  if (ret) {
    goto cleanup;
  }

  int sockfd = setup_stream(ctx->conf);
  if (sockfd < 0) {
    ret = -EIO;
    goto cleanup;
  }

  int clientfd = accept_conn(sockfd);
  if (clientfd < 0) {
    ret = -EIO;
    goto cleanup;
  }

  bool incoming_stream = true;
  bool new_frame = true;

  while (true) {
    if (incoming_stream) {
      uint64_t timestamp = 0;
      ssize_t pkt_size = recv_from_stream(
        clientfd,
        (char*)&timestamp,
        sizeof(timestamp)
      );

      if (pkt_size != sizeof(timestamp)) {
        snprintf(
          logstr,
          sizeof(logstr),
          "Received unexpected timestamp size with %zd bytes from cam %s",
          pkt_size,
          ctx->conf->name
        );
        log(ERROR, logstr);
        break;
      }

      if (memcmp(&timestamp, "EOSTREAM", 8) == 0) {
        log(INFO, "Received end of stream signal");
        incoming_stream = false;
        ret = flush_decoder(&viddec);
        continue;
      }

      ret = enqueue(&timestamp_queue, (void*)&timestamp);
      if (ret) {
        break;
      }

      uint32_t frame_size = 0;
      pkt_size = recv_from_stream(
        clientfd,
        (char*)&frame_size,
        sizeof(frame_size)
      );

      if (pkt_size != sizeof(frame_size)) {
        snprintf(
          logstr,
          sizeof(logstr),
          "Received unexpected frame size buffer with %ld bytes from cam %s",
          pkt_size,
          ctx->conf->name
        );
        log(ERROR, logstr);
        break;
      }

      if (frame_size > ENCODED_FRAME_BUF_SIZE) {
        snprintf(
          logstr,
          sizeof(logstr),
          "Received frame size that is larger than the allocated buffer of %d bytes: %d",
          ENCODED_FRAME_BUF_SIZE,
          frame_size
        );
        log(ERROR, logstr);
        break;
      }

      pkt_size = recv_from_stream(
        clientfd,
        (char*)enc_frame_buf,
        frame_size
      );

      if (pkt_size != frame_size) {
        snprintf(
          logstr,
          sizeof(logstr),
          "Received unexpected frame size with %zd bytes from cam %s",
          pkt_size,
          ctx->conf->name
        );
        log(ERROR, logstr);
        break;
      }

      snprintf(
        logstr,
        sizeof(logstr),
        "Received frame with %zd bytes from cam %s with timestamp %lu",
        pkt_size,
        ctx->conf->name,
        timestamp
      );
      log(INFO, logstr);

      ret = decode_packet(
        &viddec,
        enc_frame_buf,
        frame_size
      );
      if (ret) {
        break;
      }
    }

    if (new_frame) {
      ret = recv_frame(
        &viddec,
        ctx->frame_buf
      );

      if (ret == EAGAIN) {
        continue;
      } else if (ret == ENODATA) {
        log(INFO, "Recvd EOF from decoder");
        break;
      } else if (ret) {
        break;
      } else {
        log(INFO, "Recvd frame from decoder");

        uint64_t ts = 0;
        dequeue(&timestamp_queue, (void*)&ts);
        snprintf(
          logstr,
          sizeof(logstr),
          "Dequeued timestamp from cam %s with timestamp %lu",
          ctx->conf->name,
          ts
        );
        log(INFO, logstr);
      }
    }
  }

cleanup:
  if (enc_frame_buf) {
    free(enc_frame_buf);
  }
  cleanup_decoder(&viddec);
  cleanup_queue(&timestamp_queue);
  if (sockfd >= 0) {
    close(sockfd);
  }
  if (clientfd >= 0) {
    close(clientfd);
  }

  return NULL;
}

static int pin_to_core(int core) {
  int ret = 0;
  char logstr[128];

  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core, &cpuset);

  pid_t tid = gettid();
  ret = sched_setaffinity(
    tid,
    sizeof(cpu_set_t),
    &cpuset
  );
  if (ret == -1) {
    snprintf(
      logstr,
      sizeof(logstr),
      "Error pinning thread %d to core %d, err: %s",
      tid,
      core,
      strerror(errno)
    );
    log(ERROR, logstr);
    return -errno;
  }

  snprintf(
    logstr,
    sizeof(logstr),
    "Successfuly pinned thread %d to core %d",
    tid,
    core
  );
  log(INFO, logstr);

  return ret;
}
