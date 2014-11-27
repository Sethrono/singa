// Copyright © 2014 Anh Dinh. All Rights Reserved.
#include <glog/logging.h>
#include <gflags/gflags.h>
#include <unordered_set>
#include <boost/thread.hpp>
#include <boost/function.hpp>
#include <google/protobuf/message.h>

#include "utils/network_thread.h"
#include "core/request_dispatcher.h"

#include "proto/worker.pb.h"

/**
 * @file request_dispatcher.cc
 * Implementation of RequestDispatcher class.
 * @see request_dispatcher.h.
 */
DECLARE_double(sleep_time);

namespace lapis {
<<<<<<< HEAD
	RequestDispatcher* RequestDispatcher::instance_;

	// initialized the queues
	RequestDispatcher::RequestDispatcher(){
		shared_ptr<GlobalContext> gc = GlobalContext::Get();

		if (gc->synchronous()){
			table_queue_ = new SyncRequestQueue(gc->num_table_servers());
		}
		else{
			table_queue_ = new ASyncRequestQueue(gc->num_table_servers());
		}

		num_outstanding_request_=0;

		//start the dispatch loop
		new boost::thread(&RequestDispatcher::table_dispatch_loop, this);
		new boost::thread(&RequestDispatcher::disk_dispatch_loop, this);
	}

	void RequestDispatcher::sync_local_get(string &key){
		if (!GlobalContext::Get()->synchronous())
			return;

		while (!table_queue_->sync_local_get(key))
			Sleep(FLAGS_sleep_time);
	}

	void RequestDispatcher::sync_local_put(string &key){
		if (!GlobalContext::Get()->synchronous())
			return;
		while (!table_queue_->sync_local_put(key))
					Sleep(FLAGS_sleep_time);
	}

	void RequestDispatcher::event_complete(string &key){
		table_queue_->event_complete(key);
	}

	bool RequestDispatcher::active(){
		return num_outstanding_request_>0;
	}
	void RequestDispatcher::Enqueue(int tag, string &data){
		if (tag==MTYPE_PUT_REQUEST || tag == MTYPE_GET_REQUEST || tag == MTYPE_UPDATE_REQUEST){
			table_queue_->Enqueue(tag, data);
			num_outstanding_request_++;
		}
		else{ // disk queue
			boost::recursive_mutex::scoped_lock sl(disk_lock_);
			disk_queue_.push_back(data);
		}
	}

	void RequestDispatcher::table_dispatch_loop() {
		while (true) {
			TaggedMessage t_msg;
			string key;
			table_queue_->NextRequest(&t_msg);
			table_queue_->ExtractKey(t_msg.tag, t_msg.data, &key);
      if(GlobalContext::Get()->rank()==17)
        LOG(ERROR)<<"queue size "<<num_outstanding_request_;

			boost::scoped_ptr <Message> message;
			if (t_msg.tag == MTYPE_GET_REQUEST)
				message.reset(new HashGet());
			else {
				message.reset(new TableData());
			}
			message->ParseFromArray(t_msg.data.data(), t_msg.data.size());

			if (callbacks_[t_msg.tag](message.get())){
				num_outstanding_request_--;
				table_queue_->event_complete(key);
			}
			else{ //enqueue again
				table_queue_->Enqueue(t_msg.tag, t_msg.data);
			}
		}
	}
=======
>>>>>>> origin/feature-cleanup

RequestDispatcher* RequestDispatcher::instance_;

/**
 * Initialize the request queue and start the dispatch loop.
 */
RequestDispatcher::RequestDispatcher() {
	table_queue_ = new ASyncRequestQueue(gc->num_table_servers());
	num_outstanding_request_ = 0;

	//start the thread running dispatch loop
	new boost::thread(&RequestDispatcher::table_dispatch_loop, this);
}


void RequestDispatcher::Enqueue(int tag, string &data) {
	table_queue_->Enqueue(tag, data);
	num_outstanding_request_++;
}

/**
 * Dispatch loop. Get the next request from the queue, process it
 * and put it back to the queue if the processing fails (callback function
 * returns false).
 */
void RequestDispatcher::table_dispatch_loop() {
	while (true) {
		TaggedMessage t_msg;
		table_queue_->NextRequest(&t_msg);

		boost::scoped_ptr < Message > message;
		if (t_msg.tag == MTYPE_GET_REQUEST)
			message.reset(new HashGet());
		else
			message.reset(new TableData());

		message->ParseFromArray(t_msg.data.data(), t_msg.data.size());

		if (callbacks_[t_msg.tag](message.get()))
			num_outstanding_request_--;
		else //enqueue again
			table_queue_->Enqueue(t_msg.tag, t_msg.data);
	}
}

}  // namespace lapis
