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

#ifndef HYPERSPACE_REQUESTHANDLERLOCK_H
#define HYPERSPACE_REQUESTHANDLERLOCK_H

#include "AsyncComm/ApplicationHandler.h"
#include "AsyncComm/Comm.h"
#include "AsyncComm/Event.h"


namespace Hyperspace {

  class Master;

  class RequestHandlerLock : public ApplicationHandler {
  public:
    RequestHandlerLock(Comm *comm, Master *master, uint64_t session_id,
                       EventPtr &event_ptr)
      : ApplicationHandler(event_ptr), m_comm(comm), m_master(master),
        m_session_id(session_id) { }

    virtual void run();

  private:
    Comm        *m_comm;
    Master      *m_master;
    uint64_t     m_session_id;
  };
}

#endif // HYPERSPACE_REQUESTHANDLERLOCK_H
