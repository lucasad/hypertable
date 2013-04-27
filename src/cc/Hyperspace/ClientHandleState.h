/*
 * Copyright (C) 2007-2012 Hypertable, Inc.
 *
 * This file is part of Hypertable.
 *
 * Hypertable is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 3
 * of the License, or any later version.
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

#ifndef HYPERSPACE_CLIENTHANDLESTATE_H
#define HYPERSPACE_CLIENTHANDLESTATE_H

#include <string>

#include <boost/thread/condition.hpp>

#include "Common/ReferenceCount.h"
#include "Common/Mutex.h"

#include "HandleCallback.h"
#include "LockSequencer.h"

namespace Hyperspace {

  class ClientHandleState : public Hypertable::ReferenceCount {
  public:
    uint64_t     handle;
    uint32_t     open_flags;
    uint32_t     event_mask;
    std::string  normal_name;
    HandleCallbackPtr callback;
    LockSequencer *sequencer;
    int lock_status;
    uint32_t lock_mode;
    uint64_t lock_generation;
    Mutex              mutex;
    boost::condition   cond;
  };
  typedef boost::intrusive_ptr<ClientHandleState> ClientHandleStatePtr;

}

#endif // HYPERSPACE_CLIENTHANDLESTATE_H
