/******************************************************************************
 * Copyright 2018 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

#ifndef INCLUDE_CYBER_COMMON_ASYNC_LOGGER_H_
#define INCLUDE_CYBER_COMMON_ASYNC_LOGGER_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "cyber/common/macros.h"
#include "glog/logging.h"

namespace apollo {
namespace cyber {
namespace logger {

// Wrapper for a glog Logger which asynchronously writes log messages.
// This class starts a new thread responsible for forwarding the messages
// to the logger, and performs double buffering. Writers append to the
// current buffer and then wake up the logger thread. The logger swaps in
// a new buffer and writes any accumulated messages to the wrapped
// Logger.
//
// This double-buffering design dramatically improves performance, especially
// for logging messages which require flushing the underlying file (i.e WARNING
// and above for default). The flush can take a couple of milliseconds, and in
// some cases can even block for hundreds of milliseconds or more. With the
// double-buffered approach, threads can proceed with useful work while the IO
// thread blocks.
//
// The semantics provided by this wrapper are slightly weaker than the default
// glog semantics. By default, glog will immediately (synchronously) flush
// WARNING
// and above to the underlying file, whereas here we are deferring that flush to
// a separate thread. This means that a crash just after a 'LOG_WARN' would
// may be missing the message in the logs, but the perf benefit is probably
// worth it. We do take care that a glog FATAL message flushes all buffered log
// messages before exiting.
//
// NOTE: the logger limits the total amount of buffer space, so if the
// underlying
// log blocks for too long, eventually the threads generating the log messages
// will block as well. This prevents runaway memory usage.
class AsyncLogger : public google::base::Logger {
 public:
  explicit AsyncLogger(google::base::Logger* wrapped, int max_buffer_bytes);

  ~AsyncLogger();

  void Start();

  // Stop the thread. Flush() and Write() must not be called after this.
  //
  // NOTE: this is currently only used in tests: in real life, we enable async
  // logging once when the program starts and then never disable it.
  //
  // REQUIRES: Start() must have been called.
  void Stop();

  // Write a message to the log.
  //
  // 'force_flush' is set by the GLog library based on the configured
  // '--logbuflevel' flag.
  // Any messages logged at the configured level or higher result in
  // 'force_flush'
  // being set to true, indicating that the message should be immediately
  // written to the
  // log rather than buffered in memory. See the class-level docs above for more
  // details about the implementation provided here.
  //
  // REQUIRES: Start() must have been called.
  void Write(bool force_flush, time_t timestamp, const char* message,
             int message_len) override;

  // Flush any buffered messages.
  void Flush() override;

  // Get the current LOG file size.
  // The return value is an approximate value since some
  // logged data may not have been flushed to disk yet.
  uint32_t LogSize() override;

  const std::thread* LogThread() const { return &thread_; }

 private:
  // A buffered message.
  //
  // TODO(todd): using std::string for buffered messages is convenient but not
  // as efficient as it could be. It's better to make the buffers just be
  // Arenas and allocate both the message data and Msg struct from them, forming
  // a linked list.
  struct Msg {
    time_t ts;
    std::string message;
    int32_t level;

    Msg(time_t ts, std::string&& message, int32_t level)
        : ts(ts), message(std::move(message)), level(level) {}
  };

  // A buffer of messages waiting to be flushed.
  struct Buffer {
    std::vector<Msg> messages;

    // Estimate of the size of 'messages'.
    std::atomic<int> size = {0};

    // Whether this buffer needs an explicit flush of the
    // underlying logger.
    bool flush = false;

    Buffer() {}

    inline void clear() {
      messages.clear();
      size = 0;
      flush = false;
    }

    inline void add(Msg&& msg, bool force_flush) {
      size += static_cast<int>(sizeof(msg))
              + static_cast<int>(msg.message.size());
      messages.emplace_back(std::move(msg));
      flush |= force_flush;
    }

    inline bool needs_flush_or_write() const {
      return flush || !messages.empty();
    }

   private:
    DISALLOW_COPY_AND_ASSIGN(Buffer);
  };

  bool BufferFull(const Buffer& buf) const;
  void RunThread();

  // The maximum number of bytes used by the entire class.
  int max_buffer_bytes_;

  google::base::Logger* const wrapped_;
  std::thread thread_;

  // Count of how many times the writer thread has flushed the buffers.
  // 64 bits should be enough to never worry about overflow.
  uint64_t flush_count_ = 0;

  // Count of how many times the writer thread has dropped the log messages.
  // 64 bits should be enough to never worry about overflow.
  uint64_t drop_count_ = 0;

  // Protects buffers as well as 'state_'.
  std::mutex mutex_;

  // Signaled by app threads to wake up the flusher, either for new
  // data or because 'state_' changed.
  std::condition_variable wake_flusher_cv_;

  // Signaled by the flusher thread when the flusher has swapped in
  // a free buffer to write to.
  // std::condition_variable free_buffer_cv_;

  // Signaled by the flusher thread when it has completed flushing
  // the current buffer.
  std::condition_variable flush_complete_cv_;

  // The buffer to which application threads append new log messages.
  std::unique_ptr<Buffer> active_buf_;

  // The buffer currently being flushed by the logger thread, cleared
  // after a successful flush.
  std::unique_ptr<Buffer> flushing_buf_;

  // Trigger for the logger thread to stop.
  enum State { INITTED, RUNNING, STOPPED };
  State state_ = INITTED;

  DISALLOW_COPY_AND_ASSIGN(AsyncLogger);
};

}  // namespace logger
}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_COMMON_ASYNC_LOGGER_H_
