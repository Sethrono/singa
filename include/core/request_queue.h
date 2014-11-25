// Copyright © 2014 Anh Dinh. All Rights Reserved.

#ifndef INCLUDE_CORE_REQUEST_QUEUE_H_
#define INCLUDE_CORE_REQUEST_QUEUE_H_

#include "proto/common.pb.h"
#include "utils/network_thread.h"
#include <boost/thread.hpp>
#include <boost/function.hpp>
#include <google/protobuf/message.h>
#include <mpi.h>
#include <vector>
#include <map>
#include <deque>
#include <string>


using std::vector;
using std::map;
using std::deque;
using std::string;

namespace lapis {


class RequestQueue {
 public:
  RequestQueue(int ns): num_mem_servers_(ns), key_index_(0) {}
  ~RequestQueue() {}

  virtual void NextRequest(TaggedMessage *msg) {}
  virtual void Enqueue(int tag, string &data) {}

  virtual bool sync_local_get(string &key){return true;}
  virtual bool sync_local_put(string &key){return true;}

  virtual void event_complete(string &key){}

  // extract the key from message stored in data
  void ExtractKey(int tag, string data, string *key);

  typedef deque<TaggedMessage *> Queue;
  typedef vector<boost::recursive_mutex *> Lock;

 protected:

  //mapping key(string) to lock
  Lock key_locks_;
  boost::recursive_mutex whole_queue_lock_;

  map<string, int> key_map_;

  int num_mem_servers_;
  int key_index_;
};

//  asynchronous queue
class ASyncRequestQueue: public RequestQueue {
 public:
  ASyncRequestQueue(int ns): RequestQueue(ns) {}
  void NextRequest(TaggedMessage *msg);
  void Enqueue(int tag, string &data);
  bool sync_local_get(string &key){return true;}
  bool sync_local_put(string &key){return true;}
  void event_complete(string &key){}

 private:

  vector<Queue> request_queues_;
};

//  synchronous queue
class SyncRequestQueue: public RequestQueue {
 public:
  SyncRequestQueue(int ns): RequestQueue(ns) {}
  void NextRequest(TaggedMessage *msg);
  void Enqueue(int tag, string &data);
  bool sync_local_get(string &key);
  bool sync_local_put(string &key);
  void event_complete(string &key);
 private:

  vector<Queue> put_queues_, get_queues_;
  vector<int> access_counters_;
  vector<int> is_in_put_queue_;
  vector<int> is_first_update_;
};

}  // namespace lapis

#endif  // INCLUDE_CORE_REQUEST_QUEUE_H_

