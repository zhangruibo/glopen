// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_SOCKET_TCP_CLIENT_SOCKET_POOL_H_
#define NET_SOCKET_TCP_CLIENT_SOCKET_POOL_H_

#include <string>

#include "base/basictypes.h"
#include "base/ref_counted.h"
#include "base/scoped_ptr.h"
#include "base/time.h"
#include "base/timer.h"
#include "net/base/host_port_pair.h"
#include "net/base/host_resolver.h"
#include "net/socket/client_socket_pool_base.h"
#include "net/socket/client_socket_pool_histograms.h"
#include "net/socket/client_socket_pool.h"

namespace net {

class ClientSocketFactory;

class TCPSocketParams : public base::RefCounted<TCPSocketParams> {
 public:
  TCPSocketParams(const HostPortPair& host_port_pair, RequestPriority priority,
                  const GURL& referrer, bool disable_resolver_cache);

  // TODO(willchan): Update all unittests so we don't need this.
  TCPSocketParams(const std::string& host, int port, RequestPriority priority,
                  const GURL& referrer, bool disable_resolver_cache);

  const HostResolver::RequestInfo& destination() const { return destination_; }

 private:
  friend class base::RefCounted<TCPSocketParams>;
  ~TCPSocketParams();

  void Initialize(RequestPriority priority, const GURL& referrer,
                  bool disable_resolver_cache) {
    // The referrer is used by the DNS prefetch system to correlate resolutions
    // with the page that triggered them. It doesn't impact the actual addresses
    // that we resolve to.
    destination_.set_referrer(referrer);
    destination_.set_priority(priority);
    if (disable_resolver_cache)
      destination_.set_allow_cached_response(false);
  }

  HostResolver::RequestInfo destination_;

  DISALLOW_COPY_AND_ASSIGN(TCPSocketParams);
};

// TCPConnectJob handles the host resolution necessary for socket creation
// and the tcp connect.
class TCPConnectJob : public ConnectJob {
 public:
  TCPConnectJob(const std::string& group_name,
                const scoped_refptr<TCPSocketParams>& params,
                base::TimeDelta timeout_duration,
                ClientSocketFactory* client_socket_factory,
                HostResolver* host_resolver,
                Delegate* delegate,
                NetLog* net_log);
  virtual ~TCPConnectJob();

  // ConnectJob methods.
  virtual LoadState GetLoadState() const;

 private:
  enum State {
    kStateResolveHost,
    kStateResolveHostComplete,
    kStateTCPConnect,
    kStateTCPConnectComplete,
    kStateNone,
  };

  // Begins the host resolution and the TCP connect.  Returns OK on success
  // and ERR_IO_PENDING if it cannot immediately service the request.
  // Otherwise, it returns a net error code.
  virtual int ConnectInternal();

  void OnIOComplete(int result);

  // Runs the state transition loop.
  int DoLoop(int result);

  int DoResolveHost();
  int DoResolveHostComplete(int result);
  int DoTCPConnect();
  int DoTCPConnectComplete(int result);

  scoped_refptr<TCPSocketParams> params_;
  ClientSocketFactory* const client_socket_factory_;
  CompletionCallbackImpl<TCPConnectJob> callback_;
  SingleRequestHostResolver resolver_;
  AddressList addresses_;
  State next_state_;

  // The time Connect() was called.
  base::TimeTicks start_time_;

  // The time the connect was started (after DNS finished).
  base::TimeTicks connect_start_time_;

  DISALLOW_COPY_AND_ASSIGN(TCPConnectJob);
};

class TCPClientSocketPool : public ClientSocketPool {
 public:
  TCPClientSocketPool(
      int max_sockets,
      int max_sockets_per_group,
      const scoped_refptr<ClientSocketPoolHistograms>& histograms,
      HostResolver* host_resolver,
      ClientSocketFactory* client_socket_factory,
      NetLog* net_log);

  // ClientSocketPool methods:

  virtual int RequestSocket(const std::string& group_name,
                            const void* resolve_info,
                            RequestPriority priority,
                            ClientSocketHandle* handle,
                            CompletionCallback* callback,
                            const BoundNetLog& net_log);

  virtual void CancelRequest(const std::string& group_name,
                             const ClientSocketHandle* handle);

  virtual void ReleaseSocket(const std::string& group_name,
                             ClientSocket* socket,
                             int id);

  virtual void Flush();

  virtual void CloseIdleSockets();

  virtual int IdleSocketCount() const {
    return base_.idle_socket_count();
  }

  virtual int IdleSocketCountInGroup(const std::string& group_name) const;

  virtual LoadState GetLoadState(const std::string& group_name,
                                 const ClientSocketHandle* handle) const;

  virtual base::TimeDelta ConnectionTimeout() const {
    return base_.ConnectionTimeout();
  }

  virtual scoped_refptr<ClientSocketPoolHistograms> histograms() const {
    return base_.histograms();
  }

 protected:
  virtual ~TCPClientSocketPool();

 private:
  typedef ClientSocketPoolBase<TCPSocketParams> PoolBase;

  class TCPConnectJobFactory
      : public PoolBase::ConnectJobFactory {
   public:
    TCPConnectJobFactory(ClientSocketFactory* client_socket_factory,
                         HostResolver* host_resolver,
                         NetLog* net_log)
        : client_socket_factory_(client_socket_factory),
          host_resolver_(host_resolver),
          net_log_(net_log) {}

    virtual ~TCPConnectJobFactory() {}

    // ClientSocketPoolBase::ConnectJobFactory methods.

    virtual ConnectJob* NewConnectJob(
        const std::string& group_name,
        const PoolBase::Request& request,
        ConnectJob::Delegate* delegate) const;

    virtual base::TimeDelta ConnectionTimeout() const;

   private:
    ClientSocketFactory* const client_socket_factory_;
    const scoped_refptr<HostResolver> host_resolver_;
    NetLog* net_log_;

    DISALLOW_COPY_AND_ASSIGN(TCPConnectJobFactory);
  };

  PoolBase base_;

  DISALLOW_COPY_AND_ASSIGN(TCPClientSocketPool);
};

REGISTER_SOCKET_PARAMS_FOR_POOL(TCPClientSocketPool, TCPSocketParams);

}  // namespace net

#endif  // NET_SOCKET_TCP_CLIENT_SOCKET_POOL_H_
