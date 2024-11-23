// © 2024 Alec Fessler
// MIT License
// See LICENSE file in the project root for full license information.

#include <atomic>
#include <arpa/inet.h>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <sstream>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "camera_handler.h"
#include "connection.h"
#include "logger.h"
#include "videnc.h"

extern char** environ;
volatile static sig_atomic_t running = 0;
static timer_t timerid;
static int sockfd = -1;
static sem_t queue_counter;
std::unique_ptr<logger_t> logger;
std::unique_ptr<camera_handler_t> cam;

inline int init_realtime_scheduling(int recording_cpu);
inline int init_network(const std::string& server_ip, const std::string& port);
inline int init_timer();
inline int init_signals();
inline int register_with_kernel();
inline void arm_timer();
inline int stream_pkt(conn_info_t& conn, const uint8_t* data, size_t size);
void socket_disconnect_handler(int signo, siginfo_t* info, void* context);
void capture_signal_handler(int signo, siginfo_t* info, void* context);

int main() {
  try {
    config_parser config("config.txt");
    std::string server_ip = config.get_string("SERVER_IP");
    std::string port = config.get_string("PORT");
    int recording_cpu = config.get_int("RECORDING_CPU");
    int dma_frame_buffers = config.get_int("DMA_BUFFERS");

    conn_info_t conn{sockfd, server_ip, port};

    logger = std::make_unique<logger_t>("logs.txt");

    lock_free_queue_t frame_queue(dma_frame_buffers);

    if (sem_init(&queue_counter, 0, 0) < 0) {
      logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, "Failed to initialize semaphore");
      return -errno;
    }

    cam = std::make_unique<camera_handler_t>(config, *logger, frame_queue, queue_counter);
    videnc encoder(config);

    int ret;
    if ((ret = init_realtime_scheduling(recording_cpu)) < 0) return ret;
    if ((ret = init_network(server_ip, port)) < 0) return ret;
    if ((ret = init_timer()) < 0) return ret;
    if ((ret = init_signals()) < 0) return ret;
    if ((ret = register_with_kernel()) < 0) return ret;

    running = 1;
    while (running) {
      arm_timer();
      sem_wait(&queue_counter);

      void* frame = frame_queue.dequeue();
      if (!frame) continue;

      encoder.encode_frame((uint8_t*)frame, stream_pkt, conn);
    }

  } catch (const std::exception& e) {
    if (logger)
      logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, e.what());
    else
      std::cerr << e.what() << std::endl;
    return 1;
  }

  return 0;
}

void socket_disconnect_handler(int signo, siginfo_t* info, void* context) {
  (void)info;
  (void)context;
  if (signo == SIGUSR2 && sockfd >= 0) {
    close(sockfd);
    sockfd = -1;
    logger->log(logger_t::level_t::INFO, __FILE__, __LINE__, "Socket disconnected by timer");
  }
}

void capture_signal_handler(int signo, siginfo_t* info, void* context) {
  (void)info;
  (void)context;
  if (signo == SIGUSR1 && running) {
    cam->queue_request();
    logger->log(logger_t::level_t::INFO, __FILE__, __LINE__, "Capture request queued");
  } else if (signo == SIGINT || signo == SIGTERM) {
    running = 0;
    if (sockfd >= 0) {
      close(sockfd);
      sockfd = -1;
    }
    sem_destroy(&queue_counter);
  }
}

inline int init_realtime_scheduling(int recording_cpu) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(recording_cpu, &cpuset);
  if (sched_setaffinity(0, sizeof(cpuset), &cpuset) < 0) {
    logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, "Failed to set CPU affinity");
    return -errno;
  }

  struct sched_param param;
  param.sched_priority = sched_get_priority_max(SCHED_FIFO);
  if (sched_setscheduler(0, SCHED_FIFO, &param) < 0) {
    logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, "Failed to set real-time scheduling policy");
    return -errno;
  }
  return 0;
}

inline int init_network(const std::string& server_ip, const std::string& port) {
  sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, "Failed to create socket");
    return -errno;
  }

  struct sockaddr_in server_addr;
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(std::stoi(port));
  inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr);

  if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
    logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, "Failed to connect to server");
    return -errno;
  }
  return 0;
}

inline int init_timer() {
  struct sigevent sev;
  sev.sigev_notify = SIGEV_SIGNAL;
  sev.sigev_signo = SIGUSR2;
  sev.sigev_value.sival_ptr = &timerid;

  if (timer_create(CLOCK_MONOTONIC, &sev, &timerid) == -1) {
    logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, "Failed to create socket disconnect timer");
    return -errno;
  }
  return 0;
}

inline void arm_timer() {
  struct itimerspec its;
  its.it_value.tv_sec = 0;
  its.it_value.tv_nsec = 300000000;  // 0.3 seconds
  its.it_interval.tv_sec = 0;
  its.it_interval.tv_nsec = 0;

  timer_settime(timerid, 0, &its, NULL);
}

inline int init_signals() {
  struct sigaction action;
  action.sa_sigaction = capture_signal_handler;
  action.sa_flags = SA_SIGINFO;
  sigemptyset(&action.sa_mask);

  struct sigaction timer_action;
  timer_action.sa_sigaction = socket_disconnect_handler;
  timer_action.sa_flags = SA_SIGINFO;
  sigemptyset(&timer_action.sa_mask);

  if (sigaction(SIGUSR1, &action, NULL) < 0 ||
    sigaction(SIGINT, &action, NULL) < 0 ||
    sigaction(SIGTERM, &action, NULL) < 0 ||
    sigaction(SIGUSR2, &timer_action, NULL) < 0) {
    logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, "Failed to set signal handlers");
    return -errno;
  }
  return 0;
}

inline int register_with_kernel() {
  int fd = open("/proc/gpio_interrupt_pid", O_WRONLY);
  if (fd < 0) {
    logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, "Failed to open /proc/gpio_interrupt_pid");
    return -errno;
  }

  pid_t pid = getpid();
  if (dprintf(fd, "%d", pid) < 0) {
    logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, "Failed to write to /proc/gpio_interrupt_pid");
    close(fd);
    return -errno;
  }

  close(fd);
  return 0;
}

inline int stream_pkt(conn_info_t& conn, const uint8_t* data, size_t size) {
  size_t total_bytes_written = 0;
  while (total_bytes_written < size) {
    if (conn.sockfd < 0) {
      int ret = init_network(conn.server_ip, conn.port);
      if (ret < 0) return ret;
    }

    ssize_t result = write(
      conn.sockfd,
      data + total_bytes_written,
      size - total_bytes_written
    );

    if (result < 0) {
      if (errno == EINTR) continue;
      logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, "Error transmitting frame");
      return -1;
    }

    total_bytes_written += result;
  }

  logger->log(logger_t::level_t::INFO, __FILE__, __LINE__, "Transmitted frame");
  return 0;
}