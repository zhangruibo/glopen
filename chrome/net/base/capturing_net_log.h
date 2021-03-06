// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_BASE_CAPTURING_NET_LOG_H_
#define NET_BASE_CAPTURING_NET_LOG_H_

#include <vector>

#include "base/basictypes.h"
#include "base/ref_counted.h"
#include "base/scoped_ptr.h"
#include "net/base/net_log.h"

namespace net {

// CapturingNetLog is an implementation of NetLog that saves messages to a
// bounded buffer.
class CapturingNetLog : public NetLog {
 public:
  struct Entry {
    Entry(EventType type,
          const base::TimeTicks& time,
          Source source,
          EventPhase phase,
          EventParameters* extra_parameters)
        : type(type), time(time), source(source), phase(phase),
          extra_parameters(extra_parameters) {
    }

    EventType type;
    base::TimeTicks time;
    Source source;
    EventPhase phase;
    scoped_refptr<EventParameters> extra_parameters;
  };

  // Ordered set of entries that were logged.
  typedef std::vector<Entry> EntryList;

  enum { kUnbounded = -1 };

  // Creates a CapturingNetLog that logs a maximum of |max_num_entries|
  // messages.
  explicit CapturingNetLog(size_t max_num_entries);

  // NetLog implementation:
  virtual void AddEntry(EventType type,
                        const base::TimeTicks& time,
                        const Source& source,
                        EventPhase phase,
                        EventParameters* extra_parameters);
  virtual uint32 NextID();
  virtual bool HasListener() const { return true; }

  // Returns the list of all entries in the log.
  const EntryList& entries() const { return entries_; }

  void Clear();

 private:
  uint32 next_id_;
  size_t max_num_entries_;
  EntryList entries_;

  DISALLOW_COPY_AND_ASSIGN(CapturingNetLog);
};

// Helper class that exposes a similar API as BoundNetLog, but uses a
// CapturingNetLog rather than the more generic NetLog.
//
// CapturingBoundNetLog can easily be converted to a BoundNetLog using the
// bound() method.
class CapturingBoundNetLog {
 public:
  CapturingBoundNetLog(const NetLog::Source& source, CapturingNetLog* net_log)
      : source_(source), capturing_net_log_(net_log) {
  }

  explicit CapturingBoundNetLog(size_t max_num_entries)
      : capturing_net_log_(new CapturingNetLog(max_num_entries)) {}

  // The returned BoundNetLog is only valid while |this| is alive.
  BoundNetLog bound() const {
    return BoundNetLog(source_, capturing_net_log_.get());
  }

  // Returns the list of all entries in the log.
  const CapturingNetLog::EntryList& entries() const {
    return capturing_net_log_->entries();
  }

  void Clear();

  // Sends all of captured messages to |net_log|, using the same source ID
  // as |net_log|.
  void AppendTo(const BoundNetLog& net_log) const;

 private:
  NetLog::Source source_;
  scoped_ptr<CapturingNetLog> capturing_net_log_;

  DISALLOW_COPY_AND_ASSIGN(CapturingBoundNetLog);
};

}  // namespace net

#endif  // NET_BASE_CAPTURING_NET_LOG_H_

