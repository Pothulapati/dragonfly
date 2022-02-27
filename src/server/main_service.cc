// Copyright 2021, Roman Gershman.  All rights reserved.
// See LICENSE for licensing terms.
//

#include "server/main_service.h"

extern "C" {
#include "redis/redis_aux.h"
}

#include <absl/cleanup/cleanup.h>
#include <absl/strings/ascii.h>
#include <xxhash.h>

#include <boost/fiber/operations.hpp>
#include <filesystem>

#include "base/logging.h"
#include "server/conn_context.h"
#include "server/error.h"
#include "server/generic_family.h"
#include "server/list_family.h"
#include "server/script_mgr.h"
#include "server/server_state.h"
#include "server/string_family.h"
#include "server/transaction.h"
#include "util/metrics/metrics.h"
#include "util/uring/uring_fiber_algo.h"
#include "util/varz.h"

DEFINE_uint32(port, 6380, "Redis port");
DEFINE_uint32(memcache_port, 0, "Memcached port");
DECLARE_string(requirepass);

namespace dfly {

using namespace std;
using namespace util;
using base::VarzValue;
using ::boost::intrusive_ptr;
namespace fibers = ::boost::fibers;
namespace this_fiber = ::boost::this_fiber;

namespace {

DEFINE_VARZ(VarzMapAverage, request_latency_usec);

std::optional<VarzFunction> engine_varz;
metrics::CounterFamily cmd_req("requests_total", "Number of served redis requests");

constexpr size_t kMaxThreadSize = 1024;

class InterpreterReplier : public RedisReplyBuilder {
 public:
  InterpreterReplier(ObjectExplorer* explr) : RedisReplyBuilder(nullptr), explr_(explr) {
  }

  void SendError(std::string_view str) override;
  void SendStored() override;

  void SendSimpleString(std::string_view str) final;
  void SendMGetResponse(const OptResp* resp, uint32_t count) final;
  void SendSimpleStrArr(const string_view* arr, uint32_t count) final;
  void SendNullArray() final;

  void SendStringArr(absl::Span<const string_view> arr) final;
  void SendNull() final;

  void SendLong(long val) final;
  void SendDouble(double val) final;

  void SendBulkString(std::string_view str) final;

  void StartArray(unsigned len) final;

 private:
  void PostItem();

  ObjectExplorer* explr_;
  vector<pair<unsigned, unsigned>> array_len_;
  unsigned num_elems_ = 0;
};

class EvalSerializer : public ObjectExplorer {
 public:
  EvalSerializer(RedisReplyBuilder* rb) : rb_(rb) {
  }

  void OnBool(bool b) final {
    if (b) {
      rb_->SendLong(1);
    } else {
      rb_->SendNull();
    }
  }

  void OnString(string_view str) final {
    rb_->SendBulkString(str);
  }

  void OnDouble(double d) final {
    rb_->SendDouble(d);
  }

  void OnInt(int64_t val) final {
    rb_->SendLong(val);
  }

  void OnArrayStart(unsigned len) final {
    LOG(FATAL) << "TBD";
  }

  void OnArrayEnd() final {
    LOG(FATAL) << "TBD";
  }

  void OnNil() final {
    rb_->SendNull();
  }

  void OnStatus(string_view str) {
    rb_->SendSimpleString(str);
  }

  void OnError(string_view str) {
    rb_->SendError(str);
  }

 private:
  RedisReplyBuilder* rb_;
};

void InterpreterReplier::PostItem() {
  if (array_len_.empty()) {
    DCHECK_EQ(0u, num_elems_);
    ++num_elems_;
  } else {
    ++num_elems_;

    while (num_elems_ == array_len_.back().second) {
      num_elems_ = array_len_.back().first;
      explr_->OnArrayEnd();

      array_len_.pop_back();
      if (array_len_.empty())
        break;
    }
  }
}

void InterpreterReplier::SendError(string_view str) {
  DCHECK(array_len_.empty());
  explr_->OnError(str);
}

void InterpreterReplier::SendStored() {
  DCHECK(array_len_.empty());
  SendSimpleString("OK");
}

void InterpreterReplier::SendSimpleString(string_view str) {
  if (array_len_.empty())
    explr_->OnStatus(str);
  else
    explr_->OnString(str);
  PostItem();
}

void InterpreterReplier::SendMGetResponse(const OptResp* resp, uint32_t count) {
  DCHECK(array_len_.empty());

  explr_->OnArrayStart(count);
  for (uint32_t i = 0; i < count; ++i) {
    if (resp[i].has_value()) {
      explr_->OnString(resp[i]->value);
    } else {
      explr_->OnNil();
    }
  }
  explr_->OnArrayEnd();
}

void InterpreterReplier::SendSimpleStrArr(const string_view* arr, uint32_t count) {
  explr_->OnArrayStart(count);
  for (uint32_t i = 0; i < count; ++i) {
    explr_->OnString(arr[i]);
  }
  explr_->OnArrayEnd();
  PostItem();
}

void InterpreterReplier::SendNullArray() {
  SendSimpleStrArr(nullptr, 0);
  PostItem();
}

void InterpreterReplier::SendStringArr(absl::Span<const string_view> arr) {
  SendSimpleStrArr(arr.data(), arr.size());
  PostItem();
}

void InterpreterReplier::SendNull() {
  explr_->OnNil();
  PostItem();
}

void InterpreterReplier::SendLong(long val) {
  explr_->OnInt(val);
  PostItem();
}

void InterpreterReplier::SendDouble(double val) {
  explr_->OnDouble(val);
  PostItem();
}

void InterpreterReplier::SendBulkString(string_view str) {
  explr_->OnString(str);
  PostItem();
}

void InterpreterReplier::StartArray(unsigned len) {
  explr_->OnArrayStart(len);

  if (len == 0) {
    explr_->OnArrayEnd();
    PostItem();
  } else {
    array_len_.emplace_back(num_elems_ + 1, len);
    num_elems_ = 0;
  }
}

bool IsSHA(string_view str) {
  for (auto c : str) {
    if (!absl::ascii_isxdigit(c))
      return false;
  }
  return true;
}

bool IsTransactional(const CommandId* cid) {
  if (cid->first_key_pos() > 0 || (cid->opt_mask() & CO::GLOBAL_TRANS))
    return true;

  string_view name{cid->name()};

  if (name == "EVAL" || name == "EVALSHA")
    return true;

  return false;
}

bool EvalValidator(CmdArgList args, ConnectionContext* cntx) {
  string_view num_keys_str = ArgS(args, 2);
  int32_t num_keys;

  if (!absl::SimpleAtoi(num_keys_str, &num_keys) || num_keys < 0) {
    (*cntx)->SendError(kInvalidIntErr);
    return false;
  }

  if (unsigned(num_keys) > args.size() - 3) {
    (*cntx)->SendError("Number of keys can't be greater than number of args");
    return false;
  }

  return true;
}

}  // namespace

Service::Service(ProactorPool* pp) : pp_(*pp), shard_set_(pp), server_family_(this) {
  CHECK(pp);

  // We support less than 1024 threads and we support less than 1024 shards.
  // For example, Scan uses 10 bits in cursor to encode shard id it currently traverses.
  CHECK_LT(pp->size(), kMaxThreadSize);
  RegisterCommands();

  engine_varz.emplace("engine", [this] { return GetVarzStats(); });
}

Service::~Service() {
}

void Service::Init(util::AcceptServer* acceptor, const InitOpts& opts) {
  InitRedisTables();

  uint32_t shard_num = pp_.size() > 1 ? pp_.size() - 1 : pp_.size();
  shard_set_.Init(shard_num);

  pp_.Await([&](uint32_t index, ProactorBase* pb) {
    ServerState::tlocal()->Init();

    if (index < shard_count()) {
      shard_set_.InitThreadLocal(pb, !opts.disable_time_update);
    }
  });

  request_latency_usec.Init(&pp_);
  StringFamily::Init(&pp_);
  GenericFamily::Init(&pp_);
  cmd_req.Init(&pp_, {"type"});
}

void Service::Shutdown() {
  VLOG(1) << "Service::Shutdown";

  auto [current, switched] = server_family_.global_state()->Next(GlobalState::SHUTTING_DOWN);

  // TODO: to introduce BlockingNext that waits until the state is switched to idle.
  CHECK(switched) << "TBD " << GlobalState::Name(current);

  engine_varz.reset();
  request_latency_usec.Shutdown();

  pp_.AwaitFiberOnAll([](ProactorBase* pb) { ServerState::tlocal()->Shutdown(); });

  // to shutdown all the runtime components that depend on EngineShard.
  server_family_.Shutdown();
  StringFamily::Shutdown();
  GenericFamily::Shutdown();

  cmd_req.Shutdown();
  shard_set_.RunBlockingInParallel([&](EngineShard*) { EngineShard::DestroyThreadLocal(); });
}

void Service::DispatchCommand(CmdArgList args, ConnectionContext* cntx) {
  CHECK(!args.empty());
  DCHECK_NE(0u, shard_set_.size()) << "Init was not called";

  ToUpper(&args[0]);

  VLOG(2) << "Got: " << args;

  string_view cmd_str = ArgS(args, 0);
  bool is_trans_cmd = (cmd_str == "EXEC" || cmd_str == "MULTI");
  const CommandId* cid = registry_.Find(cmd_str);
  ServerState& etl = *ServerState::tlocal();

  etl.RecordCmd();

  absl::Cleanup multi_error = [cntx] {
    if (cntx->conn_state.exec_state != ConnectionState::EXEC_INACTIVE) {
      cntx->conn_state.exec_state = ConnectionState::EXEC_ERROR;
    }
  };

  if (cid == nullptr) {
    return (*cntx)->SendError(absl::StrCat("unknown command `", cmd_str, "`"));
  }

  if (etl.gstate() == GlobalState::LOADING || etl.gstate() == GlobalState::SHUTTING_DOWN) {
    string err = absl::StrCat("Can not execute during ", GlobalState::Name(etl.gstate()));
    (*cntx)->SendError(err);
    return;
  }

  string_view cmd_name{cid->name()};

  if ((cntx->conn_state.mask & (ConnectionState::REQ_AUTH | ConnectionState::AUTHENTICATED)) ==
      ConnectionState::REQ_AUTH) {
    if (cmd_name != "AUTH") {
      return (*cntx)->SendError("-NOAUTH Authentication required.");
    }
  }

  bool under_script = cntx->conn_state.script_info.has_value();

  if (under_script && (cid->opt_mask() & CO::NOSCRIPT)) {
    return (*cntx)->SendError("This Redis command is not allowed from script");
  }

  bool is_write_cmd =
      (cid->opt_mask() & CO::WRITE) || (under_script && cntx->conn_state.script_info->is_write);
  bool under_multi = cntx->conn_state.exec_state != ConnectionState::EXEC_INACTIVE && !is_trans_cmd;

  if (!etl.is_master && is_write_cmd) {
    (*cntx)->SendError("-READONLY You can't write against a read only replica.");
    return;
  }

  if ((cid->arity() > 0 && args.size() != size_t(cid->arity())) ||
      (cid->arity() < 0 && args.size() < size_t(-cid->arity()))) {
    return (*cntx)->SendError(WrongNumArgsError(cmd_str));
  }

  if (cid->key_arg_step() == 2 && (args.size() % 2) == 0) {
    return (*cntx)->SendError(WrongNumArgsError(cmd_str));
  }

  // Validate more complicated cases with custom validators.
  if (!cid->Validate(args, cntx)) {
    return;
  }

  if (under_multi) {
    if (cid->opt_mask() & CO::ADMIN) {
      (*cntx)->SendError("Can not run admin commands under transactions");
      return;
    }

    if (cmd_name == "SELECT") {
      (*cntx)->SendError("Can not call SELECT within a transaction");
      return;
    }
  }

  std::move(multi_error).Cancel();

  if (cntx->conn_state.exec_state != ConnectionState::EXEC_INACTIVE && !is_trans_cmd) {
    // TODO: protect against aggregating huge transactions.
    StoredCmd stored_cmd{cid};
    stored_cmd.cmd.reserve(args.size());
    for (size_t i = 0; i < args.size(); ++i) {
      stored_cmd.cmd.emplace_back(ArgS(args, i));
    }
    cntx->conn_state.exec_body.push_back(std::move(stored_cmd));

    return (*cntx)->SendSimpleString("QUEUED");
  }

  uint64_t start_usec = ProactorBase::GetMonotonicTimeNs(), end_usec;

  // Create command transaction
  intrusive_ptr<Transaction> dist_trans;

  if (under_script) {
    DCHECK(cntx->transaction);
    KeyIndex key_index = DetermineKeys(cid, args);
    for (unsigned i = key_index.start; i < key_index.end; ++i) {
      string_view key = ArgS(args, i);
      if (!cntx->conn_state.script_info->keys.contains(key)) {
        return (*cntx)->SendError("script tried accessing undeclared key");
      }
    }
    cntx->transaction->SetExecCmd(cid);
    cntx->transaction->InitByArgs(cntx->conn_state.db_index, args);
  } else {
    DCHECK(cntx->transaction == nullptr);

    if (IsTransactional(cid)) {
      dist_trans.reset(new Transaction{cid, &shard_set_});
      cntx->transaction = dist_trans.get();

      dist_trans->InitByArgs(cntx->conn_state.db_index, args);
      cntx->last_command_debug.shards_count = cntx->transaction->unique_shard_cnt();
    } else {
      cntx->transaction = nullptr;
    }
  }

  cntx->cid = cid;
  cmd_req.Inc({cmd_name});
  cid->Invoke(args, cntx);
  end_usec = ProactorBase::GetMonotonicTimeNs();

  request_latency_usec.IncBy(cmd_str, (end_usec - start_usec) / 1000);
  if (dist_trans) {
    cntx->last_command_debug.clock = dist_trans->txid();
    cntx->last_command_debug.is_ooo = dist_trans->IsOOO();
  }

  if (!under_script) {
    cntx->transaction = nullptr;
  }
}

void Service::DispatchMC(const MemcacheParser::Command& cmd, std::string_view value,
                         ConnectionContext* cntx) {
  absl::InlinedVector<MutableSlice, 8> args;
  char cmd_name[16];
  char ttl[16];
  char store_opt[32] = {0};
  char ttl_op[] = "EX";

  MCReplyBuilder* mc_builder = static_cast<MCReplyBuilder*>(cntx->reply_builder());

  switch (cmd.type) {
    case MemcacheParser::REPLACE:
      strcpy(cmd_name, "SET");
      strcpy(store_opt, "XX");
      break;
    case MemcacheParser::SET:
      strcpy(cmd_name, "SET");
      break;
    case MemcacheParser::ADD:
      strcpy(cmd_name, "SET");
      strcpy(store_opt, "NX");
      break;
    case MemcacheParser::DELETE:
      strcpy(cmd_name, "DEL");
      break;
    case MemcacheParser::INCR:
      strcpy(cmd_name, "INCRBY");
      absl::numbers_internal::FastIntToBuffer(cmd.delta, store_opt);
      break;
    case MemcacheParser::DECR:
      strcpy(cmd_name, "DECRBY");
      absl::numbers_internal::FastIntToBuffer(cmd.delta, store_opt);
      break;
    case MemcacheParser::APPEND:
      strcpy(cmd_name, "APPEND");
      break;
    case MemcacheParser::PREPEND:
      strcpy(cmd_name, "PREPEND");
      break;
    case MemcacheParser::GET:
      strcpy(cmd_name, "MGET");
      break;
    case MemcacheParser::QUIT:
      strcpy(cmd_name, "QUIT");
      break;
    case MemcacheParser::STATS:
      server_family_.StatsMC(cmd.key, cntx);
      return;
    case MemcacheParser::VERSION:
      mc_builder->SendDirect(absl::StrCat("VERSION ", gflags::VersionString(), "\r\n"));
      return;
    default:
      mc_builder->SendClientError("bad command line format");
      return;
  }

  args.emplace_back(cmd_name, strlen(cmd_name));

  if (!cmd.key.empty()) {
    char* key = const_cast<char*>(cmd.key.data());
    args.emplace_back(key, cmd.key.size());
  }

  if (MemcacheParser::IsStoreCmd(cmd.type)) {
    char* v = const_cast<char*>(value.data());
    args.emplace_back(v, value.size());

    if (store_opt[0]) {
      args.emplace_back(store_opt, strlen(store_opt));
    }

    if (cmd.expire_ts && memcmp(cmd_name, "SET", 3) == 0) {
      char* next = absl::numbers_internal::FastIntToBuffer(cmd.expire_ts, ttl);
      args.emplace_back(ttl_op, 2);
      args.emplace_back(ttl, next - ttl);
    }
    cntx->conn_state.memcache_flag = cmd.flags;
  } else if (cmd.type < MemcacheParser::QUIT) {  // read commands
    for (auto s : cmd.keys_ext) {
      char* key = const_cast<char*>(s.data());
      args.emplace_back(key, s.size());
    }
  } else {  // write commands.
    if (store_opt[0]) {
      args.emplace_back(store_opt, strlen(store_opt));
    }
  }

  DispatchCommand(CmdArgList{args}, cntx);

  // Reset back.
  cntx->conn_state.memcache_flag = 0;
}

bool Service::IsLocked(DbIndex db_index, std::string_view key) const {
  ShardId sid = Shard(key, shard_count());
  KeyLockArgs args;
  args.db_index = db_index;
  args.args = ArgSlice{&key, 1};
  args.key_step = 1;
  bool is_open = pp_.at(sid)->AwaitBrief(
      [args] { return EngineShard::tlocal()->db_slice().CheckLock(IntentLock::EXCLUSIVE, args); });
  return !is_open;
}

bool Service::IsShardSetLocked() const {
  std::atomic_uint res{0};

  shard_set_.RunBriefInParallel([&](EngineShard* shard) {
    bool unlocked = shard->shard_lock()->Check(IntentLock::SHARED);
    res.fetch_add(!unlocked, memory_order_relaxed);
  });

  return res.load() != 0;
}

bool Service::IsPassProtected() const {
  return !FLAGS_requirepass.empty();
}

void Service::RegisterHttp(HttpListenerBase* listener) {
  CHECK_NOTNULL(listener);
  http_listener_ = listener;
}

void Service::Quit(CmdArgList args, ConnectionContext* cntx) {
  if (cntx->protocol() == Protocol::REDIS)
    (*cntx)->SendOk();

  SinkReplyBuilder* builder = static_cast<SinkReplyBuilder*>(cntx->reply_builder());
  builder->CloseConnection();
}

void Service::Multi(CmdArgList args, ConnectionContext* cntx) {
  if (cntx->conn_state.exec_state != ConnectionState::EXEC_INACTIVE) {
    return (*cntx)->SendError("MULTI calls can not be nested");
  }
  cntx->conn_state.exec_state = ConnectionState::EXEC_COLLECT;
  // TODO: to protect against huge exec transactions.
  return (*cntx)->SendOk();
}

void Service::CallFromScript(CmdArgList args, ObjectExplorer* reply, ConnectionContext* cntx) {
  DCHECK(cntx->transaction);
  InterpreterReplier replier(reply);
  ReplyBuilderInterface* orig = cntx->Inject(&replier);

  DispatchCommand(std::move(args), cntx);

  cntx->Inject(orig);
}

void Service::Eval(CmdArgList args, ConnectionContext* cntx) {
  uint32_t num_keys;

  CHECK(absl::SimpleAtoi(ArgS(args, 2), &num_keys));  // we already validated this

  string_view body = ArgS(args, 1);
  body = absl::StripAsciiWhitespace(body);

  if (body.empty()) {
    return (*cntx)->SendNull();
  }

  ServerState* ss = ServerState::tlocal();
  Interpreter& script = ss->GetInterpreter();

  string result;
  Interpreter::AddResult add_result = script.AddFunction(body, &result);
  if (add_result == Interpreter::COMPILE_ERR) {
    return (*cntx)->SendError(result);
  }

  if (add_result == Interpreter::ADD_OK) {
    server_family_.script_mgr()->InsertFunction(result, body);
  }

  EvalArgs eval_args;
  eval_args.sha = result;
  eval_args.keys = args.subspan(3, num_keys);
  eval_args.args = args.subspan(3 + num_keys);
  EvalInternal(eval_args, &script, cntx);
}

void Service::EvalSha(CmdArgList args, ConnectionContext* cntx) {
  string_view num_keys_str = ArgS(args, 2);
  uint32_t num_keys;

  CHECK(absl::SimpleAtoi(num_keys_str, &num_keys));

  ToLower(&args[1]);

  string_view sha = ArgS(args, 1);
  ServerState* ss = ServerState::tlocal();
  Interpreter& script = ss->GetInterpreter();
  bool exists = script.Exists(sha);

  if (!exists) {
    const char* body = (sha.size() == 40) ? server_family_.script_mgr()->Find(sha) : nullptr;
    if (!body) {
      return (*cntx)->SendError(kScriptNotFound);
    }

    string res;
    CHECK_EQ(Interpreter::ADD_OK, script.AddFunction(body, &res));
    CHECK_EQ(res, sha);
  }

  EvalArgs ev_args;
  ev_args.sha = sha;
  ev_args.keys = args.subspan(3, num_keys);
  ev_args.args = args.subspan(3 + num_keys);

  EvalInternal(ev_args, &script, cntx);
}

void Service::EvalInternal(const EvalArgs& eval_args, Interpreter* interpreter,
                           ConnectionContext* cntx) {
  DCHECK(!eval_args.sha.empty());

  // Sanitizing the input to avoid code injection.
  if (eval_args.sha.size() != 40 || !IsSHA(eval_args.sha)) {
    return (*cntx)->SendError(kScriptNotFound);
  }

  bool exists = interpreter->Exists(eval_args.sha);

  if (!exists) {
    const char* body = server_family_.script_mgr()->Find(eval_args.sha);
    if (!body) {
      return (*cntx)->SendError(kScriptNotFound);
    }

    string res;
    CHECK_EQ(Interpreter::ADD_OK, interpreter->AddFunction(body, &res));
    CHECK_EQ(res, eval_args.sha);
  }

  string error;

  DCHECK(!cntx->conn_state.script_info);  // we should not call eval from the script.

  // TODO: to determine whether the script is RO by scanning all "redis.p?call" calls
  // and checking whether all invocations consist of RO commands.
  // we can do it once during script insertion into script mgr.
  cntx->conn_state.script_info.emplace();
  for (size_t i = 0; i < eval_args.keys.size(); ++i) {
    cntx->conn_state.script_info->keys.insert(ArgS(eval_args.keys, i));
  }
  DCHECK(cntx->transaction);

  if (!eval_args.keys.empty())
    cntx->transaction->Schedule();

  auto lk = interpreter->Lock();

  interpreter->SetGlobalArray("KEYS", eval_args.keys);
  interpreter->SetGlobalArray("ARGV", eval_args.args);
  interpreter->SetRedisFunc(
      [cntx, this](CmdArgList args, ObjectExplorer* reply) { CallFromScript(args, reply, cntx); });

  Interpreter::RunResult result = interpreter->RunFunction(eval_args.sha, &error);

  cntx->conn_state.script_info.reset();  // reset script_info

  // Conclude the transaction.
  if (!eval_args.keys.empty())
    cntx->transaction->UnlockMulti();

  if (result == Interpreter::RUN_ERR) {
    string resp = absl::StrCat("Error running script (call to ", eval_args.sha, "): ", error);
    return (*cntx)->SendError(resp);
  }
  CHECK(result == Interpreter::RUN_OK);

  EvalSerializer ser{static_cast<RedisReplyBuilder*>(cntx->reply_builder())};

  if (!interpreter->IsResultSafe()) {
    (*cntx)->SendError("reached lua stack limit");
  } else {
    interpreter->SerializeResult(&ser);
  }
  interpreter->ResetStack();
}

void Service::Exec(CmdArgList args, ConnectionContext* cntx) {
  RedisReplyBuilder* rb = (*cntx).operator->();

  if (cntx->conn_state.exec_state == ConnectionState::EXEC_INACTIVE) {
    return rb->SendError("EXEC without MULTI");
  }

  if (cntx->conn_state.exec_state == ConnectionState::EXEC_ERROR) {
    cntx->conn_state.exec_state = ConnectionState::EXEC_INACTIVE;
    cntx->conn_state.exec_body.clear();
    return rb->SendError("-EXECABORT Transaction discarded because of previous errors");
  }

  rb->StartArray(cntx->conn_state.exec_body.size());
  if (!cntx->conn_state.exec_body.empty()) {
    CmdArgVec str_list;

    for (auto& scmd : cntx->conn_state.exec_body) {
      str_list.resize(scmd.cmd.size());
      for (size_t i = 0; i < scmd.cmd.size(); ++i) {
        string& s = scmd.cmd[i];
        str_list[i] = MutableSlice{s.data(), s.size()};
      }

      cntx->transaction->SetExecCmd(scmd.descr);
      CmdArgList cmd_arg_list{str_list.data(), str_list.size()};
      cntx->transaction->InitByArgs(cntx->conn_state.db_index, cmd_arg_list);
      scmd.descr->Invoke(cmd_arg_list, cntx);
      if (rb->GetError())
        break;
    }

    VLOG(1) << "Exec unlocking " << cntx->conn_state.exec_body.size() << " commands";
    cntx->transaction->UnlockMulti();
  }

  cntx->conn_state.exec_state = ConnectionState::EXEC_INACTIVE;
  cntx->conn_state.exec_body.clear();
  VLOG(1) << "Exec completed";
}

VarzValue::Map Service::GetVarzStats() {
  VarzValue::Map res;

  Metrics m = server_family_.GetMetrics();

  res.emplace_back("keys", VarzValue::FromInt(m.db.key_count));
  res.emplace_back("obj_mem_usage", VarzValue::FromInt(m.db.obj_memory_usage));
  double load = double(m.db.key_count) / (1 + m.db.bucket_count);
  res.emplace_back("table_load_factor", VarzValue::FromDouble(load));

  return res;
}

using ServiceFunc = void (Service::*)(CmdArgList, ConnectionContext* cntx);

#define HFUNC(x) SetHandler(&Service::x)
#define MFUNC(x) \
  SetHandler([this](CmdArgList sp, ConnectionContext* cntx) { this->x(std::move(sp), cntx); })

void Service::RegisterCommands() {
  using CI = CommandId;

  constexpr auto kExecMask = CO::LOADING | CO::NOSCRIPT | CO::GLOBAL_TRANS;

  registry_ << CI{"QUIT", CO::READONLY | CO::FAST, 1, 0, 0, 0}.HFUNC(Quit)
            << CI{"MULTI", CO::NOSCRIPT | CO::FAST | CO::LOADING, 1, 0, 0, 0}.HFUNC(Multi)
            << CI{"EVAL", CO::NOSCRIPT, -3, 0, 0, 0}.MFUNC(Eval).SetValidator(&EvalValidator)
            << CI{"EVALSHA", CO::NOSCRIPT, -3, 0, 0, 0}.MFUNC(EvalSha).SetValidator(&EvalValidator)
            << CI{"EXEC", kExecMask, 1, 0, 0, 0}.MFUNC(Exec);

  StringFamily::Register(&registry_);
  GenericFamily::Register(&registry_);
  ListFamily::Register(&registry_);
  server_family_.Register(&registry_);

  LOG(INFO) << "Multi-key commands are: ";

  registry_.Traverse([](std::string_view key, const CI& cid) {
    if (cid.is_multi_key()) {
      string key_len;
      if (cid.last_key_pos() < 0)
        key_len = "unlimited";
      else
        key_len = absl::StrCat(cid.last_key_pos() - cid.first_key_pos() + 1);
      LOG(INFO) << "    " << key << ": with " << key_len << " keys";
    }
  });
}

}  // namespace dfly