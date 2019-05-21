#include "worker.h"

#include <glog/logging.h>
#include <list>
#include <cctype>
#include <utility>
#include <algorithm>

#include "redis_request.h"
#include "redis_connection.h"
#include "server.h"
#include "util.h"

Worker::Worker(Server *svr, Config *config, bool repl) : svr_(svr), repl_(repl) {
  base_ = event_base_new();
  if (!base_) throw std::exception();

  timer_ = event_new(base_, -1, EV_PERSIST, TimerCB, this);
  timeval tm = {10, 0};
  evtimer_add(timer_, &tm);

  int port = repl ? config->repl_port : config->port;
  auto binds = repl ? config->repl_binds : config->binds;
  for (const auto &bind : binds) {
    Status s = listen(bind, port, config->backlog);
    if (!s.IsOK()) {
      LOG(ERROR) << "[worker] Failed to listen on: "<< bind << ":" << port
                 << ", encounter error: " << s.Msg();
      exit(1);
    }
  }
}

Worker::~Worker() {
  std::list<Redis::Connection*> conns;
  for (const auto &iter : conns_) {
    conns.emplace_back(iter.second);
  }
  for (const auto &iter : conns) {
    iter->Close();
  }
  event_free(timer_);
  if (rate_limit_group_ != nullptr) {
    bufferevent_rate_limit_group_free(rate_limit_group_);
  }
  if (rate_limit_group_cfg_ != nullptr) {
    ev_token_bucket_cfg_free(rate_limit_group_cfg_);
  }
  event_base_free(base_);
}

void Worker::TimerCB(int, int16_t events, void *ctx) {
  auto worker = static_cast<Worker*>(ctx);
  auto config = worker->svr_->GetConfig();
  if (config->timeout == 0) return;
  worker->KickoutIdleClients(config->timeout);
}

void Worker::newConnection(evconnlistener *listener, evutil_socket_t fd,
                           sockaddr *address, int socklen, void *ctx) {
  auto worker = static_cast<Worker *>(ctx);
  if (worker->IsRepl()) {
    DLOG(INFO) << "[worker] New connection: fd=" << fd
               << " from port: " << worker->svr_->GetConfig()->repl_port << " thread #"
               << worker->tid_;
  } else {
    DLOG(INFO) << "[worker] New connection: fd=" << fd
               << " from port: " << worker->svr_->GetConfig()->port << " thread #"
               << worker->tid_;
  }
  int enable = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<void*>(&enable), sizeof(enable)) < 0) {
    LOG(ERROR) << "[worker] Failed to set tcp-keepalive, err:" << evutil_socket_geterror(fd);
    evutil_closesocket(fd);
    return;
  }
  event_base *base = evconnlistener_get_base(listener);
  bufferevent *bev = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);
  auto conn = new Redis::Connection(bev, worker);
  bufferevent_setcb(bev, Redis::Connection::OnRead, Redis::Connection::OnWrite,
                    Redis::Connection::OnEvent, conn);
  bufferevent_enable(bev, EV_READ);
  Status status = worker->AddConnection(conn);
  std::string ip;
  uint32_t port;
  if (Util::GetPeerAddr(fd, &ip, &port) == 0) {
    conn->SetAddr(ip, port);
  }
  if (!status.IsOK()) {
    std::string err_msg = Redis::Error("ERR " + status.Msg());
    write(fd, err_msg.data(), err_msg.size());
    conn->Close();
  }

  if (worker->rate_limit_group_ != nullptr) {
    bufferevent_add_to_rate_limit_group(bev, worker->rate_limit_group_);
  }
}

Status Worker::listen(const std::string &host, int port, int backlog) {
  sockaddr_in sin{};
  sin.sin_family = AF_INET;
  evutil_inet_pton(AF_INET, host.data(), &(sin.sin_addr));
  sin.sin_port = htons(port);
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  int sock_opt = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &sock_opt, sizeof(sock_opt)) < 0) {
    return Status(Status::NotOK, evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
  }
  // to support multi-thread binding on macOS
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &sock_opt, sizeof(sock_opt)) < 0) {
    return Status(Status::NotOK, evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
  }
  if (bind(fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
    return Status(Status::NotOK, evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
  }
  evutil_make_socket_nonblocking(fd);
  auto lev = evconnlistener_new(base_, newConnection, this,
                                LEV_OPT_CLOSE_ON_FREE, backlog, fd);
  listen_events_.emplace_back(lev);
  return Status::OK();
}

void Worker::Run(std::thread::id tid) {
  tid_ = tid;
  if (event_base_dispatch(base_) != 0) {
    LOG(ERROR) << "[worker] Failed to run server, err: " << strerror(errno);
  }
}

void Worker::Stop() {
  event_base_loopbreak(base_);
  for (const auto &lev : listen_events_) {
    evutil_socket_t fd = evconnlistener_get_fd(lev);
    if (fd > 0) close(fd);
    evconnlistener_free(lev);
  }
}

Status Worker::AddConnection(Redis::Connection *c) {
  std::unique_lock<std::mutex> lock(conns_mu_);
  auto iter = conns_.find(c->GetFD());
  if (iter != conns_.end()) {
    return Status(Status::NotOK, "connection was exists");
  }
  int max_clients = svr_->GetConfig()->maxclients;
  if (svr_->IncrClientNum() >= max_clients) {
    svr_->DecrClientNum();
    return Status(Status::NotOK, "max number of clients reached");
  }
  conns_.insert(std::pair<int, Redis::Connection*>(c->GetFD(), c));
  uint64_t id = svr_->GetClientID()->fetch_add(1, std::memory_order_relaxed);
  c->SetID(id);
  return Status::OK();
}


Redis::Connection *Worker::removeConnection(int fd) {
  Redis::Connection *conn = nullptr;
  std::unique_lock<std::mutex> lock(conns_mu_);
  auto iter = conns_.find(fd);
  if (iter != conns_.end()) {
    conn = iter->second;
    conns_.erase(iter);
    svr_->DecrClientNum();
  }
  iter = monitor_conns_.find(fd);
  if (iter != monitor_conns_.end()) {
    conn = iter->second;
    monitor_conns_.erase(iter);
    svr_->DecrClientNum();
    svr_->DecrMonitorClientNum();
  }
  return conn;
}

void Worker::DetachConnection(Redis::Connection *conn) {
  if (!conn) return;
  removeConnection(conn->GetFD());
  if (rate_limit_group_ != nullptr) {
    bufferevent_remove_from_rate_limit_group(conn->GetBufferEvent());
  }
  auto bev = conn->GetBufferEvent();
  bufferevent_disable(bev, EV_READ|EV_WRITE);
  bufferevent_setcb(bev, nullptr, nullptr, nullptr, nullptr);
}

void Worker::FreeConnection(Redis::Connection *conn) {
  if (!conn) return;
  removeConnection(conn->GetFD());
  if (rate_limit_group_ != nullptr) {
    bufferevent_remove_from_rate_limit_group(conn->GetBufferEvent());
  }
  delete conn;
}

void Worker::FreeConnectionByID(int fd, uint64_t id) {
  std::unique_lock<std::mutex> lock(conns_mu_);
  auto iter = conns_.find(fd);
  if (iter != conns_.end() && iter->second->GetID() == id) {
    if (rate_limit_group_ != nullptr) {
      bufferevent_remove_from_rate_limit_group(iter->second->GetBufferEvent());
    }
    delete iter->second;
    conns_.erase(iter);
    svr_->DecrClientNum();
  }
  iter = monitor_conns_.find(fd);
  if (iter != monitor_conns_.end() && iter->second->GetID() == id) {
    delete iter->second;
    monitor_conns_.erase(iter);
    svr_->DecrClientNum();
    svr_->DecrMonitorClientNum();
  }
}

Status Worker::EnableWriteEvent(int fd) {
  std::unique_lock<std::mutex> lock(conns_mu_);
  auto iter = conns_.find(fd);
  if (iter != conns_.end()) {
    auto bev = iter->second->GetBufferEvent();
    bufferevent_enable(bev, EV_WRITE);
    return Status::OK();
  }
  return Status(Status::NotOK);
}

Status Worker::Reply(int fd, const std::string &reply) {
  std::unique_lock<std::mutex> lock(conns_mu_);
  auto iter = conns_.find(fd);
  if (iter != conns_.end()) {
    Redis::Reply(iter->second->Output(), reply);
    return Status::OK();
  }
  return Status(Status::NotOK, "connection doesn't exist");
}

int Worker::SetReplicationRateLimit(uint64_t max_replication_bytes) {
  auto write_limit = EV_RATE_LIMIT_MAX;
  if (max_replication_bytes > 0) {
    write_limit = max_replication_bytes;
  }
  struct timeval cfg_tick = {1, 0};
  auto old_cfg = rate_limit_group_cfg_;
  rate_limit_group_cfg_ = ev_token_bucket_cfg_new(
      EV_RATE_LIMIT_MAX, EV_RATE_LIMIT_MAX,
      write_limit, write_limit,
      &cfg_tick);
  if (rate_limit_group_cfg_ == nullptr) {
    LOG(ERROR) << "[server] ev_token_bucket_cfg_new error";
    rate_limit_group_cfg_ = old_cfg;
    return -1;
  }

  if (rate_limit_group_ != nullptr) {
    bufferevent_rate_limit_group_set_cfg(rate_limit_group_, rate_limit_group_cfg_);
  } else {
    rate_limit_group_ = bufferevent_rate_limit_group_new(base_, rate_limit_group_cfg_);
  }

  if (old_cfg != nullptr) {
    ev_token_bucket_cfg_free(old_cfg);
  }

  return 0;
}

void Worker::BecomeMonitorConn(Redis::Connection *conn) {
  conns_mu_.lock();
  conns_.erase(conn->GetFD());
  monitor_conns_[conn->GetFD()] = conn;
  conns_mu_.unlock();
  svr_->IncrMonitorClientNum();
  conn->EnableFlag(Redis::Connection::kMonitor);
}

void Worker::FeedMonitorConns(Redis::Connection *conn, const std::vector<std::string> &tokens) {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  std::string output;
  output += std::to_string(tv.tv_sec) + "." + std::to_string(tv.tv_usec);
  output += " [" + conn->GetNamespace() + " " + conn->GetAddr() + "]";
  for (const auto &token : tokens) {
    output += " \"" + token + "\"";
  }
  std::unique_lock<std::mutex> lock(conns_mu_);
  for (const auto &iter : monitor_conns_) {
    if (conn == iter.second) continue;  // skip the monitor command
    if (conn->GetNamespace() == iter.second->GetNamespace()
        || iter.second->GetNamespace() == kDefaultNamespace) {
      iter.second->Reply(Redis::SimpleString(output));
    }
  }
}

std::string Worker::GetClientsStr() {
  std::unique_lock<std::mutex> lock(conns_mu_);
  std::string output;
  for (const auto iter : conns_) {
    Redis::Connection *conn = iter.second;
    output.append(conn->ToString());
  }
  return output;
}

void Worker::KillClient(Redis::Connection *self, uint64_t id, std::string addr, bool skipme, int64_t *killed) {
  conns_mu_.lock();
  for (const auto iter : conns_) {
    Redis::Connection* conn = iter.second;
    if (skipme && self == conn) continue;
    if ((!addr.empty() && conn->GetAddr() == addr) || (id != 0 && conn->GetID() == id)) {
      conn->EnableFlag(Redis::Connection::kCloseAfterReply);
      // enable write event to notify worker wake up ASAP, and remove the connection
      if (!conn->IsFlagEnabled(Redis::Connection::kSlave)) {  // don't enable any event in slave connection
        auto bev = conn->GetBufferEvent();
        bufferevent_enable(bev, EV_WRITE);
      }
      (*killed)++;
    }
  }
  conns_mu_.unlock();
}

void Worker::KickoutIdleClients(int timeout) {
  conns_mu_.lock();
  std::list<std::pair<int, uint64_t>> to_be_killed_conns;
  if (conns_.empty()) {
    conns_mu_.unlock();
    return;
  }
  int iterations = std::min(static_cast<int>(conns_.size()), 50);
  auto iter = conns_.upper_bound(last_iter_conn_fd);
  while (iterations--) {
    if (iter == conns_.end()) iter = conns_.begin();
    if (static_cast<int>(iter->second->GetIdleTime()) >= timeout) {
      to_be_killed_conns.emplace_back(std::make_pair(iter->first, iter->second->GetID()));
    }
    iter++;
  }
  iter--;
  last_iter_conn_fd = iter->first;
  conns_mu_.unlock();

  for (const auto conn : to_be_killed_conns) {
    FreeConnectionByID(conn.first, conn.second);
  }
}

void WorkerThread::Start() {
  try {
    t_ = std::thread([this]() {
      if (this->worker_->IsRepl()) {
        Util::ThreadSetName("repl-worker");
      } else {
        Util::ThreadSetName("worker");
      }
      this->worker_->Run(t_.get_id());
    });
  } catch (const std::system_error &e) {
    LOG(ERROR) << "[worker] Failed to start worker thread, err: " << e.what();
    return;
  }
  LOG(INFO) << "[worker] Thread #" << t_.get_id() << " started";
}

void WorkerThread::Stop() {
  worker_->Stop();
}

void WorkerThread::Join() {
  if (t_.joinable()) t_.join();
}
