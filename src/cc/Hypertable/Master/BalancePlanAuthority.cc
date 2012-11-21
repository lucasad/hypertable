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
#include "Common/Serialization.h"
#include "Common/Mutex.h"

#include "Hypertable/Lib/CommitLogReader.h"

#include "BalancePlanAuthority.h"
#include "Context.h"
#include "EntityType.h"
#include "Utility.h"

using namespace Hypertable;

BalancePlanAuthority::BalancePlanAuthority(ContextPtr context,
        MetaLog::WriterPtr &mml_writer)
  : MetaLog::Entity(MetaLog::EntityType::BALANCE_PLAN_AUTHORITY),
    m_context(context), m_mml_writer(mml_writer), m_generation(0)
{
  HT_ASSERT(m_mml_writer);
  m_mml_writer->record_state(this);
}

BalancePlanAuthority::BalancePlanAuthority(ContextPtr context,
        MetaLog::WriterPtr &mml_writer, const MetaLog::EntityHeader &header)
  : MetaLog::Entity(header), m_context(context), m_mml_writer(mml_writer),
    m_generation(0)
{
}

void
BalancePlanAuthority::display(std::ostream &os)
{
  ScopedLock lock(m_mutex);
  os << " generation " << m_generation << " ";
  RecoveryPlanMap::iterator it = m_map.begin();
  while (it != m_map.end()) {
    os << it->first << ": ";
    for (int i = 0; i < 4; i++)
      if (it->second.plans[i])
        os << *(it->second.plans[i]) << " ";
    it++;
  }
}

size_t 
BalancePlanAuthority::encoded_length() const
{
  size_t len = 4 + 4;
  RecoveryPlanMap::const_iterator it = m_map.begin();
  while (it != m_map.end()) {
    len += Serialization::encoded_length_vstr(it->first);
    for (int i = 0; i < 4; i++) {
      len += 1;
      if (it->second.plans[i])
        len += it->second.plans[i]->encoded_length();
    }
    it++;
  }
  return len;
}

void 
BalancePlanAuthority::encode(uint8_t **bufp) const
{
  Serialization::encode_i32(bufp, m_generation);
  Serialization::encode_i32(bufp, m_map.size());
  RecoveryPlanMap::const_iterator it = m_map.begin();
  while (it != m_map.end()) {
    Serialization::encode_vstr(bufp, it->first);
    for (int i = 0; i < 4; i++) {
      if (it->second.plans[i]) {
        Serialization::encode_bool(bufp, true);
        it->second.plans[i]->encode(bufp);
      }
      else
        Serialization::encode_bool(bufp, false);
    }
    it++;
  }
}

void 
BalancePlanAuthority::decode(const uint8_t **bufp, size_t *remainp)
{
  ScopedLock lock(m_mutex);
  m_generation = Serialization::decode_i32(bufp, remainp);

  int num_servers = Serialization::decode_i32(bufp, remainp);
  for (int i = 0; i < num_servers; i++) {
    String rs = Serialization::decode_vstr(bufp, remainp);
    RecoveryPlans plans;
    for (int j = 0; j < 4; j++) {
      bool b = Serialization::decode_bool(bufp, remainp);
      if (!b) {
        plans.plans[j] = 0;
        continue;
      }
      plans.plans[j] = new RangeRecoveryPlan();
      plans.plans[j]->decode(bufp, remainp);
    }
    m_map[rs] = plans;
  }
}

void
BalancePlanAuthority::set_mml_writer(MetaLog::WriterPtr &mml_writer)
{
  ScopedLock lock(m_mutex);
  m_mml_writer = mml_writer;
}

void
BalancePlanAuthority::copy_recovery_plan(const String &location, int type,
        RangeRecoveryPlan &out, int &generation)
{
  ScopedLock lock(m_mutex);
  HT_ASSERT(m_map.find(location) != m_map.end());
  RangeRecoveryPlanPtr plan = m_map[location].plans[type];

  out.clear();
  if (plan) {
    HT_ASSERT(plan->type == type);
    out.replay_plan = plan->replay_plan;
    plan->receiver_plan.copy(out.receiver_plan);
  }
  out.type = type;

  generation = m_generation;
}

void
BalancePlanAuthority::remove_recovery_plan(const String &location)
{
  ScopedLock lock(m_mutex);
  RecoveryPlanMap::iterator it = m_map.find(location);
  if (it == m_map.end())
     return;
  m_map.erase(it);
  m_mml_writer->record_state(this);
}

void BalancePlanAuthority::remove_from_receiver_plan(const String &location, int type,
                                                     const vector<QualifiedRangeSpec> &specs) {
  ScopedLock lock(m_mutex);
  HT_ASSERT(m_map.find(location) != m_map.end());
  RangeRecoveryPlanPtr plan = m_map[location].plans[type];
  HT_ASSERT(plan && plan->type == type);

  foreach_ht (const QualifiedRangeSpec &spec, specs)
    plan->receiver_plan.remove(spec);

  m_mml_writer->record_state(this);
}

void BalancePlanAuthority::remove_from_replay_plan(const String &recovery_location, int type,
                                                   const String &replay_location) {

  ScopedLock lock(m_mutex);
  HT_ASSERT(m_map.find(recovery_location) != m_map.end());
  RangeRecoveryPlanPtr plan = m_map[recovery_location].plans[type];
  HT_ASSERT(plan && plan->type == type);

  plan->replay_plan.remove_location(replay_location);

  m_mml_writer->record_state(this);  
}


bool
BalancePlanAuthority::is_empty()
{
  return (m_map.empty());
}

void
BalancePlanAuthority::create_recovery_plan(const String &location,
                              const vector<QualifiedRangeSpec> &root_specs,
                              const vector<RangeState> &root_states,
                              const vector<QualifiedRangeSpec> &metadata_specs,
                              const vector<RangeState> &metadata_states,
                              const vector<QualifiedRangeSpec> &system_specs,
                              const vector<RangeState> &system_states,
                              const vector<QualifiedRangeSpec> &user_specs,
                              const vector<RangeState> &user_states)
{
  ScopedLock lock(m_mutex);

  HT_INFO_OUT << "Creating recovery plan for " << location << HT_END;

  // check if this RangeServer was already recovered; if yes then do not add
  // a new plan (this happens i.e. in rangeserver-failover-master-16)
  if (m_map.find(location) != m_map.end())
    return;

  // walk through the existing plans and move all ranges from that server
  // to other servers
  RecoveryPlanMap::iterator it;
  for (it = m_map.begin(); it != m_map.end(); ++it) {
    if (it->second.plans[RangeSpec::ROOT])
      update_range_plan(it->second.plans[RangeSpec::ROOT], location, root_specs);
    if (it->second.plans[RangeSpec::METADATA])
      update_range_plan(it->second.plans[RangeSpec::METADATA], location, metadata_specs);
    if (it->second.plans[RangeSpec::SYSTEM])
      update_range_plan(it->second.plans[RangeSpec::SYSTEM], location, system_specs);
    if (it->second.plans[RangeSpec::USER])
      update_range_plan(it->second.plans[RangeSpec::USER], location, user_specs);
  }

  // then create the new plan for the failed server
  RecoveryPlans plans;
  plans.plans[RangeSpec::ROOT]
    = create_range_plan(location, RangeSpec::ROOT, root_specs, root_states);
  plans.plans[RangeSpec::METADATA]
    = create_range_plan(location, RangeSpec::METADATA, metadata_specs, metadata_states);
  plans.plans[RangeSpec::SYSTEM]
    = create_range_plan(location, RangeSpec::SYSTEM, system_specs, system_states);
  plans.plans[RangeSpec::USER]
    = create_range_plan(location, RangeSpec::USER, user_specs, user_states);

  // Remove ALL ranges that are FROM the failing node or are about
  // to be moved TO the failing node. These ranges are already part
  // of the balance plan anyway.
  // TODO really? verify this
  MoveSetT::iterator iter = m_current_set.begin();
  while (iter != m_current_set.end()) {
    if (location == (*iter)->dest_location) {
      HT_INFO_OUT << "XXX " << location << ": " << *iter << HT_END;
      MoveSetT::iterator iter2 = iter;
      m_current_set.erase(iter2);
    }
    ++iter;
  }

  // store the new plan
  m_generation++;
  m_map[location] = plans;

  /**
   * Put the RangeServerConnection object into the 'removed' state and then
   * atomically persist both the RangeServerConnection object and the plan to
   * the MML
   */
  {
    vector<MetaLog::Entity *> entities;
    RangeServerConnectionPtr rsc;
    if (!m_context->rsc_manager->find_server_by_location(location, rsc))
      HT_FATALF("Unable to location RangeServerConnection object for %s", location.c_str());
    rsc->set_removed();
    entities.push_back(this);
    entities.push_back(rsc.get());
    m_mml_writer->record_state(entities);
  }

  lock.unlock(); // otherwise operator<< will deadlock
  HT_INFO_OUT << "Global recovery plan was modified: " << *this << HT_END;
}

RangeRecoveryPlan * 
BalancePlanAuthority::create_range_plan(const String &location, int type,
                                        const vector<QualifiedRangeSpec> &specs,
                                        const vector<RangeState> &states)
{
  if (specs.empty())
    return 0;

  const char *type_strings[] = { "root", "metadata", "system", "user", "UNKNOWN" };

  StringSet active_locations;
  m_context->rsc_manager->get_connected_servers(active_locations);

  vector<uint32_t> fragments;

  RangeRecoveryPlan *plan = new RangeRecoveryPlan(type);

  // read the fragments from the commit log
  String log_dir = m_context->toplevel_dir + "/servers/" + location
      + "/log/" + type_strings[type];
  CommitLogReader clr(m_context->dfs, log_dir);
  clr.get_init_fragment_ids(fragments);

  StringSet::const_iterator location_it = active_locations.begin();

  HT_ASSERT(specs.size() == states.size());

  // round robin through the locations and assign the ranges
  for (size_t i=0; i<specs.size(); i++) {
    if (location_it == active_locations.end())
      location_it = active_locations.begin();
    plan->receiver_plan.insert(*location_it, specs[i], states[i]);
    ++location_it;
  }

  location_it = active_locations.begin();
  // round robin through the locations and assign the fragments
  foreach_ht (uint32_t fragment, fragments) {
    if (location_it == active_locations.end())
      location_it = active_locations.begin();
    plan->replay_plan.insert(fragment, *location_it);
    ++location_it;
  }

  return plan;
}


void
BalancePlanAuthority::update_range_plan(RangeRecoveryPlanPtr &plan,
                                        const String &location,
                                        const vector<QualifiedRangeSpec> &new_specs)
{ 
  StringSet active_locations;
  m_context->rsc_manager->get_connected_servers(active_locations);

  HT_ASSERT(active_locations.find(location) == active_locations.end());

  StringSet::const_iterator location_it = active_locations.begin();

  vector<uint32_t> fragments;
  plan->replay_plan.get_fragments(location.c_str(), fragments);
  // round robin through the locations and assign the fragments
  foreach_ht (uint32_t fragment, fragments) {
    if (location_it == active_locations.end())
      location_it = active_locations.begin();
    plan->replay_plan.insert(fragment, *location_it);
    ++location_it;
  }

  std::set<QualifiedRangeSpec> purge_ranges;
  foreach_ht (const QualifiedRangeSpec spec, new_specs)
    purge_ranges.insert(spec);

  location_it = active_locations.begin();
  vector<QualifiedRangeSpec> specs;
  vector<RangeState> states;
  plan->receiver_plan.get_range_specs_and_states(location, specs, states);
  // round robin through the locations and assign the ranges
  for (size_t i=0; i<specs.size(); i++) {
    if (purge_ranges.count(specs[i]) > 0)
      plan->receiver_plan.remove(specs[i]);
    else {
      if (location_it == active_locations.end())
        location_it = active_locations.begin();
      plan->receiver_plan.insert(*location_it, specs[i], states[i]);
      ++location_it;
    }
  }

  if (plan->receiver_plan.empty())
    plan = 0;
}

bool
BalancePlanAuthority::register_balance_plan(BalancePlanPtr &plan, int generation,
                                            std::vector<Entity *> &entities) {
  ScopedLock lock(m_mutex);

  if (generation != m_generation)
    return false;

  // Insert moves into current set
  foreach_ht (RangeMoveSpecPtr &move, plan->moves)
    m_current_set.insert(move);

  entities.push_back(this);
  m_mml_writer->record_state(entities);

  HT_INFO_OUT << "Balance plan registered move " << plan->moves.size()
        << " ranges" << ", BalancePlan = " << *plan<< HT_END;

  return true;
}

bool
BalancePlanAuthority::get_balance_destination(const TableIdentifier &table,
                  const RangeSpec &range, String &location) {
  ScopedLock lock(m_mutex);

  RangeMoveSpecPtr move_spec = new RangeMoveSpec();

  move_spec->table = table;
  move_spec->table.generation = 0;
  move_spec->range = range;

  MoveSetT::iterator iter;

  if ((iter = m_current_set.find(move_spec)) != m_current_set.end())
    location = (*iter)->dest_location;
  else {
    if (!Utility::next_available_server(m_context, location))
      return false;
    move_spec->dest_location = location;
    m_current_set.insert(move_spec);
    m_mml_writer->record_state(this);
  }

  return true;
}

bool
BalancePlanAuthority::balance_move_complete(const TableIdentifier &table,
                const RangeSpec &range, int32_t error) {
  ScopedLock lock(m_mutex);
  RangeMoveSpecPtr move_spec = new RangeMoveSpec();

  HT_INFO_OUT << "balance_move_complete for " << table << " " << range << HT_END;

  move_spec->table = table;
  move_spec->table.generation = 0;
  move_spec->range = range;

  MoveSetT::iterator iter;

  if ((iter = m_current_set.find(move_spec)) != m_current_set.end()) {
    // the 'complete' and 'error' fields currently are not in use
    (*iter)->complete = true;
    (*iter)->error = error;
    m_current_set.erase(iter);
  }

  return true;
}

