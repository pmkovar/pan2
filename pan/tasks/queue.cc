/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 * Pan - A Newsreader for Gtk+
 * Copyright (C) 2002-2006  Charles Kerr <charles@rebelbase.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <config.h>
#include <pan/general/debug.h>
#include <pan/general/foreach.h>
#include <pan/general/messages.h>
#include <pan/data/server-info.h>
#include "queue.h"
#include "task.h"

/***
****
***/

using namespace pan;

Queue :: Queue (ServerInfo         & server_info,
                TaskArchive        & archive,
                Socket::Creator    * socket_creator, 
                bool                 online):
  _server_info (server_info),
  _is_online (online),
  _socket_creator (socket_creator),
  _needs_saving (false),
  _last_time_saved (0),
  _archive (archive)
{
  tasks_t tasks;
  _archive.load_tasks (tasks);
  add_tasks (tasks, BOTTOM);

  _tasks.add_listener (this);
}

Queue :: ~Queue ()
{
  _tasks.remove_listener (this);

  foreach (pools_t, _pools, it)
    delete it->second;
  _pools.clear ();

  foreach (TaskSet, _tasks, it)
    delete *it;
}

void
Queue :: on_pool_has_nntp_available (const Quark& server)
{
  Task * task = find_first_task_needing_server (server);
  if (task != 0)
    process_task (task);
}

void
Queue :: on_pool_error (const Quark& server, const StringView& message)
{
  fire_queue_error (message);
}

NNTP_Pool&
Queue :: get_pool (const Quark& servername)
{
  NNTP_Pool * pool (0);

  pools_t::iterator it (_pools.find (servername));
  if (it != _pools.end())
  {
    pool = it->second;
  }
  else // have to build one
  {
    pool = new NNTP_Pool (servername, _server_info, _socket_creator);
    pool->add_listener (this);
    _pools[servername] = pool;
  }

  return *pool;
}

/***
****
***/

void
Queue :: upkeep ()
{
  // maybe save the task list.
  // only do it once in awhile to prevent thrashing.
  const tasks_t tmp (_tasks.begin(), _tasks.end());
  const time_t now (time(0));
  if (_needs_saving && _last_time_saved<(now-10)) {
    _archive.save_tasks (tmp);
    _needs_saving = false;
    _last_time_saved = now;
  }

  // remove completed tasks.
  foreach_const (tasks_t, tmp, it) {
    Task * task  (*it);
    const Task::State& state (task->get_state());
    if (state._work==Task::COMPLETED || _removing.count(task))
      remove_task (task);
  }

  // upkeep on running tasks... this lets us pop open
  // extra connections if the task can handle >1 connection
  std::set<Task*> active; 
  foreach (nntp_to_task_t, _nntp_to_task, it)
    active.insert (it->second);
  foreach (std::set<Task*>, active, it)
    process_task (*it);

  // idle socket upkeep
  foreach (pools_t, _pools, it)
    it->second->idle_upkeep ();

  // maybe fire counts changed events...
  fire_if_counts_have_changed ();
}

void
Queue :: fire_if_counts_have_changed ()
{
  // if our number of connections has changed, fire an event...
  static int previous_count (-1);
  int count (0);
  foreach_const (pools_t, _pools, it) {
    int active, idle, pending, max;
    it->second->get_counts (active, idle, pending, max);
    count += (active + idle + pending);
  }
  const bool count_changed (previous_count != count);
  previous_count = count;
  if (count_changed)
    fire_connection_count_changed (count);

  // if our number of tasks has changed, fire an event...
  static int prev_active (-1);
  static int prev_total (-1);
  int active, total;
  get_task_counts (active, total);
  const bool counts_changed (active!=prev_active || total!=prev_total);
  prev_active = active;
  prev_total = total;
  if (counts_changed)
    fire_size_changed (active, total);
}

void
Queue :: get_task_counts (int& active, int& total)
{
  std::set<Task*> active_tasks;
  foreach_const (nntp_to_task_t, _nntp_to_task, it)
    active_tasks.insert (it->second);
  active = active_tasks.size ();
  total = _tasks.size ();
}


void
Queue :: give_task_a_connection (Task * task, NNTP * nntp)
{
  const bool was_active (task_is_active (task));
  _nntp_to_task[nntp] = task; // it's active now...
  if (!was_active)
    fire_task_active_changed (task, true);
  nntp->_socket->reset_speed_counter ();
  task->give_nntp (this, nntp);
}


void
Queue :: process_task (Task * task)
{
  pan_return_if_fail (task!=0);

  debug ("in process_task with a task of type " << task->get_type());

  const Task::State& state (task->get_state());

  if (state._work == Task::COMPLETED)
  {
    debug ("completed");
    remove_task (task);
  }
  else if (_removing.count(task))
  {
    debug ("removing");
    remove_task (task);
  }
  else if (_stopped.count(task))
  {
    debug ("stopped");
    // do nothing
  }
  else if (state._health == FAIL)
  {
    debug ("fail");
    // do nothing
  }
  else if (state._work == Task::WORKING)
  {
    debug ("working");
    // do nothing
  }
  else while (state._work == Task::NEED_NNTP)
  {
    // make the requests...
    const Task::State::unique_servers_t& servers (state._servers);
    foreach_const (Task::State::unique_servers_t, servers, it)
      get_pool(*it).request_nntp ();

    Quark server;
    if (!find_best_server (servers, server))
      break;

    NNTP * nntp (get_pool(server).check_out ());
    if (!nntp)
      break;

    give_task_a_connection (task, nntp);
  }
}

/***
****
***/

Task*
Queue :: find_first_task_needing_server (const Quark& server)
{
  Task * retval (0);

  foreach (TaskSet, _tasks, it)
  {
    const Task::State& state ((*it)->get_state ());
    if  ((state._health != FAIL)
      && (state._work == Task::NEED_NNTP)
      && (state._servers.count(server))
      && (!_stopped.count (*it))
      && (!_removing.count (*it)))
    {
      retval = *it;
      break;
    }
  }

  return retval;
}

bool
Queue :: find_best_server (const Task::State::unique_servers_t& servers, Quark& setme)
{
  int max_score (0);
  Quark best_server;

  foreach_const (Task::State::unique_servers_t, servers, it)
  {
    const Quark& server (*it);
    const NNTP_Pool& pool (get_pool(server));

    int score (0);
    if (_is_online) {
      int active, idle, pending, max;
      pool.get_counts (active, idle, pending, max);
      const int empty_slots (max - (idle+active));
      score = idle*10 + empty_slots;
    }

    if (score > max_score) {
      max_score = score;
      best_server = server;
    }
  }

  if (max_score)
    setme = best_server;

  return max_score!=0;
}

/***
****
***/

void
Queue :: on_set_items_added (TaskSet& container, TaskSet::items_t& tasks, int pos)
{
  _needs_saving = true;
  fire_tasks_added (pos, tasks.size());
}

void
Queue :: on_set_item_removed (TaskSet& container, Task*& task, int pos)
{
  _needs_saving = true;
  fire_task_removed (task, pos);
}

void
Queue :: on_set_item_moved (TaskSet& container, Task*& task, int new_pos, int old_pos)
{
  _needs_saving = true;
  fire_task_moved (task, new_pos, old_pos);
}

/***
****
***/

void
Queue :: fire_queue_error (const StringView& message)
{
  for (lit it(_listeners.begin()), end(_listeners.end()); it!=end; )
    (*it++)->on_queue_error (*this, message);
}
void
Queue :: fire_tasks_added (int pos, int count)
{
  for (lit it(_listeners.begin()), end(_listeners.end()); it!=end; )
    (*it++)->on_queue_tasks_added (*this, pos, count);
}
void
Queue :: fire_task_removed (Task*& task, int pos)
{
  for (lit it(_listeners.begin()), end(_listeners.end()); it!=end; )
    (*it++)->on_queue_task_removed (*this, *task, pos);
}
void
Queue :: fire_task_moved (Task*& task, int new_pos, int old_pos)
{
  for (lit it(_listeners.begin()), end(_listeners.end()); it!=end; )
    (*it++)->on_queue_task_moved (*this, *task, new_pos, old_pos);
}
void
Queue :: fire_connection_count_changed (int count)
{
  for (lit it(_listeners.begin()), end(_listeners.end()); it!=end; )
    (*it++)->on_queue_connection_count_changed (*this, count);
}
void
Queue :: fire_size_changed (int active, int total)
{
  for (lit it(_listeners.begin()), end(_listeners.end()); it!=end; )
    (*it++)->on_queue_size_changed (*this, active, total);
}
void
Queue :: fire_online_changed (bool online)
{
  for (lit it(_listeners.begin()), end(_listeners.end()); it!=end; )
    (*it++)->on_queue_online_changed (*this, online);
}
void
Queue :: fire_task_active_changed (Task * task, bool active)
{
  for (lit it(_listeners.begin()), end(_listeners.end()); it!=end; )
    (*it++)->on_queue_task_active_changed (*this, *task, active);
}

/***
****
***/

void
Queue :: set_online (bool online)
{
  _is_online = online;
  fire_online_changed (_is_online);
}

void
Queue :: stop_tasks (const tasks_t& tasks)
{
  foreach_const (tasks_t, tasks, it) {
    Task * task (*it);
    if (_tasks.index_of (task) != -1)
      _stopped.insert (task);
  }
}

void
Queue :: restart_tasks (const tasks_t& tasks)
{
  foreach_const (tasks_t, tasks, it) {
    Task * task (*it);
    if (_tasks.index_of(task) != -1) {
      _stopped.erase (task);
      process_task (task);
    }
  }
}

void
Queue :: add_task (Task * task, AddMode mode)
{
  tasks_t tasks;
  tasks.push_back (task);
  add_tasks (tasks, mode);
}

void
Queue :: add_tasks (const tasks_t& tasks, AddMode mode)
{
  foreach_const (tasks_t, tasks, it) {
    TaskArticle * ta (dynamic_cast<TaskArticle*>(*it));
    if (ta)
      _mids.insert (ta->get_article().message_id);
  }

  if (mode == TOP)
    _tasks.add_top (tasks);
  else if (mode == BOTTOM)
    _tasks.add_bottom (tasks);
  else
    _tasks.add (tasks);

  // asdf
  tasks_t tmp (tasks);
  foreach (tasks_t, tmp, it)
    process_task (*it);
}

bool
Queue :: contains (const Quark& mid) const
{
  return _mids.count (mid);
}

void
Queue :: remove_latest_task ()
{
  if (!_tasks.empty())
    remove_task (_tasks[_tasks.size()-1]);
}

bool
Queue :: task_is_active (const Task * task) const
{
  bool task_has_nntp (false);
  foreach_const (nntp_to_task_t, _nntp_to_task, it)
    if ((task_has_nntp = task==it->second))
      break;
  return task_has_nntp;
}

void
Queue :: remove_tasks (const tasks_t& tasks)
{
  foreach_const (tasks_t, tasks, it)
    remove_task (*it);
}

void
Queue :: remove_task (Task * task)
{
  const int index (_tasks.index_of (task));
  pan_return_if_fail (index != -1);

  if (task_is_active (task)) // wait for the NNTPs to finish
  {
    debug ("can't delete this task right now because it's got server connections");
    _removing.insert (task);
  }
  else // no NNTPs working, we can remove right now.
  {
    TaskArticle * ta (dynamic_cast<TaskArticle*>(task));
    if (ta)
      _mids.erase (ta->get_article().message_id);

    _stopped.erase (task);
    _removing.erase (task);
    _tasks.remove (index);
    delete task;
  }
}

void
Queue :: get_all_task_states (task_states_t& setme)
{
  setme.tasks.clear ();
  setme.tasks.insert (setme.tasks.begin(), _tasks.begin(), _tasks.end());

  setme._queued.get_container() = setme.tasks;
  setme._queued.sort ();

  std::vector<Task*>& stopped (setme._stopped.get_container());
  stopped.clear ();
  stopped.insert (stopped.end(), _stopped.begin(), _stopped.end());

  std::vector<Task*>& removing (setme._removing.get_container());
  removing.clear ();
  removing.insert (removing.end(), _removing.begin(), _removing.end());

  std::vector<Task*>& running (setme._running.get_container());
  std::set<Task*> tmp; 
  foreach (nntp_to_task_t, _nntp_to_task, it) tmp.insert (it->second);
  running.clear ();
  running.insert (running.end(), tmp.begin(), tmp.end());
}

void
Queue :: check_in (NNTP * nntp, bool is_ok)
{
  Task * task (_nntp_to_task[nntp]);

  // if the same task still needs a connection to this server,
  // shoot it straight back.  This can be a lot faster than
  // returning the NNTP to the pool and checking it out again.
  const Task::State state (task->get_state ());

  if (is_ok
    && (state._health!=FAIL)
    && (state._work==Task::NEED_NNTP)
    && !_removing.count(task)
    && state._servers.count(nntp->_server)
    && find_first_task_needing_server(nntp->_server)==task)
  {
    task->give_nntp (this, nntp);
  }
  else
  {
    // take care of our nntp counting
    Task * task (_nntp_to_task[nntp]);
    _nntp_to_task.erase (nntp);

    // take care of the task's state
    const bool is_active (task_is_active (task));
    if (!is_active) // if it's not active anymore...
      fire_task_active_changed (task, is_active);

    // return the nntp to the pool
    const Quark& servername (nntp->_server);
    NNTP_Pool& pool (get_pool (servername));
    pool.check_in (nntp, is_ok);

    // what to do now with this task...
    process_task (task);
  }
}

void
Queue :: move_up (const tasks_t& tasks)
{
  foreach_const (tasks_t, tasks, it) {
    Task * task (*it);
    const int old_pos (_tasks.index_of (task));
    _tasks.move_up (old_pos);
  }
}

void
Queue :: move_down (const tasks_t& tasks)
{
  foreach_const_r (tasks_t, tasks, it) {
    Task * task (*it);
    const int old_pos (_tasks.index_of (task));
    _tasks.move_down (old_pos);
  }
}

void
Queue :: move_top (const tasks_t& tasks)
{
  foreach_const_r (tasks_t, tasks, it) {
    Task * task (*it);
    const int old_pos (_tasks.index_of (task));
    _tasks.move_top (old_pos);
  }
}

void
Queue :: move_bottom (const tasks_t& tasks)
{
  foreach_const (tasks_t, tasks, it) {
    Task * task (*it);
    const int old_pos (_tasks.index_of (task));
    _tasks.move_bottom (old_pos);
  }
}

void
Queue :: get_connection_counts (int& setme_active,
                                int& setme_idle,
                                int& setme_connecting) const
{
  setme_active = setme_idle = setme_connecting = 0;

  foreach_const (pools_t, _pools, it)
  {
    int active, idle, connecting, unused;
    it->second->get_counts (active, idle, connecting, unused);
    setme_active += active;
    setme_idle += idle;
    setme_connecting += connecting;
  }
}

void
Queue :: get_full_connection_counts (std::vector<ServerConnectionCounts>& setme) const
{
  setme.resize (_pools.size());
  ServerConnectionCounts * cit = &setme.front();

  int unused;
  foreach_const (pools_t, _pools, it) {
    ServerConnectionCounts& counts = *cit++;
    counts.server_id = it->first;
    it->second->get_counts (counts.active, counts.idle, counts.connecting, unused);
    foreach_const (nntp_to_task_t, _nntp_to_task, nit)
      if (nit->first->_server == counts.server_id)
        counts.KiBps += nit->first->_socket->get_speed_KiBps ();
  }
}

double
Queue :: get_speed_KiBps () const
{
  double KiBps (0.0);
  foreach_const (nntp_to_task_t, _nntp_to_task, it)
    KiBps += it->first->_socket->get_speed_KiBps ();
  return KiBps;
}

void
Queue :: get_task_speed_KiBps (const Task  * task,
                               double      & setme_KiBps,
                               int         & setme_connections) const
{
  double KiBps (0.0);
  int connections (0);
  foreach_const (nntp_to_task_t, _nntp_to_task, it) {
    if (it->second==task) {
      ++connections;
      KiBps += it->first->_socket->get_speed_KiBps ();
    }
  }

  setme_KiBps = KiBps;
  setme_connections = connections;
}
