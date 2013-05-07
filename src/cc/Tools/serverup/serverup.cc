/**
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

#include "Common/Compat.h"
#include <cstdlib>
#include <iostream>
#include <map>

#include <boost/algorithm/string.hpp>

extern "C" {
#include <netdb.h>
#include <sys/types.h>
#include <signal.h>
}

#include "Common/Config.h"
#include "Common/Error.h"
#include "Common/InetAddr.h"
#include "Common/Logger.h"
#include "Common/Init.h"
#include "Common/Timer.h"
#include "Common/Usage.h"

#include "AsyncComm/ApplicationQueue.h"
#include "AsyncComm/Comm.h"
#include "AsyncComm/ConnectionManager.h"
#include "AsyncComm/ReactorFactory.h"

#include "DfsBroker/Lib/Client.h"

#include "Hyperspace/Session.h"
#include "Hypertable/Lib/Config.h"
#include "Hypertable/Lib/MasterClient.h"
#include "Hypertable/Lib/RangeServerClient.h"
#include "Hypertable/Lib/ReplicationMasterProtocol.h"
#include "Hypertable/Lib/ReplicationSlaveProtocol.h"
#include "Hypertable/Lib/Types.h"

#ifdef HT_WITH_THRIFT
# include "ThriftBroker/Config.h"
# include "ThriftBroker/Client.h"
#endif

#include "serverup.h"

using namespace Hypertable;
using namespace Config;
using namespace std;

namespace {

  const char *usage =
    "Usage: serverup [options] <server-name>\n\n"
    "Description:\n"
    "  This program checks to see if the server, specified by <server-name>\n"
    "  is up. return 0 if true, 1 otherwise.\n"
    "  Run `serverup list` to retrieve a list of all supported server names.\n"
    "Options";

  struct AppPolicy : Config::Policy {
    static void init_options() {
      cmdline_desc(usage).add_options()
          ("wait", i32()->default_value(2000), "Check wait time in ms")
          ("host", str(), "Specifies the hostname of the server(s)")
          ("display-address", boo()->default_value(false),
           "Displays hostname and port of the server(s), then exits")
          ;
      cmdline_hidden_desc().add_options()("server-name", str(), "");
      cmdline_positional_desc().add("server-name", -1);
    }
    static void init() {
      // we want to override the default behavior that verbose
      // turns on debugging by clearing the defaulted flag
      if (defaulted("logging-level"))
        properties->set("logging-level", String("fatal"));
    }
  };

#ifdef HT_WITH_THRIFT
  typedef Meta::list<AppPolicy, DfsClientPolicy, HyperspaceClientPolicy,
          MasterClientPolicy, RangeServerClientPolicy, ThriftClientPolicy,
          DefaultCommPolicy> Policies;
#else
  typedef Meta::list<AppPolicy, DfsClientPolicy, HyperspaceClientPolicy,
          MasterClientPolicy, RangeServerClientPolicy, DefaultCommPolicy>
          Policies;
#endif

  void check_dfsbroker(ConnectionManagerPtr &conn_mgr, uint32_t wait_ms) {
    HT_DEBUG_OUT << "Checking dfsbroker at " << get_str("dfs-host")
        << ':' << get_i16("dfs-port") << HT_END;

    if (properties->has("host")) {
      properties->set("DfsBroker.Host", properties->get_str("host"));
      properties->set("dfs-host", properties->get_str("host"));
    }

    if (get_bool("display-address")) {
      std::cout << get_str("dfs-host") << ":" << get_i16("dfs-port")
          << std::endl;
      _exit(0);
    }

    DfsBroker::ClientPtr dfs = new DfsBroker::Client(conn_mgr, properties);

    if (!dfs->wait_for_connection(wait_ms))
      HT_THROW(Error::REQUEST_TIMEOUT, "connecting to dfsbroker");

    HT_TRY("getting dfsbroker status", dfs->status());

  }

  Hyperspace::Session *hyperspace;

  void check_hyperspace(ConnectionManagerPtr &conn_mgr, uint32_t max_wait_ms) {
    HT_DEBUG_OUT << "Checking hyperspace"<< HT_END;
    Timer timer(max_wait_ms, true);
    int error;

    String host = "localhost";
    if (properties->has("host")) {
      host = properties->get_str("host");
      std::vector<String> vec;
      vec.push_back(host);
      properties->set("Hyperspace.Replica.Host", vec);
    }

    if (get_bool("display-address")) {
      std::cout << host << ":" <<
          properties->get_i16("Hyperspace.Replica.Port") << std::endl;
      _exit(0);
    }

    hyperspace = new Hyperspace::Session(conn_mgr->get_comm(), properties);

    if (!hyperspace->wait_for_connection(max_wait_ms))
      HT_THROW(Error::REQUEST_TIMEOUT, "connecting to hyperspace");

    if ((error = hyperspace->status(&timer)) != Error::OK &&
      error != Error::HYPERSPACE_NOT_MASTER_LOCATION) {
      HT_THROW(error, "getting hyperspace status");
    }
  }

  void check_global_master(ConnectionManagerPtr &conn_mgr, uint32_t wait_ms) {
    HT_DEBUG_OUT << "Checking master via hyperspace" << HT_END;
    Timer timer(wait_ms, true);

    if (get_bool("display-address")) {
      std::cout << get_str("Hypertable.Master.Host") << ":" <<
          get_i16("Hypertable.Master.Port") << std::endl;
      _exit(0);
    }

    if (!hyperspace) {
      hyperspace = new Hyperspace::Session(conn_mgr->get_comm(), properties);
      if (!hyperspace->wait_for_connection(wait_ms))
        HT_THROW(Error::REQUEST_TIMEOUT, "connecting to hyperspace");
    }

    ApplicationQueueInterfacePtr app_queue = new ApplicationQueue(1);
    Hyperspace::SessionPtr hyperspace_ptr = hyperspace;

    String toplevel_dir = properties->get_str("Hypertable.Directory");
    boost::trim_if(toplevel_dir, boost::is_any_of("/"));
    toplevel_dir = String("/") + toplevel_dir;

    MasterClient *master = new MasterClient(conn_mgr, hyperspace_ptr,
                                            toplevel_dir, wait_ms, app_queue);

    if (!master->wait_for_connection(wait_ms))
      HT_THROW(Error::REQUEST_TIMEOUT, "connecting to master");

    master->status(&timer);
  }

  void check_master(ConnectionManagerPtr &conn_mgr, uint32_t wait_ms) {
    Timer timer(wait_ms, true);
    uint16_t port = properties->get_i16("Hypertable.Master.Port");

    const char *host;
    if (properties->has("host"))
      host = properties->get_str("host").c_str();
    else
      host = "localhost";

    if (get_bool("display-address")) {
      std::cout << host << ":" << port << std::endl;
      _exit(0);
    }

    HT_DEBUG_OUT << "Checking master on " << host << ":" << port << HT_END;
    InetAddr addr(host, port);

    // issue 816: try to connect via MasterClient. If it refuses, and if
    // the host name is localhost then check if there's a
    // Hypertable.Master.state file and verify the pid
    MasterClient *master = new MasterClient(conn_mgr, addr, wait_ms);

    if (!master->wait_for_connection(wait_ms)) {
      if (strcmp(host, "localhost") && strcmp(host, "127.0.0.1"))
        HT_THROW(Error::REQUEST_TIMEOUT, "connecting to master");

      String state_file = System::install_dir + "/run/Hypertable.Master.state";
      if (!FileUtils::exists(state_file))
        HT_THROW(Error::REQUEST_TIMEOUT, "connecting to master");

      String pidstr;
      String pid_file = System::install_dir + "/run/Hypertable.Master.pid";
      if (FileUtils::read(pid_file, pidstr) <= 0)
        HT_THROW(Error::REQUEST_TIMEOUT, "connecting to master");
      pid_t pid = (pid_t)strtoul(pidstr.c_str(), 0, 0);
      if (pid <= 0)
        HT_THROW(Error::REQUEST_TIMEOUT, "connecting to master");
      // (kill(pid, 0) does not send any signal but checks if the process exists
      if (::kill(pid, 0) < 0)
        HT_THROW(Error::REQUEST_TIMEOUT, "connecting to master");
    }
    else
      master->status(&timer);
  }

  void check_rangeserver(ConnectionManagerPtr &conn_mgr, uint32_t wait_ms) {
    HT_DEBUG_OUT <<"Checking rangeserver at "<< get_str("rs-host")
                 <<':'<< get_i16("rs-port") << HT_END;

    if (properties->has("host"))
      properties->set("rs-host", properties->get_str("host"));

    if (get_bool("display-address")) {
      std::cout << get_str("rs-host") << ":" << get_i16("rs-port") << std::endl;
      _exit(0);
    }

    InetAddr addr(get_str("rs-host"), get_i16("rs-port"));

    wait_for_connection("range server", conn_mgr, addr, wait_ms, wait_ms);

    RangeServerClient *range_server =
      new RangeServerClient(conn_mgr->get_comm(), wait_ms);
    Timer timer(wait_ms, true);
    range_server->status(addr, timer);
  }

  void check_thriftbroker(ConnectionManagerPtr &conn_mgr, int wait_ms) {
#ifdef HT_WITH_THRIFT
    if (properties->has("host"))
      properties->set("thrift-host", properties->get_str("host"));

    if (get_bool("display-address")) {
      std::cout << get_str("thrift-host") << ":" << get_i16("thrift-port")
          << std::endl;
      _exit(0);
    }

    String table_id;
    InetAddr addr(get_str("thrift-host"), get_i16("thrift-port"));

    try {
      Thrift::Client client(get_str("thrift-host"), get_i16("thrift-port"));
      ThriftGen::Namespace ns = client.open_namespace("sys");
      client.get_table_id(table_id, ns, "METADATA");
    }
    catch (ThriftGen::ClientException &e) {
      HT_THROW(e.code, e.message);
    }
    catch (std::exception &e) {
      HT_THROW(Error::EXTERNAL, e.what());
    }
    HT_EXPECT(table_id == TableIdentifier::METADATA_ID, Error::INVALID_METADATA);
#else
    HT_THROW(Error::FAILED_EXPECTATION, "Thrift support not installed");
#endif
  }

  void
  check_repmaster(ConnectionManagerPtr &conn_mgr, uint32_t wait_ms) {
    if (properties->has("host"))
      properties->set("repmaster-host",
                      properties->get_str("host"));
    else
      properties->set("repmaster-host",
                      String("localhost"));

    if (properties->has("Hypertable.Replication.Master.Port"))
      properties->set("repmaster-port",
                      properties->get_i16("Hypertable.Replication.Master.Port"));
    else
      properties->set("repmaster-port",
                      properties->get_i16("Hypertable.Replication.Master.Port"));

    if (get_bool("display-address")) {
      std::cout << get_str("repmaster-host") << ":"
                << get_i16("repmaster-port") << std::endl;
      _exit(0);
    }

    InetAddr addr(get_str("repmaster-host"), get_i16("repmaster-port"));

    conn_mgr->add(addr, wait_ms, "Replication.Master");

    DispatchHandlerSynchronizer sync_handler;
    EventPtr event;
    CommBufPtr cbp(ReplicationMasterProtocol::create_status_request());

    int error;
    if (!(error = Comm::instance()->send_request(addr, wait_ms, cbp,
                                                 &sync_handler))) {
      if (sync_handler.wait_for_reply(event))
        return;
    }

    // if connection times out: check if the process is running; this might
    // be a second master waiting to get its hyperspace lock
    String pidstr;
    String pid_file = System::install_dir + "/run/Replication.Master.pid";
    if (FileUtils::read(pid_file, pidstr) <= 0)
      HT_THROW(Error::REQUEST_TIMEOUT, "connecting to master");
    pid_t pid = (pid_t)strtoul(pidstr.c_str(), 0, 0);
    if (pid <= 0)
      HT_THROW(Error::REQUEST_TIMEOUT, "connecting to master");
    // (kill(pid, 0) does not send any signal but checks if the process exists
    if (::kill(pid, 0) < 0)
      HT_THROW(Error::REQUEST_TIMEOUT, "connecting to master");
  }

  void
  check_repslave(ConnectionManagerPtr &conn_mgr, uint32_t wait_ms) {
    if (properties->has("host"))
      properties->set("repslave-host", properties->get_str("host"));
    else
      properties->set("repslave-host", String("localhost"));

    if (properties->has("Hypertable.Replication.Slave.Port"))
      properties->set("repslave-port",
                      properties->get_i16("Hypertable.Replication.Slave.Port"));
    else
      properties->set("repslave-port",
                      properties->get_i16("Hypertable.Replication.Slave.Port"));

    if (get_bool("display-address")) {
      std::cout << get_str("repslave-host") << ":"
                << get_i16("repslave-port") << std::endl;
      _exit(0);
    }

    InetAddr addr(get_str("repslave-host"), get_i16("repslave-port"));

    wait_for_connection("replication slave", conn_mgr, addr, wait_ms, wait_ms);

    DispatchHandlerSynchronizer sync_handler;
    EventPtr event;
    CommBufPtr cbp(ReplicationSlaveProtocol::create_status_request());

    int error;
    if ((error = Comm::instance()->send_request(addr, wait_ms, cbp,
                                                &sync_handler)))
      HT_THROW(error, String("Comm::send_request failure: ")
               + Error::get_text(error));

    if (!sync_handler.wait_for_reply(event))
      HT_THROW((int)Protocol::response_code(event),
               String("Replication.Slave status() failure: ")
               + Protocol::string_format_message(event));
  }

} // local namespace

namespace Hypertable {

  void
  wait_for_connection(const char *server, ConnectionManagerPtr &conn_mgr,
                      InetAddr &addr, int timeout_ms, int wait_ms) {
    HT_DEBUG_OUT << "Checking " << server << " at " << addr << HT_END;

    conn_mgr->add(addr, timeout_ms, server);

    if (!conn_mgr->wait_for_connection(addr, wait_ms))
      HT_THROWF(Error::REQUEST_TIMEOUT, "connecting to %s", server);
  }

} // namespace Hypertable

Hypertable::CheckerMap Hypertable::global_map;
bool Hypertable::verbose = false;

static bool
is_down(const String &name, ConnectionManagerPtr &conn_mgr, uint32_t wait_ms) {
  if (global_map.find(name) == global_map.end()) {
    cout << name << " - unknown service" << endl;
    return true;
  }

  try {
    global_map[name](conn_mgr, wait_ms);
  }
  catch (Exception &e) {
    if (verbose) {
      HT_DEBUG_OUT << e << HT_END;
      cout << name << " - down" << endl;
    }
    return true;
  }
  if (verbose)
    cout << name << " - up" << endl;
  return false;
}

int main(int argc, char **argv) {
  bool down = true;
  try {
    init_with_policies<Policies>(argc, argv);

    bool silent = get_bool("silent");
    uint32_t wait_ms = get_i32("wait");
    String server_name = get("server-name", String());
    verbose = get("verbose", false);

    global_map["dfsbroker"] = &check_dfsbroker;
    global_map["hyperspace"] = &check_hyperspace;
    global_map["global-master"] = &check_global_master;
    global_map["global_master"] = &check_global_master;
    global_map["master"] = &check_master;
    global_map["rangeserver"] = &check_rangeserver;
    global_map["thriftbroker"] = &check_thriftbroker;
    global_map["Replication.Master"] = &check_repmaster;
    global_map["repmaster"] = &check_repmaster;
    global_map["Replication.Slave"] = &check_repslave;
    global_map["repslave"] = &check_repslave;

    if (server_name == "list") {
      for (CheckerMap::iterator it = global_map.begin();
              it != global_map.end(); ++it) {
        cout << it->first << endl;
      }
      _exit(0);
    }

    ConnectionManagerPtr conn_mgr = new ConnectionManager();
    conn_mgr->set_quiet_mode(silent);

    properties->set("DfsBroker.Timeout", (int32_t)wait_ms);

    if (!server_name.empty()) {
      down = is_down(server_name, conn_mgr, wait_ms);
    }
    else {
      down = is_down("dfsbroker", conn_mgr, wait_ms)
            | is_down("hyperspace", conn_mgr, wait_ms)
            | is_down("global_master", conn_mgr, wait_ms)
            | is_down("rangeserver", conn_mgr, wait_ms)
#ifdef HT_WITH_THRIFT
            | is_down("thriftbroker", conn_mgr, wait_ms)
#endif
            ;
    }

    if (!silent)
      cout << (down ? "false" : "true") << endl;
  }
  catch (Exception &e) {
    HT_ERROR_OUT << e << HT_END;
    _exit(1);    // don't bother with global/static objects
  }

  _exit(down ? 1 : 0);   // ditto
}
