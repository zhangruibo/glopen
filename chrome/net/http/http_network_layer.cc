// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/http/http_network_layer.h"

#include "base/field_trial.h"
#include "base/logging.h"
#include "base/string_util.h"
#include "net/http/http_network_session.h"
#include "net/http/http_network_transaction.h"
#include "net/socket/client_socket_factory.h"
#include "net/spdy/spdy_framer.h"
#include "net/spdy/spdy_network_transaction.h"
#include "net/spdy/spdy_session.h"
#include "net/spdy/spdy_session_pool.h"

namespace net {

//-----------------------------------------------------------------------------

// static
HttpTransactionFactory* HttpNetworkLayer::CreateFactory(
    HostResolver* host_resolver,
    ProxyService* proxy_service,
    SSLConfigService* ssl_config_service,
    HttpAuthHandlerFactory* http_auth_handler_factory,
    HttpNetworkDelegate* network_delegate,
    NetLog* net_log) {
  DCHECK(proxy_service);

  return new HttpNetworkLayer(ClientSocketFactory::GetDefaultFactory(),
                              host_resolver, proxy_service, ssl_config_service,
                              http_auth_handler_factory,
                              network_delegate,
                              net_log);
}

// static
HttpTransactionFactory* HttpNetworkLayer::CreateFactory(
    HttpNetworkSession* session) {
  DCHECK(session);

  return new HttpNetworkLayer(session);
}

//-----------------------------------------------------------------------------
bool HttpNetworkLayer::force_spdy_ = false;

HttpNetworkLayer::HttpNetworkLayer(
    ClientSocketFactory* socket_factory,
    HostResolver* host_resolver,
    ProxyService* proxy_service,
    SSLConfigService* ssl_config_service,
    HttpAuthHandlerFactory* http_auth_handler_factory,
    HttpNetworkDelegate* network_delegate,
    NetLog* net_log)
    : socket_factory_(socket_factory),
      host_resolver_(host_resolver),
      proxy_service_(proxy_service),
      ssl_config_service_(ssl_config_service),
      session_(NULL),
      spdy_session_pool_(NULL),
      http_auth_handler_factory_(http_auth_handler_factory),
      network_delegate_(network_delegate),
      net_log_(net_log),
      suspended_(false) {
  DCHECK(proxy_service_);
  DCHECK(ssl_config_service_.get());
}

HttpNetworkLayer::HttpNetworkLayer(HttpNetworkSession* session)
    : socket_factory_(ClientSocketFactory::GetDefaultFactory()),
      ssl_config_service_(NULL),
      session_(session),
      spdy_session_pool_(session->spdy_session_pool()),
      http_auth_handler_factory_(NULL),
      network_delegate_(NULL),
      net_log_(NULL),
      suspended_(false) {
  DCHECK(session_.get());
}

HttpNetworkLayer::~HttpNetworkLayer() {
}

int HttpNetworkLayer::CreateTransaction(scoped_ptr<HttpTransaction>* trans) {
  if (suspended_)
    return ERR_NETWORK_IO_SUSPENDED;

  if (force_spdy_)
    trans->reset(new SpdyNetworkTransaction(GetSession()));
  else
    trans->reset(new HttpNetworkTransaction(GetSession()));
  return OK;
}

HttpCache* HttpNetworkLayer::GetCache() {
  return NULL;
}

void HttpNetworkLayer::Suspend(bool suspend) {
  suspended_ = suspend;

  if (suspend && session_)
    session_->tcp_socket_pool()->CloseIdleSockets();
}

HttpNetworkSession* HttpNetworkLayer::GetSession() {
  if (!session_) {
    DCHECK(proxy_service_);
    SpdySessionPool* spdy_pool = new SpdySessionPool();
    session_ = new HttpNetworkSession(host_resolver_, proxy_service_,
        socket_factory_, ssl_config_service_, spdy_pool,
        http_auth_handler_factory_, network_delegate_, net_log_);
    // These were just temps for lazy-initializing HttpNetworkSession.
    host_resolver_ = NULL;
    proxy_service_ = NULL;
    socket_factory_ = NULL;
    http_auth_handler_factory_ = NULL;
    net_log_ = NULL;
    network_delegate_ = NULL;
  }
  return session_;
}

// static
void HttpNetworkLayer::EnableSpdy(const std::string& mode) {
  static const char kDisableSSL[] = "no-ssl";
  static const char kDisableCompression[] = "no-compress";
  static const char kDisableAltProtocols[] = "no-alt-protocols";

  // We want an A/B experiment between SPDY enabled and SPDY disabled,
  // but only for pages where SPDY *could have been* negotiated.  To do
  // this, we use NPN, but prevent it from negotiating SPDY.  If the
  // server negotiates HTTP, rather than SPDY, today that will only happen
  // on servers that installed NPN (and could have done SPDY).  But this is
  // a bit of a hack, as this correlation between NPN and SPDY is not
  // really guaranteed.
  static const char kEnableNPN[] = "npn";
  static const char kEnableNpnHttpOnly[] = "npn-http";

  // Except for the first element, the order is irrelevant.  First element
  // specifies the fallback in case nothing matches
  // (SSLClientSocket::kNextProtoNoOverlap).  Otherwise, the SSL library
  // will choose the first overlapping protocol in the server's list, since
  // it presumedly has a better understanding of which protocol we should
  // use, therefore the rest of the ordering here is not important.
  static const char kNpnProtosFull[] =
      "\x08http/1.1\x07http1.1\x06spdy/1\x04spdy";
  // No spdy specified.
  static const char kNpnProtosHttpOnly[] = "\x08http/1.1\x07http1.1";

  std::vector<std::string> spdy_options;
  SplitString(mode, ',', &spdy_options);

  // Force spdy mode (use SpdyNetworkTransaction for all http requests).
  force_spdy_ = true;

  bool use_alt_protocols = true;

  for (std::vector<std::string>::iterator it = spdy_options.begin();
       it != spdy_options.end(); ++it) {
    const std::string& option = *it;
    if (option == kDisableSSL) {
      SpdySession::SetSSLMode(false);  // Disable SSL
    } else if (option == kDisableCompression) {
      spdy::SpdyFramer::set_enable_compression_default(false);
    } else if (option == kEnableNPN) {
      HttpNetworkTransaction::SetUseAlternateProtocols(use_alt_protocols);
      HttpNetworkTransaction::SetNextProtos(kNpnProtosFull);
      force_spdy_ = false;
    } else if (option == kEnableNpnHttpOnly) {
      HttpNetworkTransaction::SetUseAlternateProtocols(use_alt_protocols);
      HttpNetworkTransaction::SetNextProtos(kNpnProtosHttpOnly);
      force_spdy_ = false;
    } else if (option == kDisableAltProtocols) {
      use_alt_protocols = false;
      HttpNetworkTransaction::SetUseAlternateProtocols(false);
    } else if (option.empty() && it == spdy_options.begin()) {
      continue;
    } else {
      LOG(DFATAL) << "Unrecognized spdy option: " << option;
    }
  }
}

}  // namespace net
