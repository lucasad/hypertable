/** -*- c++ -*-
 * Copyright (C) 2007-2012 Hypertable, Inc.
 *
 * This file is part of Hypertable.
 *
 * Hypertable is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 3 of the
 * License, or any later version.
 *
 * Hypertable is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#ifndef HYPERTABLE_RECOVERYCOUNTER_H
#define HYPERTABLE_RECOVERYCOUNTER_H

#include <boost/thread/condition.hpp>

#include <set>
#include <vector>

#include "Common/ReferenceCount.h"
#include "Common/Time.h"
#include "Common/Timer.h"

#include "Hypertable/Lib/Types.h"

namespace Hypertable {

  /**
   * Tracks outstanding RangeServer recover requests.
   */
  class RecoveryCounter : public ReferenceCount {
  public:

    struct Result {
      QualifiedRangeSpec range;
      int error;
    };

    RecoveryCounter(uint32_t attempt)
      : m_attempt(attempt), m_done(false), m_errors(false), m_timed_out(false) { }

    void add(const std::vector<QualifiedRangeSpec> &ranges) {
      ScopedLock lock(m_mutex);
      foreach_ht (const QualifiedRangeSpec &range, ranges)
        m_outstanding_ranges.insert(range);
    }

    void result_callback(uint32_t attempt, std::vector<Result> &results) {
      ScopedLock lock(m_mutex);

      if (!m_outstanding_ranges.size() || attempt != m_attempt) {
        HT_INFOF("Callback for abandoned attempt %u (current attempt ID = %u)",
                attempt, m_attempt);
        return;
      }

      foreach_ht (Result &rr, results) {
        if (rr.error != Error::OK) {
          m_errors = true;
          HT_INFO_OUT << "Received error " << rr.error << " for range "
              << rr.range << HT_END;
        }

        std::set<QualifiedRangeSpec>::iterator set_it
                    = m_outstanding_ranges.find(rr.range);
        HT_ASSERT(set_it != m_outstanding_ranges.end());
        rr.range = *set_it;
        m_results.push_back(rr);
        m_outstanding_ranges.erase(set_it);
      }
      if (!m_outstanding_ranges.size())
        m_cond.notify_all();
    }

    bool wait_for_completion(Timer &timer) {
      ScopedLock lock(m_mutex);
      boost::xtime expire_time;

      timer.start();

      while (m_outstanding_ranges.size()) {
        boost::xtime_get(&expire_time, boost::TIME_UTC_);
        xtime_add_millis(expire_time, timer.remaining());
        if (!m_cond.timed_wait(lock, expire_time)) {
          HT_WARN_OUT << "RecoveryCounter timed out" << HT_END;
          m_errors = true;
          m_timed_out = true;
          foreach_ht (const QualifiedRangeSpec &range, m_outstanding_ranges) {
            HT_INFO_OUT << "Range " << range.range.start_row << ".."
                << range.range.end_row << " timed out" << HT_END;
            Result rr;
            rr.range = range;
            rr.error = Error::REQUEST_TIMEOUT;
            m_results.push_back(rr);
          }
          m_outstanding_ranges.clear();
        }
      }
      m_done = true;
      return !(m_errors);
    }

    std::vector<Result> &get_results() {
      ScopedLock lock(m_mutex);
      HT_ASSERT(m_done);
      return m_results;
    }

    bool timed_out() {
      ScopedLock lock(m_mutex);
      return m_timed_out;
    }

    uint32_t get_attempt() const { return m_attempt; }

    void set_range_errors(const std::vector<QualifiedRangeSpec> &ranges, int error) {
      ScopedLock lock(m_mutex);
      std::set<QualifiedRangeSpec>::iterator set_it;

      foreach_ht (const QualifiedRangeSpec &range, ranges) {
        set_it = m_outstanding_ranges.find(range);
        if (set_it != m_outstanding_ranges.end()) {
          Result rr;
          rr.range = *set_it;
          rr.error = error;
          m_results.push_back(rr);
          m_outstanding_ranges.erase(set_it);
        }
      }
    }

  protected:
    uint32_t m_attempt;
    Mutex m_mutex;
    boost::condition m_cond;
    bool m_done;
    bool m_errors;
    bool m_timed_out;
    std::vector<Result> m_results;
    std::set<QualifiedRangeSpec> m_outstanding_ranges;
    std::set<QualifiedRangeSpec> m_success;
  };
  typedef intrusive_ptr<RecoveryCounter> RecoveryCounterPtr;
}

#endif // HYPERTABLE_RECOVERYCOUNTER_H
