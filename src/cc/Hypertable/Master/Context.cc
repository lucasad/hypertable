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

#include "Common/Compat.h"
#include "Common/FailureInducer.h"

#include "Context.h"
#include "LoadBalancer.h"
#include "Operation.h"
#include "OperationRecover.h"
#include "ReferenceManager.h"
#include "RecoveryReplayCounter.h"
#include "BalancePlanAuthority.h"

using namespace Hypertable;
using namespace std;

Context::~Context() {
  if (hyperspace && master_file_handle > 0) {
    hyperspace->close(master_file_handle);
    master_file_handle = 0;
  }
  delete balancer;
  delete reference_manager;
  delete m_balance_plan_authority;
}


void Context::notification_hook(int type, const String &message) {
  String type_str;
  switch (type) {
    case NotificationHookType::NOTICE:
      type_str = "NOTICE";
      break;
    case NotificationHookType::ERROR:
      type_str = "ERROR";
      break;
    default:
      type_str = "unknown type";
      break;
  };

  String cmd = format("%s/conf/notification-hook.sh '%s' '%s'", 
          System::install_dir.c_str(), type_str.c_str(), message.c_str());

  HT_DEBUG_OUT << "notification-hook (" << type_str << "): " << cmd << HT_END;

  int ret = ::system(cmd.c_str());
  HT_INFO_OUT << "notification-hook returned: " << ret << HT_END;
  if (ret != 0) {
    HT_WARNF("shell script conf/notification-hook.sh ('%s') returned "
            "error %d", cmd.c_str(), ret);
  }
}

void Context::set_balance_plan_authority(BalancePlanAuthority *bpa) {
  ScopedLock lock(mutex);
  m_balance_plan_authority = bpa;
}

BalancePlanAuthority *Context::get_balance_plan_authority() {
  ScopedLock lock(mutex);
  if (!m_balance_plan_authority)
    m_balance_plan_authority = new BalancePlanAuthority(this, mml_writer);
  return m_balance_plan_authority;
}

void Context::replay_complete(EventPtr &event) {
  int64_t id;
  uint32_t attempt, fragment;
  map<uint32_t, int> error_map;
  const uint8_t *decode_ptr = event->payload;
  size_t decode_remain = event->payload_len;
  int nn, error;
  bool success;

  id       = Serialization::decode_i64(&decode_ptr, &decode_remain);
  attempt  = Serialization::decode_i32(&decode_ptr, &decode_remain);
  nn       = Serialization::decode_i32(&decode_ptr, &decode_remain);

  HT_DEBUG_OUT << "Received replay_complete for op_id=" << id << " attempt="
      << attempt << " num_ranges=" << nn << " from " << event->proxy << HT_END;

  for (int ii = 0; ii < nn; ++ii) {
    fragment = Serialization::decode_i32(&decode_ptr, &decode_remain);
    error    = Serialization::decode_i32(&decode_ptr, &decode_remain);
    error_map[fragment] = error;
  }
  success  = Serialization::decode_bool(&decode_ptr, &decode_remain);

  {
    ScopedLock lock(m_recovery_state.m_mutex);
    RecoveryState::ReplayMap::iterator it = m_recovery_state.m_replay_map.find(id);
    RecoveryReplayCounterPtr replay_counter;
    if (it != m_recovery_state.m_replay_map.end()) {
      replay_counter = it->second;
      if (!replay_counter->complete(attempt, success, error_map)) {
        HT_WARN_OUT << "non-pending player complete message received for "
            "operation=" << id << " attempt=" << attempt << HT_END;
      }
    }
    else {
      HT_WARN_OUT << "No RecoveryReplayCounter found for operation="
          << id << " attempt=" << attempt << HT_END;
    }

    // if there were any corrupt fragments then notify the admin
    bool notify = replay_counter->has_errors();
    RecoveryReplayCounter::ErrorMap errors = replay_counter->get_errors();
    if (HT_FAILURE_SIGNALLED("bad-log-fragments-1")
        && !replay_counter->has_errors()) {
      errors[1] = Error::BAD_SCHEMA;
      errors[2] = Error::BLOCK_COMPRESSOR_DEFLATE_ERROR;
      notify = true;
    }
    if (notify) {
      String msg;
      msg = 
"Dear administrator,\\n"
"\\n"
"The recently started recovery operation reported failures when recovering\\n"
"log file fragments.  The following fragments are corrupt:\\n\\n";
      RecoveryReplayCounter::ErrorMap::iterator it;
      for (it = errors.begin(); it != errors.end(); ++it) {
        if (it->second != Error::OK) {
          msg += format("\t fragment id %u: error 0x%x (%s)\\n",
                  it->first, it->second, Error::get_text(it->second));
          HT_ERRORF("Corrupted logfile fragment %u: error 0x%x (%s)",
                  it->first, it->second, Error::get_text(it->second));
        }
      }
      msg += 
"\\n\\n"
"The log files on the Master and the RangeServer(s) will have more information\\n"
"about the error.\\n\\n";

      notification_hook(NotificationHookType::ERROR, msg);
    }
  }

  HT_DEBUG_OUT << "Exitting replay_complete for op_id=" << id << " attempt="
      << attempt << " num_ranges=" << nn << " from " << event->proxy << HT_END;
}

void Context::prepare_complete(EventPtr &event) {
  const uint8_t *decode_ptr = event->payload;
  size_t decode_remain = event->payload_len;

  int64_t id       = Serialization::decode_i64(&decode_ptr, &decode_remain);
  String location  = Serialization::decode_vstr(&decode_ptr, &decode_remain);
  int32_t error    = Serialization::decode_i32(&decode_ptr, &decode_remain);
  String error_msg = Serialization::decode_vstr(&decode_ptr, &decode_remain);

  HT_INFOF("prepare_complete(%lld, %s) = %s", (Lld)id, location.c_str(), Error::get_text(error));

  if (error != Error::OK) {
    HT_WARN_OUT << "prepare_complete(" << id << ", " << location << ") received from " << event->proxy
                << " reports error " << Error::get_text(error) << " (" << error_msg << ")" << HT_END;
    // TODO: Notify administrator
    return;
  }

  RecoveryStepFuturePtr future = m_recovery_state.get_prepare_future(id);

  if (future)
    future->success(event->proxy);
  else
    HT_WARN_OUT << "No Recovery prepare step future found for operation=" << id << HT_END;

  return;
}

void Context::commit_complete(EventPtr &event) {
  const uint8_t *decode_ptr = event->payload;
  size_t decode_remain = event->payload_len;

  int64_t id       = Serialization::decode_i64(&decode_ptr, &decode_remain);
  String location  = Serialization::decode_vstr(&decode_ptr, &decode_remain);
  int32_t error    = Serialization::decode_i32(&decode_ptr, &decode_remain);
  String error_msg = Serialization::decode_vstr(&decode_ptr, &decode_remain);

  if (error != Error::OK) {
    HT_WARN_OUT << "commit_complete(" << id << ", " << location << ") received from " << event->proxy
                << " reports error " << Error::get_text(error) << " (" << error_msg << ")" << HT_END;
    // TODO: Notify administrator
    return;
  }

  RecoveryStepFuturePtr future = m_recovery_state.get_commit_future(id);

  if (future)
    future->success(event->proxy);
  else
    HT_WARN_OUT << "No Recovery commit step future found for operation=" << id << HT_END;

  return;
}


bool Context::reassigned(TableIdentifier *table, RangeSpec &range, String &location) {
  // TBD
  return false;
}


bool Context::can_accept_ranges(const RangeServerStatistics &stats)
{
  static int threshold = 0;
  if (!threshold)
    threshold = props->get_i32("Hypertable.Master.DiskThreshold.Percentage");

  // system info was not yet initialized; assume that the disks are available
  if (!stats.stats) {
    HT_WARNF("RangeServer %s: no disk usage statistics available",
            stats.location.c_str());
    return true;
  }

  // accept new ranges if there's at least one disk below the threshold
  foreach_ht (const FsStat &fs, stats.stats->system.fs_stat) {
    if (fs.use_pct < threshold)
      return true;
  }
  HT_WARNF("RangeServer %s: all disks are above threshold of %d %% "
          "(Hypertable.Master.DiskThresholdPct); will not assign ranges",
          stats.location.c_str(), threshold);
  return false;
}

RecoveryReplayCounterPtr
Context::RecoveryState::create_replay_counter(int64_t id, uint32_t attempt)
{
  ScopedLock lock(m_mutex);

  RecoveryReplayCounterPtr counter = new RecoveryReplayCounter(attempt);

  if (m_replay_map.find(id) != m_replay_map.end())
    m_replay_map.erase(id);

  m_replay_map.insert(make_pair(id, counter));
  return counter;
}

void 
Context::RecoveryState::erase_replay_counter(int64_t id)
{
  ScopedLock lock(m_mutex);
  m_replay_map.erase(id);
}

void
Context::RecoveryState::install_prepare_future(int64_t id,
                                               RecoveryStepFuturePtr &future) {
  ScopedLock lock(m_mutex);
  HT_ASSERT(m_prepare_map.find(id) == m_prepare_map.end());
  m_prepare_map.insert(make_pair(id, future));    
}

RecoveryStepFuturePtr
Context::RecoveryState::get_prepare_future(int64_t id) {
  ScopedLock lock(m_mutex);
  RecoveryStepFuturePtr future;
  if (m_prepare_map.find(id) != m_prepare_map.end())
    future = m_prepare_map[id];
  return future;
}

void
Context::RecoveryState::erase_prepare_future(int64_t id) {
  ScopedLock lock(m_mutex);
  m_prepare_map.erase(id);
}


void
Context::RecoveryState::install_commit_future(int64_t id,
                                               RecoveryStepFuturePtr &future) {
  ScopedLock lock(m_mutex);
  HT_ASSERT(m_commit_map.find(id) == m_commit_map.end());
  m_commit_map.insert(make_pair(id, future));    
}

RecoveryStepFuturePtr
Context::RecoveryState::get_commit_future(int64_t id) {
  ScopedLock lock(m_mutex);
  RecoveryStepFuturePtr future;
  if (m_commit_map.find(id) != m_commit_map.end())
    future = m_commit_map[id];
  return future;
}

void
Context::RecoveryState::erase_commit_future(int64_t id) {
  ScopedLock lock(m_mutex);
  m_commit_map.erase(id);
}

