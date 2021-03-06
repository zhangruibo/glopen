// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_SOCKET_CLIENT_SOCKET_HANDLE_H_
#define NET_SOCKET_CLIENT_SOCKET_HANDLE_H_

#include <string>

#include "base/logging.h"
#include "base/ref_counted.h"
#include "base/scoped_ptr.h"
#include "base/time.h"
#include "net/base/completion_callback.h"
#include "net/base/load_states.h"
#include "net/base/net_errors.h"
#include "net/base/net_log.h"
#include "net/base/request_priority.h"
#include "net/http/http_response_info.h"
#include "net/socket/client_socket.h"
#include "net/socket/client_socket_pool.h"

namespace net {

// A container for a ClientSocket.
//
// The handle's |group_name| uniquely identifies the origin and type of the
// connection.  It is used by the ClientSocketPool to group similar connected
// client socket objects.
//
class ClientSocketHandle {
 public:
  typedef enum {
    UNUSED = 0,   // unused socket that just finished connecting
    UNUSED_IDLE,  // unused socket that has been idle for awhile
    REUSED_IDLE,  // previously used socket
    NUM_TYPES,
  } SocketReuseType;

  ClientSocketHandle();
  ~ClientSocketHandle();

  // Initializes a ClientSocketHandle object, which involves talking to the
  // ClientSocketPool to obtain a connected socket, possibly reusing one.  This
  // method returns either OK or ERR_IO_PENDING.  On ERR_IO_PENDING, |priority|
  // is used to determine the placement in ClientSocketPool's wait list.
  //
  // If this method succeeds, then the socket member will be set to an existing
  // connected socket if an existing connected socket was available to reuse,
  // otherwise it will be set to a new connected socket.  Consumers can then
  // call is_reused() to see if the socket was reused.  If not reusing an
  // existing socket, ClientSocketPool may need to establish a new
  // connection using |socket_params|.
  //
  // This method returns ERR_IO_PENDING if it cannot complete synchronously, in
  // which case the consumer will be notified of completion via |callback|.
  //
  // If the pool was not able to reuse an existing socket, the new socket
  // may report a recoverable error.  In this case, the return value will
  // indicate an error and the socket member will be set.  If it is determined
  // that the error is not recoverable, the Disconnect method should be used
  // on the socket, so that it does not get reused.
  //
  // A non-recoverable error may set additional state in the ClientSocketHandle
  // to allow the caller to determine what went wrong.
  //
  // Init may be called multiple times.
  //
  // Profiling information for the request is saved to |net_log| if non-NULL.
  //
  template <typename SocketParams, typename PoolType>
  int Init(const std::string& group_name,
           const scoped_refptr<SocketParams>& socket_params,
           RequestPriority priority,
           CompletionCallback* callback,
           const scoped_refptr<PoolType>& pool,
           const BoundNetLog& net_log);

  // An initialized handle can be reset, which causes it to return to the
  // un-initialized state.  This releases the underlying socket, which in the
  // case of a socket that still has an established connection, indicates that
  // the socket may be kept alive for use by a subsequent ClientSocketHandle.
  //
  // NOTE: To prevent the socket from being kept alive, be sure to call its
  // Disconnect method.  This will result in the ClientSocketPool deleting the
  // ClientSocket.
  void Reset();

  // Used after Init() is called, but before the ClientSocketPool has
  // initialized the ClientSocketHandle.
  LoadState GetLoadState() const;

  // Returns true when Init() has completed successfully.
  bool is_initialized() const { return socket_ != NULL; }

  // Returns the time tick when Init() was called.
  base::TimeTicks init_time() const { return init_time_; }

  // Returns the time between Init() and when is_initialized() becomes true.
  base::TimeDelta setup_time() const { return setup_time_; }

  // Used by ClientSocketPool to initialize the ClientSocketHandle.
  void set_is_reused(bool is_reused) { is_reused_ = is_reused; }
  void set_socket(ClientSocket* s) { socket_.reset(s); }
  void set_idle_time(base::TimeDelta idle_time) { idle_time_ = idle_time; }
  void set_pool_id(int id) { pool_id_ = id; }
  void set_tunnel_auth_response_info(
      const scoped_refptr<HttpResponseHeaders>& headers,
      const scoped_refptr<AuthChallengeInfo>& auth_challenge) {
    tunnel_auth_response_info_.headers = headers;
    tunnel_auth_response_info_.auth_challenge = auth_challenge;
  }
  void set_is_ssl_error(bool is_ssl_error) { is_ssl_error_ = is_ssl_error; }

  // These may only be used if is_initialized() is true.
  const std::string& group_name() const { return group_name_; }
  ClientSocket* socket() { return socket_.get(); }
  ClientSocket* release_socket() { return socket_.release(); }
  const HttpResponseInfo& tunnel_auth_response_info() const {
    return tunnel_auth_response_info_;
  }
  // Only valid if there is no |socket_|.
  bool is_ssl_error() const {
    DCHECK(socket_.get() == NULL);
    return is_ssl_error_;
  }
  bool is_reused() const { return is_reused_; }
  base::TimeDelta idle_time() const { return idle_time_; }
  SocketReuseType reuse_type() const {
    if (is_reused()) {
      return REUSED_IDLE;
    } else if (idle_time() == base::TimeDelta()) {
      return UNUSED;
    } else {
      return UNUSED_IDLE;
    }
  }
  bool ShouldResendFailedRequest(int error) const {
    // NOTE: we resend a request only if we reused a keep-alive connection.
    // This automatically prevents an infinite resend loop because we'll run
    // out of the cached keep-alive connections eventually.
    if (  // We used a socket that was never idle.
        reuse_type() == ClientSocketHandle::UNUSED ||
        // We used an unused, idle socket and got a error that wasn't a TCP RST.
        (reuse_type() == ClientSocketHandle::UNUSED_IDLE &&
         (error != OK && error != ERR_CONNECTION_RESET))) {
      return false;
    }
    return true;
  }

 private:
  // Called on asynchronous completion of an Init() request.
  void OnIOComplete(int result);

  // Called on completion (both asynchronous & synchronous) of an Init()
  // request.
  void HandleInitCompletion(int result);

  // Resets the state of the ClientSocketHandle.  |cancel| indicates whether or
  // not to try to cancel the request with the ClientSocketPool.  Does not
  // reset the supplemental error state.
  void ResetInternal(bool cancel);

  // Resets the supplemental error state.
  void ResetErrorState();

  scoped_refptr<ClientSocketPool> pool_;
  scoped_ptr<ClientSocket> socket_;
  std::string group_name_;
  bool is_reused_;
  CompletionCallbackImpl<ClientSocketHandle> callback_;
  CompletionCallback* user_callback_;
  base::TimeDelta idle_time_;
  int pool_id_;  // See ClientSocketPool::ReleaseSocket() for an explanation.
  bool is_ssl_error_;
  HttpResponseInfo tunnel_auth_response_info_;
  base::TimeTicks init_time_;
  base::TimeDelta setup_time_;

  NetLog::Source requesting_source_;

  DISALLOW_COPY_AND_ASSIGN(ClientSocketHandle);
};

// Template function implementation:
template <typename SocketParams, typename PoolType>
int ClientSocketHandle::Init(const std::string& group_name,
                             const scoped_refptr<SocketParams>& socket_params,
                             RequestPriority priority,
                             CompletionCallback* callback,
                             const scoped_refptr<PoolType>& pool,
                             const BoundNetLog& net_log) {
  requesting_source_ = net_log.source();

  CHECK(!group_name.empty());
  // Note that this will result in a compile error if the SocketParams has not
  // been registered for the PoolType via REGISTER_SOCKET_PARAMS_FOR_POOL
  // (defined in client_socket_pool.h).
  CheckIsValidSocketParamsForPool<PoolType, SocketParams>();
  ResetInternal(true);
  ResetErrorState();
  pool_ = pool;
  group_name_ = group_name;
  init_time_ = base::TimeTicks::Now();
  int rv = pool_->RequestSocket(
      group_name, &socket_params, priority, this, &callback_, net_log);
  if (rv == ERR_IO_PENDING) {
    user_callback_ = callback;
  } else {
    HandleInitCompletion(rv);
  }
  return rv;
}

}  // namespace net

#endif  // NET_SOCKET_CLIENT_SOCKET_HANDLE_H_
