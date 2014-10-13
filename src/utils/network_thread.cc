// Copyright © 2014 Anh Dinh. All Rights Reserved.
// modified from piccolo/rpc.cc
#include <glog/logging.h>
#include <unordered_set>
#include "utils/network_thread.h"
#include "utils/global_context.h"
#include "utils/tuple.h"
#include "utils/stringpiece.h"
#include "utils/timer.h"

#include "proto/worker.pb.h"

// sleep duration between reading messages off the network.
DEFINE_double(sleep_time, 0.001, "");

namespace lapis {

string FIRST_BYTE_RECEIVED="first byte received";
string LAST_BYTE_RECEIVED="last byte received";
string TOTAL_BYTE_RECEIVED="total byte received";


std::shared_ptr<NetworkThread> NetworkThread::instance_;
void Sleep(double t){
  timespec req;
  req.tv_sec = (int)t;
  req.tv_nsec = (int64_t)(1e9 * (t - (int64_t)t));
  nanosleep(&req, NULL);
}
void ShutdownMPI() {
  NetworkThread::Get()->Shutdown();
}
std::shared_ptr<NetworkThread> NetworkThread::Get() {
  if(!instance_)
    instance_.reset(new NetworkThread());
  //atexit(&ShutdownMPI);
  return instance_;
}
NetworkThread::NetworkThread() {
  if (!getenv("OMPI_COMM_WORLD_RANK")) {
    world_ = NULL;
    id_ = -1;
    running_ = false;
    return;
  }
  MPI::Init_thread(MPI_THREAD_SINGLE);
  world_ = &MPI::COMM_WORLD;
  running_ = 1;
  id_ = world_->Get_rank();
    VLOG(3)<<"rank of this process "<<id_;
    for (int i = 0; i < kMaxMethods; ++i) {
      callbacks_[i] = NULL;
      handles_[i] = NULL;
    }
    disk_write_handle_ = NULL;

  sender_and_reciever_thread_ = new boost::thread(&NetworkThread::NetworkLoop,
      this);
  processing_thread_ = new boost::thread(&NetworkThread::ProcessLoop, this);
  disk_thread_ = new boost::thread(&NetworkThread::WriteToDiskLoop, this);

//  initialize message queue
  auto gc = GlobalContext::Get();
  if (gc->synchronous())
    request_queue_ = new SyncRequestQueue(gc->num_table_servers());
  else
    request_queue_ = new AsyncRequestQueue(gc->num_table_servers());

  //  init stats
	network_thread_stats_[FIRST_BYTE_RECEIVED] =
			network_thread_stats_[LAST_BYTE_RECEIVED] =
					network_thread_stats_[TOTAL_BYTE_RECEIVED] = 0;
}

bool NetworkThread::active() const {
  return active_sends_.size() + pending_sends_.size() > 0;
}

void NetworkThread::CollectActive() {
  if (active_sends_.empty())
    return;
  boost::recursive_mutex::scoped_lock sl(send_lock);
  std::unordered_set<RPCRequest *>::iterator i = active_sends_.begin();
  while (i != active_sends_.end()) {
    RPCRequest *r = (*i);
    //VLOG(3) << "Pending: " << MP(id(), MP(r->target, r->rpc_type));
    if (r->finished()) {
      if (r->failures > 0) {
        LOG(INFO) << "Send " << MP(id(), r->target) << " of size " << r->payload.size()
                  << " succeeded after " << r->failures << " failures.";
      }
      delete r;
      i = active_sends_.erase(i);
      continue;
    }
    ++i;
  }
}

//  loop that receives messages. unlike in piccolo, all requests
//  are added to the queue. Other requests (shard assignment, etc.)
//  are processed right away
void NetworkThread::NetworkLoop() {
	while (running_) {
		MPI::Status st;
		if (world_->Iprobe(MPI::ANY_SOURCE, MPI::ANY_TAG, st)) {
			int tag = st.Get_tag();
			int source = st.Get_source();
			int bytes = st.Get_count(MPI::BYTE);
			string data;
			data.resize(bytes);
			if (tag == MTYPE_DATA_PUT_REQUEST
					&& network_thread_stats_[FIRST_BYTE_RECEIVED] == 0)
				network_thread_stats_[FIRST_BYTE_RECEIVED] = Now();

			world_->Recv(&data[0], bytes, MPI::BYTE, source, tag, st);
			if (tag == MTYPE_DATA_PUT_REQUEST) {
				network_thread_stats_[LAST_BYTE_RECEIVED] = Now();
				network_thread_stats_[TOTAL_BYTE_RECEIVED] += bytes;
			}

			//  put request to the queue
			if (tag == MTYPE_PUT_REQUEST || tag == MTYPE_GET_REQUEST) {
				request_queue_->Enqueue(tag, data);
			} else if (tag == MTYPE_DATA_PUT_REQUEST
					|| tag == MTYPE_DATA_PUT_REQUEST_FINISH) {
				boost::recursive_mutex::scoped_lock sl(disk_lock_);
				disk_queue_.push_back(data);
			}
			else { //  put reponse, etc. to the response queue. This is read
				//  actively by the client
				boost::recursive_mutex::scoped_lock sl(response_queue_locks_[tag]);
				response_queue_[tag][source].push_back(data);
			}
			//  other messages that need to be processed right away, e.g. shard assignment
			if (callbacks_[tag] != NULL) {
				callbacks_[tag]();
			}
		} else {
			Sleep (FLAGS_sleep_time);
		}
		//  push the send queue through
		while (!pending_sends_.empty()) {
			boost::recursive_mutex::scoped_lock sl(send_lock);
			RPCRequest *s = pending_sends_.front();
			pending_sends_.pop_front();
			s->start_time = Now();
			s->mpi_req = world_->Isend(s->payload.data(), s->payload.size(),
					MPI::BYTE, s->target, s->rpc_type);
			active_sends_.insert(s);
		}
		CollectActive();
	}
}

//  loop through the request queue and process messages
//  get the next message, then invoke call back
void NetworkThread::ProcessLoop() {
  while (running_) {
    TaggedMessage msg;
    request_queue_->NextRequest(&msg);
    ProcessRequest(msg);
  }
}

void NetworkThread::WriteToDiskLoop() {
	while (running_) {
		DiskData *data = new DiskData();
		boost::recursive_mutex::scoped_lock sl(disk_lock_);
		if (disk_queue_.empty())
			Sleep(FLAGS_sleep_time);
		else{
			const string &s = disk_queue_.front();
			data->ParseFromArray(s.data(), s.size());
			disk_queue_.pop_front();
			disk_write_handle_(data);
		}
	}
}

void NetworkThread::ProcessRequest(const TaggedMessage &t_msg) {
  boost::scoped_ptr<Message> message;
  if (t_msg.tag == MTYPE_GET_REQUEST)
    message.reset(new HashGet());
  else {
    CHECK_EQ(t_msg.tag, MTYPE_PUT_REQUEST);
    message.reset(new TableData());
  }
  message->ParseFromArray(t_msg.data.data(), t_msg.data.size());
  handles_[t_msg.tag](message.get());
}

//  for now, only PUT_RESPONSE message are being pulled from this.
//  besides top-priority messages: REGISTER_WORKER, SHARD_ASSIGNMENT, etc.
bool NetworkThread::check_queue(int src, int type, Message *data) {
  Queue &q = response_queue_[type][src];
  if (!q.empty()) {
    boost::recursive_mutex::scoped_lock sl(response_queue_locks_[type]);
    if (q.empty())
      return false;
    const string &s = q.front();
    if (data) {
      data->ParseFromArray(s.data(), s.size());
    }
    q.pop_front();
    return true;
  }
  return false;
}

bool NetworkThread::is_empty_queue(int src, int type){
	boost::recursive_mutex::scoped_lock sl(response_queue_locks_[type]);
	Queue &q = response_queue_[type][src];
	return q.empty();
}

//  blocking read for the given source and message type.
void NetworkThread::Read(int desired_src, int type, Message *data,
                         int *source) {
  while (!TryRead(desired_src, type, data, source)) {
    Sleep(FLAGS_sleep_time);
  }
}

//  non-blocking read
bool NetworkThread::TryRead(int src, int type, Message *data, int *source) {
  if (src == MPI::ANY_SOURCE) {
    for (int i = 0; i < world_->Get_size(); ++i) {
      if (TryRead(i, type, data, source)) {
        return true;
      }
    }
  } else {
    if (check_queue(src, type, data)) {
      if (source) {
        *source = src;
      }
      return true;
    }
  }
  return false;
}

//  send = put request to the send queue
void NetworkThread::Send(RPCRequest *req) {
  boost::recursive_mutex::scoped_lock sl(send_lock);
  pending_sends_.push_back(req);
}

void NetworkThread::Send(int dst, int method, const Message &msg) {
  RPCRequest *r = new RPCRequest(dst, method, msg);
  Send(r);
}

void NetworkThread::Shutdown() {
  if (running_) {
    running_ = false;
    sender_and_reciever_thread_->join();
    disk_thread_->join();
    //processing_thread_->join();
    MPI_Finalize();
  }
}

//  wait for the message queue to clear
void NetworkThread::Flush() {
  while (active()) {
    Sleep(FLAGS_sleep_time);
  }
}

//  broadcast to all non-coordinator servers: 0-(size-1)
void NetworkThread::Broadcast(int method, const Message &msg) {
  for (int i = 0; i < size() - 1; ++i) {
    Send(i, method, msg);
  }
}

void NetworkThread::SyncBroadcast(int method, int reply, const Message &msg) {
  Broadcast(method, msg);
  WaitForSync(reply, size() - 1);
}

void NetworkThread::WaitForSync(int reply, int count) {
  EmptyMessage empty;
  while (count > 0) {
    Read(MPI::ANY_SOURCE, reply, &empty, NULL);
    --count;
  }
}

void NetworkThread::PrintStats(){
	VLOG(3) << "Network throughput = "
			<< network_thread_stats_[TOTAL_BYTE_RECEIVED]
					/ (network_thread_stats_[LAST_BYTE_RECEIVED]
							- network_thread_stats_[FIRST_BYTE_RECEIVED]);
}
void NetworkThread::barrier(){
  if (GlobalContext::Get()->AmICoordinator()){
    SyncBroadcast(MTYPE_BARRIER_REQUEST, MTYPE_BARRIER_REPLY, EmptyMessage());
    Broadcast(MTYPE_BARRIER_READY, EmptyMessage());
  }
  else{
    EmptyMessage msg;
    Read(GlobalContext::kCoordinator, MTYPE_BARRIER_REQUEST, &msg);
    Flush();
    Send(GlobalContext::kCoordinator, MTYPE_BARRIER_REPLY, msg);
    Read(GlobalContext::kCoordinator, MTYPE_BARRIER_READY, &msg);
  }
}
}  // namespace lapis
