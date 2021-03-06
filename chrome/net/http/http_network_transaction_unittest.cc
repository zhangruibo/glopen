// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/http/http_network_transaction.h"

#include <math.h>  // ceil
#include <vector>

#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "base/file_path.h"
#include "base/file_util.h"
#include "base/scoped_ptr.h"
#include "net/base/capturing_net_log.h"
#include "net/base/completion_callback.h"
#include "net/base/mock_host_resolver.h"
#include "net/base/net_log.h"
#include "net/base/net_log_unittest.h"
#include "net/base/request_priority.h"
#include "net/base/ssl_config_service_defaults.h"
#include "net/base/ssl_info.h"
#include "net/base/test_completion_callback.h"
#include "net/base/upload_data.h"
#include "net/http/http_auth_handler_digest.h"
#include "net/http/http_auth_handler_mock.h"
#include "net/http/http_auth_handler_ntlm.h"
#include "net/http/http_basic_stream.h"
#include "net/http/http_network_session.h"
#include "net/http/http_stream.h"
#include "net/http/http_transaction_unittest.h"
#include "net/proxy/proxy_config_service_fixed.h"
#include "net/proxy/proxy_resolver.h"
#include "net/proxy/proxy_service.h"
#include "net/socket/client_socket_factory.h"
#include "net/socket/socket_test_util.h"
#include "net/socket/ssl_client_socket.h"
#include "net/spdy/spdy_framer.h"
#include "net/spdy/spdy_session.h"
#include "net/spdy/spdy_session_pool.h"
#include "net/spdy/spdy_test_util.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/platform_test.h"

//-----------------------------------------------------------------------------

namespace net {

class HttpNetworkSessionPeer {
 public:
  explicit HttpNetworkSessionPeer(
      const scoped_refptr<HttpNetworkSession>& session)
      : session_(session) {}

  void SetTCPSocketPool(const scoped_refptr<TCPClientSocketPool>& pool) {
    session_->tcp_socket_pool_ = pool;
  }

  void SetSocketPoolForSOCKSProxy(
      const HostPortPair& socks_proxy,
      const scoped_refptr<SOCKSClientSocketPool>& pool) {
    session_->socks_socket_pools_[socks_proxy] = pool;
  }

  void SetSocketPoolForHTTPProxy(
      const HostPortPair& http_proxy,
      const scoped_refptr<HttpProxyClientSocketPool>& pool) {
    session_->http_proxy_socket_pools_[http_proxy] = pool;
  }

  void SetSSLSocketPool(const scoped_refptr<SSLClientSocketPool>& pool) {
    session_->ssl_socket_pool_ = pool;
  }

  void SetSocketPoolForSSLWithProxy(
      const HostPortPair& proxy_host,
      const scoped_refptr<SSLClientSocketPool>& pool) {
    session_->ssl_socket_pools_for_proxies_[proxy_host] = pool;
  }

 private:
  const scoped_refptr<HttpNetworkSession> session_;

  DISALLOW_COPY_AND_ASSIGN(HttpNetworkSessionPeer);
};

// Helper to manage the lifetimes of the dependencies for a
// HttpNetworkTransaction.
struct SessionDependencies {
  // Default set of dependencies -- "null" proxy service.
  SessionDependencies()
      : host_resolver(new MockHostResolver),
        proxy_service(ProxyService::CreateNull()),
        ssl_config_service(new SSLConfigServiceDefaults),
        http_auth_handler_factory(HttpAuthHandlerFactory::CreateDefault()),
        spdy_session_pool(new SpdySessionPool()),
        net_log(NULL) {}

  // Custom proxy service dependency.
  explicit SessionDependencies(ProxyService* proxy_service)
      : host_resolver(new MockHostResolver),
        proxy_service(proxy_service),
        ssl_config_service(new SSLConfigServiceDefaults),
        http_auth_handler_factory(HttpAuthHandlerFactory::CreateDefault()),
        spdy_session_pool(new SpdySessionPool()),
        net_log(NULL) {}

  scoped_refptr<MockHostResolverBase> host_resolver;
  scoped_refptr<ProxyService> proxy_service;
  scoped_refptr<SSLConfigService> ssl_config_service;
  MockClientSocketFactory socket_factory;
  scoped_ptr<HttpAuthHandlerFactory> http_auth_handler_factory;
  scoped_refptr<SpdySessionPool> spdy_session_pool;
  NetLog* net_log;
};

ProxyService* CreateFixedProxyService(const std::string& proxy) {
  net::ProxyConfig proxy_config;
  proxy_config.proxy_rules().ParseFromString(proxy);
  return ProxyService::CreateFixed(proxy_config);
}

HttpNetworkSession* CreateSession(SessionDependencies* session_deps) {
  return new HttpNetworkSession(session_deps->host_resolver,
                                session_deps->proxy_service,
                                &session_deps->socket_factory,
                                session_deps->ssl_config_service,
                                session_deps->spdy_session_pool,
                                session_deps->http_auth_handler_factory.get(),
                                NULL,
                                session_deps->net_log);
}

class HttpNetworkTransactionTest : public PlatformTest {
 public:
  virtual void SetUp() {
    spdy::SpdyFramer::set_enable_compression_default(false);
  }

  virtual void TearDown() {
    spdy::SpdyFramer::set_enable_compression_default(true);
    // Empty the current queue.
    MessageLoop::current()->RunAllPending();
    PlatformTest::TearDown();
  }

 protected:
  void KeepAliveConnectionResendRequestTest(const MockRead& read_failure);

  struct SimpleGetHelperResult {
    int rv;
    std::string status_line;
    std::string response_data;
  };

  SimpleGetHelperResult SimpleGetHelper(MockRead data_reads[],
                                        size_t reads_count) {
    SimpleGetHelperResult out;

    SessionDependencies session_deps;
    scoped_ptr<HttpTransaction> trans(
        new HttpNetworkTransaction(CreateSession(&session_deps)));

    HttpRequestInfo request;
    request.method = "GET";
    request.url = GURL("http://www.google.com/");
    request.load_flags = 0;

    StaticSocketDataProvider data(data_reads, reads_count, NULL, 0);
    session_deps.socket_factory.AddSocketDataProvider(&data);

    TestCompletionCallback callback;

    CapturingBoundNetLog log(CapturingNetLog::kUnbounded);
    int rv = trans->Start(&request, &callback, log.bound());
    EXPECT_EQ(ERR_IO_PENDING, rv);

    out.rv = callback.WaitForResult();
    if (out.rv != OK)
      return out;

    const HttpResponseInfo* response = trans->GetResponseInfo();
    EXPECT_TRUE(response != NULL);

    EXPECT_TRUE(response->headers != NULL);
    out.status_line = response->headers->GetStatusLine();

    rv = ReadTransaction(trans.get(), &out.response_data);
    EXPECT_EQ(OK, rv);
    size_t pos = ExpectLogContainsSomewhere(
        log.entries(), 0, NetLog::TYPE_HTTP_TRANSACTION_SEND_REQUEST_HEADERS,
        NetLog::PHASE_NONE);
    ExpectLogContainsSomewhere(
        log.entries(), pos,
        NetLog::TYPE_HTTP_TRANSACTION_READ_RESPONSE_HEADERS,
        NetLog::PHASE_NONE);

    return out;
  }

  void ConnectStatusHelperWithExpectedStatus(const MockRead& status,
                                             int expected_status);

  void ConnectStatusHelper(const MockRead& status);
};

// Fill |str| with a long header list that consumes >= |size| bytes.
void FillLargeHeadersString(std::string* str, int size) {
  const char* row =
      "SomeHeaderName: xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\r\n";
  const int sizeof_row = strlen(row);
  const int num_rows = static_cast<int>(
      ceil(static_cast<float>(size) / sizeof_row));
  const int sizeof_data = num_rows * sizeof_row;
  DCHECK(sizeof_data >= size);
  str->reserve(sizeof_data);

  for (int i = 0; i < num_rows; ++i)
    str->append(row, sizeof_row);
}

// Alternative functions that eliminate randomness and dependency on the local
// host name so that the generated NTLM messages are reproducible.
void MockGenerateRandom1(uint8* output, size_t n) {
  static const uint8 bytes[] = {
    0x55, 0x29, 0x66, 0x26, 0x6b, 0x9c, 0x73, 0x54
  };
  static size_t current_byte = 0;
  for (size_t i = 0; i < n; ++i) {
    output[i] = bytes[current_byte++];
    current_byte %= arraysize(bytes);
  }
}

void MockGenerateRandom2(uint8* output, size_t n) {
  static const uint8 bytes[] = {
    0x96, 0x79, 0x85, 0xe7, 0x49, 0x93, 0x70, 0xa1,
    0x4e, 0xe7, 0x87, 0x45, 0x31, 0x5b, 0xd3, 0x1f
  };
  static size_t current_byte = 0;
  for (size_t i = 0; i < n; ++i) {
    output[i] = bytes[current_byte++];
    current_byte %= arraysize(bytes);
  }
}

std::string MockGetHostName() {
  return "WTC-WIN7";
}

template<typename ParentPool>
class CaptureGroupNameSocketPool : public ParentPool {
 public:
  explicit CaptureGroupNameSocketPool(HttpNetworkSession* session);

  const std::string last_group_name_received() const {
    return last_group_name_;
  }

  virtual int RequestSocket(const std::string& group_name,
                            const void* socket_params,
                            RequestPriority priority,
                            ClientSocketHandle* handle,
                            CompletionCallback* callback,
                            const BoundNetLog& net_log) {
    last_group_name_ = group_name;
    return ERR_IO_PENDING;
  }
  virtual void CancelRequest(const std::string& group_name,
                             const ClientSocketHandle* handle) { }
  virtual void ReleaseSocket(const std::string& group_name,
                             ClientSocket* socket) {}
  virtual void CloseIdleSockets() {}
  virtual HostResolver* GetHostResolver() const {
    return NULL;
  }
  virtual int IdleSocketCount() const {
    return 0;
  }
  virtual int IdleSocketCountInGroup(const std::string& group_name) const {
    return 0;
  }
  virtual LoadState GetLoadState(const std::string& group_name,
                                 const ClientSocketHandle* handle) const {
    return LOAD_STATE_IDLE;
  }
  virtual base::TimeDelta ConnectionTimeout() const {
    return base::TimeDelta();
  }

 private:
  std::string last_group_name_;
};

typedef CaptureGroupNameSocketPool<TCPClientSocketPool>
CaptureGroupNameTCPSocketPool;
typedef CaptureGroupNameSocketPool<HttpProxyClientSocketPool>
CaptureGroupNameHttpProxySocketPool;
typedef CaptureGroupNameSocketPool<SOCKSClientSocketPool>
CaptureGroupNameSOCKSSocketPool;
typedef CaptureGroupNameSocketPool<SSLClientSocketPool>
CaptureGroupNameSSLSocketPool;

template<typename ParentPool>
CaptureGroupNameSocketPool<ParentPool>::CaptureGroupNameSocketPool(
    HttpNetworkSession* session)
    : ParentPool(0, 0, NULL, session->host_resolver(), NULL, NULL) {}

template<>
CaptureGroupNameSSLSocketPool::CaptureGroupNameSocketPool(
    HttpNetworkSession* session)
    : SSLClientSocketPool(0, 0, NULL, session->host_resolver(), NULL, NULL,
                          NULL, NULL, NULL) {}

//-----------------------------------------------------------------------------

TEST_F(HttpNetworkTransactionTest, Basic) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));
}

TEST_F(HttpNetworkTransactionTest, SimpleGET) {
  MockRead data_reads[] = {
    MockRead("HTTP/1.0 200 OK\r\n\r\n"),
    MockRead("hello world"),
    MockRead(false, OK),
  };
  SimpleGetHelperResult out = SimpleGetHelper(data_reads,
                                              arraysize(data_reads));
  EXPECT_EQ(OK, out.rv);
  EXPECT_EQ("HTTP/1.0 200 OK", out.status_line);
  EXPECT_EQ("hello world", out.response_data);
}

// Response with no status line.
TEST_F(HttpNetworkTransactionTest, SimpleGETNoHeaders) {
  MockRead data_reads[] = {
    MockRead("hello world"),
    MockRead(false, OK),
  };
  SimpleGetHelperResult out = SimpleGetHelper(data_reads,
                                              arraysize(data_reads));
  EXPECT_EQ(OK, out.rv);
  EXPECT_EQ("HTTP/0.9 200 OK", out.status_line);
  EXPECT_EQ("hello world", out.response_data);
}

// Allow up to 4 bytes of junk to precede status line.
TEST_F(HttpNetworkTransactionTest, StatusLineJunk2Bytes) {
  MockRead data_reads[] = {
    MockRead("xxxHTTP/1.0 404 Not Found\nServer: blah\n\nDATA"),
    MockRead(false, OK),
  };
  SimpleGetHelperResult out = SimpleGetHelper(data_reads,
                                              arraysize(data_reads));
  EXPECT_EQ(OK, out.rv);
  EXPECT_EQ("HTTP/1.0 404 Not Found", out.status_line);
  EXPECT_EQ("DATA", out.response_data);
}

// Allow up to 4 bytes of junk to precede status line.
TEST_F(HttpNetworkTransactionTest, StatusLineJunk4Bytes) {
  MockRead data_reads[] = {
    MockRead("\n\nQJHTTP/1.0 404 Not Found\nServer: blah\n\nDATA"),
    MockRead(false, OK),
  };
  SimpleGetHelperResult out = SimpleGetHelper(data_reads,
                                              arraysize(data_reads));
  EXPECT_EQ(OK, out.rv);
  EXPECT_EQ("HTTP/1.0 404 Not Found", out.status_line);
  EXPECT_EQ("DATA", out.response_data);
}

// Beyond 4 bytes of slop and it should fail to find a status line.
TEST_F(HttpNetworkTransactionTest, StatusLineJunk5Bytes) {
  MockRead data_reads[] = {
    MockRead("xxxxxHTTP/1.1 404 Not Found\nServer: blah"),
    MockRead(false, OK),
  };
  SimpleGetHelperResult out = SimpleGetHelper(data_reads,
                                              arraysize(data_reads));
  EXPECT_EQ(OK, out.rv);
  EXPECT_EQ("HTTP/0.9 200 OK", out.status_line);
  EXPECT_EQ("xxxxxHTTP/1.1 404 Not Found\nServer: blah", out.response_data);
}

// Same as StatusLineJunk4Bytes, except the read chunks are smaller.
TEST_F(HttpNetworkTransactionTest, StatusLineJunk4Bytes_Slow) {
  MockRead data_reads[] = {
    MockRead("\n"),
    MockRead("\n"),
    MockRead("Q"),
    MockRead("J"),
    MockRead("HTTP/1.0 404 Not Found\nServer: blah\n\nDATA"),
    MockRead(false, OK),
  };
  SimpleGetHelperResult out = SimpleGetHelper(data_reads,
                                              arraysize(data_reads));
  EXPECT_EQ(OK, out.rv);
  EXPECT_EQ("HTTP/1.0 404 Not Found", out.status_line);
  EXPECT_EQ("DATA", out.response_data);
}

// Close the connection before enough bytes to have a status line.
TEST_F(HttpNetworkTransactionTest, StatusLinePartial) {
  MockRead data_reads[] = {
    MockRead("HTT"),
    MockRead(false, OK),
  };
  SimpleGetHelperResult out = SimpleGetHelper(data_reads,
                                              arraysize(data_reads));
  EXPECT_EQ(OK, out.rv);
  EXPECT_EQ("HTTP/0.9 200 OK", out.status_line);
  EXPECT_EQ("HTT", out.response_data);
}

// Simulate a 204 response, lacking a Content-Length header, sent over a
// persistent connection.  The response should still terminate since a 204
// cannot have a response body.
TEST_F(HttpNetworkTransactionTest, StopsReading204) {
  MockRead data_reads[] = {
    MockRead("HTTP/1.1 204 No Content\r\n\r\n"),
    MockRead("junk"),  // Should not be read!!
    MockRead(false, OK),
  };
  SimpleGetHelperResult out = SimpleGetHelper(data_reads,
                                              arraysize(data_reads));
  EXPECT_EQ(OK, out.rv);
  EXPECT_EQ("HTTP/1.1 204 No Content", out.status_line);
  EXPECT_EQ("", out.response_data);
}

// A simple request using chunked encoding with some extra data after.
// (Like might be seen in a pipelined response.)
TEST_F(HttpNetworkTransactionTest, ChunkedEncoding) {
  MockRead data_reads[] = {
    MockRead("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"),
    MockRead("5\r\nHello\r\n"),
    MockRead("1\r\n"),
    MockRead(" \r\n"),
    MockRead("5\r\nworld\r\n"),
    MockRead("0\r\n\r\nHTTP/1.1 200 OK\r\n"),
    MockRead(false, OK),
  };
  SimpleGetHelperResult out = SimpleGetHelper(data_reads,
                                              arraysize(data_reads));
  EXPECT_EQ(OK, out.rv);
  EXPECT_EQ("HTTP/1.1 200 OK", out.status_line);
  EXPECT_EQ("Hello world", out.response_data);
}

// Do a request using the HEAD method. Verify that we don't try to read the
// message body (since HEAD has none).
TEST_F(HttpNetworkTransactionTest, Head) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "HEAD";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;

  MockWrite data_writes1[] = {
    MockWrite("HEAD / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Content-Length: 0\r\n\r\n"),
  };
  MockRead data_reads1[] = {
    MockRead("HTTP/1.1 404 Not Found\r\n"),
    MockRead("Server: Blah\r\n"),
    MockRead("Content-Length: 1234\r\n\r\n"),

    // No response body because the test stops reading here.
    MockRead(false, ERR_UNEXPECTED),  // Should not be reached.
  };

  StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                 data_writes1, arraysize(data_writes1));
  session_deps.socket_factory.AddSocketDataProvider(&data1);

  TestCompletionCallback callback1;

  int rv = trans->Start(&request, &callback1, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback1.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);

  // Check that the headers got parsed.
  EXPECT_TRUE(response->headers != NULL);
  EXPECT_EQ(1234, response->headers->GetContentLength());
  EXPECT_EQ("HTTP/1.1 404 Not Found", response->headers->GetStatusLine());

  std::string server_header;
  void* iter = NULL;
  bool has_server_header = response->headers->EnumerateHeader(
      &iter, "Server", &server_header);
  EXPECT_TRUE(has_server_header);
  EXPECT_EQ("Blah", server_header);

  // Reading should give EOF right away, since there is no message body
  // (despite non-zero content-length).
  std::string response_data;
  rv = ReadTransaction(trans.get(), &response_data);
  EXPECT_EQ(OK, rv);
  EXPECT_EQ("", response_data);
}

TEST_F(HttpNetworkTransactionTest, ReuseConnection) {
  SessionDependencies session_deps;
  scoped_refptr<HttpNetworkSession> session = CreateSession(&session_deps);

  MockRead data_reads[] = {
    MockRead("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n"),
    MockRead("hello"),
    MockRead("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n"),
    MockRead("world"),
    MockRead(false, OK),
  };
  StaticSocketDataProvider data(data_reads, arraysize(data_reads), NULL, 0);
  session_deps.socket_factory.AddSocketDataProvider(&data);

  const char* kExpectedResponseData[] = {
    "hello", "world"
  };

  for (int i = 0; i < 2; ++i) {
    scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));

    HttpRequestInfo request;
    request.method = "GET";
    request.url = GURL("http://www.google.com/");
    request.load_flags = 0;

    TestCompletionCallback callback;

    int rv = trans->Start(&request, &callback, BoundNetLog());
    EXPECT_EQ(ERR_IO_PENDING, rv);

    rv = callback.WaitForResult();
    EXPECT_EQ(OK, rv);

    const HttpResponseInfo* response = trans->GetResponseInfo();
    EXPECT_TRUE(response != NULL);

    EXPECT_TRUE(response->headers != NULL);
    EXPECT_EQ("HTTP/1.1 200 OK", response->headers->GetStatusLine());

    std::string response_data;
    rv = ReadTransaction(trans.get(), &response_data);
    EXPECT_EQ(OK, rv);
    EXPECT_EQ(kExpectedResponseData[i], response_data);
  }
}

TEST_F(HttpNetworkTransactionTest, Ignores100) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "POST";
  request.url = GURL("http://www.foo.com/");
  request.upload_data = new UploadData;
  request.upload_data->AppendBytes("foo", 3);
  request.load_flags = 0;

  MockRead data_reads[] = {
    MockRead("HTTP/1.0 100 Continue\r\n\r\n"),
    MockRead("HTTP/1.0 200 OK\r\n\r\n"),
    MockRead("hello world"),
    MockRead(false, OK),
  };
  StaticSocketDataProvider data(data_reads, arraysize(data_reads), NULL, 0);
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_TRUE(response != NULL);

  EXPECT_TRUE(response->headers != NULL);
  EXPECT_EQ("HTTP/1.0 200 OK", response->headers->GetStatusLine());

  std::string response_data;
  rv = ReadTransaction(trans.get(), &response_data);
  EXPECT_EQ(OK, rv);
  EXPECT_EQ("hello world", response_data);
}

// This test is almost the same as Ignores100 above, but the response contains
// a 102 instead of a 100. Also, instead of HTTP/1.0 the response is
// HTTP/1.1 and the two status headers are read in one read.
TEST_F(HttpNetworkTransactionTest, Ignores1xx) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.foo.com/");
  request.load_flags = 0;

  MockRead data_reads[] = {
    MockRead("HTTP/1.1 102 Unspecified status code\r\n\r\n"
             "HTTP/1.1 200 OK\r\n\r\n"),
    MockRead("hello world"),
    MockRead(false, OK),
  };
  StaticSocketDataProvider data(data_reads, arraysize(data_reads), NULL, 0);
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_TRUE(response != NULL);

  EXPECT_TRUE(response->headers != NULL);
  EXPECT_EQ("HTTP/1.1 200 OK", response->headers->GetStatusLine());

  std::string response_data;
  rv = ReadTransaction(trans.get(), &response_data);
  EXPECT_EQ(OK, rv);
  EXPECT_EQ("hello world", response_data);
}

TEST_F(HttpNetworkTransactionTest, Incomplete100ThenEOF) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "POST";
  request.url = GURL("http://www.foo.com/");
  request.load_flags = 0;

  MockRead data_reads[] = {
    MockRead(false, "HTTP/1.0 100 Continue\r\n"),
    MockRead(true, 0),
  };
  StaticSocketDataProvider data(data_reads, arraysize(data_reads), NULL, 0);
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);

  std::string response_data;
  rv = ReadTransaction(trans.get(), &response_data);
  EXPECT_EQ(OK, rv);
  EXPECT_EQ("", response_data);
}

TEST_F(HttpNetworkTransactionTest, EmptyResponse) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "POST";
  request.url = GURL("http://www.foo.com/");
  request.load_flags = 0;

  MockRead data_reads[] = {
    MockRead(true, 0),
  };
  StaticSocketDataProvider data(data_reads, arraysize(data_reads), NULL, 0);
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(ERR_EMPTY_RESPONSE, rv);
}

// read_failure specifies a read failure that should cause the network
// transaction to resend the request.
void HttpNetworkTransactionTest::KeepAliveConnectionResendRequestTest(
    const MockRead& read_failure) {
  SessionDependencies session_deps;
  scoped_refptr<HttpNetworkSession> session = CreateSession(&session_deps);

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.foo.com/");
  request.load_flags = 0;

  MockRead data1_reads[] = {
    MockRead("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n"),
    MockRead("hello"),
    read_failure,  // Now, we reuse the connection and fail the first read.
  };
  StaticSocketDataProvider data1(data1_reads, arraysize(data1_reads), NULL, 0);
  session_deps.socket_factory.AddSocketDataProvider(&data1);

  MockRead data2_reads[] = {
    MockRead("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n"),
    MockRead("world"),
    MockRead(true, OK),
  };
  StaticSocketDataProvider data2(data2_reads, arraysize(data2_reads), NULL, 0);
  session_deps.socket_factory.AddSocketDataProvider(&data2);

  const char* kExpectedResponseData[] = {
    "hello", "world"
  };

  for (int i = 0; i < 2; ++i) {
    TestCompletionCallback callback;

    scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));

    int rv = trans->Start(&request, &callback, BoundNetLog());
    EXPECT_EQ(ERR_IO_PENDING, rv);

    rv = callback.WaitForResult();
    EXPECT_EQ(OK, rv);

    const HttpResponseInfo* response = trans->GetResponseInfo();
    EXPECT_TRUE(response != NULL);

    EXPECT_TRUE(response->headers != NULL);
    EXPECT_EQ("HTTP/1.1 200 OK", response->headers->GetStatusLine());

    std::string response_data;
    rv = ReadTransaction(trans.get(), &response_data);
    EXPECT_EQ(OK, rv);
    EXPECT_EQ(kExpectedResponseData[i], response_data);
  }
}

TEST_F(HttpNetworkTransactionTest, KeepAliveConnectionReset) {
  MockRead read_failure(true, ERR_CONNECTION_RESET);
  KeepAliveConnectionResendRequestTest(read_failure);
}

TEST_F(HttpNetworkTransactionTest, KeepAliveConnectionEOF) {
  MockRead read_failure(false, OK);  // EOF
  KeepAliveConnectionResendRequestTest(read_failure);
}

TEST_F(HttpNetworkTransactionTest, NonKeepAliveConnectionReset) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;

  MockRead data_reads[] = {
    MockRead(true, ERR_CONNECTION_RESET),
    MockRead("HTTP/1.0 200 OK\r\n\r\n"),  // Should not be used
    MockRead("hello world"),
    MockRead(false, OK),
  };
  StaticSocketDataProvider data(data_reads, arraysize(data_reads), NULL, 0);
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(ERR_CONNECTION_RESET, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_TRUE(response == NULL);
}

// What do various browsers do when the server closes a non-keepalive
// connection without sending any response header or body?
//
// IE7: error page
// Safari 3.1.2 (Windows): error page
// Firefox 3.0.1: blank page
// Opera 9.52: after five attempts, blank page
// Us with WinHTTP: error page (ERR_INVALID_RESPONSE)
// Us: error page (EMPTY_RESPONSE)
TEST_F(HttpNetworkTransactionTest, NonKeepAliveConnectionEOF) {
  MockRead data_reads[] = {
    MockRead(false, OK),  // EOF
    MockRead("HTTP/1.0 200 OK\r\n\r\n"),  // Should not be used
    MockRead("hello world"),
    MockRead(false, OK),
  };
  SimpleGetHelperResult out = SimpleGetHelper(data_reads,
                                              arraysize(data_reads));
  EXPECT_EQ(ERR_EMPTY_RESPONSE, out.rv);
}

// Test that we correctly reuse a keep-alive connection after receiving a 304.
TEST_F(HttpNetworkTransactionTest, KeepAliveAfter304) {
  SessionDependencies session_deps;
  scoped_refptr<HttpNetworkSession> session = CreateSession(&session_deps);

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.foo.com/");
  request.load_flags = 0;

  MockRead data1_reads[] = {
    MockRead("HTTP/1.1 304 Not Modified\r\n\r\n"),
    MockRead("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n"),
    MockRead("hello"),
  };
  StaticSocketDataProvider data1(data1_reads, arraysize(data1_reads), NULL, 0);
  session_deps.socket_factory.AddSocketDataProvider(&data1);

  MockRead data2_reads[] = {
    MockRead(false, ERR_UNEXPECTED),  // Should not be reached.
  };
  StaticSocketDataProvider data2(data2_reads, arraysize(data2_reads), NULL, 0);
  session_deps.socket_factory.AddSocketDataProvider(&data2);

  for (int i = 0; i < 2; ++i) {
    TestCompletionCallback callback;

    scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));

    int rv = trans->Start(&request, &callback, BoundNetLog());
    EXPECT_EQ(ERR_IO_PENDING, rv);

    rv = callback.WaitForResult();
    EXPECT_EQ(OK, rv);

    const HttpResponseInfo* response = trans->GetResponseInfo();
    EXPECT_TRUE(response != NULL);

    EXPECT_TRUE(response->headers != NULL);
    if (i == 0) {
      EXPECT_EQ("HTTP/1.1 304 Not Modified",
                response->headers->GetStatusLine());
      // We intentionally don't read the response in this case, to reflect how
      // HttpCache::Transaction uses HttpNetworkTransaction.
    } else {
      EXPECT_EQ("HTTP/1.1 200 OK", response->headers->GetStatusLine());
      std::string response_data;
      rv = ReadTransaction(trans.get(), &response_data);
      EXPECT_EQ(OK, rv);
      EXPECT_EQ("hello", response_data);
    }
  }
}

// Test the request-challenge-retry sequence for basic auth.
// (basic auth is the easiest to mock, because it has no randomness).
TEST_F(HttpNetworkTransactionTest, BasicAuth) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;

  MockWrite data_writes1[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n\r\n"),
  };

  MockRead data_reads1[] = {
    MockRead("HTTP/1.0 401 Unauthorized\r\n"),
    // Give a couple authenticate options (only the middle one is actually
    // supported).
    MockRead("WWW-Authenticate: Basic invalid\r\n"),  // Malformed.
    MockRead("WWW-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
    MockRead("WWW-Authenticate: UNSUPPORTED realm=\"FOO\"\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    // Large content-length -- won't matter, as connection will be reset.
    MockRead("Content-Length: 10000\r\n\r\n"),
    MockRead(false, ERR_FAILED),
  };

  // After calling trans->RestartWithAuth(), this is the request we should
  // be issuing -- the final header line contains the credentials.
  MockWrite data_writes2[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Authorization: Basic Zm9vOmJhcg==\r\n\r\n"),
  };

  // Lastly, the server responds with the actual content.
  MockRead data_reads2[] = {
    MockRead("HTTP/1.0 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 100\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                 data_writes1, arraysize(data_writes1));
  StaticSocketDataProvider data2(data_reads2, arraysize(data_reads2),
                                 data_writes2, arraysize(data_writes2));
  session_deps.socket_factory.AddSocketDataProvider(&data1);
  session_deps.socket_factory.AddSocketDataProvider(&data2);

  TestCompletionCallback callback1;

  int rv = trans->Start(&request, &callback1, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback1.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);

  // The password prompt info should have been set in response->auth_challenge.
  EXPECT_FALSE(response->auth_challenge.get() == NULL);

  EXPECT_EQ(L"www.google.com:80", response->auth_challenge->host_and_port);
  EXPECT_EQ(L"MyRealm1", response->auth_challenge->realm);
  EXPECT_EQ(L"basic", response->auth_challenge->scheme);

  TestCompletionCallback callback2;

  rv = trans->RestartWithAuth(L"foo", L"bar", &callback2);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback2.WaitForResult();
  EXPECT_EQ(OK, rv);

  response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);
  EXPECT_TRUE(response->auth_challenge.get() == NULL);
  EXPECT_EQ(100, response->headers->GetContentLength());
}

TEST_F(HttpNetworkTransactionTest, DoNotSendAuth) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = net::LOAD_DO_NOT_SEND_AUTH_DATA;

  MockWrite data_writes[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n\r\n"),
  };

  MockRead data_reads[] = {
    MockRead("HTTP/1.0 401 Unauthorized\r\n"),
    MockRead("WWW-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    // Large content-length -- won't matter, as connection will be reset.
    MockRead("Content-Length: 10000\r\n\r\n"),
    MockRead(false, ERR_FAILED),
  };

  StaticSocketDataProvider data(data_reads, arraysize(data_reads),
                                data_writes, arraysize(data_writes));
  session_deps.socket_factory.AddSocketDataProvider(&data);
  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(0, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  ASSERT_FALSE(response == NULL);
  EXPECT_TRUE(response->auth_challenge.get() == NULL);
}

// Test the request-challenge-retry sequence for basic auth, over a keep-alive
// connection.
TEST_F(HttpNetworkTransactionTest, BasicAuthKeepAlive) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;

  MockWrite data_writes1[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n\r\n"),

    // After calling trans->RestartWithAuth(), this is the request we should
    // be issuing -- the final header line contains the credentials.
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Authorization: Basic Zm9vOmJhcg==\r\n\r\n"),
  };

  MockRead data_reads1[] = {
    MockRead("HTTP/1.1 401 Unauthorized\r\n"),
    MockRead("WWW-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 14\r\n\r\n"),
    MockRead("Unauthorized\r\n"),

    // Lastly, the server responds with the actual content.
    MockRead("HTTP/1.1 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 100\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                 data_writes1, arraysize(data_writes1));
  session_deps.socket_factory.AddSocketDataProvider(&data1);

  TestCompletionCallback callback1;

  int rv = trans->Start(&request, &callback1, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback1.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);

  // The password prompt info should have been set in response->auth_challenge.
  EXPECT_FALSE(response->auth_challenge.get() == NULL);

  EXPECT_EQ(L"www.google.com:80", response->auth_challenge->host_and_port);
  EXPECT_EQ(L"MyRealm1", response->auth_challenge->realm);
  EXPECT_EQ(L"basic", response->auth_challenge->scheme);

  TestCompletionCallback callback2;

  rv = trans->RestartWithAuth(L"foo", L"bar", &callback2);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback2.WaitForResult();
  EXPECT_EQ(OK, rv);

  response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);
  EXPECT_TRUE(response->auth_challenge.get() == NULL);
  EXPECT_EQ(100, response->headers->GetContentLength());
}

// Test the request-challenge-retry sequence for basic auth, over a keep-alive
// connection and with no response body to drain.
TEST_F(HttpNetworkTransactionTest, BasicAuthKeepAliveNoBody) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;

  MockWrite data_writes1[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n\r\n"),

    // After calling trans->RestartWithAuth(), this is the request we should
    // be issuing -- the final header line contains the credentials.
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Authorization: Basic Zm9vOmJhcg==\r\n\r\n"),
  };

  MockRead data_reads1[] = {
    MockRead("HTTP/1.1 401 Unauthorized\r\n"),
    MockRead("WWW-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
    MockRead("Content-Length: 0\r\n\r\n"),  // No response body.

    // Lastly, the server responds with the actual content.
    MockRead("HTTP/1.1 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 100\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                 data_writes1, arraysize(data_writes1));
  session_deps.socket_factory.AddSocketDataProvider(&data1);

  TestCompletionCallback callback1;

  int rv = trans->Start(&request, &callback1, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback1.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);

  // The password prompt info should have been set in response->auth_challenge.
  EXPECT_FALSE(response->auth_challenge.get() == NULL);

  EXPECT_EQ(L"www.google.com:80", response->auth_challenge->host_and_port);
  EXPECT_EQ(L"MyRealm1", response->auth_challenge->realm);
  EXPECT_EQ(L"basic", response->auth_challenge->scheme);

  TestCompletionCallback callback2;

  rv = trans->RestartWithAuth(L"foo", L"bar", &callback2);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback2.WaitForResult();
  EXPECT_EQ(OK, rv);

  response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);
  EXPECT_TRUE(response->auth_challenge.get() == NULL);
  EXPECT_EQ(100, response->headers->GetContentLength());
}

// Test the request-challenge-retry sequence for basic auth, over a keep-alive
// connection and with a large response body to drain.
TEST_F(HttpNetworkTransactionTest, BasicAuthKeepAliveLargeBody) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;

  MockWrite data_writes1[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n\r\n"),

    // After calling trans->RestartWithAuth(), this is the request we should
    // be issuing -- the final header line contains the credentials.
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Authorization: Basic Zm9vOmJhcg==\r\n\r\n"),
  };

  // Respond with 5 kb of response body.
  std::string large_body_string("Unauthorized");
  large_body_string.append(5 * 1024, ' ');
  large_body_string.append("\r\n");

  MockRead data_reads1[] = {
    MockRead("HTTP/1.1 401 Unauthorized\r\n"),
    MockRead("WWW-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    // 5134 = 12 + 5 * 1024 + 2
    MockRead("Content-Length: 5134\r\n\r\n"),
    MockRead(true, large_body_string.data(), large_body_string.size()),

    // Lastly, the server responds with the actual content.
    MockRead("HTTP/1.1 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 100\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                 data_writes1, arraysize(data_writes1));
  session_deps.socket_factory.AddSocketDataProvider(&data1);

  TestCompletionCallback callback1;

  int rv = trans->Start(&request, &callback1, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback1.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);

  // The password prompt info should have been set in response->auth_challenge.
  EXPECT_FALSE(response->auth_challenge.get() == NULL);

  EXPECT_EQ(L"www.google.com:80", response->auth_challenge->host_and_port);
  EXPECT_EQ(L"MyRealm1", response->auth_challenge->realm);
  EXPECT_EQ(L"basic", response->auth_challenge->scheme);

  TestCompletionCallback callback2;

  rv = trans->RestartWithAuth(L"foo", L"bar", &callback2);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback2.WaitForResult();
  EXPECT_EQ(OK, rv);

  response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);
  EXPECT_TRUE(response->auth_challenge.get() == NULL);
  EXPECT_EQ(100, response->headers->GetContentLength());
}

// Test the request-challenge-retry sequence for basic auth, over a keep-alive
// connection, but the server gets impatient and closes the connection.
TEST_F(HttpNetworkTransactionTest, BasicAuthKeepAliveImpatientServer) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;

  MockWrite data_writes1[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n\r\n"),
    // This simulates the seemingly successful write to a closed connection
    // if the bug is not fixed.
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Authorization: Basic Zm9vOmJhcg==\r\n\r\n"),
  };

  MockRead data_reads1[] = {
    MockRead("HTTP/1.1 401 Unauthorized\r\n"),
    MockRead("WWW-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 14\r\n\r\n"),
    // Tell MockTCPClientSocket to simulate the server closing the connection.
    MockRead(false, ERR_TEST_PEER_CLOSE_AFTER_NEXT_MOCK_READ),
    MockRead("Unauthorized\r\n"),
    MockRead(false, OK),  // The server closes the connection.
  };

  // After calling trans->RestartWithAuth(), this is the request we should
  // be issuing -- the final header line contains the credentials.
  MockWrite data_writes2[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Authorization: Basic Zm9vOmJhcg==\r\n\r\n"),
  };

  // Lastly, the server responds with the actual content.
  MockRead data_reads2[] = {
    MockRead("HTTP/1.1 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 100\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                 data_writes1, arraysize(data_writes1));
  StaticSocketDataProvider data2(data_reads2, arraysize(data_reads2),
                                 data_writes2, arraysize(data_writes2));
  session_deps.socket_factory.AddSocketDataProvider(&data1);
  session_deps.socket_factory.AddSocketDataProvider(&data2);

  TestCompletionCallback callback1;

  int rv = trans->Start(&request, &callback1, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback1.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);

  // The password prompt info should have been set in response->auth_challenge.
  EXPECT_FALSE(response->auth_challenge.get() == NULL);

  EXPECT_EQ(L"www.google.com:80", response->auth_challenge->host_and_port);
  EXPECT_EQ(L"MyRealm1", response->auth_challenge->realm);
  EXPECT_EQ(L"basic", response->auth_challenge->scheme);

  TestCompletionCallback callback2;

  rv = trans->RestartWithAuth(L"foo", L"bar", &callback2);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback2.WaitForResult();
  EXPECT_EQ(OK, rv);

  response = trans->GetResponseInfo();
  ASSERT_FALSE(response == NULL);
  EXPECT_TRUE(response->auth_challenge.get() == NULL);
  EXPECT_EQ(100, response->headers->GetContentLength());
}

// Test the request-challenge-retry sequence for basic auth, over a keep-alive
// proxy connection, when setting up an SSL tunnel.
TEST_F(HttpNetworkTransactionTest, BasicAuthProxyKeepAlive) {
  // Configure against proxy server "myproxy:70".
  SessionDependencies session_deps(CreateFixedProxyService("myproxy:70"));
  CapturingBoundNetLog log(CapturingNetLog::kUnbounded);
  session_deps.net_log = log.bound().net_log();
  scoped_refptr<HttpNetworkSession> session(CreateSession(&session_deps));

  scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("https://www.google.com/");
  // Ensure that proxy authentication is attempted even
  // when the no authentication data flag is set.
  request.load_flags = net::LOAD_DO_NOT_SEND_AUTH_DATA;

  // Since we have proxy, should try to establish tunnel.
  MockWrite data_writes1[] = {
    MockWrite("CONNECT www.google.com:443 HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Proxy-Connection: keep-alive\r\n\r\n"),

    // After calling trans->RestartWithAuth(), this is the request we should
    // be issuing -- the final header line contains the credentials.
    MockWrite("CONNECT www.google.com:443 HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Proxy-Connection: keep-alive\r\n"
              "Proxy-Authorization: Basic Zm9vOmJheg==\r\n\r\n"),
  };

  // The proxy responds to the connect with a 407, using a persistent
  // connection.
  MockRead data_reads1[] = {
    // No credentials.
    MockRead("HTTP/1.1 407 Proxy Authentication Required\r\n"),
    MockRead("Proxy-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
    MockRead("Content-Length: 10\r\n\r\n"),
    MockRead("0123456789"),

    // Wrong credentials (wrong password).
    MockRead("HTTP/1.1 407 Proxy Authentication Required\r\n"),
    MockRead("Proxy-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
    MockRead("Content-Length: 10\r\n\r\n"),
    // No response body because the test stops reading here.
    MockRead(false, ERR_UNEXPECTED),  // Should not be reached.
  };

  StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                 data_writes1, arraysize(data_writes1));
  session_deps.socket_factory.AddSocketDataProvider(&data1);

  TestCompletionCallback callback1;

  int rv = trans->Start(&request, &callback1, log.bound());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback1.WaitForResult();
  EXPECT_EQ(OK, rv);
  size_t pos = ExpectLogContainsSomewhere(
      log.entries(), 0, NetLog::TYPE_HTTP_TRANSACTION_SEND_TUNNEL_HEADERS,
      NetLog::PHASE_NONE);
  ExpectLogContainsSomewhere(
      log.entries(), pos,
      NetLog::TYPE_HTTP_TRANSACTION_READ_TUNNEL_RESPONSE_HEADERS,
      NetLog::PHASE_NONE);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);

  EXPECT_TRUE(response->headers->IsKeepAlive());
  EXPECT_EQ(407, response->headers->response_code());
  EXPECT_EQ(10, response->headers->GetContentLength());
  EXPECT_TRUE(HttpVersion(1, 1) == response->headers->GetHttpVersion());

  // The password prompt info should have been set in response->auth_challenge.
  EXPECT_FALSE(response->auth_challenge.get() == NULL);

  EXPECT_EQ(L"myproxy:70", response->auth_challenge->host_and_port);
  EXPECT_EQ(L"MyRealm1", response->auth_challenge->realm);
  EXPECT_EQ(L"basic", response->auth_challenge->scheme);

  TestCompletionCallback callback2;

  // Wrong password (should be "bar").
  rv = trans->RestartWithAuth(L"foo", L"baz", &callback2);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback2.WaitForResult();
  EXPECT_EQ(OK, rv);

  response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);

  EXPECT_TRUE(response->headers->IsKeepAlive());
  EXPECT_EQ(407, response->headers->response_code());
  EXPECT_EQ(10, response->headers->GetContentLength());
  EXPECT_TRUE(HttpVersion(1, 1) == response->headers->GetHttpVersion());

  // The password prompt info should have been set in response->auth_challenge.
  EXPECT_FALSE(response->auth_challenge.get() == NULL);

  EXPECT_EQ(L"myproxy:70", response->auth_challenge->host_and_port);
  EXPECT_EQ(L"MyRealm1", response->auth_challenge->realm);
  EXPECT_EQ(L"basic", response->auth_challenge->scheme);

  // Flush the idle socket before the NetLog and HttpNetworkTransaction go
  // out of scope.
  session->FlushSocketPools();
}

// Test that we don't read the response body when we fail to establish a tunnel,
// even if the user cancels the proxy's auth attempt.
TEST_F(HttpNetworkTransactionTest, BasicAuthProxyCancelTunnel) {
  // Configure against proxy server "myproxy:70".
  SessionDependencies session_deps(CreateFixedProxyService("myproxy:70"));

  scoped_refptr<HttpNetworkSession> session(CreateSession(&session_deps));

  scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("https://www.google.com/");
  request.load_flags = 0;

  // Since we have proxy, should try to establish tunnel.
  MockWrite data_writes[] = {
    MockWrite("CONNECT www.google.com:443 HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Proxy-Connection: keep-alive\r\n\r\n"),
  };

  // The proxy responds to the connect with a 407.
  MockRead data_reads[] = {
    MockRead("HTTP/1.1 407 Proxy Authentication Required\r\n"),
    MockRead("Proxy-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
    MockRead("Content-Length: 10\r\n\r\n"),
    MockRead(false, ERR_UNEXPECTED),  // Should not be reached.
  };

  StaticSocketDataProvider data(data_reads, arraysize(data_reads),
                                data_writes, arraysize(data_writes));
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);

  EXPECT_TRUE(response->headers->IsKeepAlive());
  EXPECT_EQ(407, response->headers->response_code());
  EXPECT_EQ(10, response->headers->GetContentLength());
  EXPECT_TRUE(HttpVersion(1, 1) == response->headers->GetHttpVersion());

  std::string response_data;
  rv = ReadTransaction(trans.get(), &response_data);
  EXPECT_EQ(ERR_TUNNEL_CONNECTION_FAILED, rv);

  // Flush the idle socket before the HttpNetworkTransaction goes out of scope.
  session->FlushSocketPools();
}

// Test when a server (non-proxy) returns a 407 (proxy-authenticate).
// The request should fail with ERR_UNEXPECTED_PROXY_AUTH.
TEST_F(HttpNetworkTransactionTest, UnexpectedProxyAuth) {
  // We are using a DIRECT connection (i.e. no proxy) for this session.
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;

  MockWrite data_writes1[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n\r\n"),
  };

  MockRead data_reads1[] = {
    MockRead("HTTP/1.0 407 Proxy Auth required\r\n"),
    MockRead("Proxy-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
    // Large content-length -- won't matter, as connection will be reset.
    MockRead("Content-Length: 10000\r\n\r\n"),
    MockRead(false, ERR_FAILED),
  };

  StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                 data_writes1, arraysize(data_writes1));
  session_deps.socket_factory.AddSocketDataProvider(&data1);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(ERR_UNEXPECTED_PROXY_AUTH, rv);
}

void HttpNetworkTransactionTest::ConnectStatusHelperWithExpectedStatus(
    const MockRead& status, int expected_status) {
  // Configure against proxy server "myproxy:70".
  SessionDependencies session_deps(CreateFixedProxyService("myproxy:70"));

  scoped_refptr<HttpNetworkSession> session(CreateSession(&session_deps));

  scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("https://www.google.com/");
  request.load_flags = 0;

  // Since we have proxy, should try to establish tunnel.
  MockWrite data_writes[] = {
    MockWrite("CONNECT www.google.com:443 HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Proxy-Connection: keep-alive\r\n\r\n"),
  };

  MockRead data_reads[] = {
    status,
    MockRead("Content-Length: 10\r\n\r\n"),
    // No response body because the test stops reading here.
    MockRead(false, ERR_UNEXPECTED),  // Should not be reached.
  };

  StaticSocketDataProvider data(data_reads, arraysize(data_reads),
                                data_writes, arraysize(data_writes));
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(expected_status, rv);
}

void HttpNetworkTransactionTest::ConnectStatusHelper(const MockRead& status) {
  ConnectStatusHelperWithExpectedStatus(
      status, ERR_TUNNEL_CONNECTION_FAILED);
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus100) {
  ConnectStatusHelper(MockRead("HTTP/1.1 100 Continue\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus101) {
  ConnectStatusHelper(MockRead("HTTP/1.1 101 Switching Protocols\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus201) {
  ConnectStatusHelper(MockRead("HTTP/1.1 201 Created\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus202) {
  ConnectStatusHelper(MockRead("HTTP/1.1 202 Accepted\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus203) {
  ConnectStatusHelper(
      MockRead("HTTP/1.1 203 Non-Authoritative Information\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus204) {
  ConnectStatusHelper(MockRead("HTTP/1.1 204 No Content\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus205) {
  ConnectStatusHelper(MockRead("HTTP/1.1 205 Reset Content\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus206) {
  ConnectStatusHelper(MockRead("HTTP/1.1 206 Partial Content\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus300) {
  ConnectStatusHelper(MockRead("HTTP/1.1 300 Multiple Choices\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus301) {
  ConnectStatusHelper(MockRead("HTTP/1.1 301 Moved Permanently\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus302) {
  ConnectStatusHelper(MockRead("HTTP/1.1 302 Found\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus303) {
  ConnectStatusHelper(MockRead("HTTP/1.1 303 See Other\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus304) {
  ConnectStatusHelper(MockRead("HTTP/1.1 304 Not Modified\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus305) {
  ConnectStatusHelper(MockRead("HTTP/1.1 305 Use Proxy\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus306) {
  ConnectStatusHelper(MockRead("HTTP/1.1 306\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus307) {
  ConnectStatusHelper(MockRead("HTTP/1.1 307 Temporary Redirect\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus400) {
  ConnectStatusHelper(MockRead("HTTP/1.1 400 Bad Request\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus401) {
  ConnectStatusHelper(MockRead("HTTP/1.1 401 Unauthorized\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus402) {
  ConnectStatusHelper(MockRead("HTTP/1.1 402 Payment Required\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus403) {
  ConnectStatusHelper(MockRead("HTTP/1.1 403 Forbidden\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus404) {
  ConnectStatusHelper(MockRead("HTTP/1.1 404 Not Found\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus405) {
  ConnectStatusHelper(MockRead("HTTP/1.1 405 Method Not Allowed\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus406) {
  ConnectStatusHelper(MockRead("HTTP/1.1 406 Not Acceptable\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus407) {
  ConnectStatusHelperWithExpectedStatus(
      MockRead("HTTP/1.1 407 Proxy Authentication Required\r\n"),
      ERR_PROXY_AUTH_UNSUPPORTED);
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus408) {
  ConnectStatusHelper(MockRead("HTTP/1.1 408 Request Timeout\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus409) {
  ConnectStatusHelper(MockRead("HTTP/1.1 409 Conflict\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus410) {
  ConnectStatusHelper(MockRead("HTTP/1.1 410 Gone\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus411) {
  ConnectStatusHelper(MockRead("HTTP/1.1 411 Length Required\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus412) {
  ConnectStatusHelper(MockRead("HTTP/1.1 412 Precondition Failed\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus413) {
  ConnectStatusHelper(MockRead("HTTP/1.1 413 Request Entity Too Large\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus414) {
  ConnectStatusHelper(MockRead("HTTP/1.1 414 Request-URI Too Long\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus415) {
  ConnectStatusHelper(MockRead("HTTP/1.1 415 Unsupported Media Type\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus416) {
  ConnectStatusHelper(
      MockRead("HTTP/1.1 416 Requested Range Not Satisfiable\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus417) {
  ConnectStatusHelper(MockRead("HTTP/1.1 417 Expectation Failed\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus500) {
  ConnectStatusHelper(MockRead("HTTP/1.1 500 Internal Server Error\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus501) {
  ConnectStatusHelper(MockRead("HTTP/1.1 501 Not Implemented\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus502) {
  ConnectStatusHelper(MockRead("HTTP/1.1 502 Bad Gateway\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus503) {
  ConnectStatusHelper(MockRead("HTTP/1.1 503 Service Unavailable\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus504) {
  ConnectStatusHelper(MockRead("HTTP/1.1 504 Gateway Timeout\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus505) {
  ConnectStatusHelper(MockRead("HTTP/1.1 505 HTTP Version Not Supported\r\n"));
}

// Test the flow when both the proxy server AND origin server require
// authentication. Again, this uses basic auth for both since that is
// the simplest to mock.
TEST_F(HttpNetworkTransactionTest, BasicAuthProxyThenServer) {
  SessionDependencies session_deps(CreateFixedProxyService("myproxy:70"));

  // Configure against proxy server "myproxy:70".
  scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(
      CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;

  MockWrite data_writes1[] = {
    MockWrite("GET http://www.google.com/ HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Proxy-Connection: keep-alive\r\n\r\n"),
  };

  MockRead data_reads1[] = {
    MockRead("HTTP/1.0 407 Unauthorized\r\n"),
    // Give a couple authenticate options (only the middle one is actually
    // supported).
    MockRead("Proxy-Authenticate: Basic invalid\r\n"),  // Malformed.
    MockRead("Proxy-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
    MockRead("Proxy-Authenticate: UNSUPPORTED realm=\"FOO\"\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    // Large content-length -- won't matter, as connection will be reset.
    MockRead("Content-Length: 10000\r\n\r\n"),
    MockRead(false, ERR_FAILED),
  };

  // After calling trans->RestartWithAuth() the first time, this is the
  // request we should be issuing -- the final header line contains the
  // proxy's credentials.
  MockWrite data_writes2[] = {
    MockWrite("GET http://www.google.com/ HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Proxy-Connection: keep-alive\r\n"
              "Proxy-Authorization: Basic Zm9vOmJhcg==\r\n\r\n"),
  };

  // Now the proxy server lets the request pass through to origin server.
  // The origin server responds with a 401.
  MockRead data_reads2[] = {
    MockRead("HTTP/1.0 401 Unauthorized\r\n"),
    // Note: We are using the same realm-name as the proxy server. This is
    // completely valid, as realms are unique across hosts.
    MockRead("WWW-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 2000\r\n\r\n"),
    MockRead(false, ERR_FAILED),  // Won't be reached.
  };

  // After calling trans->RestartWithAuth() the second time, we should send
  // the credentials for both the proxy and origin server.
  MockWrite data_writes3[] = {
    MockWrite("GET http://www.google.com/ HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Proxy-Connection: keep-alive\r\n"
              "Proxy-Authorization: Basic Zm9vOmJhcg==\r\n"
              "Authorization: Basic Zm9vMjpiYXIy\r\n\r\n"),
  };

  // Lastly we get the desired content.
  MockRead data_reads3[] = {
    MockRead("HTTP/1.0 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 100\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                 data_writes1, arraysize(data_writes1));
  StaticSocketDataProvider data2(data_reads2, arraysize(data_reads2),
                                 data_writes2, arraysize(data_writes2));
  StaticSocketDataProvider data3(data_reads3, arraysize(data_reads3),
                                 data_writes3, arraysize(data_writes3));
  session_deps.socket_factory.AddSocketDataProvider(&data1);
  session_deps.socket_factory.AddSocketDataProvider(&data2);
  session_deps.socket_factory.AddSocketDataProvider(&data3);

  TestCompletionCallback callback1;

  int rv = trans->Start(&request, &callback1, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback1.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);

  // The password prompt info should have been set in response->auth_challenge.
  EXPECT_FALSE(response->auth_challenge.get() == NULL);

  EXPECT_EQ(L"myproxy:70", response->auth_challenge->host_and_port);
  EXPECT_EQ(L"MyRealm1", response->auth_challenge->realm);
  EXPECT_EQ(L"basic", response->auth_challenge->scheme);

  TestCompletionCallback callback2;

  rv = trans->RestartWithAuth(L"foo", L"bar", &callback2);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback2.WaitForResult();
  EXPECT_EQ(OK, rv);

  response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);
  EXPECT_FALSE(response->auth_challenge.get() == NULL);

  EXPECT_EQ(L"www.google.com:80", response->auth_challenge->host_and_port);
  EXPECT_EQ(L"MyRealm1", response->auth_challenge->realm);
  EXPECT_EQ(L"basic", response->auth_challenge->scheme);

  TestCompletionCallback callback3;

  rv = trans->RestartWithAuth(L"foo2", L"bar2", &callback3);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback3.WaitForResult();
  EXPECT_EQ(OK, rv);

  response = trans->GetResponseInfo();
  EXPECT_TRUE(response->auth_challenge.get() == NULL);
  EXPECT_EQ(100, response->headers->GetContentLength());
}

// For the NTLM implementation using SSPI, we skip the NTLM tests since we
// can't hook into its internals to cause it to generate predictable NTLM
// authorization headers.
#if defined(NTLM_PORTABLE)
// The NTLM authentication unit tests were generated by capturing the HTTP
// requests and responses using Fiddler 2 and inspecting the generated random
// bytes in the debugger.

// Enter the correct password and authenticate successfully.
TEST_F(HttpNetworkTransactionTest, NTLMAuth1) {
  HttpAuthHandlerNTLM::ScopedProcSetter proc_setter(MockGenerateRandom1,
                                                    MockGetHostName);
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://172.22.68.17/kids/login.aspx");
  request.load_flags = 0;

  MockWrite data_writes1[] = {
    MockWrite("GET /kids/login.aspx HTTP/1.1\r\n"
              "Host: 172.22.68.17\r\n"
              "Connection: keep-alive\r\n\r\n"),
  };

  MockRead data_reads1[] = {
    MockRead("HTTP/1.1 401 Access Denied\r\n"),
    // Negotiate and NTLM are often requested together.  However, we only want
    // to test NTLM. Since Negotiate is preferred over NTLM, we have to skip
    // the header that requests Negotiate for this test.
    MockRead("WWW-Authenticate: NTLM\r\n"),
    MockRead("Connection: close\r\n"),
    MockRead("Content-Length: 42\r\n"),
    MockRead("Content-Type: text/html\r\n\r\n"),
    // Missing content -- won't matter, as connection will be reset.
    MockRead(false, ERR_UNEXPECTED),
  };

  MockWrite data_writes2[] = {
    // After restarting with a null identity, this is the
    // request we should be issuing -- the final header line contains a Type
    // 1 message.
    MockWrite("GET /kids/login.aspx HTTP/1.1\r\n"
              "Host: 172.22.68.17\r\n"
              "Connection: keep-alive\r\n"
              "Authorization: NTLM "
              "TlRMTVNTUAABAAAAB4IIAAAAAAAAAAAAAAAAAAAAAAA=\r\n\r\n"),

    // After calling trans->RestartWithAuth(), we should send a Type 3 message
    // (the credentials for the origin server).  The second request continues
    // on the same connection.
    MockWrite("GET /kids/login.aspx HTTP/1.1\r\n"
              "Host: 172.22.68.17\r\n"
              "Connection: keep-alive\r\n"
              "Authorization: NTLM TlRMTVNTUAADAAAAGAAYAGgAAAAYABgAgA"
              "AAAAAAAABAAAAAGAAYAEAAAAAQABAAWAAAAAAAAAAAAAAABYIIAHQA"
              "ZQBzAHQAaQBuAGcALQBuAHQAbABtAFcAVABDAC0AVwBJAE4ANwBVKW"
              "Yma5xzVAAAAAAAAAAAAAAAAAAAAACH+gWcm+YsP9Tqb9zCR3WAeZZX"
              "ahlhx5I=\r\n\r\n"),
  };

  MockRead data_reads2[] = {
    // The origin server responds with a Type 2 message.
    MockRead("HTTP/1.1 401 Access Denied\r\n"),
    MockRead("WWW-Authenticate: NTLM "
             "TlRMTVNTUAACAAAADAAMADgAAAAFgokCjGpMpPGlYKkAAAAAAAAAALo"
             "AugBEAAAABQEoCgAAAA9HAE8ATwBHAEwARQACAAwARwBPAE8ARwBMAE"
             "UAAQAaAEEASwBFAEUAUwBBAFIAQQAtAEMATwBSAFAABAAeAGMAbwByA"
             "HAALgBnAG8AbwBnAGwAZQAuAGMAbwBtAAMAQABhAGsAZQBlAHMAYQBy"
             "AGEALQBjAG8AcgBwAC4AYQBkAC4AYwBvAHIAcAAuAGcAbwBvAGcAbAB"
             "lAC4AYwBvAG0ABQAeAGMAbwByAHAALgBnAG8AbwBnAGwAZQAuAGMAbw"
             "BtAAAAAAA=\r\n"),
    MockRead("Content-Length: 42\r\n"),
    MockRead("Content-Type: text/html\r\n\r\n"),
    MockRead("You are not authorized to view this page\r\n"),

    // Lastly we get the desired content.
    MockRead("HTTP/1.1 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=utf-8\r\n"),
    MockRead("Content-Length: 13\r\n\r\n"),
    MockRead("Please Login\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                 data_writes1, arraysize(data_writes1));
  StaticSocketDataProvider data2(data_reads2, arraysize(data_reads2),
                                 data_writes2, arraysize(data_writes2));
  session_deps.socket_factory.AddSocketDataProvider(&data1);
  session_deps.socket_factory.AddSocketDataProvider(&data2);

  TestCompletionCallback callback1;

  int rv = trans->Start(&request, &callback1, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback1.WaitForResult();
  EXPECT_EQ(OK, rv);

  EXPECT_TRUE(trans->IsReadyToRestartForAuth());
  TestCompletionCallback callback2;
  rv = trans->RestartWithAuth(std::wstring(), std::wstring(), &callback2);
  EXPECT_EQ(ERR_IO_PENDING, rv);
  rv = callback2.WaitForResult();
  EXPECT_EQ(OK, rv);
  EXPECT_FALSE(trans->IsReadyToRestartForAuth());

  const HttpResponseInfo* response = trans->GetResponseInfo();
  ASSERT_FALSE(response == NULL);

  // The password prompt info should have been set in
  // response->auth_challenge.
  EXPECT_FALSE(response->auth_challenge.get() == NULL);

  EXPECT_EQ(L"172.22.68.17:80", response->auth_challenge->host_and_port);
  EXPECT_EQ(L"", response->auth_challenge->realm);
  EXPECT_EQ(L"ntlm", response->auth_challenge->scheme);

  TestCompletionCallback callback3;

  rv = trans->RestartWithAuth(L"testing-ntlm", L"testing-ntlm", &callback3);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback3.WaitForResult();
  EXPECT_EQ(OK, rv);

  response = trans->GetResponseInfo();
  ASSERT_FALSE(response == NULL);

  EXPECT_TRUE(response->auth_challenge.get() == NULL);
  EXPECT_EQ(13, response->headers->GetContentLength());
}

// Enter a wrong password, and then the correct one.
TEST_F(HttpNetworkTransactionTest, NTLMAuth2) {
  HttpAuthHandlerNTLM::ScopedProcSetter proc_setter(MockGenerateRandom2,
                                                    MockGetHostName);
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://172.22.68.17/kids/login.aspx");
  request.load_flags = 0;

  MockWrite data_writes1[] = {
    MockWrite("GET /kids/login.aspx HTTP/1.1\r\n"
              "Host: 172.22.68.17\r\n"
              "Connection: keep-alive\r\n\r\n"),
  };

  MockRead data_reads1[] = {
    MockRead("HTTP/1.1 401 Access Denied\r\n"),
    // Negotiate and NTLM are often requested together.  However, we only want
    // to test NTLM. Since Negotiate is preferred over NTLM, we have to skip
    // the header that requests Negotiate for this test.
    MockRead("WWW-Authenticate: NTLM\r\n"),
    MockRead("Connection: close\r\n"),
    MockRead("Content-Length: 42\r\n"),
    MockRead("Content-Type: text/html\r\n\r\n"),
    // Missing content -- won't matter, as connection will be reset.
    MockRead(false, ERR_UNEXPECTED),
  };

  MockWrite data_writes2[] = {
    // After restarting with a null identity, this is the
    // request we should be issuing -- the final header line contains a Type
    // 1 message.
    MockWrite("GET /kids/login.aspx HTTP/1.1\r\n"
              "Host: 172.22.68.17\r\n"
              "Connection: keep-alive\r\n"
              "Authorization: NTLM "
              "TlRMTVNTUAABAAAAB4IIAAAAAAAAAAAAAAAAAAAAAAA=\r\n\r\n"),

    // After calling trans->RestartWithAuth(), we should send a Type 3 message
    // (the credentials for the origin server).  The second request continues
    // on the same connection.
    MockWrite("GET /kids/login.aspx HTTP/1.1\r\n"
              "Host: 172.22.68.17\r\n"
              "Connection: keep-alive\r\n"
              "Authorization: NTLM TlRMTVNTUAADAAAAGAAYAGgAAAAYABgAgA"
              "AAAAAAAABAAAAAGAAYAEAAAAAQABAAWAAAAAAAAAAAAAAABYIIAHQA"
              "ZQBzAHQAaQBuAGcALQBuAHQAbABtAFcAVABDAC0AVwBJAE4ANwCWeY"
              "XnSZNwoQAAAAAAAAAAAAAAAAAAAADLa34/phTTKzNTWdub+uyFleOj"
              "4Ww7b7E=\r\n\r\n"),
  };

  MockRead data_reads2[] = {
    // The origin server responds with a Type 2 message.
    MockRead("HTTP/1.1 401 Access Denied\r\n"),
    MockRead("WWW-Authenticate: NTLM "
             "TlRMTVNTUAACAAAADAAMADgAAAAFgokCbVWUZezVGpAAAAAAAAAAALo"
             "AugBEAAAABQEoCgAAAA9HAE8ATwBHAEwARQACAAwARwBPAE8ARwBMAE"
             "UAAQAaAEEASwBFAEUAUwBBAFIAQQAtAEMATwBSAFAABAAeAGMAbwByA"
             "HAALgBnAG8AbwBnAGwAZQAuAGMAbwBtAAMAQABhAGsAZQBlAHMAYQBy"
             "AGEALQBjAG8AcgBwAC4AYQBkAC4AYwBvAHIAcAAuAGcAbwBvAGcAbAB"
             "lAC4AYwBvAG0ABQAeAGMAbwByAHAALgBnAG8AbwBnAGwAZQAuAGMAbw"
             "BtAAAAAAA=\r\n"),
    MockRead("Content-Length: 42\r\n"),
    MockRead("Content-Type: text/html\r\n\r\n"),
    MockRead("You are not authorized to view this page\r\n"),

    // Wrong password.
    MockRead("HTTP/1.1 401 Access Denied\r\n"),
    MockRead("WWW-Authenticate: NTLM\r\n"),
    MockRead("Connection: close\r\n"),
    MockRead("Content-Length: 42\r\n"),
    MockRead("Content-Type: text/html\r\n\r\n"),
    // Missing content -- won't matter, as connection will be reset.
    MockRead(false, ERR_UNEXPECTED),
  };

  MockWrite data_writes3[] = {
    // After restarting with a null identity, this is the
    // request we should be issuing -- the final header line contains a Type
    // 1 message.
    MockWrite("GET /kids/login.aspx HTTP/1.1\r\n"
              "Host: 172.22.68.17\r\n"
              "Connection: keep-alive\r\n"
              "Authorization: NTLM "
              "TlRMTVNTUAABAAAAB4IIAAAAAAAAAAAAAAAAAAAAAAA=\r\n\r\n"),

    // After calling trans->RestartWithAuth(), we should send a Type 3 message
    // (the credentials for the origin server).  The second request continues
    // on the same connection.
    MockWrite("GET /kids/login.aspx HTTP/1.1\r\n"
              "Host: 172.22.68.17\r\n"
              "Connection: keep-alive\r\n"
              "Authorization: NTLM TlRMTVNTUAADAAAAGAAYAGgAAAAYABgAgA"
              "AAAAAAAABAAAAAGAAYAEAAAAAQABAAWAAAAAAAAAAAAAAABYIIAHQA"
              "ZQBzAHQAaQBuAGcALQBuAHQAbABtAFcAVABDAC0AVwBJAE4ANwBO54"
              "dFMVvTHwAAAAAAAAAAAAAAAAAAAACS7sT6Uzw7L0L//WUqlIaVWpbI"
              "+4MUm7c=\r\n\r\n"),
  };

  MockRead data_reads3[] = {
    // The origin server responds with a Type 2 message.
    MockRead("HTTP/1.1 401 Access Denied\r\n"),
    MockRead("WWW-Authenticate: NTLM "
             "TlRMTVNTUAACAAAADAAMADgAAAAFgokCL24VN8dgOR8AAAAAAAAAALo"
             "AugBEAAAABQEoCgAAAA9HAE8ATwBHAEwARQACAAwARwBPAE8ARwBMAE"
             "UAAQAaAEEASwBFAEUAUwBBAFIAQQAtAEMATwBSAFAABAAeAGMAbwByA"
             "HAALgBnAG8AbwBnAGwAZQAuAGMAbwBtAAMAQABhAGsAZQBlAHMAYQBy"
             "AGEALQBjAG8AcgBwAC4AYQBkAC4AYwBvAHIAcAAuAGcAbwBvAGcAbAB"
             "lAC4AYwBvAG0ABQAeAGMAbwByAHAALgBnAG8AbwBnAGwAZQAuAGMAbw"
             "BtAAAAAAA=\r\n"),
    MockRead("Content-Length: 42\r\n"),
    MockRead("Content-Type: text/html\r\n\r\n"),
    MockRead("You are not authorized to view this page\r\n"),

    // Lastly we get the desired content.
    MockRead("HTTP/1.1 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=utf-8\r\n"),
    MockRead("Content-Length: 13\r\n\r\n"),
    MockRead("Please Login\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                 data_writes1, arraysize(data_writes1));
  StaticSocketDataProvider data2(data_reads2, arraysize(data_reads2),
                                 data_writes2, arraysize(data_writes2));
  StaticSocketDataProvider data3(data_reads3, arraysize(data_reads3),
                                 data_writes3, arraysize(data_writes3));
  session_deps.socket_factory.AddSocketDataProvider(&data1);
  session_deps.socket_factory.AddSocketDataProvider(&data2);
  session_deps.socket_factory.AddSocketDataProvider(&data3);

  TestCompletionCallback callback1;

  int rv = trans->Start(&request, &callback1, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback1.WaitForResult();
  EXPECT_EQ(OK, rv);

  EXPECT_TRUE(trans->IsReadyToRestartForAuth());
  TestCompletionCallback callback2;
  rv = trans->RestartWithAuth(std::wstring(), std::wstring(), &callback2);
  EXPECT_EQ(ERR_IO_PENDING, rv);
  rv = callback2.WaitForResult();
  EXPECT_EQ(OK, rv);
  EXPECT_FALSE(trans->IsReadyToRestartForAuth());

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);

  // The password prompt info should have been set in response->auth_challenge.
  EXPECT_FALSE(response->auth_challenge.get() == NULL);

  EXPECT_EQ(L"172.22.68.17:80", response->auth_challenge->host_and_port);
  EXPECT_EQ(L"", response->auth_challenge->realm);
  EXPECT_EQ(L"ntlm", response->auth_challenge->scheme);

  TestCompletionCallback callback3;

  // Enter the wrong password.
  rv = trans->RestartWithAuth(L"testing-ntlm", L"wrongpassword", &callback3);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback3.WaitForResult();
  EXPECT_EQ(OK, rv);

  EXPECT_TRUE(trans->IsReadyToRestartForAuth());
  TestCompletionCallback callback4;
  rv = trans->RestartWithAuth(std::wstring(), std::wstring(), &callback4);
  EXPECT_EQ(ERR_IO_PENDING, rv);
  rv = callback4.WaitForResult();
  EXPECT_EQ(OK, rv);
  EXPECT_FALSE(trans->IsReadyToRestartForAuth());

  response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);

  // The password prompt info should have been set in response->auth_challenge.
  EXPECT_FALSE(response->auth_challenge.get() == NULL);

  EXPECT_EQ(L"172.22.68.17:80", response->auth_challenge->host_and_port);
  EXPECT_EQ(L"", response->auth_challenge->realm);
  EXPECT_EQ(L"ntlm", response->auth_challenge->scheme);

  TestCompletionCallback callback5;

  // Now enter the right password.
  rv = trans->RestartWithAuth(L"testing-ntlm", L"testing-ntlm", &callback5);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback5.WaitForResult();
  EXPECT_EQ(OK, rv);

  response = trans->GetResponseInfo();
  EXPECT_TRUE(response->auth_challenge.get() == NULL);
  EXPECT_EQ(13, response->headers->GetContentLength());
}
#endif  // NTLM_PORTABLE

// Test reading a server response which has only headers, and no body.
// After some maximum number of bytes is consumed, the transaction should
// fail with ERR_RESPONSE_HEADERS_TOO_BIG.
TEST_F(HttpNetworkTransactionTest, LargeHeadersNoBody) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;

  // Respond with 300 kb of headers (we should fail after 256 kb).
  std::string large_headers_string;
  FillLargeHeadersString(&large_headers_string, 300 * 1024);

  MockRead data_reads[] = {
    MockRead("HTTP/1.0 200 OK\r\n"),
    MockRead(true, large_headers_string.data(), large_headers_string.size()),
    MockRead("\r\nBODY"),
    MockRead(false, OK),
  };
  StaticSocketDataProvider data(data_reads, arraysize(data_reads), NULL, 0);
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(ERR_RESPONSE_HEADERS_TOO_BIG, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_TRUE(response == NULL);
}

// Make sure that we don't try to reuse a TCPClientSocket when failing to
// establish tunnel.
// http://code.google.com/p/chromium/issues/detail?id=3772
TEST_F(HttpNetworkTransactionTest, DontRecycleTCPSocketForSSLTunnel) {
  // Configure against proxy server "myproxy:70".
  SessionDependencies session_deps(CreateFixedProxyService("myproxy:70"));

  scoped_refptr<HttpNetworkSession> session(CreateSession(&session_deps));

  scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("https://www.google.com/");
  request.load_flags = 0;

  // Since we have proxy, should try to establish tunnel.
  MockWrite data_writes1[] = {
    MockWrite("CONNECT www.google.com:443 HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Proxy-Connection: keep-alive\r\n\r\n"),
  };

  // The proxy responds to the connect with a 404, using a persistent
  // connection. Usually a proxy would return 501 (not implemented),
  // or 200 (tunnel established).
  MockRead data_reads1[] = {
    MockRead("HTTP/1.1 404 Not Found\r\n"),
    MockRead("Content-Length: 10\r\n\r\n"),
    MockRead(false, ERR_UNEXPECTED),  // Should not be reached.
  };

  StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                 data_writes1, arraysize(data_writes1));
  session_deps.socket_factory.AddSocketDataProvider(&data1);

  TestCompletionCallback callback1;

  int rv = trans->Start(&request, &callback1, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback1.WaitForResult();
  EXPECT_EQ(ERR_TUNNEL_CONNECTION_FAILED, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_TRUE(response == NULL);

  // Empty the current queue.  This is necessary because idle sockets are
  // added to the connection pool asynchronously with a PostTask.
  MessageLoop::current()->RunAllPending();

  // We now check to make sure the TCPClientSocket was not added back to
  // the pool.
  EXPECT_EQ(0, session->tcp_socket_pool()->IdleSocketCount());
  trans.reset();
  MessageLoop::current()->RunAllPending();
  // Make sure that the socket didn't get recycled after calling the destructor.
  EXPECT_EQ(0, session->tcp_socket_pool()->IdleSocketCount());
}

// Make sure that we recycle a socket after reading all of the response body.
TEST_F(HttpNetworkTransactionTest, RecycleSocket) {
  SessionDependencies session_deps;
  scoped_refptr<HttpNetworkSession> session(CreateSession(&session_deps));

  scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;

  MockRead data_reads[] = {
    // A part of the response body is received with the response headers.
    MockRead("HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\nhel"),
    // The rest of the response body is received in two parts.
    MockRead("lo"),
    MockRead(" world"),
    MockRead("junk"),  // Should not be read!!
    MockRead(false, OK),
  };

  StaticSocketDataProvider data(data_reads, arraysize(data_reads), NULL, 0);
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_TRUE(response != NULL);

  EXPECT_TRUE(response->headers != NULL);
  std::string status_line = response->headers->GetStatusLine();
  EXPECT_EQ("HTTP/1.1 200 OK", status_line);

  EXPECT_EQ(0, session->tcp_socket_pool()->IdleSocketCount());

  std::string response_data;
  rv = ReadTransaction(trans.get(), &response_data);
  EXPECT_EQ(OK, rv);
  EXPECT_EQ("hello world", response_data);

  // Empty the current queue.  This is necessary because idle sockets are
  // added to the connection pool asynchronously with a PostTask.
  MessageLoop::current()->RunAllPending();

  // We now check to make sure the socket was added back to the pool.
  EXPECT_EQ(1, session->tcp_socket_pool()->IdleSocketCount());
}

// Make sure that we recycle a socket after a zero-length response.
// http://crbug.com/9880
TEST_F(HttpNetworkTransactionTest, RecycleSocketAfterZeroContentLength) {
  SessionDependencies session_deps;
  scoped_refptr<HttpNetworkSession> session(CreateSession(&session_deps));

  scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/csi?v=3&s=web&action=&"
                     "tran=undefined&ei=mAXcSeegAo-SMurloeUN&"
                     "e=17259,18167,19592,19773,19981,20133,20173,20233&"
                     "rt=prt.2642,ol.2649,xjs.2951");
  request.load_flags = 0;

  MockRead data_reads[] = {
    MockRead("HTTP/1.1 204 No Content\r\n"
             "Content-Length: 0\r\n"
             "Content-Type: text/html\r\n\r\n"),
    MockRead("junk"),  // Should not be read!!
    MockRead(false, OK),
  };

  StaticSocketDataProvider data(data_reads, arraysize(data_reads), NULL, 0);
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_TRUE(response != NULL);

  EXPECT_TRUE(response->headers != NULL);
  std::string status_line = response->headers->GetStatusLine();
  EXPECT_EQ("HTTP/1.1 204 No Content", status_line);

  EXPECT_EQ(0, session->tcp_socket_pool()->IdleSocketCount());

  std::string response_data;
  rv = ReadTransaction(trans.get(), &response_data);
  EXPECT_EQ(OK, rv);
  EXPECT_EQ("", response_data);

  // Empty the current queue.  This is necessary because idle sockets are
  // added to the connection pool asynchronously with a PostTask.
  MessageLoop::current()->RunAllPending();

  // We now check to make sure the socket was added back to the pool.
  EXPECT_EQ(1, session->tcp_socket_pool()->IdleSocketCount());
}

TEST_F(HttpNetworkTransactionTest, ResendRequestOnWriteBodyError) {
  HttpRequestInfo request[2];
  // Transaction 1: a GET request that succeeds.  The socket is recycled
  // after use.
  request[0].method = "GET";
  request[0].url = GURL("http://www.google.com/");
  request[0].load_flags = 0;
  // Transaction 2: a POST request.  Reuses the socket kept alive from
  // transaction 1.  The first attempts fails when writing the POST data.
  // This causes the transaction to retry with a new socket.  The second
  // attempt succeeds.
  request[1].method = "POST";
  request[1].url = GURL("http://www.google.com/login.cgi");
  request[1].upload_data = new UploadData;
  request[1].upload_data->AppendBytes("foo", 3);
  request[1].load_flags = 0;

  SessionDependencies session_deps;
  scoped_refptr<HttpNetworkSession> session = CreateSession(&session_deps);

  // The first socket is used for transaction 1 and the first attempt of
  // transaction 2.

  // The response of transaction 1.
  MockRead data_reads1[] = {
    MockRead("HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\n"),
    MockRead("hello world"),
    MockRead(false, OK),
  };
  // The mock write results of transaction 1 and the first attempt of
  // transaction 2.
  MockWrite data_writes1[] = {
    MockWrite(false, 64),  // GET
    MockWrite(false, 93),  // POST
    MockWrite(false, ERR_CONNECTION_ABORTED),  // POST data
  };
  StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                 data_writes1, arraysize(data_writes1));

  // The second socket is used for the second attempt of transaction 2.

  // The response of transaction 2.
  MockRead data_reads2[] = {
    MockRead("HTTP/1.1 200 OK\r\nContent-Length: 7\r\n\r\n"),
    MockRead("welcome"),
    MockRead(false, OK),
  };
  // The mock write results of the second attempt of transaction 2.
  MockWrite data_writes2[] = {
    MockWrite(false, 93),  // POST
    MockWrite(false, 3),  // POST data
  };
  StaticSocketDataProvider data2(data_reads2, arraysize(data_reads2),
                                 data_writes2, arraysize(data_writes2));

  session_deps.socket_factory.AddSocketDataProvider(&data1);
  session_deps.socket_factory.AddSocketDataProvider(&data2);

  const char* kExpectedResponseData[] = {
    "hello world", "welcome"
  };

  for (int i = 0; i < 2; ++i) {
    scoped_ptr<HttpTransaction> trans(
        new HttpNetworkTransaction(session));

    TestCompletionCallback callback;

    int rv = trans->Start(&request[i], &callback, BoundNetLog());
    EXPECT_EQ(ERR_IO_PENDING, rv);

    rv = callback.WaitForResult();
    EXPECT_EQ(OK, rv);

    const HttpResponseInfo* response = trans->GetResponseInfo();
    EXPECT_TRUE(response != NULL);

    EXPECT_TRUE(response->headers != NULL);
    EXPECT_EQ("HTTP/1.1 200 OK", response->headers->GetStatusLine());

    std::string response_data;
    rv = ReadTransaction(trans.get(), &response_data);
    EXPECT_EQ(OK, rv);
    EXPECT_EQ(kExpectedResponseData[i], response_data);
  }
}

// Test the request-challenge-retry sequence for basic auth when there is
// an identity in the URL. The request should be sent as normal, but when
// it fails the identity from the URL is used to answer the challenge.
TEST_F(HttpNetworkTransactionTest, AuthIdentityInURL) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  // Note: the URL has a username:password in it.
  request.url = GURL("http://foo:b@r@www.google.com/");

  // The password contains an escaped character -- for this test to pass it
  // will need to be unescaped by HttpNetworkTransaction.
  EXPECT_EQ("b%40r", request.url.password());

  request.load_flags = LOAD_NORMAL;

  MockWrite data_writes1[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n\r\n"),
  };

  MockRead data_reads1[] = {
    MockRead("HTTP/1.0 401 Unauthorized\r\n"),
    MockRead("WWW-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
    MockRead("Content-Length: 10\r\n\r\n"),
    MockRead(false, ERR_FAILED),
  };

  // After the challenge above, the transaction will be restarted using the
  // identity from the url (foo, b@r) to answer the challenge.
  MockWrite data_writes2[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Authorization: Basic Zm9vOmJAcg==\r\n\r\n"),
  };

  MockRead data_reads2[] = {
    MockRead("HTTP/1.0 200 OK\r\n"),
    MockRead("Content-Length: 100\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                 data_writes1, arraysize(data_writes1));
  StaticSocketDataProvider data2(data_reads2, arraysize(data_reads2),
                                 data_writes2, arraysize(data_writes2));
  session_deps.socket_factory.AddSocketDataProvider(&data1);
  session_deps.socket_factory.AddSocketDataProvider(&data2);

  TestCompletionCallback callback1;

  int rv = trans->Start(&request, &callback1, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback1.WaitForResult();
  EXPECT_EQ(OK, rv);

  EXPECT_TRUE(trans->IsReadyToRestartForAuth());
  TestCompletionCallback callback2;
  rv = trans->RestartWithAuth(std::wstring(), std::wstring(), &callback2);
  EXPECT_EQ(ERR_IO_PENDING, rv);
  rv = callback2.WaitForResult();
  EXPECT_EQ(OK, rv);
  EXPECT_FALSE(trans->IsReadyToRestartForAuth());

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);

  // There is no challenge info, since the identity in URL worked.
  EXPECT_TRUE(response->auth_challenge.get() == NULL);

  EXPECT_EQ(100, response->headers->GetContentLength());

  // Empty the current queue.
  MessageLoop::current()->RunAllPending();
}

// Test the request-challenge-retry sequence for basic auth when there is
// an incorrect identity in the URL. The identity from the URL should be used
// only once.
TEST_F(HttpNetworkTransactionTest, WrongAuthIdentityInURL) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  // Note: the URL has a username:password in it.  The password "baz" is
  // wrong (should be "bar").
  request.url = GURL("http://foo:baz@www.google.com/");

  request.load_flags = LOAD_NORMAL;

  MockWrite data_writes1[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n\r\n"),
  };

  MockRead data_reads1[] = {
    MockRead("HTTP/1.0 401 Unauthorized\r\n"),
    MockRead("WWW-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
    MockRead("Content-Length: 10\r\n\r\n"),
    MockRead(false, ERR_FAILED),
  };

  // After the challenge above, the transaction will be restarted using the
  // identity from the url (foo, baz) to answer the challenge.
  MockWrite data_writes2[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Authorization: Basic Zm9vOmJheg==\r\n\r\n"),
  };

  MockRead data_reads2[] = {
    MockRead("HTTP/1.0 401 Unauthorized\r\n"),
    MockRead("WWW-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
    MockRead("Content-Length: 10\r\n\r\n"),
    MockRead(false, ERR_FAILED),
  };

  // After the challenge above, the transaction will be restarted using the
  // identity supplied by the user (foo, bar) to answer the challenge.
  MockWrite data_writes3[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Authorization: Basic Zm9vOmJhcg==\r\n\r\n"),
  };

  MockRead data_reads3[] = {
    MockRead("HTTP/1.0 200 OK\r\n"),
    MockRead("Content-Length: 100\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                 data_writes1, arraysize(data_writes1));
  StaticSocketDataProvider data2(data_reads2, arraysize(data_reads2),
                                 data_writes2, arraysize(data_writes2));
  StaticSocketDataProvider data3(data_reads3, arraysize(data_reads3),
                                 data_writes3, arraysize(data_writes3));
  session_deps.socket_factory.AddSocketDataProvider(&data1);
  session_deps.socket_factory.AddSocketDataProvider(&data2);
  session_deps.socket_factory.AddSocketDataProvider(&data3);

  TestCompletionCallback callback1;

  int rv = trans->Start(&request, &callback1, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback1.WaitForResult();
  EXPECT_EQ(OK, rv);

  EXPECT_TRUE(trans->IsReadyToRestartForAuth());
  TestCompletionCallback callback2;
  rv = trans->RestartWithAuth(std::wstring(), std::wstring(), &callback2);
  EXPECT_EQ(ERR_IO_PENDING, rv);
  rv = callback2.WaitForResult();
  EXPECT_EQ(OK, rv);
  EXPECT_FALSE(trans->IsReadyToRestartForAuth());

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);
  // The password prompt info should have been set in response->auth_challenge.
  EXPECT_FALSE(response->auth_challenge.get() == NULL);

  EXPECT_EQ(L"www.google.com:80", response->auth_challenge->host_and_port);
  EXPECT_EQ(L"MyRealm1", response->auth_challenge->realm);
  EXPECT_EQ(L"basic", response->auth_challenge->scheme);

  TestCompletionCallback callback3;
  rv = trans->RestartWithAuth(L"foo", L"bar", &callback3);
  EXPECT_EQ(ERR_IO_PENDING, rv);
  rv = callback3.WaitForResult();
  EXPECT_EQ(OK, rv);
  EXPECT_FALSE(trans->IsReadyToRestartForAuth());

  response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);

  // There is no challenge info, since the identity worked.
  EXPECT_TRUE(response->auth_challenge.get() == NULL);

  EXPECT_EQ(100, response->headers->GetContentLength());

  // Empty the current queue.
  MessageLoop::current()->RunAllPending();
}

// Test that previously tried username/passwords for a realm get re-used.
TEST_F(HttpNetworkTransactionTest, BasicAuthCacheAndPreauth) {
  SessionDependencies session_deps;
  scoped_refptr<HttpNetworkSession> session = CreateSession(&session_deps);

  // Transaction 1: authenticate (foo, bar) on MyRealm1
  {
    scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));

    HttpRequestInfo request;
    request.method = "GET";
    request.url = GURL("http://www.google.com/x/y/z");
    request.load_flags = 0;

    MockWrite data_writes1[] = {
      MockWrite("GET /x/y/z HTTP/1.1\r\n"
                "Host: www.google.com\r\n"
                "Connection: keep-alive\r\n\r\n"),
    };

    MockRead data_reads1[] = {
      MockRead("HTTP/1.0 401 Unauthorized\r\n"),
      MockRead("WWW-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
      MockRead("Content-Length: 10000\r\n\r\n"),
      MockRead(false, ERR_FAILED),
    };

    // Resend with authorization (username=foo, password=bar)
    MockWrite data_writes2[] = {
      MockWrite("GET /x/y/z HTTP/1.1\r\n"
                "Host: www.google.com\r\n"
                "Connection: keep-alive\r\n"
                "Authorization: Basic Zm9vOmJhcg==\r\n\r\n"),
    };

    // Sever accepts the authorization.
    MockRead data_reads2[] = {
      MockRead("HTTP/1.0 200 OK\r\n"),
      MockRead("Content-Length: 100\r\n\r\n"),
      MockRead(false, OK),
    };

    StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                   data_writes1, arraysize(data_writes1));
    StaticSocketDataProvider data2(data_reads2, arraysize(data_reads2),
                                   data_writes2, arraysize(data_writes2));
    session_deps.socket_factory.AddSocketDataProvider(&data1);
    session_deps.socket_factory.AddSocketDataProvider(&data2);

    TestCompletionCallback callback1;

    int rv = trans->Start(&request, &callback1, BoundNetLog());
    EXPECT_EQ(ERR_IO_PENDING, rv);

    rv = callback1.WaitForResult();
    EXPECT_EQ(OK, rv);

    const HttpResponseInfo* response = trans->GetResponseInfo();
    EXPECT_FALSE(response == NULL);

    // The password prompt info should have been set in
    // response->auth_challenge.
    EXPECT_FALSE(response->auth_challenge.get() == NULL);

    EXPECT_EQ(L"www.google.com:80", response->auth_challenge->host_and_port);
    EXPECT_EQ(L"MyRealm1", response->auth_challenge->realm);
    EXPECT_EQ(L"basic", response->auth_challenge->scheme);

    TestCompletionCallback callback2;

    rv = trans->RestartWithAuth(L"foo", L"bar", &callback2);
    EXPECT_EQ(ERR_IO_PENDING, rv);

    rv = callback2.WaitForResult();
    EXPECT_EQ(OK, rv);

    response = trans->GetResponseInfo();
    EXPECT_FALSE(response == NULL);
    EXPECT_TRUE(response->auth_challenge.get() == NULL);
    EXPECT_EQ(100, response->headers->GetContentLength());
  }

  // ------------------------------------------------------------------------

  // Transaction 2: authenticate (foo2, bar2) on MyRealm2
  {
    scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));

    HttpRequestInfo request;
    request.method = "GET";
    // Note that Transaction 1 was at /x/y/z, so this is in the same
    // protection space as MyRealm1.
    request.url = GURL("http://www.google.com/x/y/a/b");
    request.load_flags = 0;

    MockWrite data_writes1[] = {
      MockWrite("GET /x/y/a/b HTTP/1.1\r\n"
                "Host: www.google.com\r\n"
                "Connection: keep-alive\r\n"
                // Send preemptive authorization for MyRealm1
                "Authorization: Basic Zm9vOmJhcg==\r\n\r\n"),
    };

    // The server didn't like the preemptive authorization, and
    // challenges us for a different realm (MyRealm2).
    MockRead data_reads1[] = {
      MockRead("HTTP/1.0 401 Unauthorized\r\n"),
      MockRead("WWW-Authenticate: Basic realm=\"MyRealm2\"\r\n"),
      MockRead("Content-Length: 10000\r\n\r\n"),
      MockRead(false, ERR_FAILED),
    };

    // Resend with authorization for MyRealm2 (username=foo2, password=bar2)
    MockWrite data_writes2[] = {
      MockWrite("GET /x/y/a/b HTTP/1.1\r\n"
                "Host: www.google.com\r\n"
                "Connection: keep-alive\r\n"
                "Authorization: Basic Zm9vMjpiYXIy\r\n\r\n"),
    };

    // Sever accepts the authorization.
    MockRead data_reads2[] = {
      MockRead("HTTP/1.0 200 OK\r\n"),
      MockRead("Content-Length: 100\r\n\r\n"),
      MockRead(false, OK),
    };

    StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                   data_writes1, arraysize(data_writes1));
    StaticSocketDataProvider data2(data_reads2, arraysize(data_reads2),
                                   data_writes2, arraysize(data_writes2));
    session_deps.socket_factory.AddSocketDataProvider(&data1);
    session_deps.socket_factory.AddSocketDataProvider(&data2);

    TestCompletionCallback callback1;

    int rv = trans->Start(&request, &callback1, BoundNetLog());
    EXPECT_EQ(ERR_IO_PENDING, rv);

    rv = callback1.WaitForResult();
    EXPECT_EQ(OK, rv);

    const HttpResponseInfo* response = trans->GetResponseInfo();
    EXPECT_FALSE(response == NULL);

    // The password prompt info should have been set in
    // response->auth_challenge.
    EXPECT_FALSE(response->auth_challenge.get() == NULL);

    EXPECT_EQ(L"www.google.com:80", response->auth_challenge->host_and_port);
    EXPECT_EQ(L"MyRealm2", response->auth_challenge->realm);
    EXPECT_EQ(L"basic", response->auth_challenge->scheme);

    TestCompletionCallback callback2;

    rv = trans->RestartWithAuth(L"foo2", L"bar2", &callback2);
    EXPECT_EQ(ERR_IO_PENDING, rv);

    rv = callback2.WaitForResult();
    EXPECT_EQ(OK, rv);

    response = trans->GetResponseInfo();
    EXPECT_FALSE(response == NULL);
    EXPECT_TRUE(response->auth_challenge.get() == NULL);
    EXPECT_EQ(100, response->headers->GetContentLength());
  }

  // ------------------------------------------------------------------------

  // Transaction 3: Resend a request in MyRealm's protection space --
  // succeed with preemptive authorization.
  {
    scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));

    HttpRequestInfo request;
    request.method = "GET";
    request.url = GURL("http://www.google.com/x/y/z2");
    request.load_flags = 0;

    MockWrite data_writes1[] = {
      MockWrite("GET /x/y/z2 HTTP/1.1\r\n"
                "Host: www.google.com\r\n"
                "Connection: keep-alive\r\n"
                // The authorization for MyRealm1 gets sent preemptively
                // (since the url is in the same protection space)
                "Authorization: Basic Zm9vOmJhcg==\r\n\r\n"),
    };

    // Sever accepts the preemptive authorization
    MockRead data_reads1[] = {
      MockRead("HTTP/1.0 200 OK\r\n"),
      MockRead("Content-Length: 100\r\n\r\n"),
      MockRead(false, OK),
    };

    StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                   data_writes1, arraysize(data_writes1));
    session_deps.socket_factory.AddSocketDataProvider(&data1);

    TestCompletionCallback callback1;

    int rv = trans->Start(&request, &callback1, BoundNetLog());
    EXPECT_EQ(ERR_IO_PENDING, rv);

    rv = callback1.WaitForResult();
    EXPECT_EQ(OK, rv);

    const HttpResponseInfo* response = trans->GetResponseInfo();
    EXPECT_FALSE(response == NULL);

    EXPECT_TRUE(response->auth_challenge.get() == NULL);
    EXPECT_EQ(100, response->headers->GetContentLength());
  }

  // ------------------------------------------------------------------------

  // Transaction 4: request another URL in MyRealm (however the
  // url is not known to belong to the protection space, so no pre-auth).
  {
    scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));

    HttpRequestInfo request;
    request.method = "GET";
    request.url = GURL("http://www.google.com/x/1");
    request.load_flags = 0;

    MockWrite data_writes1[] = {
      MockWrite("GET /x/1 HTTP/1.1\r\n"
                "Host: www.google.com\r\n"
                "Connection: keep-alive\r\n\r\n"),
    };

    MockRead data_reads1[] = {
      MockRead("HTTP/1.0 401 Unauthorized\r\n"),
      MockRead("WWW-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
      MockRead("Content-Length: 10000\r\n\r\n"),
      MockRead(false, ERR_FAILED),
    };

    // Resend with authorization from MyRealm's cache.
    MockWrite data_writes2[] = {
      MockWrite("GET /x/1 HTTP/1.1\r\n"
                "Host: www.google.com\r\n"
                "Connection: keep-alive\r\n"
                "Authorization: Basic Zm9vOmJhcg==\r\n\r\n"),
    };

    // Sever accepts the authorization.
    MockRead data_reads2[] = {
      MockRead("HTTP/1.0 200 OK\r\n"),
      MockRead("Content-Length: 100\r\n\r\n"),
      MockRead(false, OK),
    };

    StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                   data_writes1, arraysize(data_writes1));
    StaticSocketDataProvider data2(data_reads2, arraysize(data_reads2),
                                   data_writes2, arraysize(data_writes2));
    session_deps.socket_factory.AddSocketDataProvider(&data1);
    session_deps.socket_factory.AddSocketDataProvider(&data2);

    TestCompletionCallback callback1;

    int rv = trans->Start(&request, &callback1, BoundNetLog());
    EXPECT_EQ(ERR_IO_PENDING, rv);

    rv = callback1.WaitForResult();
    EXPECT_EQ(OK, rv);

    EXPECT_TRUE(trans->IsReadyToRestartForAuth());
    TestCompletionCallback callback2;
    rv = trans->RestartWithAuth(std::wstring(), std::wstring(), &callback2);
    EXPECT_EQ(ERR_IO_PENDING, rv);
    rv = callback2.WaitForResult();
    EXPECT_EQ(OK, rv);
    EXPECT_FALSE(trans->IsReadyToRestartForAuth());

    const HttpResponseInfo* response = trans->GetResponseInfo();
    EXPECT_FALSE(response == NULL);
    EXPECT_TRUE(response->auth_challenge.get() == NULL);
    EXPECT_EQ(100, response->headers->GetContentLength());
  }

  // ------------------------------------------------------------------------

  // Transaction 5: request a URL in MyRealm, but the server rejects the
  // cached identity. Should invalidate and re-prompt.
  {
    scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));

    HttpRequestInfo request;
    request.method = "GET";
    request.url = GURL("http://www.google.com/p/q/t");
    request.load_flags = 0;

    MockWrite data_writes1[] = {
      MockWrite("GET /p/q/t HTTP/1.1\r\n"
                "Host: www.google.com\r\n"
                "Connection: keep-alive\r\n\r\n"),
    };

    MockRead data_reads1[] = {
      MockRead("HTTP/1.0 401 Unauthorized\r\n"),
      MockRead("WWW-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
      MockRead("Content-Length: 10000\r\n\r\n"),
      MockRead(false, ERR_FAILED),
    };

    // Resend with authorization from cache for MyRealm.
    MockWrite data_writes2[] = {
      MockWrite("GET /p/q/t HTTP/1.1\r\n"
                "Host: www.google.com\r\n"
                "Connection: keep-alive\r\n"
                "Authorization: Basic Zm9vOmJhcg==\r\n\r\n"),
    };

    // Sever rejects the authorization.
    MockRead data_reads2[] = {
      MockRead("HTTP/1.0 401 Unauthorized\r\n"),
      MockRead("WWW-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
      MockRead("Content-Length: 10000\r\n\r\n"),
      MockRead(false, ERR_FAILED),
    };

    // At this point we should prompt for new credentials for MyRealm.
    // Restart with username=foo3, password=foo4.
    MockWrite data_writes3[] = {
      MockWrite("GET /p/q/t HTTP/1.1\r\n"
                "Host: www.google.com\r\n"
                "Connection: keep-alive\r\n"
                "Authorization: Basic Zm9vMzpiYXIz\r\n\r\n"),
    };

    // Sever accepts the authorization.
    MockRead data_reads3[] = {
      MockRead("HTTP/1.0 200 OK\r\n"),
      MockRead("Content-Length: 100\r\n\r\n"),
      MockRead(false, OK),
    };

    StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                   data_writes1, arraysize(data_writes1));
    StaticSocketDataProvider data2(data_reads2, arraysize(data_reads2),
                                   data_writes2, arraysize(data_writes2));
    StaticSocketDataProvider data3(data_reads3, arraysize(data_reads3),
                                   data_writes3, arraysize(data_writes3));
    session_deps.socket_factory.AddSocketDataProvider(&data1);
    session_deps.socket_factory.AddSocketDataProvider(&data2);
    session_deps.socket_factory.AddSocketDataProvider(&data3);

    TestCompletionCallback callback1;

    int rv = trans->Start(&request, &callback1, BoundNetLog());
    EXPECT_EQ(ERR_IO_PENDING, rv);

    rv = callback1.WaitForResult();
    EXPECT_EQ(OK, rv);

    EXPECT_TRUE(trans->IsReadyToRestartForAuth());
    TestCompletionCallback callback2;
    rv = trans->RestartWithAuth(std::wstring(), std::wstring(), &callback2);
    EXPECT_EQ(ERR_IO_PENDING, rv);
    rv = callback2.WaitForResult();
    EXPECT_EQ(OK, rv);
    EXPECT_FALSE(trans->IsReadyToRestartForAuth());

    const HttpResponseInfo* response = trans->GetResponseInfo();
    EXPECT_FALSE(response == NULL);

    // The password prompt info should have been set in
    // response->auth_challenge.
    EXPECT_FALSE(response->auth_challenge.get() == NULL);

    EXPECT_EQ(L"www.google.com:80", response->auth_challenge->host_and_port);
    EXPECT_EQ(L"MyRealm1", response->auth_challenge->realm);
    EXPECT_EQ(L"basic", response->auth_challenge->scheme);

    TestCompletionCallback callback3;

    rv = trans->RestartWithAuth(L"foo3", L"bar3", &callback3);
    EXPECT_EQ(ERR_IO_PENDING, rv);

    rv = callback3.WaitForResult();
    EXPECT_EQ(OK, rv);

    response = trans->GetResponseInfo();
    EXPECT_FALSE(response == NULL);
    EXPECT_TRUE(response->auth_challenge.get() == NULL);
    EXPECT_EQ(100, response->headers->GetContentLength());
  }
}

// Tests that nonce count increments when multiple auth attempts
// are started with the same nonce.
TEST_F(HttpNetworkTransactionTest, DigestPreAuthNonceCount) {
  SessionDependencies session_deps;
  scoped_refptr<HttpNetworkSession> session = CreateSession(&session_deps);
  HttpAuthHandlerDigest::SetFixedCnonce(true);

  // Transaction 1: authenticate (foo, bar) on MyRealm1
  {
    scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));

    HttpRequestInfo request;
    request.method = "GET";
    request.url = GURL("http://www.google.com/x/y/z");
    request.load_flags = 0;

    MockWrite data_writes1[] = {
      MockWrite("GET /x/y/z HTTP/1.1\r\n"
                "Host: www.google.com\r\n"
                "Connection: keep-alive\r\n\r\n"),
    };

    MockRead data_reads1[] = {
      MockRead("HTTP/1.0 401 Unauthorized\r\n"),
      MockRead("WWW-Authenticate: Digest realm=\"digestive\", nonce=\"OU812\", "
               "algorithm=MD5, qop=\"auth\"\r\n\r\n"),
      MockRead(false, OK),
    };

    // Resend with authorization (username=foo, password=bar)
    MockWrite data_writes2[] = {
      MockWrite("GET /x/y/z HTTP/1.1\r\n"
                "Host: www.google.com\r\n"
                "Connection: keep-alive\r\n"
                "Authorization: Digest username=\"foo\", realm=\"digestive\", "
                "nonce=\"OU812\", uri=\"/x/y/z\", algorithm=MD5, "
                "response=\"03ffbcd30add722589c1de345d7a927f\", qop=auth, "
                "nc=00000001, cnonce=\"0123456789abcdef\"\r\n\r\n"),
    };

    // Sever accepts the authorization.
    MockRead data_reads2[] = {
      MockRead("HTTP/1.0 200 OK\r\n"),
      MockRead(false, OK),
    };

    StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                   data_writes1, arraysize(data_writes1));
    StaticSocketDataProvider data2(data_reads2, arraysize(data_reads2),
                                   data_writes2, arraysize(data_writes2));
    session_deps.socket_factory.AddSocketDataProvider(&data1);
    session_deps.socket_factory.AddSocketDataProvider(&data2);

    TestCompletionCallback callback1;

    int rv = trans->Start(&request, &callback1, BoundNetLog());
    EXPECT_EQ(ERR_IO_PENDING, rv);

    rv = callback1.WaitForResult();
    EXPECT_EQ(OK, rv);

    const HttpResponseInfo* response = trans->GetResponseInfo();
    ASSERT_FALSE(response == NULL);

    // The password prompt info should have been set in
    // response->auth_challenge.
    ASSERT_FALSE(response->auth_challenge.get() == NULL);

    EXPECT_EQ(L"www.google.com:80", response->auth_challenge->host_and_port);
    EXPECT_EQ(L"digestive", response->auth_challenge->realm);
    EXPECT_EQ(L"digest", response->auth_challenge->scheme);

    TestCompletionCallback callback2;

    rv = trans->RestartWithAuth(L"foo", L"bar", &callback2);
    EXPECT_EQ(ERR_IO_PENDING, rv);

    rv = callback2.WaitForResult();
    EXPECT_EQ(OK, rv);

    response = trans->GetResponseInfo();
    ASSERT_FALSE(response == NULL);
    EXPECT_TRUE(response->auth_challenge.get() == NULL);
  }

  // ------------------------------------------------------------------------

  // Transaction 2: Request another resource in digestive's protection space.
  // This will preemptively add an Authorization header which should have an
  // "nc" value of 2 (as compared to 1 in the first use.
  {
    scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));

    HttpRequestInfo request;
    request.method = "GET";
    // Note that Transaction 1 was at /x/y/z, so this is in the same
    // protection space as digest.
    request.url = GURL("http://www.google.com/x/y/a/b");
    request.load_flags = 0;

    MockWrite data_writes1[] = {
      MockWrite("GET /x/y/a/b HTTP/1.1\r\n"
                "Host: www.google.com\r\n"
                "Connection: keep-alive\r\n"
                "Authorization: Digest username=\"foo\", realm=\"digestive\", "
                "nonce=\"OU812\", uri=\"/x/y/a/b\", algorithm=MD5, "
                "response=\"d6f9a2c07d1c5df7b89379dca1269b35\", qop=auth, "
                "nc=00000002, cnonce=\"0123456789abcdef\"\r\n\r\n"),
    };

    // Sever accepts the authorization.
    MockRead data_reads1[] = {
      MockRead("HTTP/1.0 200 OK\r\n"),
      MockRead("Content-Length: 100\r\n\r\n"),
      MockRead(false, OK),
    };

    StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                   data_writes1, arraysize(data_writes1));
    session_deps.socket_factory.AddSocketDataProvider(&data1);

    TestCompletionCallback callback1;

    int rv = trans->Start(&request, &callback1, BoundNetLog());
    EXPECT_EQ(ERR_IO_PENDING, rv);

    rv = callback1.WaitForResult();
    EXPECT_EQ(OK, rv);

    const HttpResponseInfo* response = trans->GetResponseInfo();
    ASSERT_FALSE(response == NULL);
    EXPECT_TRUE(response->auth_challenge.get() == NULL);
  }
}

// Test the ResetStateForRestart() private method.
TEST_F(HttpNetworkTransactionTest, ResetStateForRestart) {
  // Create a transaction (the dependencies aren't important).
  SessionDependencies session_deps;
  scoped_ptr<HttpNetworkTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  // Setup some state (which we expect ResetStateForRestart() will clear).
  trans->read_buf_ = new IOBuffer(15);
  trans->read_buf_len_ = 15;
  trans->request_headers_ = "Authorization: NTLM";

  // Setup state in response_
  HttpResponseInfo* response = &trans->response_;
  response->auth_challenge = new AuthChallengeInfo();
  response->ssl_info.cert_status = -15;
  response->response_time = base::Time::Now();
  response->was_cached = true;  // (Wouldn't ever actually be true...)

  { // Setup state for response_.vary_data
    HttpRequestInfo request;
    std::string temp("HTTP/1.1 200 OK\nVary: foo, bar\n\n");
    std::replace(temp.begin(), temp.end(), '\n', '\0');
    scoped_refptr<HttpResponseHeaders> headers = new HttpResponseHeaders(temp);
    request.extra_headers.SetHeader("Foo", "1");
    request.extra_headers.SetHeader("bar", "23");
    EXPECT_TRUE(response->vary_data.Init(request, *headers));
  }

  // Cause the above state to be reset.
  trans->ResetStateForRestart();

  // Verify that the state that needed to be reset, has been reset.
  EXPECT_TRUE(trans->read_buf_.get() == NULL);
  EXPECT_EQ(0, trans->read_buf_len_);
  EXPECT_EQ(0U, trans->request_headers_.size());
  EXPECT_TRUE(response->auth_challenge.get() == NULL);
  EXPECT_TRUE(response->headers.get() == NULL);
  EXPECT_EQ(false, response->was_cached);
  EXPECT_EQ(0, response->ssl_info.cert_status);
  EXPECT_FALSE(response->vary_data.is_valid());
}

// Test HTTPS connections to a site with a bad certificate
TEST_F(HttpNetworkTransactionTest, HTTPSBadCertificate) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("https://www.google.com/");
  request.load_flags = 0;

  MockWrite data_writes[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n\r\n"),
  };

  MockRead data_reads[] = {
    MockRead("HTTP/1.0 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 100\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider ssl_bad_certificate;
  StaticSocketDataProvider data(data_reads, arraysize(data_reads),
                                data_writes, arraysize(data_writes));
  SSLSocketDataProvider ssl_bad(true, ERR_CERT_AUTHORITY_INVALID);
  SSLSocketDataProvider ssl(true, OK);

  session_deps.socket_factory.AddSocketDataProvider(&ssl_bad_certificate);
  session_deps.socket_factory.AddSocketDataProvider(&data);
  session_deps.socket_factory.AddSSLSocketDataProvider(&ssl_bad);
  session_deps.socket_factory.AddSSLSocketDataProvider(&ssl);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(ERR_CERT_AUTHORITY_INVALID, rv);

  rv = trans->RestartIgnoringLastError(&callback);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();

  EXPECT_FALSE(response == NULL);
  EXPECT_EQ(100, response->headers->GetContentLength());
}

// Test HTTPS connections to a site with a bad certificate, going through a
// proxy
TEST_F(HttpNetworkTransactionTest, HTTPSBadCertificateViaProxy) {
  SessionDependencies session_deps(CreateFixedProxyService("myproxy:70"));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("https://www.google.com/");
  request.load_flags = 0;

  MockWrite proxy_writes[] = {
    MockWrite("CONNECT www.google.com:443 HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Proxy-Connection: keep-alive\r\n\r\n"),
  };

  MockRead proxy_reads[] = {
    MockRead("HTTP/1.0 200 Connected\r\n\r\n"),
    MockRead(false, OK)
  };

  MockWrite data_writes[] = {
    MockWrite("CONNECT www.google.com:443 HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Proxy-Connection: keep-alive\r\n\r\n"),
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n\r\n"),
  };

  MockRead data_reads[] = {
    MockRead("HTTP/1.0 200 Connected\r\n\r\n"),
    MockRead("HTTP/1.0 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 100\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider ssl_bad_certificate(
      proxy_reads, arraysize(proxy_reads),
      proxy_writes, arraysize(proxy_writes));
  StaticSocketDataProvider data(data_reads, arraysize(data_reads),
                                data_writes, arraysize(data_writes));
  SSLSocketDataProvider ssl_bad(true, ERR_CERT_AUTHORITY_INVALID);
  SSLSocketDataProvider ssl(true, OK);

  session_deps.socket_factory.AddSocketDataProvider(&ssl_bad_certificate);
  session_deps.socket_factory.AddSocketDataProvider(&data);
  session_deps.socket_factory.AddSSLSocketDataProvider(&ssl_bad);
  session_deps.socket_factory.AddSSLSocketDataProvider(&ssl);

  TestCompletionCallback callback;

  for (int i = 0; i < 2; i++) {
    session_deps.socket_factory.ResetNextMockIndexes();

    scoped_ptr<HttpTransaction> trans(
        new HttpNetworkTransaction(CreateSession(&session_deps)));

    int rv = trans->Start(&request, &callback, BoundNetLog());
    EXPECT_EQ(ERR_IO_PENDING, rv);

    rv = callback.WaitForResult();
    EXPECT_EQ(ERR_CERT_AUTHORITY_INVALID, rv);

    rv = trans->RestartIgnoringLastError(&callback);
    EXPECT_EQ(ERR_IO_PENDING, rv);

    rv = callback.WaitForResult();
    EXPECT_EQ(OK, rv);

    const HttpResponseInfo* response = trans->GetResponseInfo();

    EXPECT_FALSE(response == NULL);
    EXPECT_EQ(100, response->headers->GetContentLength());
  }
}

TEST_F(HttpNetworkTransactionTest, BuildRequest_UserAgent) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.extra_headers.SetHeader(HttpRequestHeaders::kUserAgent,
                                  "Chromium Ultra Awesome X Edition");

  MockWrite data_writes[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "User-Agent: Chromium Ultra Awesome X Edition\r\n\r\n"),
  };

  // Lastly, the server responds with the actual content.
  MockRead data_reads[] = {
    MockRead("HTTP/1.0 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 100\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data(data_reads, arraysize(data_reads),
                                data_writes, arraysize(data_writes));
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);
}

TEST_F(HttpNetworkTransactionTest, BuildRequest_Referer) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;
  request.referrer = GURL("http://the.previous.site.com/");

  MockWrite data_writes[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Referer: http://the.previous.site.com/\r\n\r\n"),
  };

  // Lastly, the server responds with the actual content.
  MockRead data_reads[] = {
    MockRead("HTTP/1.0 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 100\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data(data_reads, arraysize(data_reads),
                                data_writes, arraysize(data_writes));
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);
}

TEST_F(HttpNetworkTransactionTest, BuildRequest_PostContentLengthZero) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "POST";
  request.url = GURL("http://www.google.com/");

  MockWrite data_writes[] = {
    MockWrite("POST / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Content-Length: 0\r\n\r\n"),
  };

  // Lastly, the server responds with the actual content.
  MockRead data_reads[] = {
    MockRead("HTTP/1.0 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 100\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data(data_reads, arraysize(data_reads),
                                data_writes, arraysize(data_writes));
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);
}

TEST_F(HttpNetworkTransactionTest, BuildRequest_PutContentLengthZero) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "PUT";
  request.url = GURL("http://www.google.com/");

  MockWrite data_writes[] = {
    MockWrite("PUT / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Content-Length: 0\r\n\r\n"),
  };

  // Lastly, the server responds with the actual content.
  MockRead data_reads[] = {
    MockRead("HTTP/1.0 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 100\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data(data_reads, arraysize(data_reads),
                                data_writes, arraysize(data_writes));
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);
}

TEST_F(HttpNetworkTransactionTest, BuildRequest_HeadContentLengthZero) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "HEAD";
  request.url = GURL("http://www.google.com/");

  MockWrite data_writes[] = {
    MockWrite("HEAD / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Content-Length: 0\r\n\r\n"),
  };

  // Lastly, the server responds with the actual content.
  MockRead data_reads[] = {
    MockRead("HTTP/1.0 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 100\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data(data_reads, arraysize(data_reads),
                                data_writes, arraysize(data_writes));
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);
}

TEST_F(HttpNetworkTransactionTest, BuildRequest_CacheControlNoCache) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = LOAD_BYPASS_CACHE;

  MockWrite data_writes[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Pragma: no-cache\r\n"
              "Cache-Control: no-cache\r\n\r\n"),
  };

  // Lastly, the server responds with the actual content.
  MockRead data_reads[] = {
    MockRead("HTTP/1.0 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 100\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data(data_reads, arraysize(data_reads),
                                data_writes, arraysize(data_writes));
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);
}

TEST_F(HttpNetworkTransactionTest,
       BuildRequest_CacheControlValidateCache) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = LOAD_VALIDATE_CACHE;

  MockWrite data_writes[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Cache-Control: max-age=0\r\n\r\n"),
  };

  // Lastly, the server responds with the actual content.
  MockRead data_reads[] = {
    MockRead("HTTP/1.0 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 100\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data(data_reads, arraysize(data_reads),
                                data_writes, arraysize(data_writes));
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);
}

TEST_F(HttpNetworkTransactionTest, BuildRequest_ExtraHeaders) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.extra_headers.SetHeader("FooHeader", "Bar");

  MockWrite data_writes[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "FooHeader: Bar\r\n\r\n"),
  };

  // Lastly, the server responds with the actual content.
  MockRead data_reads[] = {
    MockRead("HTTP/1.0 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 100\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data(data_reads, arraysize(data_reads),
                                data_writes, arraysize(data_writes));
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);
}

TEST_F(HttpNetworkTransactionTest, BuildRequest_ExtraHeadersStripped) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.extra_headers.SetHeader("referer", "www.foo.com");
  request.extra_headers.SetHeader("hEllo", "Kitty");
  request.extra_headers.SetHeader("FoO", "bar");

  MockWrite data_writes[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "hEllo: Kitty\r\n"
              "FoO: bar\r\n\r\n"),
  };

  // Lastly, the server responds with the actual content.
  MockRead data_reads[] = {
    MockRead("HTTP/1.0 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 100\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data(data_reads, arraysize(data_reads),
                                data_writes, arraysize(data_writes));
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);
}

TEST_F(HttpNetworkTransactionTest, SOCKS4_HTTP_GET) {
  SessionDependencies session_deps(
      CreateFixedProxyService("socks4://myproxy:1080"));

  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;

  char write_buffer[] = { 0x04, 0x01, 0x00, 0x50, 127, 0, 0, 1, 0 };
  char read_buffer[] = { 0x00, 0x5A, 0x00, 0x00, 0, 0, 0, 0 };

  MockWrite data_writes[] = {
    MockWrite(true, write_buffer, arraysize(write_buffer)),
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n\r\n")
  };

  MockRead data_reads[] = {
    MockRead(true, read_buffer, arraysize(read_buffer)),
    MockRead("HTTP/1.0 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n\r\n"),
    MockRead("Payload"),
    MockRead(false, OK)
  };

  StaticSocketDataProvider data(data_reads, arraysize(data_reads),
                                data_writes, arraysize(data_writes));
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);

  std::string response_text;
  rv = ReadTransaction(trans.get(), &response_text);
  EXPECT_EQ(OK, rv);
  EXPECT_EQ("Payload", response_text);
}

TEST_F(HttpNetworkTransactionTest, SOCKS4_SSL_GET) {
  SessionDependencies session_deps(
      CreateFixedProxyService("socks4://myproxy:1080"));

  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("https://www.google.com/");
  request.load_flags = 0;

  unsigned char write_buffer[] = { 0x04, 0x01, 0x01, 0xBB, 127, 0, 0, 1, 0 };
  unsigned char read_buffer[] = { 0x00, 0x5A, 0x00, 0x00, 0, 0, 0, 0 };

  MockWrite data_writes[] = {
    MockWrite(true, reinterpret_cast<char*>(write_buffer),
              arraysize(write_buffer)),
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n\r\n")
  };

  MockRead data_reads[] = {
    MockWrite(true, reinterpret_cast<char*>(read_buffer),
              arraysize(read_buffer)),
    MockRead("HTTP/1.0 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n\r\n"),
    MockRead("Payload"),
    MockRead(false, OK)
  };

  StaticSocketDataProvider data(data_reads, arraysize(data_reads),
                                data_writes, arraysize(data_writes));
  session_deps.socket_factory.AddSocketDataProvider(&data);

  SSLSocketDataProvider ssl(true, OK);
  session_deps.socket_factory.AddSSLSocketDataProvider(&ssl);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);

  std::string response_text;
  rv = ReadTransaction(trans.get(), &response_text);
  EXPECT_EQ(OK, rv);
  EXPECT_EQ("Payload", response_text);
}

TEST_F(HttpNetworkTransactionTest, SOCKS5_HTTP_GET) {
  SessionDependencies session_deps(
      CreateFixedProxyService("socks5://myproxy:1080"));

  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;

  const char kSOCKS5GreetRequest[] = { 0x05, 0x01, 0x00 };
  const char kSOCKS5GreetResponse[] = { 0x05, 0x00 };
  const char kSOCKS5OkRequest[] = {
    0x05,  // Version
    0x01,  // Command (CONNECT)
    0x00,  // Reserved.
    0x03,  // Address type (DOMAINNAME).
    0x0E,  // Length of domain (14)
    // Domain string:
    'w', 'w', 'w', '.', 'g', 'o', 'o', 'g', 'l', 'e', '.', 'c', 'o', 'm',
    0x00, 0x50,  // 16-bit port (80)
  };
  const char kSOCKS5OkResponse[] =
      { 0x05, 0x00, 0x00, 0x01, 127, 0, 0, 1, 0x00, 0x50 };

  MockWrite data_writes[] = {
    MockWrite(true, kSOCKS5GreetRequest, arraysize(kSOCKS5GreetRequest)),
    MockWrite(true, kSOCKS5OkRequest, arraysize(kSOCKS5OkRequest)),
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n\r\n")
  };

  MockRead data_reads[] = {
    MockWrite(true, kSOCKS5GreetResponse, arraysize(kSOCKS5GreetResponse)),
    MockWrite(true, kSOCKS5OkResponse, arraysize(kSOCKS5OkResponse)),
    MockRead("HTTP/1.0 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n\r\n"),
    MockRead("Payload"),
    MockRead(false, OK)
  };

  StaticSocketDataProvider data(data_reads, arraysize(data_reads),
                                data_writes, arraysize(data_writes));
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);

  std::string response_text;
  rv = ReadTransaction(trans.get(), &response_text);
  EXPECT_EQ(OK, rv);
  EXPECT_EQ("Payload", response_text);
}

TEST_F(HttpNetworkTransactionTest, SOCKS5_SSL_GET) {
  SessionDependencies session_deps(
      CreateFixedProxyService("socks5://myproxy:1080"));

  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("https://www.google.com/");
  request.load_flags = 0;

  const char kSOCKS5GreetRequest[] = { 0x05, 0x01, 0x00 };
  const char kSOCKS5GreetResponse[] = { 0x05, 0x00 };
  const unsigned char kSOCKS5OkRequest[] = {
    0x05,  // Version
    0x01,  // Command (CONNECT)
    0x00,  // Reserved.
    0x03,  // Address type (DOMAINNAME).
    0x0E,  // Length of domain (14)
    // Domain string:
    'w', 'w', 'w', '.', 'g', 'o', 'o', 'g', 'l', 'e', '.', 'c', 'o', 'm',
    0x01, 0xBB,  // 16-bit port (443)
  };

  const char kSOCKS5OkResponse[] =
      { 0x05, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0x00, 0x00 };

  MockWrite data_writes[] = {
    MockWrite(true, kSOCKS5GreetRequest, arraysize(kSOCKS5GreetRequest)),
    MockWrite(true, reinterpret_cast<const char*>(kSOCKS5OkRequest),
              arraysize(kSOCKS5OkRequest)),
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n\r\n")
  };

  MockRead data_reads[] = {
    MockWrite(true, kSOCKS5GreetResponse, arraysize(kSOCKS5GreetResponse)),
    MockWrite(true, kSOCKS5OkResponse, arraysize(kSOCKS5OkResponse)),
    MockRead("HTTP/1.0 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n\r\n"),
    MockRead("Payload"),
    MockRead(false, OK)
  };

  StaticSocketDataProvider data(data_reads, arraysize(data_reads),
                                data_writes, arraysize(data_writes));
  session_deps.socket_factory.AddSocketDataProvider(&data);

  SSLSocketDataProvider ssl(true, OK);
  session_deps.socket_factory.AddSSLSocketDataProvider(&ssl);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);

  std::string response_text;
  rv = ReadTransaction(trans.get(), &response_text);
  EXPECT_EQ(OK, rv);
  EXPECT_EQ("Payload", response_text);
}

// Tests that for connection endpoints the group names are correctly set.

struct GroupNameTest {
  std::string proxy_server;
  std::string url;
  std::string expected_group_name;
  bool ssl;
};

scoped_refptr<HttpNetworkSession> SetupSessionForGroupNameTests(
    const std::string& proxy_server) {
  SessionDependencies session_deps(CreateFixedProxyService(proxy_server));
  scoped_refptr<HttpNetworkSession> session(CreateSession(&session_deps));

  HttpAlternateProtocols* alternate_protocols =
      session->mutable_alternate_protocols();
  alternate_protocols->SetAlternateProtocolFor(
      HostPortPair("host.with.alternate", 80), 443,
      HttpAlternateProtocols::NPN_SPDY_1);

  return session;
}

int GroupNameTransactionHelper(
    const std::string& url,
    const scoped_refptr<HttpNetworkSession>& session) {
  scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL(url);
  request.load_flags = 0;

  TestCompletionCallback callback;

  // We do not complete this request, the dtor will clean the transaction up.
  return trans->Start(&request, &callback, BoundNetLog());
}

TEST_F(HttpNetworkTransactionTest, GroupNameForDirectConnections) {
  const GroupNameTest tests[] = {
    {
      "",  // unused
      "http://www.google.com/direct",
      "www.google.com:80",
      false,
    },
    {
      "",  // unused
      "http://[2001:1418:13:1::25]/direct",
      "[2001:1418:13:1::25]:80",
      false,
    },

    // SSL Tests
    {
      "",  // unused
      "https://www.google.com/direct_ssl",
      "ssl/www.google.com:443",
      true,
    },
    {
      "",  // unused
      "https://[2001:1418:13:1::25]/direct",
      "ssl/[2001:1418:13:1::25]:443",
      true,
    },
    {
      "",  // unused
      "http://host.with.alternate/direct",
      "ssl/host.with.alternate:443",
      true,
    },
  };

  HttpNetworkTransaction::SetUseAlternateProtocols(true);

  for (size_t i = 0; i < ARRAYSIZE_UNSAFE(tests); ++i) {
    scoped_refptr<HttpNetworkSession> session(
        SetupSessionForGroupNameTests(tests[i].proxy_server));

    HttpNetworkSessionPeer peer(session);
    scoped_refptr<CaptureGroupNameTCPSocketPool> tcp_conn_pool(
        new CaptureGroupNameTCPSocketPool(session.get()));
    peer.SetTCPSocketPool(tcp_conn_pool);
    scoped_refptr<CaptureGroupNameSSLSocketPool> ssl_conn_pool(
        new CaptureGroupNameSSLSocketPool(session.get()));
    peer.SetSSLSocketPool(ssl_conn_pool);

    EXPECT_EQ(ERR_IO_PENDING,
              GroupNameTransactionHelper(tests[i].url, session));
    if (tests[i].ssl)
      EXPECT_EQ(tests[i].expected_group_name,
                ssl_conn_pool->last_group_name_received());
    else
      EXPECT_EQ(tests[i].expected_group_name,
                tcp_conn_pool->last_group_name_received());
  }

  HttpNetworkTransaction::SetUseAlternateProtocols(false);
}

TEST_F(HttpNetworkTransactionTest, GroupNameForHTTPProxyConnections) {
  const GroupNameTest tests[] = {
    {
      "http_proxy",
      "http://www.google.com/http_proxy_normal",
      "www.google.com:80",
      false,
    },

    // SSL Tests
    {
      "http_proxy",
      "https://www.google.com/http_connect_ssl",
      "ssl/www.google.com:443",
      true,
    },

    {
      "http_proxy",
      "http://host.with.alternate/direct",
      "ssl/host.with.alternate:443",
      true,
    },
  };

  HttpNetworkTransaction::SetUseAlternateProtocols(true);

  for (size_t i = 0; i < ARRAYSIZE_UNSAFE(tests); ++i) {
    scoped_refptr<HttpNetworkSession> session(
        SetupSessionForGroupNameTests(tests[i].proxy_server));

    HttpNetworkSessionPeer peer(session);

    HostPortPair proxy_host("http_proxy", 80);
    scoped_refptr<CaptureGroupNameHttpProxySocketPool> http_proxy_pool(
        new CaptureGroupNameHttpProxySocketPool(session.get()));
    peer.SetSocketPoolForHTTPProxy(proxy_host, http_proxy_pool);
    scoped_refptr<CaptureGroupNameSSLSocketPool> ssl_conn_pool(
        new CaptureGroupNameSSLSocketPool(session.get()));
    peer.SetSocketPoolForSSLWithProxy(proxy_host, ssl_conn_pool);

    EXPECT_EQ(ERR_IO_PENDING,
              GroupNameTransactionHelper(tests[i].url, session));
    if (tests[i].ssl)
      EXPECT_EQ(tests[i].expected_group_name,
                ssl_conn_pool->last_group_name_received());
    else
      EXPECT_EQ(tests[i].expected_group_name,
                http_proxy_pool->last_group_name_received());
  }

  HttpNetworkTransaction::SetUseAlternateProtocols(false);
}

TEST_F(HttpNetworkTransactionTest, GroupNameForSOCKSConnections) {
  const GroupNameTest tests[] = {
    {
      "socks4://socks_proxy:1080",
      "http://www.google.com/socks4_direct",
      "socks4/www.google.com:80",
      false,
    },
    {
      "socks5://socks_proxy:1080",
      "http://www.google.com/socks5_direct",
      "socks5/www.google.com:80",
      false,
    },

    // SSL Tests
    {
      "socks4://socks_proxy:1080",
      "https://www.google.com/socks4_ssl",
      "socks4/ssl/www.google.com:443",
      true,
    },
    {
      "socks5://socks_proxy:1080",
      "https://www.google.com/socks5_ssl",
      "socks5/ssl/www.google.com:443",
      true,
    },

    {
      "socks4://socks_proxy:1080",
      "http://host.with.alternate/direct",
      "socks4/ssl/host.with.alternate:443",
      true,
    },
  };

  HttpNetworkTransaction::SetUseAlternateProtocols(true);

  for (size_t i = 0; i < ARRAYSIZE_UNSAFE(tests); ++i) {
    scoped_refptr<HttpNetworkSession> session(
        SetupSessionForGroupNameTests(tests[i].proxy_server));
    HttpNetworkSessionPeer peer(session);

    HostPortPair proxy_host("socks_proxy", 1080);
    scoped_refptr<CaptureGroupNameSOCKSSocketPool> socks_conn_pool(
        new CaptureGroupNameSOCKSSocketPool(session.get()));
    peer.SetSocketPoolForSOCKSProxy(proxy_host, socks_conn_pool);
    scoped_refptr<CaptureGroupNameSSLSocketPool> ssl_conn_pool(
        new CaptureGroupNameSSLSocketPool(session.get()));
    peer.SetSocketPoolForSSLWithProxy(proxy_host, ssl_conn_pool);

    scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));

    EXPECT_EQ(ERR_IO_PENDING,
              GroupNameTransactionHelper(tests[i].url, session));
    if (tests[i].ssl)
      EXPECT_EQ(tests[i].expected_group_name,
                ssl_conn_pool->last_group_name_received());
    else
      EXPECT_EQ(tests[i].expected_group_name,
                socks_conn_pool->last_group_name_received());
  }

  HttpNetworkTransaction::SetUseAlternateProtocols(false);
}

TEST_F(HttpNetworkTransactionTest, ReconsiderProxyAfterFailedConnection) {
  SessionDependencies session_deps(
      CreateFixedProxyService("myproxy:70;foobar:80"));

  // This simulates failure resolving all hostnames; that means we will fail
  // connecting to both proxies (myproxy:70 and foobar:80).
  session_deps.host_resolver->rules()->AddSimulatedFailure("*");

  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(ERR_NAME_NOT_RESOLVED, rv);
}

// Host resolution observer used by
// HttpNetworkTransactionTest.ResolveMadeWithReferrer to check that host
// resovle requests are issued with a referrer of |expected_referrer|.
class ResolutionReferrerObserver : public HostResolver::Observer {
 public:
  explicit ResolutionReferrerObserver(const GURL& expected_referrer)
      : expected_referrer_(expected_referrer),
        called_start_with_referrer_(false),
        called_finish_with_referrer_(false) {
  }

  virtual void OnStartResolution(int id,
                                 const HostResolver::RequestInfo& info) {
    if (info.referrer() == expected_referrer_)
      called_start_with_referrer_ = true;
  }

  virtual void OnFinishResolutionWithStatus(
      int id, bool was_resolved, const HostResolver::RequestInfo& info ) {
    if (info.referrer() == expected_referrer_)
      called_finish_with_referrer_ = true;
  }

  virtual void OnCancelResolution(int id,
                                  const HostResolver::RequestInfo& info ) {
    FAIL() << "Should not be cancelling any requests!";
  }

  bool did_complete_with_expected_referrer() const {
    return called_start_with_referrer_ && called_finish_with_referrer_;
  }

 private:
  GURL expected_referrer_;
  bool called_start_with_referrer_;
  bool called_finish_with_referrer_;

  DISALLOW_COPY_AND_ASSIGN(ResolutionReferrerObserver);
};

// Make sure that when HostResolver::Resolve() is invoked, it passes through
// the "referrer". This is depended on by the DNS prefetch observer.
TEST_F(HttpNetworkTransactionTest, ResolveMadeWithReferrer) {
  GURL referrer = GURL("http://expected-referrer/");
  EXPECT_TRUE(referrer.is_valid());
  ResolutionReferrerObserver resolution_observer(referrer);

  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(
      CreateSession(&session_deps)));

  // Attach an observer to watch the host resolutions being made.
  session_deps.host_resolver->AddObserver(&resolution_observer);

  // Connect up a mock socket which will fail when reading.
  MockRead data_reads[] = {
    MockRead(false, ERR_FAILED),
  };
  StaticSocketDataProvider data(data_reads, arraysize(data_reads), NULL, 0);
  session_deps.socket_factory.AddSocketDataProvider(&data);

  // Issue a request, containing an HTTP referrer.
  HttpRequestInfo request;
  request.method = "GET";
  request.referrer = referrer;
  request.url = GURL("http://www.google.com/");

  // Run the request until it fails reading from the socket.
  TestCompletionCallback callback;
  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);
  rv = callback.WaitForResult();
  EXPECT_EQ(ERR_FAILED, rv);

  // Check that the host resolution observer saw |referrer|.
  EXPECT_TRUE(resolution_observer.did_complete_with_expected_referrer());
}

// Base test to make sure that when the load flags for a request specify to
// bypass the cache, the DNS cache is not used.
void BypassHostCacheOnRefreshHelper(int load_flags) {
  SessionDependencies session_deps;

  // Select a host resolver that does caching.
  session_deps.host_resolver = new MockCachingHostResolver;

  scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(
      CreateSession(&session_deps)));

  // Warm up the host cache so it has an entry for "www.google.com" (by doing
  // a synchronous lookup.)
  AddressList addrlist;
  int rv = session_deps.host_resolver->Resolve(
      HostResolver::RequestInfo("www.google.com", 80), &addrlist,
      NULL, NULL, BoundNetLog());
  EXPECT_EQ(OK, rv);

  // Verify that it was added to host cache, by doing a subsequent async lookup
  // and confirming it completes synchronously.
  TestCompletionCallback resolve_callback;
  rv = session_deps.host_resolver->Resolve(
      HostResolver::RequestInfo("www.google.com", 80), &addrlist,
      &resolve_callback, NULL, BoundNetLog());
  ASSERT_EQ(OK, rv);

  // Inject a failure the next time that "www.google.com" is resolved. This way
  // we can tell if the next lookup hit the cache, or the "network".
  // (cache --> success, "network" --> failure).
  session_deps.host_resolver->rules()->AddSimulatedFailure("www.google.com");

  // Connect up a mock socket which will fail with ERR_UNEXPECTED during the
  // first read -- this won't be reached as the host resolution will fail first.
  MockRead data_reads[] = { MockRead(false, ERR_UNEXPECTED) };
  StaticSocketDataProvider data(data_reads, arraysize(data_reads), NULL, 0);
  session_deps.socket_factory.AddSocketDataProvider(&data);

  // Issue a request, asking to bypass the cache(s).
  HttpRequestInfo request;
  request.method = "GET";
  request.load_flags = load_flags;
  request.url = GURL("http://www.google.com/");

  // Run the request.
  TestCompletionCallback callback;
  rv = trans->Start(&request, &callback, BoundNetLog());
  ASSERT_EQ(ERR_IO_PENDING, rv);
  rv = callback.WaitForResult();

  // If we bypassed the cache, we would have gotten a failure while resolving
  // "www.google.com".
  EXPECT_EQ(ERR_NAME_NOT_RESOLVED, rv);
}

// There are multiple load flags that should trigger the host cache bypass.
// Test each in isolation:
TEST_F(HttpNetworkTransactionTest, BypassHostCacheOnRefresh1) {
  BypassHostCacheOnRefreshHelper(LOAD_BYPASS_CACHE);
}

TEST_F(HttpNetworkTransactionTest, BypassHostCacheOnRefresh2) {
  BypassHostCacheOnRefreshHelper(LOAD_VALIDATE_CACHE);
}

TEST_F(HttpNetworkTransactionTest, BypassHostCacheOnRefresh3) {
  BypassHostCacheOnRefreshHelper(LOAD_DISABLE_CACHE);
}

// Make sure we can handle an error when writing the request.
TEST_F(HttpNetworkTransactionTest, RequestWriteError) {
  SessionDependencies session_deps;
  scoped_refptr<HttpNetworkSession> session = CreateSession(&session_deps);

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.foo.com/");
  request.load_flags = 0;

  MockWrite write_failure[] = {
    MockWrite(true, ERR_CONNECTION_RESET),
  };
  StaticSocketDataProvider data(NULL, 0,
                                write_failure, arraysize(write_failure));
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(ERR_CONNECTION_RESET, rv);
}

// Check that a connection closed after the start of the headers finishes ok.
TEST_F(HttpNetworkTransactionTest, ConnectionClosedAfterStartOfHeaders) {
  SessionDependencies session_deps;
  scoped_refptr<HttpNetworkSession> session = CreateSession(&session_deps);

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.foo.com/");
  request.load_flags = 0;

  MockRead data_reads[] = {
    MockRead("HTTP/1."),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data(data_reads, arraysize(data_reads), NULL, 0);
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_TRUE(response != NULL);

  EXPECT_TRUE(response->headers != NULL);
  EXPECT_EQ("HTTP/1.0 200 OK", response->headers->GetStatusLine());

  std::string response_data;
  rv = ReadTransaction(trans.get(), &response_data);
  EXPECT_EQ(OK, rv);
  EXPECT_EQ("", response_data);
}

// Make sure that a dropped connection while draining the body for auth
// restart does the right thing.
TEST_F(HttpNetworkTransactionTest, DrainResetOK) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;

  MockWrite data_writes1[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n\r\n"),
  };

  MockRead data_reads1[] = {
    MockRead("HTTP/1.1 401 Unauthorized\r\n"),
    MockRead("WWW-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 14\r\n\r\n"),
    MockRead("Unauth"),
    MockRead(true, ERR_CONNECTION_RESET),
  };

  StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                 data_writes1, arraysize(data_writes1));
  session_deps.socket_factory.AddSocketDataProvider(&data1);

  // After calling trans->RestartWithAuth(), this is the request we should
  // be issuing -- the final header line contains the credentials.
  MockWrite data_writes2[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Authorization: Basic Zm9vOmJhcg==\r\n\r\n"),
  };

  // Lastly, the server responds with the actual content.
  MockRead data_reads2[] = {
    MockRead("HTTP/1.1 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 100\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data2(data_reads2, arraysize(data_reads2),
                                 data_writes2, arraysize(data_writes2));
  session_deps.socket_factory.AddSocketDataProvider(&data2);

  TestCompletionCallback callback1;

  int rv = trans->Start(&request, &callback1, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback1.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);

  // The password prompt info should have been set in response->auth_challenge.
  EXPECT_FALSE(response->auth_challenge.get() == NULL);

  EXPECT_EQ(L"www.google.com:80", response->auth_challenge->host_and_port);
  EXPECT_EQ(L"MyRealm1", response->auth_challenge->realm);
  EXPECT_EQ(L"basic", response->auth_challenge->scheme);

  TestCompletionCallback callback2;

  rv = trans->RestartWithAuth(L"foo", L"bar", &callback2);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback2.WaitForResult();
  EXPECT_EQ(OK, rv);

  response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);
  EXPECT_TRUE(response->auth_challenge.get() == NULL);
  EXPECT_EQ(100, response->headers->GetContentLength());
}

// Test HTTPS connections going through a proxy that sends extra data.
TEST_F(HttpNetworkTransactionTest, HTTPSViaProxyWithExtraData) {
  SessionDependencies session_deps(CreateFixedProxyService("myproxy:70"));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("https://www.google.com/");
  request.load_flags = 0;

  MockRead proxy_reads[] = {
    MockRead("HTTP/1.0 200 Connected\r\n\r\nExtra data"),
    MockRead(false, OK)
  };

  StaticSocketDataProvider data(proxy_reads, arraysize(proxy_reads), NULL, 0);
  SSLSocketDataProvider ssl(true, OK);

  session_deps.socket_factory.AddSocketDataProvider(&data);
  session_deps.socket_factory.AddSSLSocketDataProvider(&ssl);

  TestCompletionCallback callback;

  session_deps.socket_factory.ResetNextMockIndexes();

  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(ERR_TUNNEL_CONNECTION_FAILED, rv);
}

TEST_F(HttpNetworkTransactionTest, LargeContentLengthThenClose) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;

  MockRead data_reads[] = {
    MockRead("HTTP/1.0 200 OK\r\nContent-Length:6719476739\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data(data_reads, arraysize(data_reads), NULL, 0);
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  EXPECT_EQ(OK, callback.WaitForResult());

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_TRUE(response != NULL);

  EXPECT_TRUE(response->headers != NULL);
  EXPECT_EQ("HTTP/1.0 200 OK", response->headers->GetStatusLine());

  std::string response_data;
  rv = ReadTransaction(trans.get(), &response_data);
  EXPECT_EQ(ERR_CONNECTION_CLOSED, rv);
}

TEST_F(HttpNetworkTransactionTest, UploadFileSmallerThanLength) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "POST";
  request.url = GURL("http://www.google.com/upload");
  request.upload_data = new UploadData;
  request.load_flags = 0;

  FilePath temp_file_path;
  ASSERT_TRUE(file_util::CreateTemporaryFile(&temp_file_path));
  const uint64 kFakeSize = 100000;  // file is actually blank

  std::vector<UploadData::Element> elements;
  UploadData::Element element;
  element.SetToFilePath(temp_file_path);
  element.SetContentLength(kFakeSize);
  elements.push_back(element);
  request.upload_data->set_elements(elements);
  EXPECT_EQ(kFakeSize, request.upload_data->GetContentLength());

  MockRead data_reads[] = {
    MockRead("HTTP/1.0 200 OK\r\n\r\n"),
    MockRead("hello world"),
    MockRead(false, OK),
  };
  StaticSocketDataProvider data(data_reads, arraysize(data_reads), NULL, 0);
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_TRUE(response != NULL);

  EXPECT_TRUE(response->headers != NULL);
  EXPECT_EQ("HTTP/1.0 200 OK", response->headers->GetStatusLine());

  std::string response_data;
  rv = ReadTransaction(trans.get(), &response_data);
  EXPECT_EQ(OK, rv);
  EXPECT_EQ("hello world", response_data);

  file_util::Delete(temp_file_path, false);
}

TEST_F(HttpNetworkTransactionTest, UploadUnreadableFile) {
  // If we try to upload an unreadable file, the network stack should report
  // the file size as zero and upload zero bytes for that file.
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  FilePath temp_file;
  ASSERT_TRUE(file_util::CreateTemporaryFile(&temp_file));
  std::string temp_file_content("Unreadable file.");
  ASSERT_TRUE(file_util::WriteFile(temp_file, temp_file_content.c_str(),
                                   temp_file_content.length()));
  ASSERT_TRUE(file_util::MakeFileUnreadable(temp_file));

  HttpRequestInfo request;
  request.method = "POST";
  request.url = GURL("http://www.google.com/upload");
  request.upload_data = new UploadData;
  request.load_flags = 0;

  std::vector<UploadData::Element> elements;
  UploadData::Element element;
  element.SetToFilePath(temp_file);
  elements.push_back(element);
  request.upload_data->set_elements(elements);

  MockRead data_reads[] = {
    MockRead("HTTP/1.0 200 OK\r\n\r\n"),
    MockRead(false, OK),
  };
  MockWrite data_writes[] = {
    MockWrite("POST /upload HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Content-Length: 0\r\n\r\n"),
    MockWrite(false, OK),
  };
  StaticSocketDataProvider data(data_reads, arraysize(data_reads), data_writes,
                                arraysize(data_writes));
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_TRUE(response != NULL);
  EXPECT_TRUE(response->headers != NULL);
  EXPECT_EQ("HTTP/1.0 200 OK", response->headers->GetStatusLine());

  file_util::Delete(temp_file, false);
}

TEST_F(HttpNetworkTransactionTest, UnreadableUploadFileAfterAuthRestart) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  FilePath temp_file;
  ASSERT_TRUE(file_util::CreateTemporaryFile(&temp_file));
  std::string temp_file_contents("Unreadable file.");
  std::string unreadable_contents(temp_file_contents.length(), '\0');
  ASSERT_TRUE(file_util::WriteFile(temp_file, temp_file_contents.c_str(),
                                   temp_file_contents.length()));

  HttpRequestInfo request;
  request.method = "POST";
  request.url = GURL("http://www.google.com/upload");
  request.upload_data = new UploadData;
  request.load_flags = 0;

  std::vector<UploadData::Element> elements;
  UploadData::Element element;
  element.SetToFilePath(temp_file);
  elements.push_back(element);
  request.upload_data->set_elements(elements);

  MockRead data_reads[] = {
    MockRead("HTTP/1.1 401 Unauthorized\r\n"),
    MockRead("WWW-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
    MockRead("Content-Length: 0\r\n\r\n"),  // No response body.

    MockRead("HTTP/1.1 200 OK\r\n"),
    MockRead("Content-Length: 0\r\n\r\n"),
    MockRead(false, OK),
  };
  MockWrite data_writes[] = {
    MockWrite("POST /upload HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Content-Length: 16\r\n\r\n"),
    MockWrite(false, temp_file_contents.c_str()),

    MockWrite("POST /upload HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Content-Length: 16\r\n"
              "Authorization: Basic Zm9vOmJhcg==\r\n\r\n"),
    MockWrite(false, unreadable_contents.c_str(), temp_file_contents.length()),
    MockWrite(false, OK),
  };
  StaticSocketDataProvider data(data_reads, arraysize(data_reads), data_writes,
                                arraysize(data_writes));
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback1;

  int rv = trans->Start(&request, &callback1, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback1.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_TRUE(response != NULL);
  EXPECT_TRUE(response->headers != NULL);
  EXPECT_EQ("HTTP/1.1 401 Unauthorized", response->headers->GetStatusLine());

  // The password prompt info should have been set in response->auth_challenge.
  EXPECT_TRUE(response->auth_challenge.get() != NULL);
  EXPECT_EQ(L"www.google.com:80", response->auth_challenge->host_and_port);
  EXPECT_EQ(L"MyRealm1", response->auth_challenge->realm);
  EXPECT_EQ(L"basic", response->auth_challenge->scheme);

  // Now make the file unreadable and try again.
  ASSERT_TRUE(file_util::MakeFileUnreadable(temp_file));

  TestCompletionCallback callback2;

  rv = trans->RestartWithAuth(L"foo", L"bar", &callback2);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback2.WaitForResult();
  EXPECT_EQ(OK, rv);

  response = trans->GetResponseInfo();
  EXPECT_TRUE(response != NULL);
  EXPECT_TRUE(response->headers != NULL);
  EXPECT_TRUE(response->auth_challenge.get() == NULL);
  EXPECT_EQ("HTTP/1.1 200 OK", response->headers->GetStatusLine());

  file_util::Delete(temp_file, false);
}

// Tests that changes to Auth realms are treated like auth rejections.
TEST_F(HttpNetworkTransactionTest, ChangeAuthRealms) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;

  // First transaction will request a resource and receive a Basic challenge
  // with realm="first_realm".
  MockWrite data_writes1[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "\r\n"),
  };
  MockRead data_reads1[] = {
    MockRead("HTTP/1.1 401 Unauthorized\r\n"
             "WWW-Authenticate: Basic realm=\"first_realm\"\r\n"
             "\r\n"),
  };

  // After calling trans->RestartWithAuth(), provide an Authentication header
  // for first_realm. The server will reject and provide a challenge with
  // second_realm.
  MockWrite data_writes2[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Authorization: Basic Zmlyc3Q6YmF6\r\n"
              "\r\n"),
  };
  MockRead data_reads2[] = {
    MockRead("HTTP/1.1 401 Unauthorized\r\n"
             "WWW-Authenticate: Basic realm=\"second_realm\"\r\n"
             "\r\n"),
  };

  // This again fails, and goes back to first_realm. Make sure that the
  // entry is removed from cache.
  MockWrite data_writes3[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Authorization: Basic c2Vjb25kOmZvdQ==\r\n"
              "\r\n"),
  };
  MockRead data_reads3[] = {
    MockRead("HTTP/1.1 401 Unauthorized\r\n"
             "WWW-Authenticate: Basic realm=\"first_realm\"\r\n"
             "\r\n"),
  };

  // Try one last time (with the correct password) and get the resource.
  MockWrite data_writes4[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Authorization: Basic Zmlyc3Q6YmFy\r\n"
              "\r\n"),
  };
  MockRead data_reads4[] = {
    MockRead("HTTP/1.1 200 OK\r\n"
             "Content-Type: text/html; charset=iso-8859-1\r\n"
             "Content-Length: 100\r\n"
             "\r\n"),
  };

  StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                 data_writes1, arraysize(data_writes1));
  StaticSocketDataProvider data2(data_reads2, arraysize(data_reads2),
                                 data_writes2, arraysize(data_writes2));
  StaticSocketDataProvider data3(data_reads3, arraysize(data_reads3),
                                 data_writes3, arraysize(data_writes3));
  StaticSocketDataProvider data4(data_reads4, arraysize(data_reads4),
                                 data_writes4, arraysize(data_writes4));
  session_deps.socket_factory.AddSocketDataProvider(&data1);
  session_deps.socket_factory.AddSocketDataProvider(&data2);
  session_deps.socket_factory.AddSocketDataProvider(&data3);
  session_deps.socket_factory.AddSocketDataProvider(&data4);

  TestCompletionCallback callback1;

  // Issue the first request with Authorize headers. There should be a
  // password prompt for first_realm waiting to be filled in after the
  // transaction completes.
  int rv = trans->Start(&request, &callback1, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);
  rv = callback1.WaitForResult();
  EXPECT_EQ(OK, rv);
  const HttpResponseInfo* response = trans->GetResponseInfo();
  ASSERT_FALSE(response == NULL);
  ASSERT_FALSE(response->auth_challenge.get() == NULL);
  EXPECT_EQ(L"www.google.com:80", response->auth_challenge->host_and_port);
  EXPECT_EQ(L"first_realm", response->auth_challenge->realm);
  EXPECT_EQ(L"basic", response->auth_challenge->scheme);

  // Issue the second request with an incorrect password. There should be a
  // password prompt for second_realm waiting to be filled in after the
  // transaction completes.
  TestCompletionCallback callback2;
  rv = trans->RestartWithAuth(L"first", L"baz", &callback2);
  EXPECT_EQ(ERR_IO_PENDING, rv);
  rv = callback2.WaitForResult();
  EXPECT_EQ(OK, rv);
  response = trans->GetResponseInfo();
  ASSERT_FALSE(response == NULL);
  ASSERT_FALSE(response->auth_challenge.get() == NULL);
  EXPECT_EQ(L"www.google.com:80", response->auth_challenge->host_and_port);
  EXPECT_EQ(L"second_realm", response->auth_challenge->realm);
  EXPECT_EQ(L"basic", response->auth_challenge->scheme);

  // Issue the third request with another incorrect password. There should be
  // a password prompt for first_realm waiting to be filled in. If the password
  // prompt is not present, it indicates that the HttpAuthCacheEntry for
  // first_realm was not correctly removed.
  TestCompletionCallback callback3;
  rv = trans->RestartWithAuth(L"second", L"fou", &callback3);
  EXPECT_EQ(ERR_IO_PENDING, rv);
  rv = callback3.WaitForResult();
  EXPECT_EQ(OK, rv);
  response = trans->GetResponseInfo();
  ASSERT_FALSE(response == NULL);
  ASSERT_FALSE(response->auth_challenge.get() == NULL);
  EXPECT_EQ(L"www.google.com:80", response->auth_challenge->host_and_port);
  EXPECT_EQ(L"first_realm", response->auth_challenge->realm);
  EXPECT_EQ(L"basic", response->auth_challenge->scheme);

  // Issue the fourth request with the correct password and username.
  TestCompletionCallback callback4;
  rv = trans->RestartWithAuth(L"first", L"bar", &callback4);
  EXPECT_EQ(ERR_IO_PENDING, rv);
  rv = callback4.WaitForResult();
  EXPECT_EQ(OK, rv);
  response = trans->GetResponseInfo();
  ASSERT_FALSE(response == NULL);
  EXPECT_TRUE(response->auth_challenge.get() == NULL);
}

TEST_F(HttpNetworkTransactionTest, HonorAlternateProtocolHeader) {
  HttpNetworkTransaction::SetNextProtos("needs_to_be_set_for_this_test");
  HttpNetworkTransaction::SetUseAlternateProtocols(true);

  SessionDependencies session_deps;

  MockRead data_reads[] = {
    MockRead("HTTP/1.1 200 OK\r\n"),
    MockRead("Alternate-Protocol: 443:npn-spdy/1\r\n\r\n"),
    MockRead("hello world"),
    MockRead(false, OK),
  };

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;

  StaticSocketDataProvider data(data_reads, arraysize(data_reads), NULL, 0);

  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  scoped_refptr<HttpNetworkSession> session(CreateSession(&session_deps));
  scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);

  HostPortPair http_host_port_pair;
  http_host_port_pair.host = "www.google.com";
  http_host_port_pair.port = 80;
  const HttpAlternateProtocols& alternate_protocols =
      session->alternate_protocols();
  EXPECT_FALSE(
      alternate_protocols.HasAlternateProtocolFor(http_host_port_pair));

  EXPECT_EQ(OK, callback.WaitForResult());

  const HttpResponseInfo* response = trans->GetResponseInfo();
  ASSERT_TRUE(response != NULL);
  ASSERT_TRUE(response->headers != NULL);
  EXPECT_EQ("HTTP/1.1 200 OK", response->headers->GetStatusLine());
  EXPECT_FALSE(response->was_fetched_via_spdy);
  EXPECT_FALSE(response->was_npn_negotiated);
  EXPECT_FALSE(response->was_alternate_protocol_available);

  std::string response_data;
  ASSERT_EQ(OK, ReadTransaction(trans.get(), &response_data));
  EXPECT_EQ("hello world", response_data);

  ASSERT_TRUE(alternate_protocols.HasAlternateProtocolFor(http_host_port_pair));
  const HttpAlternateProtocols::PortProtocolPair alternate =
      alternate_protocols.GetAlternateProtocolFor(http_host_port_pair);
  HttpAlternateProtocols::PortProtocolPair expected_alternate;
  expected_alternate.port = 443;
  expected_alternate.protocol = HttpAlternateProtocols::NPN_SPDY_1;
  EXPECT_TRUE(expected_alternate.Equals(alternate));

  HttpNetworkTransaction::SetUseAlternateProtocols(false);
  HttpNetworkTransaction::SetNextProtos("");
}

TEST_F(HttpNetworkTransactionTest, MarkBrokenAlternateProtocol) {
  HttpNetworkTransaction::SetUseAlternateProtocols(true);
  SessionDependencies session_deps;

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;

  MockConnect mock_connect(true, ERR_CONNECTION_REFUSED);
  StaticSocketDataProvider first_data;
  first_data.set_connect_data(mock_connect);
  session_deps.socket_factory.AddSocketDataProvider(&first_data);

  MockRead data_reads[] = {
    MockRead("HTTP/1.1 200 OK\r\n\r\n"),
    MockRead("hello world"),
    MockRead(true, OK),
  };
  StaticSocketDataProvider second_data(
      data_reads, arraysize(data_reads), NULL, 0);
  session_deps.socket_factory.AddSocketDataProvider(&second_data);

  // TODO(willchan): Delete this extra data provider.  It's necessary due to a
  // ClientSocketPoolBaseHelper bug that starts up too many ConnectJobs:
  // http://crbug.com/37454.
  session_deps.socket_factory.AddSocketDataProvider(&second_data);

  TestCompletionCallback callback;

  scoped_refptr<HttpNetworkSession> session(CreateSession(&session_deps));

  HostPortPair http_host_port_pair;
  http_host_port_pair.host = "www.google.com";
  http_host_port_pair.port = 80;
  HttpAlternateProtocols* alternate_protocols =
      session->mutable_alternate_protocols();
  alternate_protocols->SetAlternateProtocolFor(
      http_host_port_pair, 1234 /* port is ignored by MockConnect anyway */,
      HttpAlternateProtocols::NPN_SPDY_1);

  scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);
  EXPECT_EQ(OK, callback.WaitForResult());

  const HttpResponseInfo* response = trans->GetResponseInfo();
  ASSERT_TRUE(response != NULL);
  ASSERT_TRUE(response->headers != NULL);
  EXPECT_EQ("HTTP/1.1 200 OK", response->headers->GetStatusLine());

  std::string response_data;
  ASSERT_EQ(OK, ReadTransaction(trans.get(), &response_data));
  EXPECT_EQ("hello world", response_data);

  ASSERT_TRUE(
      alternate_protocols->HasAlternateProtocolFor(http_host_port_pair));
  const HttpAlternateProtocols::PortProtocolPair alternate =
      alternate_protocols->GetAlternateProtocolFor(http_host_port_pair);
  EXPECT_EQ(HttpAlternateProtocols::BROKEN, alternate.protocol);
  HttpNetworkTransaction::SetUseAlternateProtocols(false);
}

// TODO(willchan): Redo this test to use TLS/NPN=>SPDY.  Currently, the code
// says that it does SPDY, but it just does the TLS handshake, but the NPN
// response does not indicate SPDY, so we just do standard HTTPS over the port.
// We should add code such that we don't fallback to HTTPS, but fallback to HTTP
// on the original port.
//  TEST_F(HttpNetworkTransactionTest, UseAlternateProtocol) {
//    SessionDependencies session_deps;
//
//    HttpRequestInfo request;
//    request.method = "GET";
//    request.url = GURL("http://www.google.com/");
//    request.load_flags = 0;
//
//    MockRead data_reads[] = {
//      MockRead("HTTP/1.1 200 OK\r\n\r\n"),
//      MockRead("hello world"),
//      MockRead(true, OK),
//    };
//    StaticSocketDataProvider data(data_reads, arraysize(data_reads), NULL, 0);
//    session_deps.socket_factory.AddSocketDataProvider(&data);
//
//    SSLSocketDataProvider ssl(true, OK);
//    session_deps.socket_factory.AddSSLSocketDataProvider(&ssl);
//
//    TestCompletionCallback callback;
//
//    scoped_refptr<HttpNetworkSession> session(CreateSession(&session_deps));
//
//    HostPortPair http_host_port_pair;
//    http_host_port_pair.host = "www.google.com";
//    http_host_port_pair.port = 80;
//    HttpAlternateProtocols* alternate_protocols =
//        session->mutable_alternate_protocols();
//    alternate_protocols->SetAlternateProtocolFor(
//        http_host_port_pair, 1234 /* port is ignored */,
//        HttpAlternateProtocols::NPN_SPDY_1);
//
//    scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));
//
//    int rv = trans->Start(&request, &callback, BoundNetLog());
//    EXPECT_EQ(ERR_IO_PENDING, rv);
//    EXPECT_EQ(OK, callback.WaitForResult());
//
//    const HttpResponseInfo* response = trans->GetResponseInfo();
//    ASSERT_TRUE(response != NULL);
//    ASSERT_TRUE(response->headers != NULL);
//    EXPECT_EQ("HTTP/1.1 200 OK", response->headers->GetStatusLine());
//
//    std::string response_data;
//    ASSERT_EQ(OK, ReadTransaction(trans.get(), &response_data));
//    EXPECT_EQ("hello world", response_data);
//  }

TEST_F(HttpNetworkTransactionTest, FailNpnSpdyAndFallback) {
  HttpNetworkTransaction::SetUseAlternateProtocols(true);
  HttpNetworkTransaction::SetNextProtos(
      "\x08http/1.1\x07http1.1\x06spdy/1\x04spdy");
  SessionDependencies session_deps;

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;

  StaticSocketDataProvider first_tcp_connect;
  session_deps.socket_factory.AddSocketDataProvider(&first_tcp_connect);

  SSLSocketDataProvider ssl(true, OK);
  session_deps.socket_factory.AddSSLSocketDataProvider(&ssl);

  MockRead data_reads[] = {
    MockRead("HTTP/1.1 200 OK\r\n\r\n"),
    MockRead("hello world"),
    MockRead(true, OK),
  };
  StaticSocketDataProvider fallback_data(
      data_reads, arraysize(data_reads), NULL, 0);
  session_deps.socket_factory.AddSocketDataProvider(&fallback_data);

  TestCompletionCallback callback;

  scoped_refptr<HttpNetworkSession> session(CreateSession(&session_deps));

  HostPortPair http_host_port_pair;
  http_host_port_pair.host = "www.google.com";
  http_host_port_pair.port = 80;
  HttpAlternateProtocols* alternate_protocols =
      session->mutable_alternate_protocols();
  alternate_protocols->SetAlternateProtocolFor(
      http_host_port_pair, 1234 /* port is ignored */,
      HttpAlternateProtocols::NPN_SPDY_1);

  scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);
  EXPECT_EQ(OK, callback.WaitForResult());

  const HttpResponseInfo* response = trans->GetResponseInfo();
  ASSERT_TRUE(response != NULL);
  ASSERT_TRUE(response->headers != NULL);
  EXPECT_EQ("HTTP/1.1 200 OK", response->headers->GetStatusLine());

  std::string response_data;
  ASSERT_EQ(OK, ReadTransaction(trans.get(), &response_data));
  EXPECT_EQ("hello world", response_data);
  HttpNetworkTransaction::SetNextProtos("");
  HttpNetworkTransaction::SetUseAlternateProtocols(false);
}

TEST_F(HttpNetworkTransactionTest, UseAlternateProtocolForNpnSpdy) {
  HttpNetworkTransaction::SetUseAlternateProtocols(true);
  HttpNetworkTransaction::SetNextProtos(
      "\x08http/1.1\x07http1.1\x06spdy/1\x04spdy");
  SessionDependencies session_deps;

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;

  MockRead data_reads[] = {
    MockRead("HTTP/1.1 200 OK\r\n"),
    MockRead("Alternate-Protocol: 443:npn-spdy/1\r\n\r\n"),
    MockRead("hello world"),
    MockRead(true, OK),
  };

  StaticSocketDataProvider first_transaction(
      data_reads, arraysize(data_reads), NULL, 0);
  session_deps.socket_factory.AddSocketDataProvider(&first_transaction);

  SSLSocketDataProvider ssl(true, OK);
  ssl.next_proto_status = SSLClientSocket::kNextProtoNegotiated;
  ssl.next_proto = "spdy/1";
  ssl.was_npn_negotiated = true;
  session_deps.socket_factory.AddSSLSocketDataProvider(&ssl);

  scoped_ptr<spdy::SpdyFrame> req(ConstructSpdyGet(NULL, 0));
  MockWrite spdy_writes[] = { CreateMockWrite(*req) };

  scoped_ptr<spdy::SpdyFrame> resp(ConstructSpdyGetSynReply(NULL, 0));
  scoped_ptr<spdy::SpdyFrame> data(ConstructSpdyBodyFrame());
  MockRead spdy_reads[] = {
    CreateMockRead(*resp),
    CreateMockRead(*data),
    MockRead(true, 0, 0),
  };

  scoped_refptr<DelayedSocketData> spdy_data(
      new DelayedSocketData(
          1,  // wait for one write to finish before reading.
          spdy_reads, arraysize(spdy_reads),
          spdy_writes, arraysize(spdy_writes)));
  session_deps.socket_factory.AddSocketDataProvider(spdy_data);

  TestCompletionCallback callback;

  scoped_refptr<HttpNetworkSession> session(CreateSession(&session_deps));
  scoped_ptr<HttpNetworkTransaction> trans(new HttpNetworkTransaction(session));

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);
  EXPECT_EQ(OK, callback.WaitForResult());

  const HttpResponseInfo* response = trans->GetResponseInfo();
  ASSERT_TRUE(response != NULL);
  ASSERT_TRUE(response->headers != NULL);
  EXPECT_EQ("HTTP/1.1 200 OK", response->headers->GetStatusLine());

  std::string response_data;
  ASSERT_EQ(OK, ReadTransaction(trans.get(), &response_data));
  EXPECT_EQ("hello world", response_data);

  trans.reset(new HttpNetworkTransaction(session));

  rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);
  EXPECT_EQ(OK, callback.WaitForResult());

  response = trans->GetResponseInfo();
  ASSERT_TRUE(response != NULL);
  ASSERT_TRUE(response->headers != NULL);
  EXPECT_EQ("HTTP/1.1 200 OK", response->headers->GetStatusLine());
  EXPECT_TRUE(response->was_fetched_via_spdy);
  EXPECT_TRUE(response->was_npn_negotiated);
  EXPECT_TRUE(response->was_alternate_protocol_available);

  ASSERT_EQ(OK, ReadTransaction(trans.get(), &response_data));
  EXPECT_EQ("hello!", response_data);

  HttpNetworkTransaction::SetNextProtos("");
  HttpNetworkTransaction::SetUseAlternateProtocols(false);
}

class CapturingProxyResolver : public ProxyResolver {
 public:
  CapturingProxyResolver() : ProxyResolver(false /* expects_pac_bytes */) {}
  virtual ~CapturingProxyResolver() {}

  virtual int GetProxyForURL(const GURL& url,
                             ProxyInfo* results,
                             CompletionCallback* callback,
                             RequestHandle* request,
                             const BoundNetLog& net_log) {
    ProxyServer proxy_server(
        ProxyServer::SCHEME_HTTP, "myproxy", 80);
    results->UseProxyServer(proxy_server);
    resolved_.push_back(url);
    return OK;
  }

  virtual void CancelRequest(RequestHandle request) {
    NOTREACHED();
  }

  const std::vector<GURL>& resolved() const { return resolved_; }

 private:
  virtual int SetPacScript(const GURL& /*pac_url*/,
                           const string16& /*pac_script*/,
                           CompletionCallback* /*callback*/) {
    return OK;
  }

  std::vector<GURL> resolved_;

  DISALLOW_COPY_AND_ASSIGN(CapturingProxyResolver);
};

TEST_F(HttpNetworkTransactionTest, UseAlternateProtocolForTunneledNpnSpdy) {
  HttpNetworkTransaction::SetUseAlternateProtocols(true);
  HttpNetworkTransaction::SetNextProtos(
          "\x08http/1.1\x07http1.1\x06spdy/1\x04spdy");

  ProxyConfig proxy_config;
  proxy_config.set_auto_detect(true);
  proxy_config.set_pac_url(GURL("http://fooproxyurl"));

  CapturingProxyResolver* capturing_proxy_resolver =
      new CapturingProxyResolver();
  SessionDependencies session_deps(new ProxyService(
      new ProxyConfigServiceFixed(proxy_config), capturing_proxy_resolver,
      NULL));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;

  MockRead data_reads[] = {
    MockRead("HTTP/1.1 200 OK\r\n"),
    MockRead("Alternate-Protocol: 443:npn-spdy/1\r\n\r\n"),
    MockRead("hello world"),
    MockRead(true, OK),
  };

  StaticSocketDataProvider first_transaction(
      data_reads, arraysize(data_reads), NULL, 0);
  session_deps.socket_factory.AddSocketDataProvider(&first_transaction);

  SSLSocketDataProvider ssl(true, OK);
  ssl.next_proto_status = SSLClientSocket::kNextProtoNegotiated;
  ssl.next_proto = "spdy/1";
  ssl.was_npn_negotiated = true;
  session_deps.socket_factory.AddSSLSocketDataProvider(&ssl);

  scoped_ptr<spdy::SpdyFrame> req(ConstructSpdyGet(NULL, 0));
  MockWrite spdy_writes[] = {
    MockWrite("CONNECT www.google.com:443 HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Proxy-Connection: keep-alive\r\n\r\n"),  // 0
    CreateMockWrite(*req)  // 3
  };

  const char kCONNECTResponse[] = "HTTP/1.1 200 Connected\r\n\r\n";

  scoped_ptr<spdy::SpdyFrame> resp(ConstructSpdyGetSynReply(NULL, 0));
  scoped_ptr<spdy::SpdyFrame> data(ConstructSpdyBodyFrame());
  MockRead spdy_reads[] = {
    MockRead(true, kCONNECTResponse, arraysize(kCONNECTResponse) - 1, 1),  // 1
    CreateMockRead(*resp.get(), 4),  // 2, 4
    CreateMockRead(*data.get(), 4),  // 5
    MockRead(true, 0, 0, 4),  // 6
  };

  scoped_refptr<OrderedSocketData> spdy_data(
      new OrderedSocketData(
          spdy_reads, arraysize(spdy_reads),
          spdy_writes, arraysize(spdy_writes)));
  session_deps.socket_factory.AddSocketDataProvider(spdy_data);

  TestCompletionCallback callback;

  scoped_refptr<HttpNetworkSession> session(CreateSession(&session_deps));
  scoped_ptr<HttpNetworkTransaction> trans(new HttpNetworkTransaction(session));

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);
  EXPECT_EQ(OK, callback.WaitForResult());

  const HttpResponseInfo* response = trans->GetResponseInfo();
  ASSERT_TRUE(response != NULL);
  ASSERT_TRUE(response->headers != NULL);
  EXPECT_EQ("HTTP/1.1 200 OK", response->headers->GetStatusLine());
  EXPECT_FALSE(response->was_fetched_via_spdy);
  EXPECT_FALSE(response->was_npn_negotiated);

  std::string response_data;
  ASSERT_EQ(OK, ReadTransaction(trans.get(), &response_data));
  EXPECT_EQ("hello world", response_data);

  trans.reset(new HttpNetworkTransaction(session));

  rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);
  EXPECT_EQ(OK, callback.WaitForResult());

  response = trans->GetResponseInfo();
  ASSERT_TRUE(response != NULL);
  ASSERT_TRUE(response->headers != NULL);
  EXPECT_EQ("HTTP/1.1 200 OK", response->headers->GetStatusLine());
  EXPECT_TRUE(response->was_fetched_via_spdy);
  EXPECT_TRUE(response->was_npn_negotiated);

  ASSERT_EQ(OK, ReadTransaction(trans.get(), &response_data));
  EXPECT_EQ("hello!", response_data);
  ASSERT_EQ(2u, capturing_proxy_resolver->resolved().size());
  EXPECT_EQ("http://www.google.com/",
            capturing_proxy_resolver->resolved()[0].spec());
  EXPECT_EQ("https://www.google.com/",
            capturing_proxy_resolver->resolved()[1].spec());

  HttpNetworkTransaction::SetNextProtos("");
  HttpNetworkTransaction::SetUseAlternateProtocols(false);
}

TEST_F(HttpNetworkTransactionTest,
       UseAlternateProtocolForNpnSpdyWithExistingSpdySession) {
  HttpNetworkTransaction::SetUseAlternateProtocols(true);
  HttpNetworkTransaction::SetNextProtos(
      "\x08http/1.1\x07http1.1\x06spdy/1\x04spdy");
  SessionDependencies session_deps;

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;

  MockRead data_reads[] = {
    MockRead("HTTP/1.1 200 OK\r\n"),
    MockRead("Alternate-Protocol: 443:npn-spdy/1\r\n\r\n"),
    MockRead("hello world"),
    MockRead(true, OK),
  };

  StaticSocketDataProvider first_transaction(
      data_reads, arraysize(data_reads), NULL, 0);
  session_deps.socket_factory.AddSocketDataProvider(&first_transaction);

  SSLSocketDataProvider ssl(true, OK);
  ssl.next_proto_status = SSLClientSocket::kNextProtoNegotiated;
  ssl.next_proto = "spdy/1";
  ssl.was_npn_negotiated = true;
  session_deps.socket_factory.AddSSLSocketDataProvider(&ssl);
  // Make sure we use ssl for spdy here.
  SpdySession::SetSSLMode(true);

  scoped_ptr<spdy::SpdyFrame> req(ConstructSpdyGet(NULL, 0));
  MockWrite spdy_writes[] = { CreateMockWrite(*req) };

  scoped_ptr<spdy::SpdyFrame> resp(ConstructSpdyGetSynReply(NULL, 0));
  scoped_ptr<spdy::SpdyFrame> data(ConstructSpdyBodyFrame());
  MockRead spdy_reads[] = {
    CreateMockRead(*resp),
    CreateMockRead(*data),
    MockRead(true, 0, 0),
  };

  scoped_refptr<DelayedSocketData> spdy_data(
      new DelayedSocketData(
          1,  // wait for one write to finish before reading.
          spdy_reads, arraysize(spdy_reads),
          spdy_writes, arraysize(spdy_writes)));
  session_deps.socket_factory.AddSocketDataProvider(spdy_data);

  TestCompletionCallback callback;

  scoped_refptr<HttpNetworkSession> session(CreateSession(&session_deps));

  scoped_ptr<HttpNetworkTransaction> trans(new HttpNetworkTransaction(session));

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);
  EXPECT_EQ(OK, callback.WaitForResult());

  const HttpResponseInfo* response = trans->GetResponseInfo();
  ASSERT_TRUE(response != NULL);
  ASSERT_TRUE(response->headers != NULL);
  EXPECT_EQ("HTTP/1.1 200 OK", response->headers->GetStatusLine());

  std::string response_data;
  ASSERT_EQ(OK, ReadTransaction(trans.get(), &response_data));
  EXPECT_EQ("hello world", response_data);

  // Set up an initial SpdySession in the pool to reuse.
  scoped_refptr<SpdySession> spdy_session =
      session->spdy_session_pool()->Get(HostPortPair("www.google.com", 443),
                                        session, BoundNetLog());
  scoped_refptr<TCPSocketParams> tcp_params =
      new TCPSocketParams("www.google.com", 443, MEDIUM, GURL(), false);
  spdy_session->Connect("www.google.com:443", tcp_params, MEDIUM);
  trans.reset(new HttpNetworkTransaction(session));

  rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);
  EXPECT_EQ(OK, callback.WaitForResult());

  response = trans->GetResponseInfo();
  ASSERT_TRUE(response != NULL);
  ASSERT_TRUE(response->headers != NULL);
  EXPECT_EQ("HTTP/1.1 200 OK", response->headers->GetStatusLine());
  EXPECT_TRUE(response->was_fetched_via_spdy);
  EXPECT_TRUE(response->was_npn_negotiated);
  EXPECT_TRUE(response->was_alternate_protocol_available);

  ASSERT_EQ(OK, ReadTransaction(trans.get(), &response_data));
  EXPECT_EQ("hello!", response_data);

  HttpNetworkTransaction::SetNextProtos("");
  HttpNetworkTransaction::SetUseAlternateProtocols(false);
}

// GenerateAuthToken is a mighty big test.
// It tests all permutation of GenerateAuthToken behavior:
//   - Synchronous and Asynchronous completion.
//   - OK or error on completion.
//   - Direct connection, non-authenticating proxy, and authenticating proxy.
//   - HTTP or HTTPS backend (to include proxy tunneling).
//   - Non-authenticating and authenticating backend.
//
// In all, there are 44 reasonable permuations (for example, if there are
// problems generating an auth token for an authenticating proxy, we don't
// need to test all permutations of the backend server).
//
// The test proceeds by going over each of the configuration cases, and
// potentially running up to three rounds in each of the tests. The TestConfig
// specifies both the configuration for the test as well as the expectations
// for the results.
TEST_F(HttpNetworkTransactionTest, GenerateAuthToken) {
  const char* kServer = "http://www.example.com";
  const char* kSecureServer = "https://www.example.com";
  const char* kProxy = "myproxy:70";
  const int kAuthErr = ERR_INVALID_AUTH_CREDENTIALS;

  enum AuthTiming {
    AUTH_NONE,
    AUTH_SYNC,
    AUTH_ASYNC,
  };

  const MockWrite kGet(
      "GET / HTTP/1.1\r\n"
      "Host: www.example.com\r\n"
      "Connection: keep-alive\r\n\r\n");
  const MockWrite kGetProxy(
      "GET http://www.example.com/ HTTP/1.1\r\n"
      "Host: www.example.com\r\n"
      "Proxy-Connection: keep-alive\r\n\r\n");
  const MockWrite kGetAuth(
      "GET / HTTP/1.1\r\n"
      "Host: www.example.com\r\n"
      "Connection: keep-alive\r\n"
      "Authorization: auth_token\r\n\r\n");
  const MockWrite kGetProxyAuth(
      "GET http://www.example.com/ HTTP/1.1\r\n"
      "Host: www.example.com\r\n"
      "Proxy-Connection: keep-alive\r\n"
      "Proxy-Authorization: auth_token\r\n\r\n");
  const MockWrite kGetAuthThroughProxy(
      "GET http://www.example.com/ HTTP/1.1\r\n"
      "Host: www.example.com\r\n"
      "Proxy-Connection: keep-alive\r\n"
      "Authorization: auth_token\r\n\r\n");
  const MockWrite kGetAuthWithProxyAuth(
      "GET http://www.example.com/ HTTP/1.1\r\n"
      "Host: www.example.com\r\n"
      "Proxy-Connection: keep-alive\r\n"
      "Proxy-Authorization: auth_token\r\n"
      "Authorization: auth_token\r\n\r\n");
  const MockWrite kConnect(
      "CONNECT www.example.com:443 HTTP/1.1\r\n"
      "Host: www.example.com\r\n"
      "Proxy-Connection: keep-alive\r\n\r\n");
  const MockWrite kConnectProxyAuth(
      "CONNECT www.example.com:443 HTTP/1.1\r\n"
      "Host: www.example.com\r\n"
      "Proxy-Connection: keep-alive\r\n"
      "Proxy-Authorization: auth_token\r\n\r\n");

  const MockRead kSuccess(
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/html; charset=iso-8859-1\r\n"
      "Content-Length: 3\r\n\r\n"
      "Yes");
  const MockRead kFailure(
      "Should not be called.");
  const MockRead kServerChallenge(
      "HTTP/1.1 401 Unauthorized\r\n"
      "WWW-Authenticate: Mock realm=server\r\n"
      "Content-Type: text/html; charset=iso-8859-1\r\n"
      "Content-Length: 14\r\n\r\n"
      "Unauthorized\r\n");
  const MockRead kProxyChallenge(
      "HTTP/1.1 407 Unauthorized\r\n"
      "Proxy-Authenticate: Mock realm=proxy\r\n"
      "Proxy-Connection: close\r\n"
      "Content-Type: text/html; charset=iso-8859-1\r\n"
      "Content-Length: 14\r\n\r\n"
      "Unauthorized\r\n");
  const MockRead kProxyConnected(
      "HTTP/1.1 200 Connection Established\r\n\r\n");

  // NOTE(cbentzel): I wanted TestReadWriteRound to be a simple struct with
  // no constructors, but the C++ compiler on Windows warns about
  // unspecified data in compound literals. So, moved to using constructors,
  // and TestRound's created with the default constructor should not be used.
  struct TestRound {
    TestRound()
        : expected_rv(ERR_UNEXPECTED),
          extra_write(NULL),
          extra_read(NULL) {
    }
    TestRound(const MockWrite& write_arg, const MockRead& read_arg,
              int expected_rv_arg)
        : write(write_arg),
          read(read_arg),
          expected_rv(expected_rv_arg),
          extra_write(NULL),
          extra_read(NULL) {
    }
    TestRound(const MockWrite& write_arg, const MockRead& read_arg,
              int expected_rv_arg, const MockWrite* extra_write_arg,
              const MockWrite* extra_read_arg)
        : write(write_arg),
          read(read_arg),
          expected_rv(expected_rv_arg),
          extra_write(extra_write_arg),
          extra_read(extra_read_arg) {
    }
    MockWrite write;
    MockRead read;
    int expected_rv;
    const MockWrite* extra_write;
    const MockRead* extra_read;
  };

  static const int kNoSSL = 500;

  struct TestConfig {
    const char* proxy_url;
    AuthTiming proxy_auth_timing;
    int proxy_auth_rv;
    const char* server_url;
    AuthTiming server_auth_timing;
    int server_auth_rv;
    int num_auth_rounds;
    int first_ssl_round;
    TestRound rounds[3];
  } test_configs[] = {
    // Non-authenticating HTTP server with a direct connection.
    { NULL, AUTH_NONE, OK, kServer, AUTH_NONE, OK, 1, kNoSSL,
      { TestRound(kGet, kSuccess, OK)}},
    // Authenticating HTTP server with a direct connection.
    { NULL, AUTH_NONE, OK, kServer, AUTH_SYNC, OK, 2, kNoSSL,
      { TestRound(kGet, kServerChallenge, OK),
        TestRound(kGetAuth, kSuccess, OK)}},
    { NULL, AUTH_NONE, OK, kServer, AUTH_SYNC, kAuthErr, 2, kNoSSL,
      { TestRound(kGet, kServerChallenge, OK),
        TestRound(kGetAuth, kFailure, kAuthErr)}},
    { NULL, AUTH_NONE, OK, kServer, AUTH_ASYNC, OK, 2, kNoSSL,
      { TestRound(kGet, kServerChallenge, OK),
        TestRound(kGetAuth, kSuccess, OK)}},
    { NULL, AUTH_NONE, OK, kServer, AUTH_ASYNC, kAuthErr, 2, kNoSSL,
      { TestRound(kGet, kServerChallenge, OK),
        TestRound(kGetAuth, kFailure, kAuthErr)}},
    // Non-authenticating HTTP server through a non-authenticating proxy.
    { kProxy, AUTH_NONE, OK, kServer, AUTH_NONE, OK, 1, kNoSSL,
      { TestRound(kGetProxy, kSuccess, OK)}},
    // Authenticating HTTP server through a non-authenticating proxy.
    { kProxy, AUTH_NONE, OK, kServer, AUTH_SYNC, OK, 2, kNoSSL,
      { TestRound(kGetProxy, kServerChallenge, OK),
        TestRound(kGetAuthThroughProxy, kSuccess, OK)}},
    { kProxy, AUTH_NONE, OK, kServer, AUTH_SYNC, kAuthErr, 2, kNoSSL,
      { TestRound(kGetProxy, kServerChallenge, OK),
        TestRound(kGetAuthThroughProxy, kFailure, kAuthErr)}},
    { kProxy, AUTH_NONE, OK, kServer, AUTH_ASYNC, OK, 2, kNoSSL,
      { TestRound(kGetProxy, kServerChallenge, OK),
        TestRound(kGetAuthThroughProxy, kSuccess, OK)}},
    { kProxy, AUTH_NONE, OK, kServer, AUTH_ASYNC, kAuthErr, 2, kNoSSL,
      { TestRound(kGetProxy, kServerChallenge, OK),
        TestRound(kGetAuthThroughProxy, kFailure, kAuthErr)}},
    // Non-authenticating HTTP server through an authenticating proxy.
    { kProxy, AUTH_SYNC, OK, kServer, AUTH_NONE, OK, 2, kNoSSL,
      { TestRound(kGetProxy, kProxyChallenge, OK),
        TestRound(kGetProxyAuth, kSuccess, OK)}},
    { kProxy, AUTH_SYNC, kAuthErr, kServer, AUTH_NONE, OK, 2, kNoSSL,
      { TestRound(kGetProxy, kProxyChallenge, OK),
        TestRound(kGetProxyAuth, kFailure, kAuthErr)}},
    { kProxy, AUTH_ASYNC, OK, kServer, AUTH_NONE, OK, 2, kNoSSL,
      { TestRound(kGetProxy, kProxyChallenge, OK),
        TestRound(kGetProxyAuth, kSuccess, OK)}},
    { kProxy, AUTH_ASYNC, kAuthErr, kServer, AUTH_NONE, OK, 2, kNoSSL,
      { TestRound(kGetProxy, kProxyChallenge, OK),
        TestRound(kGetProxyAuth, kFailure, kAuthErr)}},
    // Authenticating HTTP server through an authenticating proxy.
    { kProxy, AUTH_SYNC, OK, kServer, AUTH_SYNC, OK, 3, kNoSSL,
      { TestRound(kGetProxy, kProxyChallenge, OK),
        TestRound(kGetProxyAuth, kServerChallenge, OK),
        TestRound(kGetAuthWithProxyAuth, kSuccess, OK)}},
    { kProxy, AUTH_SYNC, OK, kServer, AUTH_SYNC, kAuthErr, 3, kNoSSL,
      { TestRound(kGetProxy, kProxyChallenge, OK),
        TestRound(kGetProxyAuth, kServerChallenge, OK),
        TestRound(kGetAuthWithProxyAuth, kFailure, kAuthErr)}},
    { kProxy, AUTH_ASYNC, OK, kServer, AUTH_SYNC, OK, 3, kNoSSL,
      { TestRound(kGetProxy, kProxyChallenge, OK),
        TestRound(kGetProxyAuth, kServerChallenge, OK),
        TestRound(kGetAuthWithProxyAuth, kSuccess, OK)}},
    { kProxy, AUTH_ASYNC, OK, kServer, AUTH_SYNC, kAuthErr, 3, kNoSSL,
      { TestRound(kGetProxy, kProxyChallenge, OK),
        TestRound(kGetProxyAuth, kServerChallenge, OK),
        TestRound(kGetAuthWithProxyAuth, kFailure, kAuthErr)}},
    { kProxy, AUTH_SYNC, OK, kServer, AUTH_ASYNC, OK, 3, kNoSSL,
      { TestRound(kGetProxy, kProxyChallenge, OK),
        TestRound(kGetProxyAuth, kServerChallenge, OK),
        TestRound(kGetAuthWithProxyAuth, kSuccess, OK)}},
    { kProxy, AUTH_SYNC, OK, kServer, AUTH_ASYNC, kAuthErr, 3, kNoSSL,
      { TestRound(kGetProxy, kProxyChallenge, OK),
        TestRound(kGetProxyAuth, kServerChallenge, OK),
        TestRound(kGetAuthWithProxyAuth, kFailure, kAuthErr)}},
    { kProxy, AUTH_ASYNC, OK, kServer, AUTH_ASYNC, OK, 3, kNoSSL,
      { TestRound(kGetProxy, kProxyChallenge, OK),
        TestRound(kGetProxyAuth, kServerChallenge, OK),
        TestRound(kGetAuthWithProxyAuth, kSuccess, OK)}},
    { kProxy, AUTH_ASYNC, OK, kServer, AUTH_ASYNC, kAuthErr, 3, kNoSSL,
      { TestRound(kGetProxy, kProxyChallenge, OK),
        TestRound(kGetProxyAuth, kServerChallenge, OK),
        TestRound(kGetAuthWithProxyAuth, kFailure, kAuthErr)}},
    // Non-authenticating HTTPS server with a direct connection.
    { NULL, AUTH_NONE, OK, kSecureServer, AUTH_NONE, OK, 1, 0,
      { TestRound(kGet, kSuccess, OK)}},
    // Authenticating HTTPS server with a direct connection.
    { NULL, AUTH_NONE, OK, kSecureServer, AUTH_SYNC, OK, 2, 0,
      { TestRound(kGet, kServerChallenge, OK),
        TestRound(kGetAuth, kSuccess, OK)}},
    { NULL, AUTH_NONE, OK, kSecureServer, AUTH_SYNC, kAuthErr, 2, 0,
      { TestRound(kGet, kServerChallenge, OK),
        TestRound(kGetAuth, kFailure, kAuthErr)}},
    { NULL, AUTH_NONE, OK, kSecureServer, AUTH_ASYNC, OK, 2, 0,
      { TestRound(kGet, kServerChallenge, OK),
        TestRound(kGetAuth, kSuccess, OK)}},
    { NULL, AUTH_NONE, OK, kSecureServer, AUTH_ASYNC, kAuthErr, 2, 0,
      { TestRound(kGet, kServerChallenge, OK),
        TestRound(kGetAuth, kFailure, kAuthErr)}},
    // Non-authenticating HTTPS server with a non-authenticating proxy.
    { kProxy, AUTH_NONE, OK, kSecureServer, AUTH_NONE, OK, 1, 0,
      { TestRound(kConnect, kProxyConnected, OK, &kGet, &kSuccess)}},
    // Authenticating HTTPS server through a non-authenticating proxy.
    { kProxy, AUTH_NONE, OK, kSecureServer, AUTH_SYNC, OK, 2, 0,
      { TestRound(kConnect, kProxyConnected, OK, &kGet, &kServerChallenge),
        TestRound(kGetAuth, kSuccess, OK)}},
    { kProxy, AUTH_NONE, OK, kSecureServer, AUTH_SYNC, kAuthErr, 2, 0,
      { TestRound(kConnect, kProxyConnected, OK, &kGet, &kServerChallenge),
        TestRound(kGetAuth, kFailure, kAuthErr)}},
    { kProxy, AUTH_NONE, OK, kSecureServer, AUTH_ASYNC, OK, 2, 0,
      { TestRound(kConnect, kProxyConnected, OK, &kGet, &kServerChallenge),
        TestRound(kGetAuth, kSuccess, OK)}},
    { kProxy, AUTH_NONE, OK, kSecureServer, AUTH_ASYNC, kAuthErr, 2, 0,
      { TestRound(kConnect, kProxyConnected, OK, &kGet, &kServerChallenge),
        TestRound(kGetAuth, kFailure, kAuthErr)}},
    // Non-Authenticating HTTPS server through an authenticating proxy.
    { kProxy, AUTH_SYNC, OK, kSecureServer, AUTH_NONE, OK, 2, 1,
      { TestRound(kConnect, kProxyChallenge, OK),
        TestRound(kConnectProxyAuth, kProxyConnected, OK, &kGet, &kSuccess)}},
    { kProxy, AUTH_SYNC, kAuthErr, kSecureServer, AUTH_NONE, OK, 2, kNoSSL,
      { TestRound(kConnect, kProxyChallenge, OK),
        TestRound(kConnectProxyAuth, kFailure, kAuthErr)}},
    { kProxy, AUTH_ASYNC, OK, kSecureServer, AUTH_NONE, OK, 2, 1,
      { TestRound(kConnect, kProxyChallenge, OK),
        TestRound(kConnectProxyAuth, kProxyConnected, OK, &kGet, &kSuccess)}},
    { kProxy, AUTH_ASYNC, kAuthErr, kSecureServer, AUTH_NONE, OK, 2, kNoSSL,
      { TestRound(kConnect, kProxyChallenge, OK),
        TestRound(kConnectProxyAuth, kFailure, kAuthErr)}},
    // Authenticating HTTPS server through an authenticating proxy.
    { kProxy, AUTH_SYNC, OK, kSecureServer, AUTH_SYNC, OK, 3, 1,
      { TestRound(kConnect, kProxyChallenge, OK),
        TestRound(kConnectProxyAuth, kProxyConnected, OK,
                  &kGet, &kServerChallenge),
        TestRound(kGetAuth, kSuccess, OK)}},
    { kProxy, AUTH_SYNC, OK, kSecureServer, AUTH_SYNC, kAuthErr, 3, 1,
      { TestRound(kConnect, kProxyChallenge, OK),
        TestRound(kConnectProxyAuth, kProxyConnected, OK,
                  &kGet, &kServerChallenge),
        TestRound(kGetAuth, kFailure, kAuthErr)}},
    { kProxy, AUTH_ASYNC, OK, kSecureServer, AUTH_SYNC, OK, 3, 1,
      { TestRound(kConnect, kProxyChallenge, OK),
        TestRound(kConnectProxyAuth, kProxyConnected, OK,
                  &kGet, &kServerChallenge),
        TestRound(kGetAuth, kSuccess, OK)}},
    { kProxy, AUTH_ASYNC, OK, kSecureServer, AUTH_SYNC, kAuthErr, 3, 1,
      { TestRound(kConnect, kProxyChallenge, OK),
        TestRound(kConnectProxyAuth, kProxyConnected, OK,
                  &kGet, &kServerChallenge),
        TestRound(kGetAuth, kFailure, kAuthErr)}},
    { kProxy, AUTH_SYNC, OK, kSecureServer, AUTH_ASYNC, OK, 3, 1,
      { TestRound(kConnect, kProxyChallenge, OK),
        TestRound(kConnectProxyAuth, kProxyConnected, OK,
                  &kGet, &kServerChallenge),
        TestRound(kGetAuth, kSuccess, OK)}},
    { kProxy, AUTH_SYNC, OK, kSecureServer, AUTH_ASYNC, kAuthErr, 3, 1,
      { TestRound(kConnect, kProxyChallenge, OK),
        TestRound(kConnectProxyAuth, kProxyConnected, OK,
                  &kGet, &kServerChallenge),
        TestRound(kGetAuth, kFailure, kAuthErr)}},
    { kProxy, AUTH_ASYNC, OK, kSecureServer, AUTH_ASYNC, OK, 3, 1,
      { TestRound(kConnect, kProxyChallenge, OK),
        TestRound(kConnectProxyAuth, kProxyConnected, OK,
                  &kGet, &kServerChallenge),
        TestRound(kGetAuth, kSuccess, OK)}},
    { kProxy, AUTH_ASYNC, OK, kSecureServer, AUTH_ASYNC, kAuthErr, 3, 1,
      { TestRound(kConnect, kProxyChallenge, OK),
        TestRound(kConnectProxyAuth, kProxyConnected, OK,
                  &kGet, &kServerChallenge),
        TestRound(kGetAuth, kFailure, kAuthErr)}},
  };

  SessionDependencies session_deps;
  scoped_refptr<HttpNetworkSession> session = CreateSession(&session_deps);
  HttpAuthHandlerMock::Factory* auth_factory(
      new HttpAuthHandlerMock::Factory());
  session_deps.http_auth_handler_factory.reset(auth_factory);

  for (size_t i = 0; i < ARRAYSIZE_UNSAFE(test_configs); ++i) {
    const TestConfig& test_config = test_configs[i];

    // Set up authentication handlers as necessary.
    if (test_config.proxy_auth_timing != AUTH_NONE) {
      HttpAuthHandlerMock* auth_handler(new HttpAuthHandlerMock());
      std::string auth_challenge = "Mock realm=proxy";
      GURL origin(test_config.proxy_url);
      HttpAuth::ChallengeTokenizer tokenizer(auth_challenge.begin(),
                                             auth_challenge.end());
      auth_handler->InitFromChallenge(&tokenizer, HttpAuth::AUTH_PROXY,
                                      origin, BoundNetLog());
      auth_handler->SetGenerateExpectation(
          test_config.proxy_auth_timing == AUTH_ASYNC,
          test_config.proxy_auth_rv);
      auth_factory->set_mock_handler(auth_handler, HttpAuth::AUTH_PROXY);
    }
    if (test_config.server_auth_timing != AUTH_NONE) {
      HttpAuthHandlerMock* auth_handler(new HttpAuthHandlerMock());
      std::string auth_challenge = "Mock realm=server";
      GURL origin(test_config.server_url);
      HttpAuth::ChallengeTokenizer tokenizer(auth_challenge.begin(),
                                             auth_challenge.end());
      auth_handler->InitFromChallenge(&tokenizer, HttpAuth::AUTH_SERVER,
                                      origin, BoundNetLog());
      auth_handler->SetGenerateExpectation(
          test_config.server_auth_timing == AUTH_ASYNC,
          test_config.server_auth_rv);
      auth_factory->set_mock_handler(auth_handler, HttpAuth::AUTH_SERVER);
    }
    if (test_config.proxy_url) {
      session_deps.proxy_service =
          CreateFixedProxyService(test_config.proxy_url);
    } else {
      session_deps.proxy_service = ProxyService::CreateNull();
    }

    HttpRequestInfo request;
    request.method = "GET";
    request.url = GURL(test_config.server_url);
    request.load_flags = 0;

    scoped_ptr<HttpTransaction> trans(
        new HttpNetworkTransaction(CreateSession(&session_deps)));

    for (int round = 0; round < test_config.num_auth_rounds; ++round) {
      const TestRound& read_write_round = test_config.rounds[round];

      // Set up expected reads and writes.
      MockRead reads[2];
      reads[0] = read_write_round.read;
      size_t length_reads = 1;
      if (read_write_round.extra_read) {
        reads[1] = *read_write_round.extra_read;
        length_reads = 2;
      }

      MockWrite writes[2];
      writes[0] = read_write_round.write;
      size_t length_writes = 1;
      if (read_write_round.extra_write) {
        writes[1] = *read_write_round.extra_write;
        length_writes = 2;
      }
      StaticSocketDataProvider data_provider(
          reads, length_reads, writes, length_writes);
      session_deps.socket_factory.AddSocketDataProvider(&data_provider);

      // Add an SSL sequence if necessary.
      SSLSocketDataProvider ssl_socket_data_provider(false, OK);
      if (round >= test_config.first_ssl_round)
        session_deps.socket_factory.AddSSLSocketDataProvider(
            &ssl_socket_data_provider);

      // Start or restart the transaction.
      TestCompletionCallback callback;
      int rv;
      if (round == 0) {
        rv = trans->Start(&request, &callback, BoundNetLog());
      } else {
        rv = trans->RestartWithAuth(L"foo", L"bar", &callback);
      }
      if (rv == ERR_IO_PENDING)
        rv = callback.WaitForResult();

      // Compare results with expected data.
      EXPECT_EQ(read_write_round.expected_rv, rv);
      const HttpResponseInfo* response = trans->GetResponseInfo();
      if (read_write_round.expected_rv == OK) {
        EXPECT_FALSE(response == NULL);
      } else {
        EXPECT_TRUE(response == NULL);
        EXPECT_EQ(round + 1, test_config.num_auth_rounds);
        continue;
      }
      if (round + 1 < test_config.num_auth_rounds) {
        EXPECT_FALSE(response->auth_challenge.get() == NULL);
      } else {
        EXPECT_TRUE(response->auth_challenge.get() == NULL);
      }
    }
  }

  // Flush the idle socket before the HttpNetworkTransaction goes out of scope.
  session->FlushSocketPools();
}

class TLSDecompressionFailureSocketDataProvider : public SocketDataProvider {
 public:
  explicit TLSDecompressionFailureSocketDataProvider(bool fail_all)
      : fail_all_(fail_all) {
  }

  virtual MockRead GetNextRead() {
    if (fail_all_)
      return MockRead(false /* async */, ERR_SSL_DECOMPRESSION_FAILURE_ALERT);

    return MockRead(false /* async */,
                    "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nok.\r\n");
  }

  virtual MockWriteResult OnWrite(const std::string& data) {
    return MockWriteResult(false /* async */, data.size());
  }

  void Reset() {
  }

 private:
  const bool fail_all_;
};

// Test that we restart a connection when we see a decompression failure from
// the peer during the handshake. (In the real world we'll restart with SSLv3
// and we won't offer DEFLATE in that case.)
TEST_F(HttpNetworkTransactionTest, RestartAfterTLSDecompressionFailure) {
  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("https://tlsdecompressionfailure.example.com/");
  request.load_flags = 0;

  SessionDependencies session_deps;
  TLSDecompressionFailureSocketDataProvider socket_data_provider1(
      false /* fail all reads */);
  TLSDecompressionFailureSocketDataProvider socket_data_provider2(false);
  SSLSocketDataProvider ssl_socket_data_provider1(
      false, ERR_SSL_DECOMPRESSION_FAILURE_ALERT);
  SSLSocketDataProvider ssl_socket_data_provider2(false, OK);
  session_deps.socket_factory.AddSocketDataProvider(&socket_data_provider1);
  session_deps.socket_factory.AddSocketDataProvider(&socket_data_provider2);
  session_deps.socket_factory.AddSSLSocketDataProvider(
      &ssl_socket_data_provider1);
  session_deps.socket_factory.AddSSLSocketDataProvider(
      &ssl_socket_data_provider2);

  // Work around http://crbug.com/37454
  StaticSocketDataProvider bug37454_connection;
  bug37454_connection.set_connect_data(MockConnect(true, ERR_UNEXPECTED));
  session_deps.socket_factory.AddSocketDataProvider(&bug37454_connection);

  scoped_refptr<HttpNetworkSession> session(CreateSession(&session_deps));
  scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));
  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);
  EXPECT_EQ(OK, callback.WaitForResult());

  const HttpResponseInfo* response = trans->GetResponseInfo();
  ASSERT_TRUE(response != NULL);
  ASSERT_TRUE(response->headers != NULL);
  EXPECT_EQ("HTTP/1.1 200 OK", response->headers->GetStatusLine());

  std::string response_data;
  ASSERT_EQ(OK, ReadTransaction(trans.get(), &response_data));
  EXPECT_EQ("ok.", response_data);
}

// Test that we restart a connection if we get a decompression failure from the
// peer while reading the first bytes from the connection. This occurs when the
// peer cannot handle DEFLATE but we're using False Start, so we don't notice
// in the handshake.
TEST_F(HttpNetworkTransactionTest,
       RestartAfterTLSDecompressionFailureWithFalseStart) {
  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("https://tlsdecompressionfailure2.example.com/");
  request.load_flags = 0;

  SessionDependencies session_deps;
  TLSDecompressionFailureSocketDataProvider socket_data_provider1(
      true /* fail all reads */);
  TLSDecompressionFailureSocketDataProvider socket_data_provider2(false);
  SSLSocketDataProvider ssl_socket_data_provider1(false, OK);
  SSLSocketDataProvider ssl_socket_data_provider2(false, OK);
  session_deps.socket_factory.AddSocketDataProvider(&socket_data_provider1);
  session_deps.socket_factory.AddSocketDataProvider(&socket_data_provider2);
  session_deps.socket_factory.AddSSLSocketDataProvider(
      &ssl_socket_data_provider1);
  session_deps.socket_factory.AddSSLSocketDataProvider(
      &ssl_socket_data_provider2);

  scoped_refptr<HttpNetworkSession> session(CreateSession(&session_deps));
  scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));
  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);
  EXPECT_EQ(OK, callback.WaitForResult());

  const HttpResponseInfo* response = trans->GetResponseInfo();
  ASSERT_TRUE(response != NULL);
  ASSERT_TRUE(response->headers != NULL);
  EXPECT_EQ("HTTP/1.1 200 OK", response->headers->GetStatusLine());

  std::string response_data;
  ASSERT_EQ(OK, ReadTransaction(trans.get(), &response_data));
  EXPECT_EQ("ok.", response_data);
}

// This tests the case that a request is issued via http instead of spdy after
// npn is negotiated.
TEST_F(HttpNetworkTransactionTest, NpnWithHttpOverSSL) {
  HttpNetworkTransaction::SetUseAlternateProtocols(true);
  HttpNetworkTransaction::SetNextProtos("\x08http/1.1\x07http1.1");
  SessionDependencies session_deps;
  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("https://www.google.com/");
  request.load_flags = 0;

  MockWrite data_writes[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n\r\n"),
  };

  MockRead data_reads[] = {
    MockRead("HTTP/1.1 200 OK\r\n"),
    MockRead("Alternate-Protocol: 443:npn-spdy/1\r\n\r\n"),
    MockRead("hello world"),
    MockRead(false, OK),
  };

  SSLSocketDataProvider ssl(true, OK);
  ssl.next_proto_status = SSLClientSocket::kNextProtoNegotiated;
  ssl.next_proto = "http/1.1";

  session_deps.socket_factory.AddSSLSocketDataProvider(&ssl);

  StaticSocketDataProvider data(data_reads, arraysize(data_reads),
                                data_writes, arraysize(data_writes));
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  scoped_refptr<HttpNetworkSession> session(CreateSession(&session_deps));
  scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));

  int rv = trans->Start(&request, &callback, BoundNetLog());

  EXPECT_EQ(ERR_IO_PENDING, rv);
  EXPECT_EQ(OK, callback.WaitForResult());

  const HttpResponseInfo* response = trans->GetResponseInfo();
  ASSERT_TRUE(response != NULL);
  ASSERT_TRUE(response->headers != NULL);
  EXPECT_EQ("HTTP/1.1 200 OK", response->headers->GetStatusLine());

  std::string response_data;
  ASSERT_EQ(OK, ReadTransaction(trans.get(), &response_data));
  EXPECT_EQ("hello world", response_data);

  EXPECT_FALSE(response->was_fetched_via_spdy);
  EXPECT_TRUE(response->was_npn_negotiated);
  EXPECT_FALSE(response->was_alternate_protocol_available);

  HttpNetworkTransaction::SetNextProtos("");
  HttpNetworkTransaction::SetUseAlternateProtocols(false);
}

TEST_F(HttpNetworkTransactionTest, SpdyPostNPNServerHangup) {
  // Simulate the SSL handshake completing with an NPN negotiation
  // followed by an immediate server closing of the socket.
  // Fix crash:  http://crbug.com/46369
  HttpNetworkTransaction::SetUseAlternateProtocols(true);
  HttpNetworkTransaction::SetNextProtos(
      "\x08http/1.1\x07http1.1\x06spdy/1\x04spdy");
  SessionDependencies session_deps;

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("https://www.google.com/");
  request.load_flags = 0;

  SSLSocketDataProvider ssl(true, OK);
  ssl.next_proto_status = SSLClientSocket::kNextProtoNegotiated;
  ssl.next_proto = "spdy/1";
  ssl.was_npn_negotiated = true;
  session_deps.socket_factory.AddSSLSocketDataProvider(&ssl);

  scoped_ptr<spdy::SpdyFrame> req(ConstructSpdyGet(NULL, 0));
  MockWrite spdy_writes[] = { CreateMockWrite(*req) };

  MockRead spdy_reads[] = {
    MockRead(false, 0, 0)   // Not async - return 0 immediately.
  };

  scoped_refptr<DelayedSocketData> spdy_data(
      new DelayedSocketData(
          0,  // don't wait in this case, immediate hangup.
          spdy_reads, arraysize(spdy_reads),
          spdy_writes, arraysize(spdy_writes)));
  session_deps.socket_factory.AddSocketDataProvider(spdy_data);

  TestCompletionCallback callback;

  scoped_refptr<HttpNetworkSession> session(CreateSession(&session_deps));
  scoped_ptr<HttpNetworkTransaction> trans(new HttpNetworkTransaction(session));

  int rv = trans->Start(&request, &callback, BoundNetLog());
  EXPECT_EQ(ERR_IO_PENDING, rv);
  EXPECT_EQ(ERR_CONNECTION_CLOSED, callback.WaitForResult());

  HttpNetworkTransaction::SetNextProtos("");
  HttpNetworkTransaction::SetUseAlternateProtocols(false);
}

}  // namespace net
