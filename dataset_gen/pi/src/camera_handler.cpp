#include <cstring>
#include <iostream>
#include <semaphore.h>
#include <stdexcept>
#include <sys/mman.h>
#include "camera_handler.h"
#include "config.h"
#include "logger.h"
#include "lock_free_queue.h"

extern std::unique_ptr<logger_t> logger;

camera_handler_t::camera_handler_t(
  config& config,
  lock_free_queue_t& frame_queue,
  sem_t& queue_counter
) :
  frame_queue(frame_queue),
  queue_counter(queue_counter),
  next_req_idx_(0) {
  /**
   * Manages camera operations using the libcamera API, providing a high-level interface
   * for frame capture and buffer management. The handler coordinates three key tasks:
   *
   * 1. Camera initialization and configuration
   * 2. DMA buffer management for zero-copy frame capture
   * 3. Frame completion notification via callback system
   *
   * When a frame is captured, libcamera writes directly to a DMA buffer and invokes
   * our callback. The callback then enqueues the buffer pointer to a lock-free queue
   * and signals the main loop via semaphore that a new frame is ready for processing.
   *
   * Parameters:
   *   config:        Camera and frame settings including resolution and buffer counts
   *   frame_queue:   Lock-free queue for sharing frame buffers with main loop
   *   queue_counter: Semaphore tracking available frames in the queue
   *
   * The initialization sequence is:
   * 1. Configure frame properties (resolution, format)
   * 2. Initialize camera manager and acquire device
   * 3. Apply camera configuration
   * 4. Set up DMA buffers and memory mapping
   * 5. Configure camera controls (exposure, focus, etc)
   */
  init_frame_config(config);
  init_camera_manager();
  init_camera_config(config);
  init_dma_buffers();
  init_camera_controls(config);
}

void camera_handler_t::init_frame_config(config& config) {
  /**
   * Calculates and stores frame buffer parameters based on YUV420 format.
   *
   * YUV420 format has three planes:
   * - Y (luma): Full resolution (width × height)
   * - U, V (chroma): Quarter resolution (width/2 × height/2 each)
   *
   * Parameters:
   *   config: Contains frame dimensions and buffer count settings
   */
  dma_frame_buffers_ = config.dma_buffers;

  unsigned int y_plane_bytes = config.frame_width * config.frame_height;
  unsigned int u_plane_bytes = y_plane_bytes / 4;
  unsigned int v_plane_bytes = u_plane_bytes;
  frame_bytes_ = y_plane_bytes + u_plane_bytes + v_plane_bytes;
}

void camera_handler_t::init_camera_manager() {
  /**
   * Initializes and acquires exclusive access to the camera device.
   *
   * The initialization sequence is:
   * 1. Start the camera manager service
   * 2. Enumerate available cameras
   * 3. Select and acquire first available camera
   *
   * Throws:
   *   std::runtime_error: On any initialization failure (detailed in error message)
   */
  cm_ = std::make_unique<libcamera::CameraManager>();
  if (cm_->start() < 0) {
    const char* err = "Failed to start camera manager";
    logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
    throw std::runtime_error(err);
  }

  auto cameras = cm_->cameras();
  if (cameras.empty()) {
    const char* err = "No cameras available";
    logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
    throw std::runtime_error(err);
  }

  camera_ = cm_->get(cameras[0]->id());
  if (!camera_) {
    const char* err = "Failed to retrieve camera";
    logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
    throw std::runtime_error(err);
  }
  if (camera_->acquire() < 0) {
    const char* err = "Failed to acquire camera";
    logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
    throw std::runtime_error(err);
  }
}

void camera_handler_t::init_camera_config(config& config) {
  /**
   * Configures the camera stream for video recording.
   *
   * Sets core recording parameters:
   * - YUV420 pixel format for efficient encoding
   * - Frame resolution from config
   * - Number of DMA buffers to allocate
   *
   * The configuration is validated to ensure the camera supports these settings
   * without requiring adjustments.
   *
   * Parameters:
   *   config: Contains frame dimensions and buffer settings
   *
   * Throws:
   *   std::runtime_error: If configuration is invalid or fails to apply
   */
  config_ = camera_->generateConfiguration({ libcamera::StreamRole::VideoRecording });
  if (!config_) {
    const char* err = "Failed to generate camera configuration";
    logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
    throw std::runtime_error(err);
  }

  libcamera::StreamConfiguration& cfg = config_->at(0);
  cfg.pixelFormat = libcamera::formats::YUV420;
  cfg.size = { (unsigned int)config.frame_width, (unsigned int)config.frame_height };
  cfg.bufferCount = dma_frame_buffers_;

  if (config_->validate() == libcamera::CameraConfiguration::Invalid) {
    const char* err = "Invalid camera configuration, unable to adjust";
    logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
    throw std::runtime_error(err);
  } else if (config_->validate() == libcamera::CameraConfiguration::Adjusted) {
    const char* err = "Invalid camera configuration, adjusted";
    logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
    throw std::runtime_error(err);
  }

  if (camera_->configure(config_.get()) < 0) {
    const char* err = "Failed to configure camera";
    logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
    throw std::runtime_error(err);
  }
}

void camera_handler_t::init_dma_buffers() {
  /**
   * Sets up DMA buffer system for zero-copy frame capture.
   *
   * For each buffer in the capture pipeline:
   * 1. Creates a request with unique cookie as buffer identifier
   * 2. Associates request with a DMA buffer
   * 3. Maps buffer into process memory space
   * 4. Stores mapping in mmap_buffers_ indexed by cookie
   *
   * Finally connects the request completion callback for frame handling.
   *
   * Throws:
   *   std::runtime_error: If buffer allocation or mapping fails
   */
  allocator_ = std::make_unique<libcamera::FrameBufferAllocator>(camera_);
  stream_ = config_->at(0).stream();
  if (allocator_->allocate(stream_) < 0) {
    const char* err = "Failed to allocate buffers";
    logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
    throw std::runtime_error(err);
  }

  uint64_t req_cookie = 0; // maps request to index in mmap_buffers_
  for (const std::unique_ptr<libcamera::FrameBuffer>& buffer : allocator_->buffers(stream_)) {
    std::unique_ptr<libcamera::Request> request = camera_->createRequest(req_cookie++);
    if (!request) {
      const char* err = "Failed to create request";
      logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
      throw std::runtime_error(err);
    }

    if (request->addBuffer(stream_, buffer.get()) < 0) {
      const char* err = "Failed to add buffer to request";
      logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
      throw std::runtime_error(err);
    }

    requests_.push_back(std::move(request));

    const libcamera::FrameBuffer::Plane& y_plane = buffer->planes()[0];
    const libcamera::FrameBuffer::Plane& u_plane = buffer->planes()[1];
    const libcamera::FrameBuffer::Plane& v_plane = buffer->planes()[2];

    unsigned int y_plane_bytes = frame_bytes_ * 2/3;  // Based on YUV420
    unsigned int u_plane_bytes = y_plane_bytes / 4;
    unsigned int v_plane_bytes = u_plane_bytes;

    if (y_plane.length != y_plane_bytes || u_plane.length != u_plane_bytes || v_plane.length != v_plane_bytes) {
      const char* err = "Plane size does not match expected size";
      logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
      throw std::runtime_error(err);
    }

    void* data = mmap(
      nullptr,
      frame_bytes_,
      PROT_READ | PROT_WRITE,
      MAP_SHARED,
      y_plane.fd.get(),
      y_plane.offset
    );

    if (data == MAP_FAILED) {
      std::string err = "Failed to mmap plane data: " + std::string(strerror(errno));
      logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, err.c_str());
      throw std::runtime_error(err);
    }

    mmap_buffers_.push_back(data);
  }

  camera_->requestCompleted.connect(this, &camera_handler_t::request_complete);
}

void camera_handler_t::init_camera_controls(config& config) {
  /**
   * Configures camera settings for consistent, high-quality video capture.
   *
   * Following cinematography best practices, this method:
   * 1. Sets exposure time to half the frame interval (180° shutter rule)
   *    This prevents motion blur while maintaining natural motion appearance
   *
   * 2. Disables automatic controls that could cause frame timing variation:
   *    - Auto exposure (AE)
   *    - Auto white balance (AWB)
   *    - Auto focus (AF)
   *    - HDR mode
   *
   * 3. Fixes focus at ~12 inches (lens position 3.33)
   *    The lens position is the reciprocal of the focus distance in meters
   *
   * 4. Sets gain to 1.0 (equivalent to ISO 100) for minimal noise
   *
   * Parameters:
   *   config: Contains frame timing constraints (duration_min/max)
   *
   * Throws:
   *   std::runtime_error if camera fails to start with these settings
   *
   * Note:
   *   These parameters will change, but are fine for development
   */
  unsigned int frame_duration_min = config.frame_duration_min;
  unsigned int frame_duration_max = config.frame_duration_max;

  controls_ = std::make_unique<libcamera::ControlList>();

  // Fix exposure time to half the time between frames
  // May be able to remove frame duration limit control since we are setting exposure
  controls_->set(libcamera::controls::FrameDurationLimits, libcamera::Span<const std::int64_t, 2>({ frame_duration_min, frame_duration_max }));
  controls_->set(libcamera::controls::AeEnable, false);
  controls_->set(libcamera::controls::ExposureTime, frame_duration_min);

  // Fix focus to ~12 inches
  // Focus value should be reciprocal of distance in meters
  controls_->set(libcamera::controls::AfMode, libcamera::controls::AfModeManual);
  controls_->set(libcamera::controls::LensPosition, 3.33);

  // Fix white balance, gain, and disable HDR
  controls_->set(libcamera::controls::AwbEnable, false);
  controls_->set(libcamera::controls::AnalogueGain, 1.0);
  controls_->set(libcamera::controls::HdrMode, libcamera::controls::HdrModeOff);

  controls_->set(libcamera::controls::rpi::StatsOutputEnable, false);

  if (camera_->start(controls_.get()) < 0) {
    const char* err = "Failed to start camera";
    logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
    throw std::runtime_error(err);
  }
}

camera_handler_t::~camera_handler_t() {
  /**
   * Cleanly shuts down camera and releases resources.
   *
   * Strict cleanup order is required:
   * 1. Stop camera capture
   * 2. Unmap DMA buffers
   * 3. Free buffer allocator
   * 4. Release camera device
   * 5. Stop camera manager
   *
   * Warning: Do not modify this sequence as it may cause
   * undefined behavior or resource leaks
   */
  camera_->stop();
  for (void* data : mmap_buffers_)
    munmap(data, frame_bytes_);
  allocator_->free(stream_);
  allocator_.reset();
  camera_->release();
  camera_.reset();
  cm_->stop();
}

void camera_handler_t::queue_request() {
  /**
   * Queue the next request in the sequence.
   *
   * Before queuing the request, ensure that the number of enqueued
   * buffers is no more than dma_frame_buffers_ - 2. This is because
   * the queue counter may fall behind by, but no more than, 1. This
   * occurs when the main loop calls sem_wait, decrementing the
   * semaphore, but before it dequeues the buffer. Thus, we check
   * for 2 less than max to ensure at least one is available even
   * if the counter is behind by 1. The queue counter may also be
   * incremented externally without a frame to unblock the main
   * loop. This is also safely handled with this same check.
   *
   * If requests are not returned at the same rate as they are queued,
   * this method will throw to signal that the camera is not keeping up,
   * and this should be handled by adjusting the configuration.
   * ie. framerate, exposure, gain, etc.
   *
   * Throws:
   *  - std::runtime_error: Buffer is not ready for requeuing
   *  - std::runtime_error: Failed to queue request
   */
  int enqueued_bufs;
  sem_getvalue(&queue_counter, &enqueued_bufs);
  if (enqueued_bufs > dma_frame_buffers_ - 2) {
    const char* err = "Buffer is not ready for requeuing";
    logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
    throw std::runtime_error(err);
  }

  if (camera_->queueRequest(requests_[next_req_idx_].get()) < 0) {
    const char* err = "Failed to queue request";
    logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
    throw std::runtime_error(err);
  }

  ++next_req_idx_;
  next_req_idx_ %= requests_.size();
}

void camera_handler_t::request_complete(libcamera::Request* request) {
  /**
   * Handles frame capture completion from libcamera's callback system.
   *
   * This method executes in libcamera's thread context when a DMA buffer
   * is filled with a new frame. It performs three critical tasks:
   *
   * 1. Retrieves the filled buffer using the request's cookie as an index
   * 2. Enqueues the buffer pointer to the lock-free queue for processing
   * 3. Signals frame availability by incrementing the semaphore
   *
   * The lock-free design ensures this callback can safely preempt the main
   * loop, even in the midst of a dequeue operation, as the CAS operation
   * will detect the change and retry upon this functions return. The use
   * of the semaphore ensures the main loop never does any polling or busy
   * waiting, only awakening when there are available frames.
   *
   * Parameters:
   *   request: Contains metadata about the completed capture, including
   *           the cookie identifying which DMA buffer was used
   *
   * Note: Returns immediately if request status is RequestCancelled
   */
  if (request->status() == libcamera::Request::RequestCancelled)
    return;

  logger->log(logger_t::level_t::INFO, __FILE__, __LINE__, "Request completed");

  void* data = mmap_buffers_[request->cookie()];
  frame_queue.enqueue(data);

  sem_post(&queue_counter);
  request->reuse(libcamera::Request::ReuseBuffers);
}
