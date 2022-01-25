/**
 * RPiPlay - An open-source AirPlay mirroring server for Raspberry Pi
 * Copyright (C) 2019 Florian Draschbacher
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 */

#include "video_renderer.h"

#include <cassert>
#include <chrono>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <ratio>
#include <thread>

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#ifdef X_DISPLAY_FIX
#include "x_display_fix.h"
#endif

struct video_renderer_s {
  GstElement *appsrc, *pipeline, *sink;
  GstBus* bus;
#ifdef X_DISPLAY_FIX
  const char* server_name;
  X11_Window_t* gst_window;
#endif
};

static void append_videoflip(GString* launch, const videoflip_t* flip, const videoflip_t* rot) {
  /* videoflip image transform */
  switch (*flip) {
  case INVERT:
    switch (*rot) {
    case LEFT:
      g_string_append(launch, "videoflip method=clockwise ! ");
      break;
    case RIGHT:
      g_string_append(launch, "videoflip method=counterclockwise ! ");
      break;
    default:
      g_string_append(launch, "videoflip method=rotate-180 ! ");
      break;
    }
    break;
  case HFLIP:
    switch (*rot) {
    case LEFT:
      g_string_append(launch, "videoflip method=upper-left-diagonal ! ");
      break;
    case RIGHT:
      g_string_append(launch, "videoflip method=upper-right-diagonal ! ");
      break;
    default:
      g_string_append(launch, "videoflip method=horizontal-flip ! ");
      break;
    }
    break;
  case VFLIP:
    switch (*rot) {
    case LEFT:
      g_string_append(launch, "videoflip method=upper-right-diagonal ! ");
      break;
    case RIGHT:
      g_string_append(launch, "videoflip method=upper-left-diagonal ! ");
      break;
    default:
      g_string_append(launch, "videoflip method=vertical-flip ! ");
      break;
    }
    break;
  default:
    switch (*rot) {
    case LEFT:
      g_string_append(launch, "videoflip method=counterclockwise ! ");
      break;
    case RIGHT:
      g_string_append(launch, "videoflip method=clockwise ! ");
      break;
    default:
      break;
    }
    break;
  }
}

static video_renderer_t* renderer = NULL;
static logger_t* logger = NULL;
static unsigned short width, height, width_source, height_source; /* not currently used */

void video_renderer_size(float* f_width_source, float* f_height_source, float* f_width,
                         float* f_height) {
  width_source = (unsigned short)*f_width_source;
  height_source = (unsigned short)*f_height_source;
  width = (unsigned short)*f_width;
  height = (unsigned short)*f_height;
  logger_log(logger, LOGGER_DEBUG, "begin video stream wxh = %dx%d; source %dx%d", width, height,
             width_source, height_source);
}

void video_renderer_init(logger_t* render_logger, const char* server_name, videoflip_t videoflip[2],
                         const char* decoder, const char* videosink) {
  GError* error = NULL;
  logger = render_logger;

  /* this call to g_set_application_name makes server_name appear in the  X11 display window title
   * bar, */
  /* (instead of the program name uxplay taken from (argv[0]). It is only set one time. */

  const gchar* appname = g_get_application_name();
  if (!appname || strcmp(appname, server_name))
    g_set_application_name(server_name);
  appname = NULL;

  renderer = new video_renderer_t;
  assert(renderer);

  gst_init(NULL, NULL);

  GString* launch = g_string_new(
      "appsrc name=video_source stream-type=0 format=GST_FORMAT_TIME is-live=true ! queue ! ");
  g_string_append(launch, decoder);
  g_string_append(launch, " ! videoconvert ! ");
  append_videoflip(launch, &videoflip[0], &videoflip[1]);
  g_string_append(launch, videosink);
  g_string_append(launch, " name=video_sink sync=false");
  renderer->pipeline = gst_parse_launch(launch->str, &error);
  g_assert(renderer->pipeline);
  g_string_free(launch, TRUE);

  renderer->appsrc = gst_bin_get_by_name(GST_BIN(renderer->pipeline), "video_source");
  assert(renderer->appsrc);
  renderer->sink = gst_bin_get_by_name(GST_BIN(renderer->pipeline), "video_sink");
  assert(renderer->sink);

#ifdef X_DISPLAY_FIX
  renderer->server_name = server_name;
  renderer->gst_window = NULL;
  bool x_display_fix = false;
  if (strcmp(videosink, "autovideosink") == 0 || strcmp(videosink, "ximagesink") == 0 ||
      strcmp(videosink, "xvimagesink") == 0) {
    x_display_fix = true;
  }
  if (x_display_fix) {
    renderer->gst_window = calloc(1, sizeof(X11_Window_t));
    assert(renderer->gst_window);
    get_X11_Display(renderer->gst_window);
  }
#endif
  gst_element_set_state(renderer->pipeline, GST_STATE_READY);
  GstState state;
  if (gst_element_get_state(renderer->pipeline, &state, nullptr, 0) != 0) {
    if (state == GST_STATE_READY) {
      logger_log(logger, LOGGER_DEBUG, "Initialized GStreamer video renderer");
    } else {
      logger_log(logger, LOGGER_ERR, "Failed to initialize GStreamer video renderer");
    }
  } else {
    logger_log(logger, LOGGER_ERR, "Failed to initialize GStreamer video renderer");
  }
}

uint64_t milli_to_frames(uint64_t milli) {
  uint64_t a = milli * 3;
  uint64_t b1 = a / 50000;
  uint64_t b2 = a % 50000;
  return b1 + (b2 > 25000);
}

struct ThreadQueue {
  struct Data {
    std::unique_ptr<unsigned char[]> data;
    int data_len;
    uint64_t pts;

    Data(std::unique_ptr<unsigned char[]>&& data, int data_len, uint64_t pts)
        : data(std::move(data)), data_len(data_len), pts(pts) {}

    friend bool operator<(const Data& d1, const Data& d2) {
      return d1.pts < d2.pts;
    }
  };

  ThreadQueue()
      : thread_([this] {
          using clock_t = std::chrono::steady_clock;
          using timepoint_t = typename clock_t::time_point;
          using offset_t = std::chrono::duration<uint64_t, std::ratio<1, 60>>;
          using half_offset_t = std::chrono::duration<uint64_t, std::ratio<1, 120>>;
          using micros_t = std::chrono::duration<uint64_t, std::micro>;

          uint64_t start_pts{0};
          offset_t start_offset{0};
          const auto pts_offset = std::chrono::duration_cast<micros_t>(half_offset_t(19)).count();
          const timepoint_t start{clock_t::now()};
          offset_t offset{0};

          std::unique_lock lock{mutex_};
          while (not stop_) {
            ++offset;
            ++start_offset;
            // std::cout << "queue: " << queue_.size() << std::endl;
            if (not queue_.empty()) {
              auto& frame_ref = queue_.back();
              if (start_pts == 0 and frame_ref.pts != 0) {
                start_pts = frame_ref.pts;
                start_offset = offset_t(0);
              }
              const auto micro_offset = std::chrono::duration_cast<micros_t>(start_offset).count();

              if (frame_ref.pts == 0 or frame_ref.pts + pts_offset <= start_pts + micro_offset) {
                Data frame = std::move(frame_ref);
                queue_.pop_back();
                lock.unlock();
                render_buffer(frame.data.get(), frame.data_len, frame.pts);
                // std::cout << "pts: " << frame.pts << std::endl;
              } else {
                lock.unlock();
              }
            } else {
              lock.unlock();
            }
            std::this_thread::sleep_until(start + offset);
            lock.lock();
          }
        }){};

  ~ThreadQueue() {
    {
      std::lock_guard lock{mutex_};
      stop_ = true;
    }
    thread_.join();
  }

  void enqueue(unsigned char* data, int data_len, uint64_t pts) {
    // std::cout << "enqueue " << pts << std::endl;
    std::lock_guard lock{mutex_};
    auto new_data = std::make_unique_for_overwrite<unsigned char[]>(data_len);
    std::memcpy(new_data.get(), data, data_len);
    queue_.emplace_front(std::move(new_data), data_len, pts);
  }

private:
  static void render_buffer(unsigned char* data, int data_len, uint64_t pts) {
    GstBuffer* buffer;
    assert(data_len != 0);
    /* first four bytes of valid video data are 0x0, 0x0, 0x0, 0x1 */
    /* first byte of invalid data (decryption failed) is 0x1 */
    if (data[0]) {
      logger_log(logger, LOGGER_ERR, "*** ERROR decryption of video packet failed ");
    } else {
      buffer = gst_buffer_new_and_alloc(data_len);
      assert(buffer != NULL);
      // GST_BUFFER_DTS(buffer) = (GstClockTime)pts;
      gst_buffer_fill(buffer, 0, data, data_len);
      gst_app_src_push_buffer(GST_APP_SRC(renderer->appsrc), buffer);
#ifdef X_DISPLAY_FIX
      if (renderer->gst_window && !(renderer->gst_window->window)) {
        fix_x_window_name(renderer->gst_window, renderer->server_name);
      }
#endif
    }
  }

  std::mutex mutex_{};
  bool stop_{false};
  std::deque<Data> queue_{};
  std::thread thread_;
};

ThreadQueue* queue = nullptr;

void video_renderer_start() {
  gst_element_set_state(renderer->pipeline, GST_STATE_PLAYING);
  renderer->bus = gst_element_get_bus(renderer->pipeline);
  queue = new ThreadQueue();
}

void video_renderer_render_buffer(raop_ntp_t* ntp, unsigned char* data, int data_len, uint64_t pts,
                                  int type) {
  assert(queue);
  queue->enqueue(data, data_len, pts);
}

void video_renderer_flush() {}

void video_renderer_stop() {
  if (renderer) {
    gst_app_src_end_of_stream(GST_APP_SRC(renderer->appsrc));
    gst_element_set_state(renderer->pipeline, GST_STATE_NULL);
  }
  delete queue;
  queue = nullptr;
}

void video_renderer_destroy() {
  if (renderer) {
    GstState state;
    gst_element_get_state(renderer->pipeline, &state, NULL, 0);
    if (state != GST_STATE_NULL) {
      gst_app_src_end_of_stream(GST_APP_SRC(renderer->appsrc));
      gst_element_set_state(renderer->pipeline, GST_STATE_NULL);
    }
    gst_object_unref(renderer->bus);
    gst_object_unref(renderer->sink);
    gst_object_unref(renderer->appsrc);
    gst_object_unref(renderer->pipeline);
#ifdef X_DISPLAY_FIX
    if (renderer->gst_window) {
      free(renderer->gst_window);
      renderer->gst_window = NULL;
    }
#endif
    delete renderer;
    renderer = NULL;
  }
}

/* not implemented for gstreamer */
void video_renderer_update_background(int type) {}

gboolean gstreamer_pipeline_bus_callback(GstBus* bus, GstMessage* message, gpointer loop) {
  switch (GST_MESSAGE_TYPE(message)) {
  case GST_MESSAGE_ERROR: {
    GError* err;
    gchar* debug;
    gboolean flushing;
    gst_message_parse_error(message, &err, &debug);
    logger_log(logger, LOGGER_INFO, "GStreamer error: %s", err->message);
    g_error_free(err);
    g_free(debug);
    gst_app_src_end_of_stream(GST_APP_SRC(renderer->appsrc));
    flushing = TRUE;
    gst_bus_set_flushing(bus, flushing);
    gst_element_set_state(renderer->pipeline, GST_STATE_NULL);
    g_main_loop_quit((GMainLoop*)loop);
    break;
  }
  case GST_MESSAGE_EOS:
    /* end-of-stream */
    logger_log(logger, LOGGER_INFO, "GStreamer: End-Of-Stream");
    //   g_main_loop_quit( (GMainLoop *) loop);
    break;
  default:
    /* unhandled message */
    break;
  }
  return TRUE;
}

unsigned int video_renderer_listen(void* loop) {
  return (unsigned int)gst_bus_add_watch(renderer->bus, (GstBusFunc)gstreamer_pipeline_bus_callback,
                                         (gpointer)loop);
}
