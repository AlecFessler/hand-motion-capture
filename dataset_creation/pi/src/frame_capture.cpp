#include <atomic>
#include <cstdint>
#include <exception>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include "camera_handler.h"
#include "stream_thread.h"
#include "logger.h"
#include "p_ctx.h"

extern char** environ;

void sig_handler(int signo, siginfo_t* info, void* context) {
  static p_ctx_t& p_ctx = p_ctx_t::get_instance();
  if (signo == SIGUSR1 && p_ctx.running.load(std::memory_order_acquire)) {
    p_ctx.cam->queue_request();
    const char* info = "Capture request queued";
    p_ctx.logger->log(logger_t::level_t::INFO, __FILE__, __LINE__, info);
  } else if (signo == SIGINT || signo == SIGTERM) {
    p_ctx.running.store(false, std::memory_order_relaxed);
  }
}

int main() {
  p_ctx_t& p_ctx = p_ctx_t::get_instance();
  try {
    logger_t logger("logs.txt");
    p_ctx.logger = &logger;

    std::pair<unsigned int, unsigned int> resolution = std::make_pair(IMAGE_WIDTH, IMAGE_HEIGHT);
    int buffers_count = 4;
    std::pair<std::int64_t, std::int64_t> frame_duration_limits = std::make_pair(16667, 16667);
    camera_handler_t cam(resolution, buffers_count, frame_duration_limits);
    p_ctx.cam = &cam;

    int num_threads = 2;
    mm_lock_free_stack_t available_buffers(num_threads, PREALLOCATED_BUFFERS);
    p_ctx.available_buffers = &available_buffers;

    lock_free_queue_t frame_queue(num_threads, PREALLOCATED_BUFFERS);
    p_ctx.frame_queue = &frame_queue;

    auto image_buffers = std::make_unique<char[]>(IMAGE_BYTES * PREALLOCATED_BUFFERS);
    for (int i = 0; i < PREALLOCATED_BUFFERS; i++) {
      available_buffers.push((void*)&image_buffers[i * IMAGE_BYTES]);
    }

    p_ctx.running.store(false, std::memory_order_relaxed);

    #define initialize_semaphore(sem) \
      sem_t sem; \
      if (sem_init(&sem, 0, 0) < 0) { \
        const char* err = "Failed to initialize semaphore"; \
        logger.log(logger_t::level_t::ERROR, __FILE__, __LINE__, err); \
        return -errno; \
      } \
      p_ctx.sem = &sem;
    initialize_semaphore(thread1_ready);
    initialize_semaphore(thread2_ready);
    initialize_semaphore(queue_counter);

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(3, &cpuset);
    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) < 0) {
      const char* err = "Failed to set CPU affinity";
      p_ctx.logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
      return -errno;
    }

    struct sched_param param;
    param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    if (sched_setscheduler(0, SCHED_FIFO, &param) < 0) {
      const char* err = "Failed to set real-time scheduling policy";
      p_ctx.logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
      return -errno;
    }

    pthread_t thread_id;
    if (pthread_create(&thread_id, nullptr, &stream_thread, &p_ctx) < 0) {
      const char* err = "Failed to create thread";
      p_ctx.logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
      return -errno;
    }

    struct sigaction action;
    action.sa_sigaction = sig_handler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if (
      sigaction(SIGUSR1, &action, NULL) < 0 ||
      sigaction(SIGINT, &action, NULL) < 0 ||
      sigaction(SIGTERM, &action, NULL) < 0
    ) {
      const char* err = "Failed to set signal handler";
      p_ctx.logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
      return -errno;
    }

    int fd;
    fd = open("/proc/gpio_interrupt_pid", O_WRONLY);
    if (fd < 0) {
      const char* err = "Failed to open /proc/gpio_interrupt_pid";
      p_ctx.logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
      return -errno;
    }
    pid_t pid = getpid();
    if (dprintf(fd, "%d", pid) < 0) {
      const char* err = "Failed to write to /proc/gpio_interrupt_pid";
      p_ctx.logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
      close(fd);
      return -errno;
    }
    close(fd);

    sem_wait(p_ctx.thread2_ready);
    p_ctx.running.store(true, std::memory_order_relaxed);
    sem_post(p_ctx.thread1_ready);
    while (p_ctx.running.load(std::memory_order_acquire)) {
      pause();
    }

    pthread_join(thread_id, nullptr);

  } catch (const std::exception& e) {
    if (p_ctx.logger)
      p_ctx.logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, e.what());
    else
      std::cerr << e.what() << std::endl;
    return 1;
  }

  return 0;
}
