/*
 * Spdylay - SPDY Library
 *
 * Copyright (c) 2012 Tatsuhiro Tsujikawa
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "SpdyServer.h"

#include <sys/stat.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <set>
#include <iostream>

#include <openssl/err.h>

#include "spdylay_ssl.h"
#include "uri.h"
#include "util.h"
#include "EventPoll.h"

namespace spdylay {

namespace {
Config config;
const std::string STATUS_200 = "200 OK";
const std::string STATUS_304 = "304 Not Modified";
const std::string STATUS_400 = "400 Bad Request";
const std::string STATUS_404 = "404 Not Found";
const std::string DEFAULT_HTML = "index.html";
const std::string SPDYD_SERVER = "spdyd spdylay/"SPDYLAY_VERSION;
} // namespace

Config::Config(): verbose(false), daemon(false), port(0), data_ptr(0)
{}

Request::Request(int32_t stream_id)
  : stream_id(stream_id),
    file(-1)
{}

Request::~Request()
{
  if(file != -1) {
    close(file);
  }
}

EventHandler::EventHandler(const Config *config)
  : config_(config),
    mark_del_(false)
{}

namespace {
void on_close(Sessions &sessions, EventHandler *hd);
} // namespace

class Sessions {
public:
  Sessions(int max_events, SSL_CTX *ssl_ctx)
    : eventPoll_(max_events),
      ssl_ctx_(ssl_ctx)
  {}
  ~Sessions()
  {
    for(std::set<EventHandler*>::iterator i = handlers_.begin(),
          eoi = handlers_.end(); i != eoi; ++i) {
      on_close(*this, *i);
      delete *i;
    }
    SSL_CTX_free(ssl_ctx_);
  }
  void add_handler(EventHandler *handler)
  {
    handlers_.insert(handler);
  }
  void remove_handler(EventHandler *handler)
  {
    handlers_.erase(handler);
  }
  SSL* ssl_session_new(int fd)
  {
    SSL *ssl = SSL_new(ssl_ctx_);
    if(SSL_set_fd(ssl, fd) == 0) {
      SSL_free(ssl);
      return 0;
    }
    return ssl;
  }
  int add_poll(EventHandler *handler)
  {
    return update_poll_internal(handler, EP_ADD);
  }
  int mod_poll(EventHandler *handler)
  {
    return update_poll_internal(handler, EP_MOD);
  }
  int poll(int timeout)
  {
    return eventPoll_.poll(timeout);
  }
  void* get_user_data(int p)
  {
    return eventPoll_.get_user_data(p);
  }
  int get_events(int p)
  {
    return eventPoll_.get_events(p);
  }
private:
  int update_poll_internal(EventHandler *handler, int op)
  {
    int events = 0;
    if(handler->want_read()) {
      events |= EP_POLLIN;
    }
    if(handler->want_write()) {
      events |= EP_POLLOUT;
    }
    return eventPoll_.ctl_event(op, handler->fd(), events, handler);
  }

  std::set<EventHandler*> handlers_;
  EventPoll eventPoll_;
  SSL_CTX *ssl_ctx_;
};

namespace {
void print_session_id(int64_t id)
{
  std::cout << "[id=" << id << "] ";
}
} // namespace

namespace {
void on_session_closed(EventHandler *hd, int64_t session_id)
{
  if(hd->config()->verbose) {
    print_session_id(session_id);
    print_timer();
    std::cout << " closed" << std::endl;
  }
}
} // namespace

SpdyEventHandler::SpdyEventHandler(const Config* config,
                                   int fd, SSL *ssl,
                                   const spdylay_session_callbacks *callbacks,
                                   int64_t session_id)
  : EventHandler(config),
    fd_(fd), ssl_(ssl), session_id_(session_id), want_write_(false)
{
  spdylay_session_server_new(&session_, callbacks, this);
}
    
SpdyEventHandler::~SpdyEventHandler()
{
  on_session_closed(this, session_id_);
  spdylay_session_del(session_);
  for(std::map<int32_t, Request*>::iterator i = id2req_.begin(),
        eoi = id2req_.end(); i != eoi; ++i) {
    delete (*i).second;
  }
  SSL_shutdown(ssl_);
  SSL_free(ssl_);
  shutdown(fd_, SHUT_WR);
  close(fd_);
}

int SpdyEventHandler::execute(Sessions *sessions)
{
  int r;
  r = spdylay_session_recv(session_);
  if(r == 0) {
    r = spdylay_session_send(session_);
  }
  return r;
}

bool SpdyEventHandler::want_read()
{
  return spdylay_session_want_read(session_);
}

bool SpdyEventHandler::want_write()
{
  return spdylay_session_want_write(session_) || want_write_;
}

int SpdyEventHandler::fd() const
{
  return fd_;
}

bool SpdyEventHandler::finish()
{
  return !want_read() && !want_write();
}

ssize_t SpdyEventHandler::send_data(const uint8_t *data, size_t len, int flags)
{
  ssize_t r;
  r = SSL_write(ssl_, data, len);
  return r;
}

ssize_t SpdyEventHandler::recv_data(uint8_t *data, size_t len, int flags)
{
  ssize_t r;
  want_write_ = false;
  r = SSL_read(ssl_, data, len);
  if(r < 0) {
    if(SSL_get_error(ssl_, r) == SSL_ERROR_WANT_WRITE) {
      want_write_ = true;
    }
  }
  return r;
}

bool SpdyEventHandler::would_block(int r)
{
  int e = SSL_get_error(ssl_, r);
  return e == SSL_ERROR_WANT_WRITE || e == SSL_ERROR_WANT_READ;
}

int SpdyEventHandler::submit_file_response(const std::string& status,
                                           int32_t stream_id,
                                           time_t last_modified,
                                           off_t file_length,
                                           spdylay_data_provider *data_prd)
{
  const char *nv[] = {
    "status", status.c_str(),
    "version", "HTTP/1.1",
    "server", SPDYD_SERVER.c_str(),
    "content-length", util::to_str(file_length).c_str(),
    "cache-control", "max-age=3600",
    "date", util::http_date(time(0)).c_str(),
    0, 0,
    0
  };
  if(last_modified != 0) {
    nv[12] = "last-modified";
    nv[13] = util::http_date(last_modified).c_str();
  }
  return spdylay_submit_response(session_, stream_id, nv, data_prd);
}

int SpdyEventHandler::submit_response
(const std::string& status,
 int32_t stream_id,
 const std::vector<std::pair<std::string, std::string> >& headers,
 spdylay_data_provider *data_prd)
{
  const char **nv = new const char*[8+headers.size()*2+1];
  nv[0] = "status";
  nv[1] = status.c_str();
  nv[2] = "version";
  nv[3] = "HTTP/1.1";
  nv[4] = "server";
  nv[5] = SPDYD_SERVER.c_str();
  nv[6] = "date";
  nv[7] = util::http_date(time(0)).c_str();
  for(int i = 0; i < (int)headers.size(); ++i) {
    nv[8+i*2] = headers[i].first.c_str();
    nv[8+i*2+1] = headers[i].second.c_str();
  }
  nv[8+headers.size()*2] = 0;
  int r = spdylay_submit_response(session_, stream_id, nv, data_prd);
  delete [] nv;
  return r;
}

int SpdyEventHandler::submit_response(const std::string& status,
                                      int32_t stream_id,
                                      spdylay_data_provider *data_prd)
{
  const char *nv[] = {
    "status", status.c_str(),
    "version", "HTTP/1.1",
    "server", SPDYD_SERVER.c_str(),
    0
  };
  return spdylay_submit_response(session_, stream_id, nv, data_prd);
}

void SpdyEventHandler::add_stream(int32_t stream_id, Request *req)
{
  id2req_[stream_id] = req;
}

void SpdyEventHandler::remove_stream(int32_t stream_id)
{
  Request *req = id2req_[stream_id];
  id2req_.erase(stream_id);
  delete req;
}

Request* SpdyEventHandler::get_stream(int32_t stream_id)
{
  return id2req_[stream_id];
}

int64_t SpdyEventHandler::session_id() const
{
  return session_id_;
}

namespace {
ssize_t hd_send_callback(spdylay_session *session,
                         const uint8_t *data, size_t len, int flags,
                         void *user_data)
{
  SpdyEventHandler *hd = (SpdyEventHandler*)user_data;
  ssize_t r = hd->send_data(data, len, flags);
  if(r < 0) {
    if(hd->would_block(r)) {
      r = SPDYLAY_ERR_WOULDBLOCK;
    } else {
      r = SPDYLAY_ERR_CALLBACK_FAILURE;
    }
  }
  return r;
}
} // namespace

namespace {
ssize_t hd_recv_callback(spdylay_session *session,
                         uint8_t *data, size_t len, int flags, void *user_data)
{
  SpdyEventHandler *hd = (SpdyEventHandler*)user_data;
  ssize_t r = hd->recv_data(data, len, flags);
  if(r < 0) {
    if(hd->would_block(r)) {
      r = SPDYLAY_ERR_WOULDBLOCK;
    } else {
      r = SPDYLAY_ERR_CALLBACK_FAILURE;
    }
  } else if(r == 0) {
    r = SPDYLAY_ERR_CALLBACK_FAILURE;
  }
  return r;
}
} // namespace

ssize_t file_read_callback
(spdylay_session *session, int32_t stream_id,
 uint8_t *buf, size_t length, int *eof,
 spdylay_data_source *source, void *user_data)
{
  int fd = source->fd;
  ssize_t r;
  while((r = read(fd, buf, length)) == -1 && errno == EINTR);
  if(r == -1) {
    return SPDYLAY_ERR_CALLBACK_FAILURE;
  } else {
    if(r == 0) {
      *eof = 1;
    }
    return r;
  }
}

namespace {
bool check_url(const std::string& url)
{
  // We don't like '\' in url.
  return !url.empty() && url[0] == '/' &&
    url.find('\\') == std::string::npos &&
    url.find("/../") == std::string::npos &&
    url.find("/./") == std::string::npos &&
    !util::endsWith(url, "/..") && !util::endsWith(url, "/.");
}
} // namespace

namespace {
void prepare_status_response(Request *req, SpdyEventHandler *hd,
                             const std::string& status)
{
  int pipefd[2];
  if(pipe(pipefd) == -1) {
    hd->submit_response(status, req->stream_id, 0);
  } else {
    std::stringstream ss;
    ss << "<html><head><title>" << status << "</title></head><body>"
       << "<h1>" << status << "</h1>"
       << "<hr>"
       << "<address>" << SPDYD_SERVER << " at port " << hd->config()->port
       << "</address>"
       << "</body></html>";
    std::string body = ss.str();
    write(pipefd[1], body.c_str(), body.size());
    close(pipefd[1]);

    req->file = pipefd[0];
    spdylay_data_provider data_prd;
    data_prd.source.fd = pipefd[0];
    data_prd.read_callback = file_read_callback;
    hd->submit_file_response(status, req->stream_id, 0, body.size(), &data_prd);
  }
}
} // namespace

namespace {
void prepare_response(Request *req, SpdyEventHandler *hd)
{
  std::string url;
  bool url_found = false;
  bool method_found = false;
  bool scheme_found = false;
  bool version_found = false;
  time_t last_mod = 0;
  bool last_mod_found = false;
  for(int i = 0; i < (int)req->headers.size(); ++i) {
    const std::string &field = req->headers[i].first;
    const std::string &value = req->headers[i].second;
    if(!url_found && field == "url") {
      url_found = true;
      url = value;
    } else if(field == "method") {
      method_found = true;
    } else if(field == "scheme") {
      scheme_found = true;
    } else if(field == "version") {
      version_found = true;
    } else if(!last_mod_found && field == "if-modified-since") {
      last_mod_found = true;
      last_mod = util::parse_http_date(value);
    }
  }
  if(!url_found || !method_found || !scheme_found || !version_found) {
    prepare_status_response(req, hd, STATUS_400);
    return;
  }
  std::string::size_type query_pos = url.find("?");
  if(query_pos != std::string::npos) {
    url = url.substr(0, query_pos);
  }
  url = util::percentDecode(url.begin(), url.end());
  if(!check_url(url)) {
    prepare_status_response(req, hd, STATUS_404);
    return;
  }
  std::string path = hd->config()->htdocs+url;
  if(path[path.size()-1] == '/') {
    path += DEFAULT_HTML;
  }
  int file = open(path.c_str(), O_RDONLY);
  if(file == -1) {
    prepare_status_response(req, hd, STATUS_404);
  } else {
    struct stat buf;
    if(fstat(file, &buf) == -1) {
      close(file);
      prepare_status_response(req, hd, STATUS_404);
    } else {
      req->file = file;
      spdylay_data_provider data_prd;
      data_prd.source.fd = file;
      data_prd.read_callback = file_read_callback;
      if(last_mod_found && buf.st_mtime <= last_mod) {
        prepare_status_response(req, hd, STATUS_304);
      } else {
        hd->submit_file_response(STATUS_200, req->stream_id, buf.st_mtime,
                                 buf.st_size, &data_prd);
      }
    }
  }
}
} // namespace

namespace {
void append_nv(Request *req, char **nv)
{
  for(int i = 0; nv[i]; i += 2) {
    req->headers.push_back(std::make_pair(nv[i], nv[i+1]));
  }
}
} // namespace

namespace {
void hd_on_ctrl_recv_callback
(spdylay_session *session, spdylay_frame_type type, spdylay_frame *frame,
 void *user_data)
{
  SpdyEventHandler *hd = (SpdyEventHandler*)user_data;
  if(hd->config()->verbose) {
    print_session_id(hd->session_id());
    on_ctrl_recv_callback(session, type, frame, user_data);
  }
  switch(type) {
  case SPDYLAY_SYN_STREAM: {
    int32_t stream_id = frame->syn_stream.stream_id;
    Request *req = new Request(stream_id);
    append_nv(req, frame->syn_stream.nv);
    hd->add_stream(stream_id, req);
    break;
  }
  case SPDYLAY_HEADERS: {
    int32_t stream_id = frame->headers.stream_id;
    Request *req = hd->get_stream(stream_id);
    append_nv(req, frame->headers.nv);
    break;
  }
  default:
    break;
  }
}
} // namespace

void htdocs_on_request_recv_callback
(spdylay_session *session, int32_t stream_id, void *user_data)
{
  SpdyEventHandler *hd = (SpdyEventHandler*)user_data;
  prepare_response(hd->get_stream(stream_id), hd);
}

namespace {
void hd_on_ctrl_send_callback
(spdylay_session *session, spdylay_frame_type type, spdylay_frame *frame,
 void *user_data)
{
  SpdyEventHandler *hd = (SpdyEventHandler*)user_data;
  if(hd->config()->verbose) {
    print_session_id(hd->session_id());
    on_ctrl_send_callback(session, type, frame, user_data);
  }
}
} // namespace

namespace {
void on_data_chunk_recv_callback
(spdylay_session *session, uint8_t flags, int32_t stream_id,
 const uint8_t *data, size_t len, void *user_data)
{
  // TODO Handle POST
}
} // namespace

namespace {
void hd_on_data_recv_callback
(spdylay_session *session, uint8_t flags, int32_t stream_id, int32_t length,
 void *user_data)
{
  // TODO Handle POST
  SpdyEventHandler *hd = (SpdyEventHandler*)user_data;
  if(hd->config()->verbose) {
    print_session_id(hd->session_id());
    on_data_recv_callback(session, flags, stream_id, length, user_data);
  }
}
} // namespace

namespace {
void hd_on_data_send_callback
(spdylay_session *session, uint8_t flags, int32_t stream_id, int32_t length,
 void *user_data)
{
  SpdyEventHandler *hd = (SpdyEventHandler*)user_data;
  if(hd->config()->verbose) {
    print_session_id(hd->session_id());
    on_data_send_callback(session, flags, stream_id, length, user_data);
  }
}
} // namespace

namespace {
void on_stream_close_callback
(spdylay_session *session, int32_t stream_id, spdylay_status_code status_code,
 void *user_data)
{
  SpdyEventHandler *hd = (SpdyEventHandler*)user_data;
  hd->remove_stream(stream_id);
  if(hd->config()->verbose) {
    print_session_id(hd->session_id());
    print_timer();
    printf(" stream_id=%d closed\n", stream_id);
    fflush(stdout);
  }
}
} // namespace

class SSLAcceptEventHandler : public EventHandler {
public:
  SSLAcceptEventHandler(const Config *config,
                        int fd, SSL *ssl, int64_t session_id)
    : EventHandler(config),
      fd_(fd), ssl_(ssl), fail_(false), finish_(false),
      want_read_(true), want_write_(true),
      session_id_(session_id)
  {}
  virtual ~SSLAcceptEventHandler()
  {
    if(fail_) {
      on_session_closed(this, session_id_);
      SSL_shutdown(ssl_);
      SSL_free(ssl_);
      shutdown(fd_, SHUT_WR);
      close(fd_);
    }
  }
  virtual int execute(Sessions *sessions)
  {
    want_read_ = want_write_ = false;
    int r = SSL_accept(ssl_);
    if(r == 1) {
      finish_ = true;
      const unsigned char *next_proto = 0;
      unsigned int next_proto_len;
      SSL_get0_next_proto_negotiated(ssl_, &next_proto, &next_proto_len);
      if(next_proto) {
        std::string proto(next_proto, next_proto+next_proto_len);
        if(config()->verbose) {
          std::cout << "The negotiated next protocol: " << proto << std::endl;
        }
        if(proto == "spdy/2") {
          add_next_handler(sessions);
        } else {
          fail_ = true;
        }
      } else {
        fail_ = true;
      }
    } else if(r == 0) {
      int e = SSL_get_error(ssl_, r);
      if(e == SSL_ERROR_SSL) {
        if(config()->verbose) {
          std::cerr << ERR_error_string(ERR_get_error(), 0) << std::endl;
        }
      }
      finish_ = true;
      fail_ = true;
    } else {
      int d = SSL_get_error(ssl_, r);
      if(d == SSL_ERROR_WANT_READ) {
        want_read_ = true;
      } else if(d == SSL_ERROR_WANT_WRITE) {
        want_write_ = true;
      } else {
        finish_ = true;
        fail_ = true;
      }
    }
    return 0;
  }
  virtual bool want_read()
  {
    return want_read_;
  }
  virtual bool want_write()
  {
    return want_write_;
  }
  virtual int fd() const
  {
    return fd_;
  }
  virtual bool finish()
  {
    return finish_;
  }
private:
  void add_next_handler(Sessions *sessions)
  {
    spdylay_session_callbacks callbacks;
    memset(&callbacks, 0, sizeof(spdylay_session_callbacks));
    callbacks.send_callback = hd_send_callback;
    callbacks.recv_callback = hd_recv_callback;
    callbacks.on_stream_close_callback = on_stream_close_callback;
    callbacks.on_ctrl_recv_callback = hd_on_ctrl_recv_callback;
    callbacks.on_ctrl_send_callback = hd_on_ctrl_send_callback;
    callbacks.on_data_recv_callback = hd_on_data_recv_callback;
    callbacks.on_data_send_callback = hd_on_data_send_callback;
    callbacks.on_data_chunk_recv_callback = on_data_chunk_recv_callback;
    callbacks.on_request_recv_callback = config()->on_request_recv_callback;
    SpdyEventHandler *hd = new SpdyEventHandler(config(),
                                                fd_, ssl_, &callbacks,
                                                session_id_);
    if(sessions->mod_poll(hd) == -1) {
      // fd_, ssl_ are freed by ~SpdyEventHandler()
      delete hd;
    } else {
      sessions->add_handler(hd);
    }
  }
  
  int fd_;
  SSL *ssl_;
  bool fail_, finish_;
  bool want_read_, want_write_;
  int64_t session_id_;
};

class ListenEventHandler : public EventHandler {
public:
  ListenEventHandler(const Config* config,
                     int fd, int64_t *session_id_seed_ptr)
    : EventHandler(config),
      fd_(fd), session_id_seed_ptr_(session_id_seed_ptr) {}
  virtual ~ListenEventHandler()
  {}
  virtual int execute(Sessions *sessions)
  {
    int cfd;
    while((cfd = accept(fd_, 0, 0)) == -1 && errno == EINTR);
    if(cfd != -1) {
      if(make_non_block(cfd) == -1 ||
         set_tcp_nodelay(cfd) == -1) {
        close(cfd);
      } else {
        add_next_handler(sessions, cfd);
      }
    }
    return 0;
  }
  virtual bool want_read()
  {
    return true;
  }
  virtual bool want_write()
  {
    return false;
  }
  virtual int fd() const
  {
    return fd_;
  }
  virtual bool finish()
  {
    return false;
  }
private:
  void add_next_handler(Sessions *sessions, int cfd)
  {
    SSL *ssl = sessions->ssl_session_new(cfd);
    if(ssl == 0) {
      close(cfd);
      return;
    }
    SSLAcceptEventHandler *hd = new SSLAcceptEventHandler
      (config(), cfd, ssl, ++(*session_id_seed_ptr_));
    if(sessions->add_poll(hd) == -1) {
      delete hd;
      SSL_free(ssl);
      close(cfd);
    } else {
      sessions->add_handler(hd);
    }
  }

  int fd_;
  int64_t *session_id_seed_ptr_;
};

namespace {
void on_close(Sessions &sessions, EventHandler *hd)
{
  sessions.remove_handler(hd);
  delete hd;
}
} // namespace

SpdyServer::SpdyServer(const Config *config)
  : config_(config)
{
  memset(sfd_, -1, sizeof(sfd_));
}

SpdyServer::~SpdyServer()
{
  for(int i = 0; i < 2; ++i) {
    if(sfd_[i] != -1) {
      close(sfd_[i]);
    }
  }
}

int SpdyServer::listen()
{
  int families[] = { AF_INET, AF_INET6 };
  bool bind_ok = false;
  for(int i = 0; i < 2; ++i) {
    const char* ipv = (families[i] == AF_INET ? "IPv4" : "IPv6");
    int sfd = make_listen_socket(config_->host, config_->port, families[i]);
    if(sfd == -1) {
      std::cerr << ipv << ": Could not listen on port " << config_->port
                << std::endl;
      continue;
    }
    make_non_block(sfd);
    sfd_[i] = sfd;
    if(config_->verbose) {
      std::cout << ipv << ": listen on port " << config_->port << std::endl;
    }
    bind_ok = true;
  }
  if(!bind_ok) {
    return -1;
  }
  return 0;
}

namespace {
int next_proto_cb(SSL *s, const unsigned char **data, unsigned int *len,
                  void *arg)
{
  std::pair<unsigned char*, size_t> *next_proto =
    reinterpret_cast<std::pair<unsigned char*, size_t>* >(arg);
  *data = next_proto->first;
  *len = next_proto->second;
  return SSL_TLSEXT_ERR_OK;
}
} // namespace

int SpdyServer::run()
{
  SSL_CTX *ssl_ctx;
  ssl_ctx = SSL_CTX_new(SSLv23_server_method());
  if(!ssl_ctx) {
    std::cerr << ERR_error_string(ERR_get_error(), 0) << std::endl;
    return -1;
  }
  SSL_CTX_set_options(ssl_ctx, SSL_OP_ALL|SSL_OP_NO_SSLv2);
  SSL_CTX_set_mode(ssl_ctx, SSL_MODE_AUTO_RETRY);
  SSL_CTX_set_mode(ssl_ctx, SSL_MODE_RELEASE_BUFFERS);
  if(SSL_CTX_use_PrivateKey_file(ssl_ctx,
                                 config_->private_key_file.c_str(),
                                 SSL_FILETYPE_PEM) != 1) {
    std::cerr << "SSL_CTX_use_PrivateKey_file failed." << std::endl;
    return -1;
  }
  if(SSL_CTX_use_certificate_file(ssl_ctx, config_->cert_file.c_str(),
                                  SSL_FILETYPE_PEM) != 1) {
    std::cerr << "SSL_CTX_use_certificate_file failed." << std::endl;
    return -1;
  }
  if(SSL_CTX_check_private_key(ssl_ctx) != 1) {
    std::cerr << "SSL_CTX_check_private_key failed." << std::endl;
    return -1;
  }

  // We only speak "spdy/2".
  std::pair<unsigned char*, size_t> next_proto;
  unsigned char proto_list[7];
  proto_list[0] = 6;
  memcpy(&proto_list[1], "spdy/2", 6);
  next_proto.first = proto_list;
  next_proto.second = sizeof(proto_list);

  SSL_CTX_set_next_protos_advertised_cb(ssl_ctx, next_proto_cb, &next_proto);

  const size_t MAX_EVENTS = 256;
  Sessions sessions(MAX_EVENTS, ssl_ctx);

  int64_t session_id_seed = 0;
  int families[] = { AF_INET, AF_INET6 };
  bool bind_ok = false;
  for(int i = 0; i < 2; ++i) {
    const char* ipv = (families[i] == AF_INET ? "IPv4" : "IPv6");
    ListenEventHandler *listen_hd = new ListenEventHandler(config_,
                                                           sfd_[i],
                                                           &session_id_seed);
    if(sessions.add_poll(listen_hd) == -1) {
      std::cerr <<  ipv << ": Adding listening socket to poll failed."
                << std::endl;
      delete listen_hd;
    }
    sessions.add_handler(listen_hd);
    bind_ok = true;
  }
  if(!bind_ok) {
    return -1;
  }

  std::vector<EventHandler*> del_list;
  while(1) {
    int n = sessions.poll(-1);
    if(n == -1) {
      perror("EventPoll");
    } else {
      for(int i = 0; i < n; ++i) {
        EventHandler *hd = reinterpret_cast<EventHandler*>
          (sessions.get_user_data(i));
        int events = sessions.get_events(i);
        int r = 0;
        if(hd->mark_del()) {
          continue;
        }
        if((events & EP_POLLIN) || (events & EP_POLLOUT)) {
          r = hd->execute(&sessions);
        } else if(events & (EP_POLLERR | EP_POLLHUP)) {
          hd->mark_del(true);
        }
        if(r != 0) {
          hd->mark_del(true);
        } else {
          if(hd->finish()) {
            hd->mark_del(true);
          } else {
            sessions.mod_poll(hd);
          }
        }
        if(hd->mark_del()) {
          del_list.push_back(hd);
        }
      }
      for(std::vector<EventHandler*>::iterator i = del_list.begin(),
            eoi = del_list.end(); i != eoi; ++i) {
        on_close(sessions, *i);
        sessions.remove_handler(*i);
      }
      del_list.clear();
    }
  }
  return 0;
}

} // namespace spdylay