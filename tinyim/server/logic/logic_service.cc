
#include "access/access.pb.h"
#include "common/messages.pb.h"
#include "dbproxy/dbproxy.pb.h"
#include "logic/logic_service.h"
#include "idgen/idgen.pb.h"

#include <vector>

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <bthread/bthread.h>
#include <brpc/closure_guard.h>
#include <brpc/controller.h>
#include <brpc/options.pb.h>

DEFINE_int32(access_max_retry, 3, "Max retries(not including the first RPC)");
DEFINE_string(access_connection_type, "single", "Connection type. Available values: single, pooled, short");
DEFINE_int32(access_timeout_ms, 100, "RPC timeout in milliseconds");

namespace {
struct SendtoPeersArgs {
  tinyim::MsgIdRequest id_request;
  tinyim::MsgIdReply id_reply;
  tinyim::NewMsg new_msg;
  tinyim::user_id_t sender;
  tinyim::LogicServiceImpl* this_;
};

}  // namespace


namespace tinyim {
class SendtoAccessClosure: public ::google::protobuf::Closure {
 public:
  SendtoAccessClosure(brpc::Controller* cntl, Pong* pong): cntl_(cntl), pong_(pong){}

  void Run() override {
    DLOG(INFO) << "Running SendtoAccessClosure";
    if (cntl_->Failed()){
      DLOG(ERROR) << "Fail to call SendtoAccess. " << cntl_->ErrorText();
    }
    delete this;
  }

  ~SendtoAccessClosure(){
    delete cntl_;
    delete pong_;
  }

 private:
  brpc::Controller* cntl_;
  Pong* pong_;
};

LogicServiceImpl::LogicServiceImpl(brpc::Channel *id_channel,
                                   brpc::Channel *db_channel): id_channel_(id_channel),
                                                               db_channel_(db_channel) {}

LogicServiceImpl::~LogicServiceImpl() {
  DLOG(INFO) << "Calling AccessServiceImpl dtor";
}

void LogicServiceImpl::Test(google::protobuf::RpcController* controller,
                            const Ping* ping,
                            Pong* pong,
                            google::protobuf::Closure* done){
  brpc::ClosureGuard done_guard(done);
  DLOG(INFO) << "Running test";
}

void LogicServiceImpl::SendMsg(google::protobuf::RpcController* controller,
                               const NewMsg* new_msg,
                               MsgReply* reply,
                               google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  brpc::Controller* cntl = static_cast<brpc::Controller*>(controller);
  const tinyim::user_id_t user_id = new_msg->user_id();
  DLOG(INFO) << "Received request[log_id=" << cntl->log_id()
            << "] from " << cntl->remote_side()
            << " to " << cntl->local_side()
            << " user_id=" << user_id
            << " peer_id=" << new_msg->peer_id()
            << " client_time=" << new_msg->client_time()
            << " msg_type=" << new_msg->msg_type()
            << " message=" << new_msg->message()
            << " (attached=" << cntl->request_attachment() << ")";

  // 1. check duplicate
  DbproxyService_Stub db_stub(db_channel_);
  brpc::Controller db_cntl;
  UserId cur_user_id;
  cur_user_id.set_user_id(user_id);
  UserLastSendData last_send_data;
  db_stub.GetUserLastSendData(&db_cntl, &cur_user_id, &last_send_data, nullptr);
  if (db_cntl.Failed()){
    DLOG(ERROR) << "Fail to call GetUserLastSendData. " << db_cntl.ErrorText();
    cntl->SetFailed(db_cntl.ErrorCode(), db_cntl.ErrorText().c_str());
    return;
  }
  else {
    // TODO when last_send_data.client_time() = 0
    if (last_send_data.client_time() == new_msg->client_time()) {
      reply->set_msg_id(last_send_data.msg_id());
      reply->set_msg_time(last_send_data.msg_time());
      return;
    }
  }

  // 2. Get id for this msg
  std::unique_ptr<SendtoPeersArgs> sendto_peers_args(new SendtoPeersArgs);
  sendto_peers_args->sender = user_id;
  // sendto_peers_args->new_msg;

  IdGenService_Stub id_stub(id_channel_);
  MsgIdRequest &id_request = sendto_peers_args->id_request;
  MsgIdReply &id_reply = sendto_peers_args->id_reply;
  brpc::Controller id_cntl;
  id_cntl.set_log_id(cntl->log_id());
  {
    auto user_and_id_num = id_request.add_user_ids();
    user_and_id_num->set_user_id(user_id);
    user_and_id_num->set_need_msgid_num(1);
  }
  if (new_msg->msg_type() == MsgType::PRIVATE){
    auto peer_and_id_num = id_request.add_user_ids();
    peer_and_id_num->set_user_id(new_msg->peer_id());
    peer_and_id_num->set_need_msgid_num(1);
  }
  else {
    DbproxyService_Stub db_stub(db_channel_);
    GroupId group_id;
    group_id.set_group_id(new_msg->peer_id());
    brpc::Controller db_cntl;
    UserInfos user_infos;
    // UserIds user_ids;
    db_cntl.set_log_id(cntl->log_id());
    db_stub.GetGroupMembers(&db_cntl, &group_id, &user_infos, nullptr);
    if (db_cntl.Failed()){
      DLOG(ERROR) << "Fail to call GetGroupMember. " << db_cntl.ErrorText();
      cntl->SetFailed(db_cntl.ErrorCode(), db_cntl.ErrorText().c_str());
      return;
    }
    else {
      for (int i = 0; i < user_infos.user_info_size(); ++i){
        const user_id_t cur_user_id = user_infos.user_info(i).user_id();
        if (cur_user_id == user_id){
          continue;
        }
        auto user_and_id_num = id_request.add_user_ids();
        user_and_id_num->set_user_id(cur_user_id);
        user_and_id_num->set_need_msgid_num(1);
      }
    }
  }


  id_stub.IdGenerate(&id_cntl, &id_request, &id_reply, nullptr);
  if (id_cntl.Failed()){
      DLOG(ERROR) << "Fail to call IdGenerate. " << id_cntl.ErrorText();
      cntl->SetFailed(id_cntl.ErrorCode(), id_cntl.ErrorText().c_str());
      return;
  }

  // 3. save user last send data
  int32_t msg_time = std::time(nullptr);
  // brpc::Controller db_set_cntl;
  // Pong set_pong;
  // last_send_data.set_msg_id(id_reply.msg_ids(0).start_msg_id());
  // db_stub.SetUserLastSendData(&db_set_cntl, &last_send_data, &set_pong, nullptr);
  // if (db_set_cntl.Failed()){
      // DLOG(ERROR) << "Fail to call SetUserLastSendData. " << db_set_cntl.ErrorText();
      // cntl->SetFailed(db_set_cntl.ErrorCode(), db_set_cntl.ErrorText().c_str());
      // return;
  // }

  // 4. Save this msg to db
  if (new_msg->msg_type() == MsgType::PRIVATE) {
    const msg_id_t sender_msg_id = id_reply.msg_ids(0).start_msg_id();
    const msg_id_t receiver_msg_id = id_reply.msg_ids(1).start_msg_id();
    NewPrivateMsg new_private_msg;
    new_private_msg.set_sender(user_id);
    new_private_msg.set_receiver(new_msg->peer_id());
    new_private_msg.set_client_time(new_msg->client_time());
    new_private_msg.set_msg_time(msg_time); // TODO should earlier
    new_private_msg.set_message(new_msg->message());
    new_private_msg.set_sender_msg_id(sender_msg_id);
    new_private_msg.set_receiver_msg_id(receiver_msg_id);
    DbproxyService_Stub db_stub2(db_channel_);
    brpc::Controller db_cntl;
    Reply db_reply;
    db_stub2.SavePrivateMsg(&db_cntl, &new_private_msg, &db_reply, nullptr);
    if (db_cntl.Failed()){
      DLOG(ERROR) << "Fail to call SavePrivateMsg. " << db_cntl.ErrorText();
      cntl->SetFailed(db_cntl.ErrorCode(), db_cntl.ErrorText().c_str());
      return;
    }
    else{
      reply->set_msg_id(sender_msg_id);
      reply->set_last_msg_id(sender_msg_id);
      reply->set_msg_time(msg_time);
      // return;
    }
  }
  else{
    NewGroupMsg new_group_msg;
    new_group_msg.set_group_id(new_msg->peer_id());
    new_group_msg.set_message(new_msg->message());
    new_group_msg.set_client_time(new_msg->client_time());
    new_group_msg.set_msg_time(msg_time);
    new_group_msg.set_sender_user_id(user_id);
    msg_id_t msg_id = 0;
    for (int i = 0, size = id_reply.msg_ids_size(); i < size; ++i){
      auto puser_and_msgid = new_group_msg.add_user_and_msgids();
      if (user_id == id_reply.msg_ids(i).user_id()){
        // TODO sender is first one
        msg_id = id_reply.msg_ids(i).start_msg_id();
      }
      puser_and_msgid->set_user_id(id_reply.msg_ids(i).user_id());
      puser_and_msgid->set_msg_id(id_reply.msg_ids(i).start_msg_id());
    }
    DLOG_IF(INFO, msg_id == 0) << "Fail to get id from idgen msg_id=" << msg_id << " user_id=" << user_id;
    new_group_msg.set_sender_msg_id(msg_id);
    DbproxyService_Stub db_stub2(db_channel_);
    brpc::Controller db_cntl;
    Reply db_reply;
    db_stub2.SaveGroupMsg(&db_cntl, &new_group_msg, &db_reply, nullptr);
    if (db_cntl.Failed()){
      DLOG(ERROR) << "Fail to call SaveGroupMsg. " << id_cntl.ErrorText();
      cntl->SetFailed(id_cntl.ErrorCode(), id_cntl.ErrorText().c_str());
      return;
    }
    else {
      CHECK_NE(msg_id, 0) << "Id is wrong. user_id=" << user_id;
      reply->set_msg_id(msg_id);
      // reply->set_timestamp(timestamp);
    }
    reply->set_msg_id(msg_id);
    reply->set_last_msg_id(msg_id);
    reply->set_msg_time(msg_time);
  }

  sendto_peers_args->new_msg = *new_msg;

  sendto_peers_args->new_msg.set_msg_time(msg_time);
  sendto_peers_args->this_ = this;

  bthread_t bt;
  bthread_start_background(&bt, nullptr, SendtoPeers, sendto_peers_args.release());
}

void* LogicServiceImpl::SendtoPeers(void* args) {
  std::unique_ptr<SendtoPeersArgs> lazy_delete(static_cast<SendtoPeersArgs*>(args));

  tinyim::MsgIdRequest& id_request = (static_cast<SendtoPeersArgs*>(args))->id_request;
  tinyim::MsgIdReply& id_reply = (static_cast<SendtoPeersArgs*>(args))->id_reply;
  tinyim::user_id_t sender = static_cast<SendtoPeersArgs*>(args)->sender;
  tinyim::NewMsg& new_msg = static_cast<SendtoPeersArgs*>(args)->new_msg;
  tinyim::LogicServiceImpl* this_ = static_cast<SendtoPeersArgs*>(args)->this_;

  tinyim::DbproxyService_Stub session_stub(this_->db_channel_);
  brpc::Controller session_cntl;
  tinyim::Sessions sessions;
  tinyim::UserIds user_ids;

  for (int i = 1, size = id_request.user_ids_size(); i < size; ++i){
    // i = 0 is sender
    user_ids.add_user_id(id_request.user_ids(i).user_id());
  }

  session_stub.GetSessions(&session_cntl, &user_ids, &sessions, nullptr);
  if (session_cntl.Failed()){
    DLOG(ERROR) << "Fail to call GetSessions. " << session_cntl.ErrorText();
    return nullptr;
  }
  DLOG(INFO) << "GetSessions. response size=" << sessions.session_size();
  for (int i = 0; i < user_ids.user_id_size(); ++i){
    DLOG(INFO) << "i= " << i
               << "user_id=" << user_ids.user_id(i)
               << " session addr=" << sessions.session(i).addr()
               << ". " << sessions.session(i).user_id();

  }
  // CHECK_NE(msg_id, 0) << "Id is wrong. user_id=" << user_id;
  // reply->set_msg_id(msg_id);

  std::vector<brpc::Channel*> channel_vec;
  channel_vec.reserve(sessions.session_size());
  {
    std::unique_lock<std::mutex> lck(this_->access_map_mtx_);
    for (int i = 0, size = sessions.session_size(); i < size; ++i){
      if (!sessions.session(i).has_session()){
        DLOG(INFO) << "user_id=" << sessions.session(i).user_id() << " has no session";
        continue;
      }
      if (this_->access_map_.count(sessions.session(i).addr()) == 0) {
        brpc::ChannelOptions options;
        options.protocol = brpc::PROTOCOL_BAIDU_STD;
        options.connection_type = FLAGS_access_connection_type;
        options.timeout_ms = FLAGS_access_timeout_ms/*milliseconds*/;
        options.max_retry = FLAGS_access_max_retry;
        DLOG(INFO) << "Insert access_map. key" << sessions.session(i).addr();
        this_->access_map_[sessions.session(i).addr()].Init(sessions.session(i).addr().c_str(), &options);
      }
      channel_vec.push_back(&(this_->access_map_[sessions.session(i).addr()]));
      DLOG(INFO) << "channel_vec idx=" << channel_vec.size() - 1 << "session addr=" << sessions.session(i).addr();
    }
  }
  DLOG(INFO) << "channel_vec size=" << channel_vec.size();
  for (int i = 0, size = sessions.session_size(), j = 0; i < size; ++i){
    if (!sessions.session(i).has_session()){
      continue;
    }
    auto cntl = new brpc::Controller;
    tinyim::Msg msg;
    // XXX const char*
    msg.set_user_id(user_ids.user_id(i));
    msg.set_sender(sender);
    msg.set_receiver(user_ids.user_id(i));
    msg.set_message(new_msg.message().c_str());
    msg.set_msg_id(id_reply.msg_ids(i + 1).start_msg_id());
    msg.set_client_time(new_msg.client_time());
    msg.set_msg_time(new_msg.msg_time());
    if (new_msg.msg_type() == MsgType::PRIVATE){
      msg.set_group_id(0);
    }
    else{
      msg.set_group_id(new_msg.peer_id());
    }

    auto pong = new Pong;
    auto send_to_access_closure = new SendtoAccessClosure(cntl, pong);

    tinyim::AccessService_Stub stub(channel_vec[j]);
    DLOG(INFO) << "Calling SendtoAccess. receiver=" << user_ids.user_id(i) << " channel_vec idx=" << j;
    stub.SendtoAccess(cntl, &msg, pong, send_to_access_closure);
    ++j;
  }
  return nullptr;
}

void LogicServiceImpl::PullData(google::protobuf::RpcController* controller,
                                const Ping* ping,
                                Msgs* msgs,
                                google::protobuf::Closure* done){

  brpc::ClosureGuard done_guard(done);
  DLOG(ERROR) << "TODO PullData";
  // ResetHeartBeatTimer(user_id);
}

void LogicServiceImpl::GetMsgs(google::protobuf::RpcController* controller,
                               const MsgIdRange* msg_range,
                               Msgs* msgs,
                               google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  brpc::Controller* cntl = static_cast<brpc::Controller*>(controller);

  DbproxyService_Stub db_stub(db_channel_);
  brpc::Controller db_cntl;
  db_cntl.set_log_id(cntl->log_id());

  db_stub.GetMsgs(&db_cntl, msg_range, msgs, nullptr);
  if (db_cntl.Failed()) {
      DLOG(ERROR) << "Fail to call GetMsgs. " << db_cntl.ErrorText();
      cntl->SetFailed(db_cntl.ErrorCode(), db_cntl.ErrorText().c_str());
  }

}

void LogicServiceImpl::GetFriends(google::protobuf::RpcController* controller,
                                  const UserId* user_id,
                                  UserInfos* user_infos,
                                  google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  brpc::Controller* cntl = static_cast<brpc::Controller*>(controller);

  DbproxyService_Stub db_stub(db_channel_);
  brpc::Controller db_cntl;
  db_cntl.set_log_id(cntl->log_id());

  db_stub.GetFriends(&db_cntl, user_id, user_infos, nullptr);
  if (db_cntl.Failed()) {
      DLOG(ERROR) << "Fail to call GetFriends. " << db_cntl.ErrorText();
      cntl->SetFailed(db_cntl.ErrorCode(), db_cntl.ErrorText().c_str());
  }
}

void LogicServiceImpl::GetGroups(google::protobuf::RpcController* controller,
                                 const UserId* user_id,
                                 GroupInfos* group_infos,
                                 google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  brpc::Controller* cntl = static_cast<brpc::Controller*>(controller);

  DbproxyService_Stub db_stub(db_channel_);
  brpc::Controller db_cntl;
  db_cntl.set_log_id(cntl->log_id());

  db_stub.GetGroups(&db_cntl, user_id, group_infos, nullptr);
  if (db_cntl.Failed()) {
      DLOG(ERROR) << "Fail to call GetGroups. " << db_cntl.ErrorText();
      cntl->SetFailed(db_cntl.ErrorCode(), db_cntl.ErrorText().c_str());
  }
}

void LogicServiceImpl::GetGroupMembers(google::protobuf::RpcController* controller,
                                       const GroupId* group_id,
                                       UserInfos* user_infos,
                                       google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  brpc::Controller* cntl = static_cast<brpc::Controller*>(controller);

  DbproxyService_Stub db_stub(db_channel_);
  brpc::Controller db_cntl;
  db_cntl.set_log_id(cntl->log_id());

  db_stub.GetGroupMembers(&db_cntl, group_id, user_infos, nullptr);
  if (db_cntl.Failed()) {
      DLOG(ERROR) << "Fail to call GetGroupMembers. " << db_cntl.ErrorText();
      cntl->SetFailed(db_cntl.ErrorCode(), db_cntl.ErrorText().c_str());
  }
}

}  // namespace tinyim