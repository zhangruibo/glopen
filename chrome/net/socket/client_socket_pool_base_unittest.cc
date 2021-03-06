// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/socket/client_socket_pool_base.h"

#include "base/callback.h"
#include "base/compiler_specific.h"
#include "base/message_loop.h"
#include "base/platform_thread.h"
#include "base/ref_counted.h"
#include "base/scoped_vector.h"
#include "base/string_util.h"
#include "net/base/net_log.h"
#include "net/base/net_log_unittest.h"
#include "net/base/net_errors.h"
#include "net/base/request_priority.h"
#include "net/base/test_completion_callback.h"
#include "net/socket/client_socket.h"
#include "net/socket/client_socket_factory.h"
#include "net/socket/client_socket_handle.h"
#include "net/socket/client_socket_pool_histograms.h"
#include "net/socket/socket_test_util.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace net {

namespace {

const int kDefaultMaxSockets = 4;
const int kDefaultMaxSocketsPerGroup = 2;
const net::RequestPriority kDefaultPriority = MEDIUM;

class TestSocketParams : public base::RefCounted<TestSocketParams> {
 private:
  friend class base::RefCounted<TestSocketParams>;
  ~TestSocketParams() {}
};
typedef ClientSocketPoolBase<TestSocketParams> TestClientSocketPoolBase;

class MockClientSocket : public ClientSocket {
 public:
  MockClientSocket() : connected_(false) {}

  // Socket methods:
  virtual int Read(
      IOBuffer* /* buf */, int /* len */, CompletionCallback* /* callback */) {
    return ERR_UNEXPECTED;
  }

  virtual int Write(
      IOBuffer* /* buf */, int /* len */, CompletionCallback* /* callback */) {
    return ERR_UNEXPECTED;
  }
  virtual bool SetReceiveBufferSize(int32 size) { return true; }
  virtual bool SetSendBufferSize(int32 size) { return true; }

  // ClientSocket methods:

  virtual int Connect(CompletionCallback* callback) {
    connected_ = true;
    return OK;
  }

  virtual void Disconnect() { connected_ = false; }
  virtual bool IsConnected() const { return connected_; }
  virtual bool IsConnectedAndIdle() const { return connected_; }

  virtual int GetPeerAddress(AddressList* /* address */) const {
    return ERR_UNEXPECTED;
  }

  virtual const BoundNetLog& NetLog() const {
    return net_log_;
  }

 private:
  bool connected_;
  BoundNetLog net_log_;

  DISALLOW_COPY_AND_ASSIGN(MockClientSocket);
};

class TestConnectJob;

class MockClientSocketFactory : public ClientSocketFactory {
 public:
  MockClientSocketFactory() : allocation_count_(0) {}

  virtual ClientSocket* CreateTCPClientSocket(const AddressList& addresses,
                                              NetLog* /* net_log */) {
    allocation_count_++;
    return NULL;
  }

  virtual SSLClientSocket* CreateSSLClientSocket(
      ClientSocketHandle* transport_socket,
      const std::string& hostname,
      const SSLConfig& ssl_config) {
    NOTIMPLEMENTED();
    return NULL;
  }

  void WaitForSignal(TestConnectJob* job) { waiting_jobs_.push_back(job); }
  void SignalJobs();

  int allocation_count() const { return allocation_count_; }

 private:
  int allocation_count_;
  std::vector<TestConnectJob*> waiting_jobs_;
};

class TestConnectJob : public ConnectJob {
 public:
  enum JobType {
    kMockJob,
    kMockFailingJob,
    kMockPendingJob,
    kMockPendingFailingJob,
    kMockWaitingJob,
    kMockAdvancingLoadStateJob,
    kMockRecoverableJob,
    kMockPendingRecoverableJob,
    kMockAdditionalErrorStateJob,
    kMockPendingAdditionalErrorStateJob,
  };

  // The kMockPendingJob uses a slight delay before allowing the connect
  // to complete.
  static const int kPendingConnectDelay = 2;

  TestConnectJob(JobType job_type,
                 const std::string& group_name,
                 const TestClientSocketPoolBase::Request& request,
                 base::TimeDelta timeout_duration,
                 ConnectJob::Delegate* delegate,
                 MockClientSocketFactory* client_socket_factory,
                 NetLog* net_log)
      : ConnectJob(group_name, timeout_duration, delegate,
                   BoundNetLog::Make(net_log, NetLog::SOURCE_CONNECT_JOB)),
        job_type_(job_type),
        client_socket_factory_(client_socket_factory),
        method_factory_(ALLOW_THIS_IN_INITIALIZER_LIST(this)),
        load_state_(LOAD_STATE_IDLE),
        store_additional_error_state_(false) {}

  void Signal() {
    DoConnect(waiting_success_, true /* async */, false /* recoverable */);
  }

  virtual LoadState GetLoadState() const { return load_state_; }

  virtual void GetAdditionalErrorState(ClientSocketHandle* handle) {
    if (store_additional_error_state_) {
      // Set all of the additional error state fields in some way.
      handle->set_is_ssl_error(true);
      scoped_refptr<HttpResponseHeaders> headers(new HttpResponseHeaders(""));
      handle->set_tunnel_auth_response_info(headers, NULL);
    }
  }

 private:
  // ConnectJob methods:

  virtual int ConnectInternal() {
    AddressList ignored;
    client_socket_factory_->CreateTCPClientSocket(ignored, NULL);
    set_socket(new MockClientSocket());
    switch (job_type_) {
      case kMockJob:
        return DoConnect(true /* successful */, false /* sync */,
                         false /* recoverable */);
      case kMockFailingJob:
        return DoConnect(false /* error */, false /* sync */,
                         false /* recoverable */);
      case kMockPendingJob:
        set_load_state(LOAD_STATE_CONNECTING);

        // Depending on execution timings, posting a delayed task can result
        // in the task getting executed the at the earliest possible
        // opportunity or only after returning once from the message loop and
        // then a second call into the message loop. In order to make behavior
        // more deterministic, we change the default delay to 2ms. This should
        // always require us to wait for the second call into the message loop.
        //
        // N.B. The correct fix for this and similar timing problems is to
        // abstract time for the purpose of unittests. Unfortunately, we have
        // a lot of third-party components that directly call the various
        // time functions, so this change would be rather invasive.
        MessageLoop::current()->PostDelayedTask(
            FROM_HERE,
            method_factory_.NewRunnableMethod(
                &TestConnectJob::DoConnect,
                true /* successful */,
                true /* async */,
                false /* recoverable */),
            kPendingConnectDelay);
        return ERR_IO_PENDING;
      case kMockPendingFailingJob:
        set_load_state(LOAD_STATE_CONNECTING);
        MessageLoop::current()->PostDelayedTask(
            FROM_HERE,
            method_factory_.NewRunnableMethod(
                &TestConnectJob::DoConnect,
                false /* error */,
                true  /* async */,
                false /* recoverable */),
            2);
        return ERR_IO_PENDING;
      case kMockWaitingJob:
        client_socket_factory_->WaitForSignal(this);
        waiting_success_ = true;
        return ERR_IO_PENDING;
      case kMockAdvancingLoadStateJob:
        MessageLoop::current()->PostTask(
            FROM_HERE,
            method_factory_.NewRunnableMethod(
                &TestConnectJob::AdvanceLoadState, load_state_));
        return ERR_IO_PENDING;
      case kMockRecoverableJob:
        return DoConnect(false /* error */, false /* sync */,
                         true /* recoverable */);
      case kMockPendingRecoverableJob:
        set_load_state(LOAD_STATE_CONNECTING);
        MessageLoop::current()->PostDelayedTask(
            FROM_HERE,
            method_factory_.NewRunnableMethod(
                &TestConnectJob::DoConnect,
                false /* error */,
                true  /* async */,
                true  /* recoverable */),
            2);
        return ERR_IO_PENDING;
      case kMockAdditionalErrorStateJob:
        store_additional_error_state_ = true;
        return DoConnect(false /* error */, false /* sync */,
                         false /* recoverable */);
      case kMockPendingAdditionalErrorStateJob:
        set_load_state(LOAD_STATE_CONNECTING);
        store_additional_error_state_ = true;
        MessageLoop::current()->PostDelayedTask(
            FROM_HERE,
            method_factory_.NewRunnableMethod(
                &TestConnectJob::DoConnect,
                false /* error */,
                true  /* async */,
                false /* recoverable */),
            2);
        return ERR_IO_PENDING;
      default:
        NOTREACHED();
        set_socket(NULL);
        return ERR_FAILED;
    }
  }

  void set_load_state(LoadState load_state) { load_state_ = load_state; }

  int DoConnect(bool succeed, bool was_async, bool recoverable) {
    int result = OK;
    if (succeed) {
      socket()->Connect(NULL);
    } else if (recoverable) {
      result = ERR_PROXY_AUTH_REQUESTED;
    } else {
      result = ERR_CONNECTION_FAILED;
      set_socket(NULL);
    }

    if (was_async)
      NotifyDelegateOfCompletion(result);
    return result;
  }

  // This function helps simulate the progress of load states on a ConnectJob.
  // Each time it is called it advances the load state and posts a task to be
  // called again.  It stops at the last connecting load state (the one
  // before LOAD_STATE_SENDING_REQUEST).
  void AdvanceLoadState(LoadState state) {
    int tmp = state;
    tmp++;
    if (tmp < LOAD_STATE_SENDING_REQUEST) {
      state = static_cast<LoadState>(tmp);
      set_load_state(state);
      MessageLoop::current()->PostTask(
          FROM_HERE,
          method_factory_.NewRunnableMethod(&TestConnectJob::AdvanceLoadState,
                                            state));
    }
  }

  bool waiting_success_;
  const JobType job_type_;
  MockClientSocketFactory* const client_socket_factory_;
  ScopedRunnableMethodFactory<TestConnectJob> method_factory_;
  LoadState load_state_;
  bool store_additional_error_state_;

  DISALLOW_COPY_AND_ASSIGN(TestConnectJob);
};

class TestConnectJobFactory
    : public TestClientSocketPoolBase::ConnectJobFactory {
 public:
  explicit TestConnectJobFactory(MockClientSocketFactory* client_socket_factory)
      : job_type_(TestConnectJob::kMockJob),
        client_socket_factory_(client_socket_factory) {}

  virtual ~TestConnectJobFactory() {}

  void set_job_type(TestConnectJob::JobType job_type) { job_type_ = job_type; }

  void set_timeout_duration(base::TimeDelta timeout_duration) {
    timeout_duration_ = timeout_duration;
  }

  // ConnectJobFactory methods:

  virtual ConnectJob* NewConnectJob(
      const std::string& group_name,
      const TestClientSocketPoolBase::Request& request,
      ConnectJob::Delegate* delegate) const {
    return new TestConnectJob(job_type_,
                              group_name,
                              request,
                              timeout_duration_,
                              delegate,
                              client_socket_factory_,
                              NULL);
  }

  virtual base::TimeDelta ConnectionTimeout() const {
    return timeout_duration_;
  }

 private:
  TestConnectJob::JobType job_type_;
  base::TimeDelta timeout_duration_;
  MockClientSocketFactory* const client_socket_factory_;

  DISALLOW_COPY_AND_ASSIGN(TestConnectJobFactory);
};

class TestClientSocketPool : public ClientSocketPool {
 public:
  TestClientSocketPool(
      int max_sockets,
      int max_sockets_per_group,
      const scoped_refptr<ClientSocketPoolHistograms>& histograms,
      base::TimeDelta unused_idle_socket_timeout,
      base::TimeDelta used_idle_socket_timeout,
      TestClientSocketPoolBase::ConnectJobFactory* connect_job_factory)
      : base_(max_sockets, max_sockets_per_group, histograms,
              unused_idle_socket_timeout, used_idle_socket_timeout,
              connect_job_factory) {}

  virtual int RequestSocket(
      const std::string& group_name,
      const void* params,
      net::RequestPriority priority,
      ClientSocketHandle* handle,
      CompletionCallback* callback,
      const BoundNetLog& net_log) {
    const scoped_refptr<TestSocketParams>* casted_socket_params =
        static_cast<const scoped_refptr<TestSocketParams>*>(params);
    return base_.RequestSocket(group_name, *casted_socket_params, priority,
                               handle, callback, net_log);
  }

  virtual void CancelRequest(
      const std::string& group_name,
      const ClientSocketHandle* handle) {
    base_.CancelRequest(group_name, handle);
  }

  virtual void ReleaseSocket(
      const std::string& group_name,
      ClientSocket* socket,
      int id) {
    base_.ReleaseSocket(group_name, socket, id);
  }

  virtual void Flush() {
    base_.Flush();
  }

  virtual void CloseIdleSockets() {
    base_.CloseIdleSockets();
  }

  virtual int IdleSocketCount() const { return base_.idle_socket_count(); }

  virtual int IdleSocketCountInGroup(const std::string& group_name) const {
    return base_.IdleSocketCountInGroup(group_name);
  }

  virtual LoadState GetLoadState(const std::string& group_name,
                                 const ClientSocketHandle* handle) const {
    return base_.GetLoadState(group_name, handle);
  }

  virtual base::TimeDelta ConnectionTimeout() const {
    return base_.ConnectionTimeout();
  }

  virtual scoped_refptr<ClientSocketPoolHistograms> histograms() const {
    return base_.histograms();
  }

  const TestClientSocketPoolBase* base() const { return &base_; }

  int NumConnectJobsInGroup(const std::string& group_name) const {
    return base_.NumConnectJobsInGroup(group_name);
  }

  void CleanupTimedOutIdleSockets() { base_.CleanupIdleSockets(false); }

  void EnableBackupJobs() { base_.EnableBackupJobs(); }

 private:
  ~TestClientSocketPool() {}

  TestClientSocketPoolBase base_;

  DISALLOW_COPY_AND_ASSIGN(TestClientSocketPool);
};

}  // namespace

REGISTER_SOCKET_PARAMS_FOR_POOL(TestClientSocketPool, TestSocketParams);

namespace {

void MockClientSocketFactory::SignalJobs() {
  for (std::vector<TestConnectJob*>::iterator it = waiting_jobs_.begin();
       it != waiting_jobs_.end(); ++it) {
    (*it)->Signal();
  }
  waiting_jobs_.clear();
}

class TestConnectJobDelegate : public ConnectJob::Delegate {
 public:
  TestConnectJobDelegate()
      : have_result_(false), waiting_for_result_(false), result_(OK) {}
  virtual ~TestConnectJobDelegate() {}

  virtual void OnConnectJobComplete(int result, ConnectJob* job) {
    result_ = result;
    scoped_ptr<ClientSocket> socket(job->ReleaseSocket());
    // socket.get() should be NULL iff result != OK
    EXPECT_EQ(socket.get() == NULL, result != OK);
    delete job;
    have_result_ = true;
    if (waiting_for_result_)
      MessageLoop::current()->Quit();
  }

  int WaitForResult() {
    DCHECK(!waiting_for_result_);
    while (!have_result_) {
      waiting_for_result_ = true;
      MessageLoop::current()->Run();
      waiting_for_result_ = false;
    }
    have_result_ = false;  // auto-reset for next callback
    return result_;
  }

 private:
  bool have_result_;
  bool waiting_for_result_;
  int result_;
};

class ClientSocketPoolBaseTest : public ClientSocketPoolTest {
 protected:
  ClientSocketPoolBaseTest()
      : params_(new TestSocketParams()),
        histograms_(new ClientSocketPoolHistograms("ClientSocketPoolTest")) {}

  void CreatePool(int max_sockets, int max_sockets_per_group) {
    CreatePoolWithIdleTimeouts(
        max_sockets,
        max_sockets_per_group,
        base::TimeDelta::FromSeconds(
            ClientSocketPool::unused_idle_socket_timeout()),
        base::TimeDelta::FromSeconds(kUsedIdleSocketTimeout));
  }

  void CreatePoolWithIdleTimeouts(
      int max_sockets, int max_sockets_per_group,
      base::TimeDelta unused_idle_socket_timeout,
      base::TimeDelta used_idle_socket_timeout) {
    DCHECK(!pool_.get());
    connect_job_factory_ = new TestConnectJobFactory(&client_socket_factory_);
    pool_ = new TestClientSocketPool(max_sockets,
                                     max_sockets_per_group,
                                     histograms_,
                                     unused_idle_socket_timeout,
                                     used_idle_socket_timeout,
                                     connect_job_factory_);
  }

  int StartRequest(const std::string& group_name,
                   net::RequestPriority priority) {
    return StartRequestUsingPool<TestClientSocketPool, TestSocketParams>(
        pool_, group_name, priority, params_);
  }

  virtual void TearDown() {
    // We post all of our delayed tasks with a 2ms delay. I.e. they don't
    // actually become pending until 2ms after they have been created. In order
    // to flush all tasks, we need to wait so that we know there are no
    // soon-to-be-pending tasks waiting.
    PlatformThread::Sleep(10);
    MessageLoop::current()->RunAllPending();

    // Need to delete |pool_| before we turn late binding back off. We also need
    // to delete |requests_| because the pool is reference counted and requests
    // keep reference to it.
    // TODO(willchan): Remove this part when late binding becomes the default.
    pool_ = NULL;
    requests_.reset();

    ClientSocketPoolTest::TearDown();
  }

  MockClientSocketFactory client_socket_factory_;
  TestConnectJobFactory* connect_job_factory_;
  scoped_refptr<TestSocketParams> params_;
  scoped_refptr<TestClientSocketPool> pool_;
  scoped_refptr<ClientSocketPoolHistograms> histograms_;
};

// Even though a timeout is specified, it doesn't time out on a synchronous
// completion.
TEST_F(ClientSocketPoolBaseTest, ConnectJob_NoTimeoutOnSynchronousCompletion) {
  TestConnectJobDelegate delegate;
  ClientSocketHandle ignored;
  TestClientSocketPoolBase::Request request(
      &ignored, NULL, kDefaultPriority, params_, BoundNetLog());
  scoped_ptr<TestConnectJob> job(
      new TestConnectJob(TestConnectJob::kMockJob,
                         "a",
                         request,
                         base::TimeDelta::FromMicroseconds(1),
                         &delegate,
                         &client_socket_factory_,
                         NULL));
  EXPECT_EQ(OK, job->Connect());
}

TEST_F(ClientSocketPoolBaseTest, ConnectJob_TimedOut) {
  TestConnectJobDelegate delegate;
  ClientSocketHandle ignored;
  CapturingNetLog log(CapturingNetLog::kUnbounded);

  TestClientSocketPoolBase::Request request(
      &ignored, NULL, kDefaultPriority, params_, BoundNetLog());
  // Deleted by TestConnectJobDelegate.
  TestConnectJob* job =
      new TestConnectJob(TestConnectJob::kMockPendingJob,
                         "a",
                         request,
                         base::TimeDelta::FromMicroseconds(1),
                         &delegate,
                         &client_socket_factory_,
                         &log);
  ASSERT_EQ(ERR_IO_PENDING, job->Connect());
  PlatformThread::Sleep(1);
  EXPECT_EQ(ERR_TIMED_OUT, delegate.WaitForResult());

  EXPECT_EQ(6u, log.entries().size());
  EXPECT_TRUE(LogContainsBeginEvent(
      log.entries(), 0, NetLog::TYPE_SOCKET_POOL_CONNECT_JOB));
  EXPECT_TRUE(LogContainsBeginEvent(
      log.entries(), 1, NetLog::TYPE_SOCKET_POOL_CONNECT_JOB_CONNECT));
  EXPECT_TRUE(LogContainsEvent(
      log.entries(), 2, NetLog::TYPE_CONNECT_JOB_SET_SOCKET,
      NetLog::PHASE_NONE));
  EXPECT_TRUE(LogContainsEvent(
      log.entries(), 3, NetLog::TYPE_SOCKET_POOL_CONNECT_JOB_TIMED_OUT,
      NetLog::PHASE_NONE));
  EXPECT_TRUE(LogContainsEndEvent(
      log.entries(), 4, NetLog::TYPE_SOCKET_POOL_CONNECT_JOB_CONNECT));
  EXPECT_TRUE(LogContainsEndEvent(
      log.entries(), 5, NetLog::TYPE_SOCKET_POOL_CONNECT_JOB));
}

TEST_F(ClientSocketPoolBaseTest, BasicSynchronous) {
  CreatePool(kDefaultMaxSockets, kDefaultMaxSocketsPerGroup);

  TestCompletionCallback callback;
  ClientSocketHandle handle;
  CapturingBoundNetLog log(CapturingNetLog::kUnbounded);

  EXPECT_EQ(OK, handle.Init("a", params_, kDefaultPriority, &callback, pool_,
                            log.bound()));
  EXPECT_TRUE(handle.is_initialized());
  EXPECT_TRUE(handle.socket());
  handle.Reset();

  EXPECT_EQ(4u, log.entries().size());
  EXPECT_TRUE(LogContainsBeginEvent(
      log.entries(), 0, NetLog::TYPE_SOCKET_POOL));
  EXPECT_TRUE(LogContainsEvent(
      log.entries(), 1, NetLog::TYPE_SOCKET_POOL_BOUND_TO_CONNECT_JOB,
      NetLog::PHASE_NONE));
  EXPECT_TRUE(LogContainsEvent(
      log.entries(), 2, NetLog::TYPE_SOCKET_POOL_BOUND_TO_SOCKET,
      NetLog::PHASE_NONE));
  EXPECT_TRUE(LogContainsEndEvent(
      log.entries(), 3, NetLog::TYPE_SOCKET_POOL));
}

TEST_F(ClientSocketPoolBaseTest, InitConnectionFailure) {
  CreatePool(kDefaultMaxSockets, kDefaultMaxSocketsPerGroup);

  connect_job_factory_->set_job_type(TestConnectJob::kMockFailingJob);
  CapturingBoundNetLog log(CapturingNetLog::kUnbounded);

  TestSocketRequest req(&request_order_, &completion_count_);
  // Set the additional error state members to ensure that they get cleared.
  req.handle()->set_is_ssl_error(true);
  scoped_refptr<HttpResponseHeaders> headers(new HttpResponseHeaders(""));
  req.handle()->set_tunnel_auth_response_info(headers, NULL);
  EXPECT_EQ(ERR_CONNECTION_FAILED, req.handle()->Init("a", params_,
                                                      kDefaultPriority, &req,
                                                      pool_, log.bound()));
  EXPECT_FALSE(req.handle()->socket());
  EXPECT_FALSE(req.handle()->is_ssl_error());
  EXPECT_TRUE(req.handle()->tunnel_auth_response_info().headers.get() == NULL);

  EXPECT_EQ(3u, log.entries().size());
  EXPECT_TRUE(LogContainsBeginEvent(
      log.entries(), 0, NetLog::TYPE_SOCKET_POOL));
  EXPECT_TRUE(LogContainsEvent(
      log.entries(), 1, NetLog::TYPE_SOCKET_POOL_BOUND_TO_CONNECT_JOB,
      NetLog::PHASE_NONE));
  EXPECT_TRUE(LogContainsEndEvent(
      log.entries(), 2, NetLog::TYPE_SOCKET_POOL));
}

TEST_F(ClientSocketPoolBaseTest, TotalLimit) {
  CreatePool(kDefaultMaxSockets, kDefaultMaxSocketsPerGroup);

  // TODO(eroman): Check that the NetLog contains this event.

  EXPECT_EQ(OK, StartRequest("a", kDefaultPriority));
  EXPECT_EQ(OK, StartRequest("b", kDefaultPriority));
  EXPECT_EQ(OK, StartRequest("c", kDefaultPriority));
  EXPECT_EQ(OK, StartRequest("d", kDefaultPriority));

  EXPECT_EQ(static_cast<int>(requests_.size()),
            client_socket_factory_.allocation_count());
  EXPECT_EQ(requests_.size() - kDefaultMaxSockets, completion_count_);

  EXPECT_EQ(ERR_IO_PENDING, StartRequest("e", kDefaultPriority));
  EXPECT_EQ(ERR_IO_PENDING, StartRequest("f", kDefaultPriority));
  EXPECT_EQ(ERR_IO_PENDING, StartRequest("g", kDefaultPriority));

  ReleaseAllConnections(NO_KEEP_ALIVE);

  EXPECT_EQ(static_cast<int>(requests_.size()),
            client_socket_factory_.allocation_count());
  EXPECT_EQ(requests_.size() - kDefaultMaxSockets, completion_count_);

  EXPECT_EQ(1, GetOrderOfRequest(1));
  EXPECT_EQ(2, GetOrderOfRequest(2));
  EXPECT_EQ(3, GetOrderOfRequest(3));
  EXPECT_EQ(4, GetOrderOfRequest(4));
  EXPECT_EQ(5, GetOrderOfRequest(5));
  EXPECT_EQ(6, GetOrderOfRequest(6));
  EXPECT_EQ(7, GetOrderOfRequest(7));

  // Make sure we test order of all requests made.
  EXPECT_EQ(kIndexOutOfBounds, GetOrderOfRequest(8));
}

TEST_F(ClientSocketPoolBaseTest, TotalLimitReachedNewGroup) {
  CreatePool(kDefaultMaxSockets, kDefaultMaxSocketsPerGroup);

  // TODO(eroman): Check that the NetLog contains this event.

  // Reach all limits: max total sockets, and max sockets per group.
  EXPECT_EQ(OK, StartRequest("a", kDefaultPriority));
  EXPECT_EQ(OK, StartRequest("a", kDefaultPriority));
  EXPECT_EQ(OK, StartRequest("b", kDefaultPriority));
  EXPECT_EQ(OK, StartRequest("b", kDefaultPriority));

  EXPECT_EQ(static_cast<int>(requests_.size()),
            client_socket_factory_.allocation_count());
  EXPECT_EQ(requests_.size() - kDefaultMaxSockets, completion_count_);

  // Now create a new group and verify that we don't starve it.
  EXPECT_EQ(ERR_IO_PENDING, StartRequest("c", kDefaultPriority));

  ReleaseAllConnections(NO_KEEP_ALIVE);

  EXPECT_EQ(static_cast<int>(requests_.size()),
            client_socket_factory_.allocation_count());
  EXPECT_EQ(requests_.size() - kDefaultMaxSockets, completion_count_);

  EXPECT_EQ(1, GetOrderOfRequest(1));
  EXPECT_EQ(2, GetOrderOfRequest(2));
  EXPECT_EQ(3, GetOrderOfRequest(3));
  EXPECT_EQ(4, GetOrderOfRequest(4));
  EXPECT_EQ(5, GetOrderOfRequest(5));

  // Make sure we test order of all requests made.
  EXPECT_EQ(kIndexOutOfBounds, GetOrderOfRequest(6));
}

TEST_F(ClientSocketPoolBaseTest, TotalLimitRespectsPriority) {
  CreatePool(kDefaultMaxSockets, kDefaultMaxSocketsPerGroup);

  EXPECT_EQ(OK, StartRequest("b", LOWEST));
  EXPECT_EQ(OK, StartRequest("a", MEDIUM));
  EXPECT_EQ(OK, StartRequest("b", HIGHEST));
  EXPECT_EQ(OK, StartRequest("a", LOWEST));

  EXPECT_EQ(static_cast<int>(requests_.size()),
            client_socket_factory_.allocation_count());

  EXPECT_EQ(ERR_IO_PENDING, StartRequest("c", LOWEST));
  EXPECT_EQ(ERR_IO_PENDING, StartRequest("a", MEDIUM));
  EXPECT_EQ(ERR_IO_PENDING, StartRequest("b", HIGHEST));

  ReleaseAllConnections(NO_KEEP_ALIVE);

  EXPECT_EQ(requests_.size() - kDefaultMaxSockets, completion_count_);

  // First 4 requests don't have to wait, and finish in order.
  EXPECT_EQ(1, GetOrderOfRequest(1));
  EXPECT_EQ(2, GetOrderOfRequest(2));
  EXPECT_EQ(3, GetOrderOfRequest(3));
  EXPECT_EQ(4, GetOrderOfRequest(4));

  // Request ("b", HIGHEST) has the highest priority, then ("a", MEDIUM),
  // and then ("c", LOWEST).
  EXPECT_EQ(7, GetOrderOfRequest(5));
  EXPECT_EQ(6, GetOrderOfRequest(6));
  EXPECT_EQ(5, GetOrderOfRequest(7));

  // Make sure we test order of all requests made.
  EXPECT_EQ(kIndexOutOfBounds, GetOrderOfRequest(8));
}

TEST_F(ClientSocketPoolBaseTest, TotalLimitRespectsGroupLimit) {
  CreatePool(kDefaultMaxSockets, kDefaultMaxSocketsPerGroup);

  EXPECT_EQ(OK, StartRequest("a", LOWEST));
  EXPECT_EQ(OK, StartRequest("a", LOW));
  EXPECT_EQ(OK, StartRequest("b", HIGHEST));
  EXPECT_EQ(OK, StartRequest("b", MEDIUM));

  EXPECT_EQ(static_cast<int>(requests_.size()),
            client_socket_factory_.allocation_count());

  EXPECT_EQ(ERR_IO_PENDING, StartRequest("c", MEDIUM));
  EXPECT_EQ(ERR_IO_PENDING, StartRequest("a", LOW));
  EXPECT_EQ(ERR_IO_PENDING, StartRequest("b", HIGHEST));

  ReleaseAllConnections(NO_KEEP_ALIVE);

  EXPECT_EQ(static_cast<int>(requests_.size()),
            client_socket_factory_.allocation_count());
  EXPECT_EQ(requests_.size() - kDefaultMaxSockets, completion_count_);

  // First 4 requests don't have to wait, and finish in order.
  EXPECT_EQ(1, GetOrderOfRequest(1));
  EXPECT_EQ(2, GetOrderOfRequest(2));
  EXPECT_EQ(3, GetOrderOfRequest(3));
  EXPECT_EQ(4, GetOrderOfRequest(4));

  // Request ("b", 7) has the highest priority, but we can't make new socket for
  // group "b", because it has reached the per-group limit. Then we make
  // socket for ("c", 6), because it has higher priority than ("a", 4),
  // and we still can't make a socket for group "b".
  EXPECT_EQ(5, GetOrderOfRequest(5));
  EXPECT_EQ(6, GetOrderOfRequest(6));
  EXPECT_EQ(7, GetOrderOfRequest(7));

  // Make sure we test order of all requests made.
  EXPECT_EQ(kIndexOutOfBounds, GetOrderOfRequest(8));
}

// Make sure that we count connecting sockets against the total limit.
TEST_F(ClientSocketPoolBaseTest, TotalLimitCountsConnectingSockets) {
  CreatePool(kDefaultMaxSockets, kDefaultMaxSocketsPerGroup);

  EXPECT_EQ(OK, StartRequest("a", kDefaultPriority));
  EXPECT_EQ(OK, StartRequest("b", kDefaultPriority));
  EXPECT_EQ(OK, StartRequest("c", kDefaultPriority));

  // Create one asynchronous request.
  connect_job_factory_->set_job_type(TestConnectJob::kMockPendingJob);
  EXPECT_EQ(ERR_IO_PENDING, StartRequest("d", kDefaultPriority));

  // We post all of our delayed tasks with a 2ms delay. I.e. they don't
  // actually become pending until 2ms after they have been created. In order
  // to flush all tasks, we need to wait so that we know there are no
  // soon-to-be-pending tasks waiting.
  PlatformThread::Sleep(10);
  MessageLoop::current()->RunAllPending();

  // The next synchronous request should wait for its turn.
  connect_job_factory_->set_job_type(TestConnectJob::kMockJob);
  EXPECT_EQ(ERR_IO_PENDING, StartRequest("e", kDefaultPriority));

  ReleaseAllConnections(NO_KEEP_ALIVE);

  EXPECT_EQ(static_cast<int>(requests_.size()),
            client_socket_factory_.allocation_count());

  EXPECT_EQ(1, GetOrderOfRequest(1));
  EXPECT_EQ(2, GetOrderOfRequest(2));
  EXPECT_EQ(3, GetOrderOfRequest(3));
  EXPECT_EQ(4, GetOrderOfRequest(4));
  EXPECT_EQ(5, GetOrderOfRequest(5));

  // Make sure we test order of all requests made.
  EXPECT_EQ(kIndexOutOfBounds, GetOrderOfRequest(6));
}

TEST_F(ClientSocketPoolBaseTest, CorrectlyCountStalledGroups) {
  CreatePool(kDefaultMaxSockets, kDefaultMaxSockets);
  connect_job_factory_->set_job_type(TestConnectJob::kMockJob);

  EXPECT_EQ(OK, StartRequest("a", kDefaultPriority));
  EXPECT_EQ(OK, StartRequest("a", kDefaultPriority));
  EXPECT_EQ(OK, StartRequest("a", kDefaultPriority));
  EXPECT_EQ(OK, StartRequest("a", kDefaultPriority));

  connect_job_factory_->set_job_type(TestConnectJob::kMockWaitingJob);

  EXPECT_EQ(kDefaultMaxSockets, client_socket_factory_.allocation_count());

  EXPECT_EQ(ERR_IO_PENDING, StartRequest("b", kDefaultPriority));
  EXPECT_EQ(ERR_IO_PENDING, StartRequest("c", kDefaultPriority));

  EXPECT_EQ(kDefaultMaxSockets, client_socket_factory_.allocation_count());

  EXPECT_TRUE(ReleaseOneConnection(KEEP_ALIVE));
  EXPECT_EQ(kDefaultMaxSockets + 1, client_socket_factory_.allocation_count());
  EXPECT_TRUE(ReleaseOneConnection(KEEP_ALIVE));
  EXPECT_EQ(kDefaultMaxSockets + 2, client_socket_factory_.allocation_count());
  EXPECT_TRUE(ReleaseOneConnection(KEEP_ALIVE));
  EXPECT_TRUE(ReleaseOneConnection(KEEP_ALIVE));
  EXPECT_EQ(kDefaultMaxSockets + 2, client_socket_factory_.allocation_count());
}

TEST_F(ClientSocketPoolBaseTest, StallAndThenCancelAndTriggerAvailableSocket) {
  CreatePool(kDefaultMaxSockets, kDefaultMaxSockets);
  connect_job_factory_->set_job_type(TestConnectJob::kMockPendingJob);

  ClientSocketHandle handle;
  TestCompletionCallback callback;
  EXPECT_EQ(ERR_IO_PENDING, handle.Init("a", params_, kDefaultPriority,
                                        &callback, pool_, BoundNetLog()));

  ClientSocketHandle handles[4];
  for (size_t i = 0; i < arraysize(handles); ++i) {
    TestCompletionCallback callback;
    EXPECT_EQ(ERR_IO_PENDING, handles[i].Init("b", params_, kDefaultPriority,
                                              &callback, pool_, BoundNetLog()));
  }

  // One will be stalled, cancel all the handles now.
  // This should hit the OnAvailableSocketSlot() code where we previously had
  // stalled groups, but no longer have any.
  for (size_t i = 0; i < arraysize(handles); ++i)
    handles[i].Reset();
}

TEST_F(ClientSocketPoolBaseTest, CancelStalledSocketAtSocketLimit) {
  CreatePool(kDefaultMaxSockets, kDefaultMaxSocketsPerGroup);
  connect_job_factory_->set_job_type(TestConnectJob::kMockJob);

  {
    ClientSocketHandle handles[kDefaultMaxSockets];
    TestCompletionCallback callbacks[kDefaultMaxSockets];
    for (int i = 0; i < kDefaultMaxSockets; ++i) {
      EXPECT_EQ(OK, handles[i].Init(IntToString(i), params_, kDefaultPriority,
                                    &callbacks[i], pool_, BoundNetLog()));
    }

    // Force a stalled group.
    ClientSocketHandle stalled_handle;
    TestCompletionCallback callback;
    EXPECT_EQ(ERR_IO_PENDING, stalled_handle.Init("foo", params_,
                                                  kDefaultPriority, &callback,
                                                  pool_, BoundNetLog()));

    // Cancel the stalled request.
    stalled_handle.Reset();

    EXPECT_EQ(kDefaultMaxSockets, client_socket_factory_.allocation_count());
    EXPECT_EQ(0, pool_->IdleSocketCount());

    // Dropping out of scope will close all handles and return them to idle.
  }

  EXPECT_EQ(kDefaultMaxSockets, client_socket_factory_.allocation_count());
  EXPECT_EQ(kDefaultMaxSockets, pool_->IdleSocketCount());
}

TEST_F(ClientSocketPoolBaseTest, CancelPendingSocketAtSocketLimit) {
  CreatePool(kDefaultMaxSockets, kDefaultMaxSocketsPerGroup);
  connect_job_factory_->set_job_type(TestConnectJob::kMockWaitingJob);

  {
    ClientSocketHandle handles[kDefaultMaxSockets];
    for (int i = 0; i < kDefaultMaxSockets; ++i) {
      TestCompletionCallback callback;
      EXPECT_EQ(ERR_IO_PENDING, handles[i].Init(IntToString(i), params_,
                                                kDefaultPriority, &callback,
                                                pool_, BoundNetLog()));
    }

    // Force a stalled group.
    connect_job_factory_->set_job_type(TestConnectJob::kMockPendingJob);
    ClientSocketHandle stalled_handle;
    TestCompletionCallback callback;
    EXPECT_EQ(ERR_IO_PENDING, stalled_handle.Init("foo", params_,
                                                  kDefaultPriority, &callback,
                                                  pool_, BoundNetLog()));

    // Since it is stalled, it should have no connect jobs.
    EXPECT_EQ(0, pool_->NumConnectJobsInGroup("foo"));

    // Cancel the stalled request.
    handles[0].Reset();

    // Wait for the pending job to be guaranteed to complete.
    PlatformThread::Sleep(TestConnectJob::kPendingConnectDelay * 2);

    MessageLoop::current()->RunAllPending();

    // Now we should have a connect job.
    EXPECT_EQ(1, pool_->NumConnectJobsInGroup("foo"));

    // The stalled socket should connect.
    EXPECT_EQ(OK, callback.WaitForResult());

    EXPECT_EQ(kDefaultMaxSockets + 1,
              client_socket_factory_.allocation_count());
    EXPECT_EQ(0, pool_->IdleSocketCount());
    EXPECT_EQ(0, pool_->NumConnectJobsInGroup("foo"));

    // Dropping out of scope will close all handles and return them to idle.
  }

  EXPECT_EQ(1, pool_->IdleSocketCount());
}

TEST_F(ClientSocketPoolBaseTest, WaitForStalledSocketAtSocketLimit) {
  CreatePool(kDefaultMaxSockets, kDefaultMaxSocketsPerGroup);
  connect_job_factory_->set_job_type(TestConnectJob::kMockJob);

  ClientSocketHandle stalled_handle;
  TestCompletionCallback callback;
  {
    ClientSocketHandle handles[kDefaultMaxSockets];
    for (int i = 0; i < kDefaultMaxSockets; ++i) {
      TestCompletionCallback callback;
      EXPECT_EQ(OK, handles[i].Init(StringPrintf("Take 2: %d", i), params_,
                                    kDefaultPriority, &callback, pool_,
                                    BoundNetLog()));
    }

    EXPECT_EQ(kDefaultMaxSockets, client_socket_factory_.allocation_count());
    EXPECT_EQ(0, pool_->IdleSocketCount());

    // Now we will hit the socket limit.
    EXPECT_EQ(ERR_IO_PENDING, stalled_handle.Init("foo", params_,
                                                  kDefaultPriority, &callback,
                                                  pool_, BoundNetLog()));

    // Dropping out of scope will close all handles and return them to idle.
  }

  // But if we wait for it, the released idle sockets will be closed in
  // preference of the waiting request.
  EXPECT_EQ(OK, callback.WaitForResult());

  EXPECT_EQ(kDefaultMaxSockets + 1, client_socket_factory_.allocation_count());
  EXPECT_EQ(3, pool_->IdleSocketCount());
}

// Regression test for http://crbug.com/40952.
TEST_F(ClientSocketPoolBaseTest, CloseIdleSocketAtSocketLimitDeleteGroup) {
  CreatePool(kDefaultMaxSockets, kDefaultMaxSocketsPerGroup);
  pool_->EnableBackupJobs();
  connect_job_factory_->set_job_type(TestConnectJob::kMockJob);

  for (int i = 0; i < kDefaultMaxSockets; ++i) {
    ClientSocketHandle handle;
    TestCompletionCallback callback;
    EXPECT_EQ(OK, handle.Init(IntToString(i), params_, kDefaultPriority,
                              &callback, pool_, BoundNetLog()));
  }

  // Flush all the DoReleaseSocket tasks.
  MessageLoop::current()->RunAllPending();

  // Stall a group.  Set a pending job so it'll trigger a backup job if we don't
  // reuse a socket.
  connect_job_factory_->set_job_type(TestConnectJob::kMockPendingJob);
  ClientSocketHandle handle;
  TestCompletionCallback callback;

  // "0" is special here, since it should be the first entry in the sorted map,
  // which is the one which we would close an idle socket for.  We shouldn't
  // close an idle socket though, since we should reuse the idle socket.
  EXPECT_EQ(OK, handle.Init("0", params_, kDefaultPriority, &callback, pool_,
                            BoundNetLog()));

  EXPECT_EQ(kDefaultMaxSockets, client_socket_factory_.allocation_count());
  EXPECT_EQ(kDefaultMaxSockets - 1, pool_->IdleSocketCount());
}

TEST_F(ClientSocketPoolBaseTest, PendingRequests) {
  CreatePool(kDefaultMaxSockets, kDefaultMaxSocketsPerGroup);

  EXPECT_EQ(OK, StartRequest("a", kDefaultPriority));
  EXPECT_EQ(OK, StartRequest("a", kDefaultPriority));
  EXPECT_EQ(ERR_IO_PENDING, StartRequest("a", LOWEST));
  EXPECT_EQ(ERR_IO_PENDING, StartRequest("a", MEDIUM));
  EXPECT_EQ(ERR_IO_PENDING, StartRequest("a", HIGHEST));
  EXPECT_EQ(ERR_IO_PENDING, StartRequest("a", LOW));
  EXPECT_EQ(ERR_IO_PENDING, StartRequest("a", LOWEST));

  ReleaseAllConnections(KEEP_ALIVE);

  EXPECT_EQ(kDefaultMaxSocketsPerGroup,
            client_socket_factory_.allocation_count());
  EXPECT_EQ(requests_.size() - kDefaultMaxSocketsPerGroup, completion_count_);

  EXPECT_EQ(1, GetOrderOfRequest(1));
  EXPECT_EQ(2, GetOrderOfRequest(2));
  EXPECT_EQ(6, GetOrderOfRequest(3));
  EXPECT_EQ(4, GetOrderOfRequest(4));
  EXPECT_EQ(3, GetOrderOfRequest(5));
  EXPECT_EQ(5, GetOrderOfRequest(6));
  EXPECT_EQ(7, GetOrderOfRequest(7));

  // Make sure we test order of all requests made.
  EXPECT_EQ(kIndexOutOfBounds, GetOrderOfRequest(8));
}

TEST_F(ClientSocketPoolBaseTest, PendingRequests_NoKeepAlive) {
  CreatePool(kDefaultMaxSockets, kDefaultMaxSocketsPerGroup);

  EXPECT_EQ(OK, StartRequest("a", kDefaultPriority));
  EXPECT_EQ(OK, StartRequest("a", kDefaultPriority));
  EXPECT_EQ(ERR_IO_PENDING, StartRequest("a", LOWEST));
  EXPECT_EQ(ERR_IO_PENDING, StartRequest("a", MEDIUM));
  EXPECT_EQ(ERR_IO_PENDING, StartRequest("a", HIGHEST));
  EXPECT_EQ(ERR_IO_PENDING, StartRequest("a", LOW));
  EXPECT_EQ(ERR_IO_PENDING, StartRequest("a", LOWEST));

  ReleaseAllConnections(NO_KEEP_ALIVE);

  for (size_t i = kDefaultMaxSocketsPerGroup; i < requests_.size(); ++i)
    EXPECT_EQ(OK, requests_[i]->WaitForResult());

  EXPECT_EQ(static_cast<int>(requests_.size()),
            client_socket_factory_.allocation_count());
  EXPECT_EQ(requests_.size() - kDefaultMaxSocketsPerGroup, completion_count_);
}

// This test will start up a RequestSocket() and then immediately Cancel() it.
// The pending connect job will be cancelled and should not call back into
// ClientSocketPoolBase.
TEST_F(ClientSocketPoolBaseTest, CancelRequestClearGroup) {
  CreatePool(kDefaultMaxSockets, kDefaultMaxSocketsPerGroup);

  connect_job_factory_->set_job_type(TestConnectJob::kMockPendingJob);
  TestSocketRequest req(&request_order_, &completion_count_);
  EXPECT_EQ(ERR_IO_PENDING, req.handle()->Init("a", params_, kDefaultPriority,
                                               &req, pool_, BoundNetLog()));
  req.handle()->Reset();
}

TEST_F(ClientSocketPoolBaseTest, ConnectCancelConnect) {
  CreatePool(kDefaultMaxSockets, kDefaultMaxSocketsPerGroup);

  connect_job_factory_->set_job_type(TestConnectJob::kMockPendingJob);
  ClientSocketHandle handle;
  TestCompletionCallback callback;
  TestSocketRequest req(&request_order_, &completion_count_);

  EXPECT_EQ(ERR_IO_PENDING, handle.Init("a", params_, kDefaultPriority,
                                        &callback, pool_, BoundNetLog()));

  handle.Reset();

  TestCompletionCallback callback2;
  EXPECT_EQ(ERR_IO_PENDING, handle.Init("a", params_, kDefaultPriority,
                                        &callback2, pool_, BoundNetLog()));

  EXPECT_EQ(OK, callback2.WaitForResult());
  EXPECT_FALSE(callback.have_result());

  handle.Reset();
}

TEST_F(ClientSocketPoolBaseTest, CancelRequest) {
  CreatePool(kDefaultMaxSockets, kDefaultMaxSocketsPerGroup);

  EXPECT_EQ(OK, StartRequest("a", kDefaultPriority));
  EXPECT_EQ(OK, StartRequest("a", kDefaultPriority));
  EXPECT_EQ(ERR_IO_PENDING, StartRequest("a", LOWEST));
  EXPECT_EQ(ERR_IO_PENDING, StartRequest("a", MEDIUM));
  EXPECT_EQ(ERR_IO_PENDING, StartRequest("a", HIGHEST));
  EXPECT_EQ(ERR_IO_PENDING, StartRequest("a", LOW));
  EXPECT_EQ(ERR_IO_PENDING, StartRequest("a", LOWEST));

  // Cancel a request.
  size_t index_to_cancel = kDefaultMaxSocketsPerGroup + 2;
  EXPECT_FALSE(requests_[index_to_cancel]->handle()->is_initialized());
  requests_[index_to_cancel]->handle()->Reset();

  ReleaseAllConnections(KEEP_ALIVE);

  EXPECT_EQ(kDefaultMaxSocketsPerGroup,
            client_socket_factory_.allocation_count());
  EXPECT_EQ(requests_.size() - kDefaultMaxSocketsPerGroup - 1,
            completion_count_);

  EXPECT_EQ(1, GetOrderOfRequest(1));
  EXPECT_EQ(2, GetOrderOfRequest(2));
  EXPECT_EQ(5, GetOrderOfRequest(3));
  EXPECT_EQ(3, GetOrderOfRequest(4));
  EXPECT_EQ(kRequestNotFound, GetOrderOfRequest(5));  // Canceled request.
  EXPECT_EQ(4, GetOrderOfRequest(6));
  EXPECT_EQ(6, GetOrderOfRequest(7));

  // Make sure we test order of all requests made.
  EXPECT_EQ(kIndexOutOfBounds, GetOrderOfRequest(8));
}

class RequestSocketCallback : public CallbackRunner< Tuple1<int> > {
 public:
  RequestSocketCallback(ClientSocketHandle* handle,
                        TestClientSocketPool* pool,
                        TestConnectJobFactory* test_connect_job_factory,
                        TestConnectJob::JobType next_job_type)
      : handle_(handle),
        pool_(pool),
        within_callback_(false),
        test_connect_job_factory_(test_connect_job_factory),
        next_job_type_(next_job_type) {}

  virtual void RunWithParams(const Tuple1<int>& params) {
    callback_.RunWithParams(params);
    ASSERT_EQ(OK, params.a);

    if (!within_callback_) {
      test_connect_job_factory_->set_job_type(next_job_type_);

      // Don't allow reuse of the socket.  Disconnect it and then release it and
      // run through the MessageLoop once to get it completely released.
      handle_->socket()->Disconnect();
      handle_->Reset();
      {
        MessageLoop::ScopedNestableTaskAllower nestable(
            MessageLoop::current());
        MessageLoop::current()->RunAllPending();
      }
      within_callback_ = true;
      TestCompletionCallback next_job_callback;
      scoped_refptr<TestSocketParams> params = new TestSocketParams();
      int rv = handle_->Init("a", params, kDefaultPriority, &next_job_callback,
                             pool_, BoundNetLog());
      switch (next_job_type_) {
        case TestConnectJob::kMockJob:
          EXPECT_EQ(OK, rv);
          break;
        case TestConnectJob::kMockPendingJob:
          EXPECT_EQ(ERR_IO_PENDING, rv);

          // For pending jobs, wait for new socket to be created. This makes
          // sure there are no more pending operations nor any unclosed sockets
          // when the test finishes.
          // We need to give it a little bit of time to run, so that all the
          // operations that happen on timers (e.g. cleanup of idle
          // connections) can execute.
          {
            MessageLoop::ScopedNestableTaskAllower nestable(
                MessageLoop::current());
            PlatformThread::Sleep(10);
            EXPECT_EQ(OK, next_job_callback.WaitForResult());
          }
          break;
        default:
          FAIL() << "Unexpected job type: " << next_job_type_;
          break;
      }
    }
  }

  int WaitForResult() {
    return callback_.WaitForResult();
  }

 private:
  ClientSocketHandle* const handle_;
  const scoped_refptr<TestClientSocketPool> pool_;
  bool within_callback_;
  TestConnectJobFactory* const test_connect_job_factory_;
  TestConnectJob::JobType next_job_type_;
  TestCompletionCallback callback_;
};

TEST_F(ClientSocketPoolBaseTest, RequestPendingJobTwice) {
  CreatePool(kDefaultMaxSockets, kDefaultMaxSocketsPerGroup);

  connect_job_factory_->set_job_type(TestConnectJob::kMockPendingJob);
  ClientSocketHandle handle;
  RequestSocketCallback callback(
      &handle, pool_.get(), connect_job_factory_,
      TestConnectJob::kMockPendingJob);
  int rv = handle.Init("a", params_, kDefaultPriority, &callback, pool_,
                       BoundNetLog());
  ASSERT_EQ(ERR_IO_PENDING, rv);

  EXPECT_EQ(OK, callback.WaitForResult());
}

TEST_F(ClientSocketPoolBaseTest, RequestPendingJobThenSynchronous) {
  CreatePool(kDefaultMaxSockets, kDefaultMaxSocketsPerGroup);

  connect_job_factory_->set_job_type(TestConnectJob::kMockPendingJob);
  ClientSocketHandle handle;
  RequestSocketCallback callback(
      &handle, pool_.get(), connect_job_factory_, TestConnectJob::kMockJob);
  int rv = handle.Init("a", params_, kDefaultPriority, &callback, pool_,
                       BoundNetLog());
  ASSERT_EQ(ERR_IO_PENDING, rv);

  EXPECT_EQ(OK, callback.WaitForResult());
}

// Make sure that pending requests get serviced after active requests get
// cancelled.
TEST_F(ClientSocketPoolBaseTest, CancelActiveRequestWithPendingRequests) {
  CreatePool(kDefaultMaxSockets, kDefaultMaxSocketsPerGroup);

  connect_job_factory_->set_job_type(TestConnectJob::kMockPendingJob);

  EXPECT_EQ(ERR_IO_PENDING, StartRequest("a", kDefaultPriority));
  EXPECT_EQ(ERR_IO_PENDING, StartRequest("a", kDefaultPriority));
  EXPECT_EQ(ERR_IO_PENDING, StartRequest("a", kDefaultPriority));
  EXPECT_EQ(ERR_IO_PENDING, StartRequest("a", kDefaultPriority));
  EXPECT_EQ(ERR_IO_PENDING, StartRequest("a", kDefaultPriority));
  EXPECT_EQ(ERR_IO_PENDING, StartRequest("a", kDefaultPriority));
  EXPECT_EQ(ERR_IO_PENDING, StartRequest("a", kDefaultPriority));

  // Now, kDefaultMaxSocketsPerGroup requests should be active.
  // Let's cancel them.
  for (int i = 0; i < kDefaultMaxSocketsPerGroup; ++i) {
    ASSERT_FALSE(requests_[i]->handle()->is_initialized());
    requests_[i]->handle()->Reset();
  }

  // Let's wait for the rest to complete now.
  for (size_t i = kDefaultMaxSocketsPerGroup; i < requests_.size(); ++i) {
    EXPECT_EQ(OK, requests_[i]->WaitForResult());
    requests_[i]->handle()->Reset();
  }

  EXPECT_EQ(requests_.size() - kDefaultMaxSocketsPerGroup, completion_count_);
}

// Make sure that pending requests get serviced after active requests fail.
TEST_F(ClientSocketPoolBaseTest, FailingActiveRequestWithPendingRequests) {
  const size_t kMaxSockets = 5;
  CreatePool(kMaxSockets, kDefaultMaxSocketsPerGroup);

  connect_job_factory_->set_job_type(TestConnectJob::kMockPendingFailingJob);

  const size_t kNumberOfRequests = 2 * kDefaultMaxSocketsPerGroup + 1;
  ASSERT_LE(kNumberOfRequests, kMaxSockets);  // Otherwise the test will hang.

  // Queue up all the requests
  for (size_t i = 0; i < kNumberOfRequests; ++i)
    EXPECT_EQ(ERR_IO_PENDING, StartRequest("a", kDefaultPriority));

  for (size_t i = 0; i < kNumberOfRequests; ++i)
    EXPECT_EQ(ERR_CONNECTION_FAILED, requests_[i]->WaitForResult());
}

TEST_F(ClientSocketPoolBaseTest, CancelActiveRequestThenRequestSocket) {
  CreatePool(kDefaultMaxSockets, kDefaultMaxSocketsPerGroup);

  connect_job_factory_->set_job_type(TestConnectJob::kMockPendingJob);

  TestSocketRequest req(&request_order_, &completion_count_);
  int rv = req.handle()->Init("a", params_, kDefaultPriority, &req, pool_,
                              BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  // Cancel the active request.
  req.handle()->Reset();

  rv = req.handle()->Init("a", params_, kDefaultPriority, &req, pool_,
                          BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);
  EXPECT_EQ(OK, req.WaitForResult());

  EXPECT_FALSE(req.handle()->is_reused());
  EXPECT_EQ(1U, completion_count_);
  EXPECT_EQ(2, client_socket_factory_.allocation_count());
}

// Regression test for http://crbug.com/17985.
TEST_F(ClientSocketPoolBaseTest, GroupWithPendingRequestsIsNotEmpty) {
  const int kMaxSockets = 3;
  const int kMaxSocketsPerGroup = 2;
  CreatePool(kMaxSockets, kMaxSocketsPerGroup);

  const RequestPriority kHighPriority = HIGHEST;

  EXPECT_EQ(OK, StartRequest("a", kDefaultPriority));
  EXPECT_EQ(OK, StartRequest("a", kDefaultPriority));

  // This is going to be a pending request in an otherwise empty group.
  EXPECT_EQ(ERR_IO_PENDING, StartRequest("a", kDefaultPriority));

  // Reach the maximum socket limit.
  EXPECT_EQ(OK, StartRequest("b", kDefaultPriority));

  // Create a stalled group with high priorities.
  EXPECT_EQ(ERR_IO_PENDING, StartRequest("c", kHighPriority));
  EXPECT_EQ(ERR_IO_PENDING, StartRequest("c", kHighPriority));

  // Release the first two sockets from "a".  Because this is a keepalive,
  // the first release will unblock the pending request for "a".  The
  // second release will unblock a request for "c", becaue it is the next
  // high priority socket.
  EXPECT_TRUE(ReleaseOneConnection(KEEP_ALIVE));
  EXPECT_TRUE(ReleaseOneConnection(KEEP_ALIVE));

  // Closing idle sockets should not get us into trouble, but in the bug
  // we were hitting a CHECK here.
  EXPECT_EQ(0, pool_->IdleSocketCountInGroup("a"));
  pool_->CloseIdleSockets();

  MessageLoop::current()->RunAllPending();  // Run the released socket wakeups
}

TEST_F(ClientSocketPoolBaseTest, BasicAsynchronous) {
  CreatePool(kDefaultMaxSockets, kDefaultMaxSocketsPerGroup);

  connect_job_factory_->set_job_type(TestConnectJob::kMockPendingJob);
  TestSocketRequest req(&request_order_, &completion_count_);
  CapturingBoundNetLog log(CapturingNetLog::kUnbounded);
  int rv = req.handle()->Init("a", params_, LOWEST, &req, pool_, log.bound());
  EXPECT_EQ(ERR_IO_PENDING, rv);
  EXPECT_EQ(LOAD_STATE_CONNECTING, pool_->GetLoadState("a", req.handle()));
  EXPECT_EQ(OK, req.WaitForResult());
  EXPECT_TRUE(req.handle()->is_initialized());
  EXPECT_TRUE(req.handle()->socket());
  req.handle()->Reset();

  EXPECT_EQ(4u, log.entries().size());
  EXPECT_TRUE(LogContainsBeginEvent(
      log.entries(), 0, NetLog::TYPE_SOCKET_POOL));
  EXPECT_TRUE(LogContainsEvent(
      log.entries(), 1, NetLog::TYPE_SOCKET_POOL_BOUND_TO_CONNECT_JOB,
      NetLog::PHASE_NONE));
  EXPECT_TRUE(LogContainsEvent(
      log.entries(), 2, NetLog::TYPE_SOCKET_POOL_BOUND_TO_SOCKET,
      NetLog::PHASE_NONE));
  EXPECT_TRUE(LogContainsEndEvent(
      log.entries(), 3, NetLog::TYPE_SOCKET_POOL));
}

TEST_F(ClientSocketPoolBaseTest,
       InitConnectionAsynchronousFailure) {
  CreatePool(kDefaultMaxSockets, kDefaultMaxSocketsPerGroup);

  connect_job_factory_->set_job_type(TestConnectJob::kMockPendingFailingJob);
  TestSocketRequest req(&request_order_, &completion_count_);
  CapturingBoundNetLog log(CapturingNetLog::kUnbounded);
  // Set the additional error state members to ensure that they get cleared.
  req.handle()->set_is_ssl_error(true);
  scoped_refptr<HttpResponseHeaders> headers(new HttpResponseHeaders(""));
  req.handle()->set_tunnel_auth_response_info(headers, NULL);
  EXPECT_EQ(ERR_IO_PENDING, req.handle()->Init("a", params_, kDefaultPriority,
                                               &req, pool_, log.bound()));
  EXPECT_EQ(LOAD_STATE_CONNECTING, pool_->GetLoadState("a", req.handle()));
  EXPECT_EQ(ERR_CONNECTION_FAILED, req.WaitForResult());
  EXPECT_FALSE(req.handle()->is_ssl_error());
  EXPECT_TRUE(req.handle()->tunnel_auth_response_info().headers.get() == NULL);

  EXPECT_EQ(3u, log.entries().size());
  EXPECT_TRUE(LogContainsBeginEvent(
      log.entries(), 0, NetLog::TYPE_SOCKET_POOL));
  EXPECT_TRUE(LogContainsEvent(
      log.entries(), 1, NetLog::TYPE_SOCKET_POOL_BOUND_TO_CONNECT_JOB,
      NetLog::PHASE_NONE));
  EXPECT_TRUE(LogContainsEndEvent(
      log.entries(), 2, NetLog::TYPE_SOCKET_POOL));
}

TEST_F(ClientSocketPoolBaseTest, TwoRequestsCancelOne) {
  // TODO(eroman): Add back the log expectations! Removed them because the
  //               ordering is difficult, and some may fire during destructor.
  CreatePool(kDefaultMaxSockets, kDefaultMaxSocketsPerGroup);

  connect_job_factory_->set_job_type(TestConnectJob::kMockPendingJob);
  TestSocketRequest req(&request_order_, &completion_count_);
  TestSocketRequest req2(&request_order_, &completion_count_);

  EXPECT_EQ(ERR_IO_PENDING, req.handle()->Init("a", params_, kDefaultPriority,
                                               &req, pool_, BoundNetLog()));
  CapturingBoundNetLog log2(CapturingNetLog::kUnbounded);
  EXPECT_EQ(ERR_IO_PENDING, req2.handle()->Init("a", params_, kDefaultPriority,
                                                &req2, pool_, BoundNetLog()));

  req.handle()->Reset();


  // At this point, request 2 is just waiting for the connect job to finish.

  EXPECT_EQ(OK, req2.WaitForResult());
  req2.handle()->Reset();

  // Now request 2 has actually finished.
  // TODO(eroman): Add back log expectations.
}

TEST_F(ClientSocketPoolBaseTest, CancelRequestLimitsJobs) {
  CreatePool(kDefaultMaxSockets, kDefaultMaxSocketsPerGroup);

  connect_job_factory_->set_job_type(TestConnectJob::kMockPendingJob);

  EXPECT_EQ(ERR_IO_PENDING, StartRequest("a", LOWEST));
  EXPECT_EQ(ERR_IO_PENDING, StartRequest("a", LOW));
  EXPECT_EQ(ERR_IO_PENDING, StartRequest("a", MEDIUM));
  EXPECT_EQ(ERR_IO_PENDING, StartRequest("a", HIGHEST));

  EXPECT_EQ(kDefaultMaxSocketsPerGroup, pool_->NumConnectJobsInGroup("a"));
  requests_[2]->handle()->Reset();
  requests_[3]->handle()->Reset();
  EXPECT_EQ(kDefaultMaxSocketsPerGroup, pool_->NumConnectJobsInGroup("a"));

  requests_[1]->handle()->Reset();
  EXPECT_EQ(kDefaultMaxSocketsPerGroup, pool_->NumConnectJobsInGroup("a"));

  requests_[0]->handle()->Reset();
  EXPECT_EQ(kDefaultMaxSocketsPerGroup, pool_->NumConnectJobsInGroup("a"));
}

// When requests and ConnectJobs are not coupled, the request will get serviced
// by whatever comes first.
TEST_F(ClientSocketPoolBaseTest, ReleaseSockets) {
  CreatePool(kDefaultMaxSockets, kDefaultMaxSocketsPerGroup);

  // Start job 1 (async OK)
  connect_job_factory_->set_job_type(TestConnectJob::kMockPendingJob);

  TestSocketRequest req1(&request_order_, &completion_count_);
  int rv = req1.handle()->Init("a", params_, kDefaultPriority, &req1, pool_,
                               BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);
  EXPECT_EQ(OK, req1.WaitForResult());

  // Job 1 finished OK.  Start job 2 (also async OK).  Request 3 is pending
  // without a job.
  connect_job_factory_->set_job_type(TestConnectJob::kMockWaitingJob);

  TestSocketRequest req2(&request_order_, &completion_count_);
  rv = req2.handle()->Init("a", params_, kDefaultPriority, &req2, pool_,
                           BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);
  TestSocketRequest req3(&request_order_, &completion_count_);
  rv = req3.handle()->Init("a", params_, kDefaultPriority, &req3, pool_,
                           BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  // Both Requests 2 and 3 are pending.  We release socket 1 which should
  // service request 2.  Request 3 should still be waiting.
  req1.handle()->Reset();
  MessageLoop::current()->RunAllPending();  // Run the released socket wakeups
  ASSERT_TRUE(req2.handle()->socket());
  EXPECT_EQ(OK, req2.WaitForResult());
  EXPECT_FALSE(req3.handle()->socket());

  // Signal job 2, which should service request 3.

  client_socket_factory_.SignalJobs();
  EXPECT_EQ(OK, req3.WaitForResult());

  ASSERT_EQ(3U, request_order_.size());
  EXPECT_EQ(&req1, request_order_[0]);
  EXPECT_EQ(&req2, request_order_[1]);
  EXPECT_EQ(&req3, request_order_[2]);
  EXPECT_EQ(0, pool_->IdleSocketCountInGroup("a"));
}

// The requests are not coupled to the jobs.  So, the requests should finish in
// their priority / insertion order.
TEST_F(ClientSocketPoolBaseTest, PendingJobCompletionOrder) {
  CreatePool(kDefaultMaxSockets, kDefaultMaxSocketsPerGroup);
  // First two jobs are async.
  connect_job_factory_->set_job_type(TestConnectJob::kMockPendingFailingJob);

  TestSocketRequest req1(&request_order_, &completion_count_);
  int rv = req1.handle()->Init("a", params_, kDefaultPriority, &req1, pool_,
                               BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  TestSocketRequest req2(&request_order_, &completion_count_);
  rv = req2.handle()->Init("a", params_, kDefaultPriority, &req2, pool_,
                           BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  // The pending job is sync.
  connect_job_factory_->set_job_type(TestConnectJob::kMockJob);

  TestSocketRequest req3(&request_order_, &completion_count_);
  rv = req3.handle()->Init("a", params_, kDefaultPriority, &req3, pool_,
                           BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  EXPECT_EQ(ERR_CONNECTION_FAILED, req1.WaitForResult());
  EXPECT_EQ(OK, req2.WaitForResult());
  EXPECT_EQ(ERR_CONNECTION_FAILED, req3.WaitForResult());

  ASSERT_EQ(3U, request_order_.size());
  EXPECT_EQ(&req1, request_order_[0]);
  EXPECT_EQ(&req2, request_order_[1]);
  EXPECT_EQ(&req3, request_order_[2]);
}

TEST_F(ClientSocketPoolBaseTest, LoadState) {
  CreatePool(kDefaultMaxSockets, kDefaultMaxSocketsPerGroup);
  connect_job_factory_->set_job_type(
      TestConnectJob::kMockAdvancingLoadStateJob);

  TestSocketRequest req1(&request_order_, &completion_count_);
  int rv = req1.handle()->Init("a", params_, kDefaultPriority, &req1, pool_,
                               BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);
  EXPECT_EQ(LOAD_STATE_IDLE, req1.handle()->GetLoadState());

  MessageLoop::current()->RunAllPending();

  TestSocketRequest req2(&request_order_, &completion_count_);
  rv = req2.handle()->Init("a", params_, kDefaultPriority, &req2, pool_,
                           BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);
  EXPECT_NE(LOAD_STATE_IDLE, req1.handle()->GetLoadState());
  EXPECT_NE(LOAD_STATE_IDLE, req2.handle()->GetLoadState());
}

TEST_F(ClientSocketPoolBaseTest, Recoverable) {
  CreatePool(kDefaultMaxSockets, kDefaultMaxSocketsPerGroup);
  connect_job_factory_->set_job_type(TestConnectJob::kMockRecoverableJob);

  TestSocketRequest req(&request_order_, &completion_count_);
  EXPECT_EQ(ERR_PROXY_AUTH_REQUESTED, req.handle()->Init("a", params_,
                                                         kDefaultPriority,
                                                         &req, pool_,
                                                         BoundNetLog()));
  EXPECT_TRUE(req.handle()->is_initialized());
  EXPECT_TRUE(req.handle()->socket());
  req.handle()->Reset();
}

TEST_F(ClientSocketPoolBaseTest, AsyncRecoverable) {
  CreatePool(kDefaultMaxSockets, kDefaultMaxSocketsPerGroup);

  connect_job_factory_->set_job_type(
      TestConnectJob::kMockPendingRecoverableJob);
  TestSocketRequest req(&request_order_, &completion_count_);
  EXPECT_EQ(ERR_IO_PENDING, req.handle()->Init("a", params_, kDefaultPriority,
                                               &req, pool_, BoundNetLog()));
  EXPECT_EQ(LOAD_STATE_CONNECTING, pool_->GetLoadState("a", req.handle()));
  EXPECT_EQ(ERR_PROXY_AUTH_REQUESTED, req.WaitForResult());
  EXPECT_TRUE(req.handle()->is_initialized());
  EXPECT_TRUE(req.handle()->socket());
  req.handle()->Reset();
}

TEST_F(ClientSocketPoolBaseTest, AdditionalErrorStateSynchronous) {
  CreatePool(kDefaultMaxSockets, kDefaultMaxSocketsPerGroup);
  connect_job_factory_->set_job_type(
      TestConnectJob::kMockAdditionalErrorStateJob);

  TestSocketRequest req(&request_order_, &completion_count_);
  EXPECT_EQ(ERR_CONNECTION_FAILED, req.handle()->Init("a", params_,
                                                      kDefaultPriority, &req,
                                                      pool_, BoundNetLog()));
  EXPECT_FALSE(req.handle()->is_initialized());
  EXPECT_FALSE(req.handle()->socket());
  EXPECT_TRUE(req.handle()->is_ssl_error());
  EXPECT_FALSE(req.handle()->tunnel_auth_response_info().headers.get() == NULL);
  req.handle()->Reset();
}

TEST_F(ClientSocketPoolBaseTest, AdditionalErrorStateAsynchronous) {
  CreatePool(kDefaultMaxSockets, kDefaultMaxSocketsPerGroup);

  connect_job_factory_->set_job_type(
      TestConnectJob::kMockPendingAdditionalErrorStateJob);
  TestSocketRequest req(&request_order_, &completion_count_);
  EXPECT_EQ(ERR_IO_PENDING, req.handle()->Init("a", params_, kDefaultPriority,
                                               &req, pool_, BoundNetLog()));
  EXPECT_EQ(LOAD_STATE_CONNECTING, pool_->GetLoadState("a", req.handle()));
  EXPECT_EQ(ERR_CONNECTION_FAILED, req.WaitForResult());
  EXPECT_FALSE(req.handle()->is_initialized());
  EXPECT_FALSE(req.handle()->socket());
  EXPECT_TRUE(req.handle()->is_ssl_error());
  EXPECT_FALSE(req.handle()->tunnel_auth_response_info().headers.get() == NULL);
  req.handle()->Reset();
}

TEST_F(ClientSocketPoolBaseTest, CleanupTimedOutIdleSockets) {
  CreatePoolWithIdleTimeouts(
      kDefaultMaxSockets, kDefaultMaxSocketsPerGroup,
      base::TimeDelta(),  // Time out unused sockets immediately.
      base::TimeDelta::FromDays(1));  // Don't time out used sockets.

  connect_job_factory_->set_job_type(TestConnectJob::kMockPendingJob);

  // Startup two mock pending connect jobs, which will sit in the MessageLoop.

  TestSocketRequest req(&request_order_, &completion_count_);
  int rv = req.handle()->Init("a", params_, LOWEST, &req, pool_, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);
  EXPECT_EQ(LOAD_STATE_CONNECTING, pool_->GetLoadState("a", req.handle()));

  TestSocketRequest req2(&request_order_, &completion_count_);
  rv = req2.handle()->Init("a", params_, LOWEST, &req2, pool_, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);
  EXPECT_EQ(LOAD_STATE_CONNECTING, pool_->GetLoadState("a", req2.handle()));

  // Cancel one of the requests.  Wait for the other, which will get the first
  // job.  Release the socket.  Run the loop again to make sure the second
  // socket is sitting idle and the first one is released (since ReleaseSocket()
  // just posts a DoReleaseSocket() task).

  req.handle()->Reset();
  EXPECT_EQ(OK, req2.WaitForResult());
  req2.handle()->Reset();

  // We post all of our delayed tasks with a 2ms delay. I.e. they don't
  // actually become pending until 2ms after they have been created. In order
  // to flush all tasks, we need to wait so that we know there are no
  // soon-to-be-pending tasks waiting.
  PlatformThread::Sleep(10);
  MessageLoop::current()->RunAllPending();

  ASSERT_EQ(2, pool_->IdleSocketCount());

  // Invoke the idle socket cleanup check.  Only one socket should be left, the
  // used socket.  Request it to make sure that it's used.

  pool_->CleanupTimedOutIdleSockets();
  CapturingBoundNetLog log(CapturingNetLog::kUnbounded);
  rv = req.handle()->Init("a", params_, LOWEST, &req, pool_, log.bound());
  EXPECT_EQ(OK, rv);
  EXPECT_TRUE(req.handle()->is_reused());
  EXPECT_TRUE(LogContainsEntryWithType(
      log.entries(), 1, NetLog::TYPE_SOCKET_POOL_REUSED_AN_EXISTING_SOCKET));
}

// Make sure that we process all pending requests even when we're stalling
// because of multiple releasing disconnected sockets.
TEST_F(ClientSocketPoolBaseTest, MultipleReleasingDisconnectedSockets) {
  CreatePoolWithIdleTimeouts(
      kDefaultMaxSockets, kDefaultMaxSocketsPerGroup,
      base::TimeDelta(),  // Time out unused sockets immediately.
      base::TimeDelta::FromDays(1));  // Don't time out used sockets.

  connect_job_factory_->set_job_type(TestConnectJob::kMockJob);

  // Startup 4 connect jobs.  Two of them will be pending.

  TestSocketRequest req(&request_order_, &completion_count_);
  int rv = req.handle()->Init("a", params_, LOWEST, &req, pool_, BoundNetLog());
  EXPECT_EQ(OK, rv);

  TestSocketRequest req2(&request_order_, &completion_count_);
  rv = req2.handle()->Init("a", params_, LOWEST, &req2, pool_, BoundNetLog());
  EXPECT_EQ(OK, rv);

  TestSocketRequest req3(&request_order_, &completion_count_);
  rv = req3.handle()->Init("a", params_, LOWEST, &req3, pool_, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  TestSocketRequest req4(&request_order_, &completion_count_);
  rv = req4.handle()->Init("a", params_, LOWEST, &req4, pool_, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  // Release two disconnected sockets.

  req.handle()->socket()->Disconnect();
  req.handle()->Reset();
  req2.handle()->socket()->Disconnect();
  req2.handle()->Reset();

  EXPECT_EQ(OK, req3.WaitForResult());
  EXPECT_FALSE(req3.handle()->is_reused());
  EXPECT_EQ(OK, req4.WaitForResult());
  EXPECT_FALSE(req4.handle()->is_reused());
}

// Regression test for http://crbug.com/42267.
// When DoReleaseSocket() is processed for one socket, it is blocked because the
// other stalled groups all have releasing sockets, so no progress can be made.
TEST_F(ClientSocketPoolBaseTest, SocketLimitReleasingSockets) {
  CreatePoolWithIdleTimeouts(
      4 /* socket limit */, 4 /* socket limit per group */,
      base::TimeDelta(),  // Time out unused sockets immediately.
      base::TimeDelta::FromDays(1));  // Don't time out used sockets.

  connect_job_factory_->set_job_type(TestConnectJob::kMockJob);

  // Max out the socket limit with 2 per group.

  scoped_ptr<TestSocketRequest> req_a[4];
  scoped_ptr<TestSocketRequest> req_b[4];

  for (int i = 0; i < 2; ++i) {
    req_a[i].reset(new TestSocketRequest(&request_order_, &completion_count_));
    req_b[i].reset(new TestSocketRequest(&request_order_, &completion_count_));
    EXPECT_EQ(OK, req_a[i]->handle()->Init("a", params_, LOWEST, req_a[i].get(),
                                           pool_, BoundNetLog()));
    EXPECT_EQ(OK, req_b[i]->handle()->Init("b", params_, LOWEST, req_b[i].get(),
                                           pool_, BoundNetLog()));
  }

  // Make 4 pending requests, 2 per group.

  for (int i = 2; i < 4; ++i) {
    req_a[i].reset(new TestSocketRequest(&request_order_, &completion_count_));
    req_b[i].reset(new TestSocketRequest(&request_order_, &completion_count_));
    EXPECT_EQ(ERR_IO_PENDING, req_a[i]->handle()->Init("a", params_, LOWEST,
                                                       req_a[i].get(), pool_,
                                                       BoundNetLog()));
    EXPECT_EQ(ERR_IO_PENDING, req_b[i]->handle()->Init("b", params_, LOWEST,
                                                       req_b[i].get(), pool_,
                                                       BoundNetLog()));
  }

  // Release b's socket first.  The order is important, because in
  // DoReleaseSocket(), we'll process b's released socket, and since both b and
  // a are stalled, but 'a' is lower lexicographically, we'll process group 'a'
  // first, which has a releasing socket, so it refuses to start up another
  // ConnectJob.  So, we used to infinite loop on this.
  req_b[0]->handle()->socket()->Disconnect();
  req_b[0]->handle()->Reset();
  req_a[0]->handle()->socket()->Disconnect();
  req_a[0]->handle()->Reset();

  // Used to get stuck here.
  MessageLoop::current()->RunAllPending();

  req_b[1]->handle()->socket()->Disconnect();
  req_b[1]->handle()->Reset();
  req_a[1]->handle()->socket()->Disconnect();
  req_a[1]->handle()->Reset();

  for (int i = 2; i < 4; ++i) {
    EXPECT_EQ(OK, req_b[i]->WaitForResult());
    EXPECT_EQ(OK, req_a[i]->WaitForResult());
  }
}

TEST_F(ClientSocketPoolBaseTest,
       ReleasingDisconnectedSocketsMaintainsPriorityOrder) {
  CreatePool(kDefaultMaxSockets, kDefaultMaxSocketsPerGroup);

  connect_job_factory_->set_job_type(TestConnectJob::kMockPendingJob);

  EXPECT_EQ(ERR_IO_PENDING, StartRequest("a", kDefaultPriority));
  EXPECT_EQ(ERR_IO_PENDING, StartRequest("a", kDefaultPriority));
  EXPECT_EQ(ERR_IO_PENDING, StartRequest("a", kDefaultPriority));
  EXPECT_EQ(ERR_IO_PENDING, StartRequest("a", kDefaultPriority));

  EXPECT_EQ(OK, requests_[0]->WaitForResult());
  EXPECT_EQ(OK, requests_[1]->WaitForResult());
  EXPECT_EQ(2u, completion_count_);

  // Releases one connection.
  EXPECT_TRUE(ReleaseOneConnection(NO_KEEP_ALIVE));
  EXPECT_EQ(OK, requests_[2]->WaitForResult());

  EXPECT_TRUE(ReleaseOneConnection(NO_KEEP_ALIVE));
  EXPECT_EQ(OK, requests_[3]->WaitForResult());
  EXPECT_EQ(4u, completion_count_);

  EXPECT_EQ(1, GetOrderOfRequest(1));
  EXPECT_EQ(2, GetOrderOfRequest(2));
  EXPECT_EQ(3, GetOrderOfRequest(3));
  EXPECT_EQ(4, GetOrderOfRequest(4));

  // Make sure we test order of all requests made.
  EXPECT_EQ(kIndexOutOfBounds, GetOrderOfRequest(5));
}

class TestReleasingSocketRequest : public CallbackRunner< Tuple1<int> > {
 public:
  TestReleasingSocketRequest(TestClientSocketPool* pool, int expected_result,
                             bool reset_releasing_handle)
      : pool_(pool),
        expected_result_(expected_result),
        reset_releasing_handle_(reset_releasing_handle) {}

  ClientSocketHandle* handle() { return &handle_; }

  int WaitForResult() {
    return callback_.WaitForResult();
  }

  virtual void RunWithParams(const Tuple1<int>& params) {
    callback_.RunWithParams(params);
    if (reset_releasing_handle_)
                      handle_.Reset();
    scoped_refptr<TestSocketParams> con_params = new TestSocketParams();
    EXPECT_EQ(expected_result_, handle2_.Init("a", con_params, kDefaultPriority,
                                              &callback2_, pool_,
                                              BoundNetLog()));
  }

 private:
  scoped_refptr<TestClientSocketPool> pool_;
  int expected_result_;
  bool reset_releasing_handle_;
  ClientSocketHandle handle_;
  ClientSocketHandle handle2_;
  TestCompletionCallback callback_;
  TestCompletionCallback callback2_;
};


TEST_F(ClientSocketPoolBaseTest, AdditionalErrorSocketsDontUseSlot) {
  CreatePool(kDefaultMaxSockets, kDefaultMaxSocketsPerGroup);

  EXPECT_EQ(OK, StartRequest("b", kDefaultPriority));
  EXPECT_EQ(OK, StartRequest("a", kDefaultPriority));
  EXPECT_EQ(OK, StartRequest("b", kDefaultPriority));

  EXPECT_EQ(static_cast<int>(requests_.size()),
            client_socket_factory_.allocation_count());

  connect_job_factory_->set_job_type(
      TestConnectJob::kMockPendingAdditionalErrorStateJob);
  TestReleasingSocketRequest req(pool_.get(), OK, false);
  EXPECT_EQ(ERR_IO_PENDING, req.handle()->Init("a", params_, kDefaultPriority,
                                               &req, pool_, BoundNetLog()));
  // The next job should complete synchronously
  connect_job_factory_->set_job_type(TestConnectJob::kMockJob);

  EXPECT_EQ(ERR_CONNECTION_FAILED, req.WaitForResult());
  EXPECT_FALSE(req.handle()->is_initialized());
  EXPECT_FALSE(req.handle()->socket());
  EXPECT_TRUE(req.handle()->is_ssl_error());
  EXPECT_FALSE(req.handle()->tunnel_auth_response_info().headers.get() == NULL);
}

// http://crbug.com/44724 regression test.
// We start releasing the pool when we flush on network change.  When that
// happens, the only active references are in the ClientSocketHandles.  When a
// ConnectJob completes and calls back into the last ClientSocketHandle, that
// callback can release the last reference and delete the pool.  After the
// callback finishes, we go back to the stack frame within the now-deleted pool.
// Executing any code that refers to members of the now-deleted pool can cause
// crashes.
TEST_F(ClientSocketPoolBaseTest, CallbackThatReleasesPool) {
  CreatePool(kDefaultMaxSockets, kDefaultMaxSocketsPerGroup);
  connect_job_factory_->set_job_type(TestConnectJob::kMockPendingFailingJob);

  ClientSocketHandle handle;
  TestCompletionCallback callback;
  EXPECT_EQ(ERR_IO_PENDING, handle.Init("a", params_, kDefaultPriority,
                                        &callback, pool_, BoundNetLog()));

  // Simulate flushing the pool.
  pool_ = NULL;

  // We'll call back into this now.
  callback.WaitForResult();
}

TEST_F(ClientSocketPoolBaseTest, DoNotReuseSocketAfterFlush) {
  CreatePool(kDefaultMaxSockets, kDefaultMaxSocketsPerGroup);
  connect_job_factory_->set_job_type(TestConnectJob::kMockPendingJob);

  ClientSocketHandle handle;
  TestCompletionCallback callback;
  EXPECT_EQ(ERR_IO_PENDING, handle.Init("a", params_, kDefaultPriority,
                                        &callback, pool_, BoundNetLog()));
  EXPECT_EQ(OK, callback.WaitForResult());
  EXPECT_EQ(ClientSocketHandle::UNUSED, handle.reuse_type());

  pool_->Flush();

  handle.Reset();
  MessageLoop::current()->RunAllPending();

  EXPECT_EQ(ERR_IO_PENDING, handle.Init("a", params_, kDefaultPriority,
                                        &callback, pool_, BoundNetLog()));
  EXPECT_EQ(OK, callback.WaitForResult());
  EXPECT_EQ(ClientSocketHandle::UNUSED, handle.reuse_type());
}

// Cancel a pending socket request while we're at max sockets,
// and verify that the backup socket firing doesn't cause a crash.
TEST_F(ClientSocketPoolBaseTest, BackupSocketCancelAtMaxSockets) {
  // Max 4 sockets globally, max 4 sockets per group.
  CreatePool(kDefaultMaxSockets, kDefaultMaxSockets);
  pool_->EnableBackupJobs();

  // Create the first socket and set to ERR_IO_PENDING.  This creates a
  // backup job.
  connect_job_factory_->set_job_type(TestConnectJob::kMockWaitingJob);
  ClientSocketHandle handle;
  TestCompletionCallback callback;
  EXPECT_EQ(ERR_IO_PENDING, handle.Init("bar", params_, kDefaultPriority,
                                        &callback, pool_, BoundNetLog()));

  // Start (MaxSockets - 1) connected sockets to reach max sockets.
  connect_job_factory_->set_job_type(TestConnectJob::kMockJob);
  ClientSocketHandle handles[kDefaultMaxSockets];
  for (int i = 1; i < kDefaultMaxSockets; ++i) {
    TestCompletionCallback callback;
    EXPECT_EQ(OK, handles[i].Init("bar", params_, kDefaultPriority, &callback,
                                  pool_, BoundNetLog()));
  }

  MessageLoop::current()->RunAllPending();

  // Cancel the pending request.
  handle.Reset();

  // Wait for the backup timer to fire (add some slop to ensure it fires)
  PlatformThread::Sleep(ClientSocketPool::kMaxConnectRetryIntervalMs / 2 * 3);

  MessageLoop::current()->RunAllPending();
  EXPECT_EQ(kDefaultMaxSockets, client_socket_factory_.allocation_count());
}

// Test delayed socket binding for the case where we have two connects,
// and while one is waiting on a connect, the other frees up.
// The socket waiting on a connect should switch immediately to the freed
// up socket.
TEST_F(ClientSocketPoolBaseTest, DelayedSocketBindingWaitingForConnect) {
  CreatePool(kDefaultMaxSockets, kDefaultMaxSocketsPerGroup);
  connect_job_factory_->set_job_type(TestConnectJob::kMockPendingJob);

  ClientSocketHandle handle1;
  TestCompletionCallback callback;
  EXPECT_EQ(ERR_IO_PENDING, handle1.Init("a", params_, kDefaultPriority,
                                         &callback, pool_, BoundNetLog()));
  EXPECT_EQ(OK, callback.WaitForResult());

  // No idle sockets, no pending jobs.
  EXPECT_EQ(0, pool_->IdleSocketCount());
  EXPECT_EQ(0, pool_->NumConnectJobsInGroup("a"));

  // Create a second socket to the same host, but this one will wait.
  connect_job_factory_->set_job_type(TestConnectJob::kMockWaitingJob);
  ClientSocketHandle handle2;
  EXPECT_EQ(ERR_IO_PENDING, handle2.Init("a", params_, kDefaultPriority,
                                         &callback, pool_, BoundNetLog()));
  // No idle sockets, and one connecting job.
  EXPECT_EQ(0, pool_->IdleSocketCount());
  EXPECT_EQ(1, pool_->NumConnectJobsInGroup("a"));

  // Return the first handle to the pool.  This will initiate the delayed
  // binding.
  handle1.Reset();

  MessageLoop::current()->RunAllPending();

  // Still no idle sockets, still one pending connect job.
  EXPECT_EQ(0, pool_->IdleSocketCount());
  EXPECT_EQ(1, pool_->NumConnectJobsInGroup("a"));

  // The second socket connected, even though it was a Waiting Job.
  EXPECT_EQ(OK, callback.WaitForResult());

  // And we can see there is still one job waiting.
  EXPECT_EQ(1, pool_->NumConnectJobsInGroup("a"));

  // Finally, signal the waiting Connect.
  client_socket_factory_.SignalJobs();
  EXPECT_EQ(0, pool_->NumConnectJobsInGroup("a"));

  MessageLoop::current()->RunAllPending();
}

// Test delayed socket binding when a group is at capacity and one
// of the group's sockets frees up.
TEST_F(ClientSocketPoolBaseTest, DelayedSocketBindingAtGroupCapacity) {
  CreatePool(kDefaultMaxSockets, kDefaultMaxSocketsPerGroup);
  connect_job_factory_->set_job_type(TestConnectJob::kMockPendingJob);

  ClientSocketHandle handle1;
  TestCompletionCallback callback;
  EXPECT_EQ(ERR_IO_PENDING, handle1.Init("a", params_, kDefaultPriority,
                                         &callback, pool_, BoundNetLog()));
  EXPECT_EQ(OK, callback.WaitForResult());

  // No idle sockets, no pending jobs.
  EXPECT_EQ(0, pool_->IdleSocketCount());
  EXPECT_EQ(0, pool_->NumConnectJobsInGroup("a"));

  // Create a second socket to the same host, but this one will wait.
  connect_job_factory_->set_job_type(TestConnectJob::kMockWaitingJob);
  ClientSocketHandle handle2;
  EXPECT_EQ(ERR_IO_PENDING, handle2.Init("a", params_, kDefaultPriority,
                                         &callback, pool_, BoundNetLog()));
  // No idle sockets, and one connecting job.
  EXPECT_EQ(0, pool_->IdleSocketCount());
  EXPECT_EQ(1, pool_->NumConnectJobsInGroup("a"));

  // Return the first handle to the pool.  This will initiate the delayed
  // binding.
  handle1.Reset();

  MessageLoop::current()->RunAllPending();

  // Still no idle sockets, still one pending connect job.
  EXPECT_EQ(0, pool_->IdleSocketCount());
  EXPECT_EQ(1, pool_->NumConnectJobsInGroup("a"));

  // The second socket connected, even though it was a Waiting Job.
  EXPECT_EQ(OK, callback.WaitForResult());

  // And we can see there is still one job waiting.
  EXPECT_EQ(1, pool_->NumConnectJobsInGroup("a"));

  // Finally, signal the waiting Connect.
  client_socket_factory_.SignalJobs();
  EXPECT_EQ(0, pool_->NumConnectJobsInGroup("a"));

  MessageLoop::current()->RunAllPending();
}

// Test out the case where we have one socket connected, one
// connecting, when the first socket finishes and goes idle.
// Although the second connection is pending, th second request
// should complete, by taking the first socket's idle socket.
TEST_F(ClientSocketPoolBaseTest, DelayedSocketBindingAtStall) {
  CreatePool(kDefaultMaxSockets, kDefaultMaxSocketsPerGroup);
  connect_job_factory_->set_job_type(TestConnectJob::kMockPendingJob);

  ClientSocketHandle handle1;
  TestCompletionCallback callback;
  EXPECT_EQ(ERR_IO_PENDING, handle1.Init("a", params_, kDefaultPriority,
                                         &callback, pool_, BoundNetLog()));
  EXPECT_EQ(OK, callback.WaitForResult());

  // No idle sockets, no pending jobs.
  EXPECT_EQ(0, pool_->IdleSocketCount());
  EXPECT_EQ(0, pool_->NumConnectJobsInGroup("a"));

  // Create a second socket to the same host, but this one will wait.
  connect_job_factory_->set_job_type(TestConnectJob::kMockWaitingJob);
  ClientSocketHandle handle2;
  EXPECT_EQ(ERR_IO_PENDING, handle2.Init("a", params_, kDefaultPriority,
                                         &callback, pool_, BoundNetLog()));
  // No idle sockets, and one connecting job.
  EXPECT_EQ(0, pool_->IdleSocketCount());
  EXPECT_EQ(1, pool_->NumConnectJobsInGroup("a"));

  // Return the first handle to the pool.  This will initiate the delayed
  // binding.
  handle1.Reset();

  MessageLoop::current()->RunAllPending();

  // Still no idle sockets, still one pending connect job.
  EXPECT_EQ(0, pool_->IdleSocketCount());
  EXPECT_EQ(1, pool_->NumConnectJobsInGroup("a"));

  // The second socket connected, even though it was a Waiting Job.
  EXPECT_EQ(OK, callback.WaitForResult());

  // And we can see there is still one job waiting.
  EXPECT_EQ(1, pool_->NumConnectJobsInGroup("a"));

  // Finally, signal the waiting Connect.
  client_socket_factory_.SignalJobs();
  EXPECT_EQ(0, pool_->NumConnectJobsInGroup("a"));

  MessageLoop::current()->RunAllPending();
}

}  // namespace

}  // namespace net
