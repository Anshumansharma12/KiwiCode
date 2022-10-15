// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/http/http_proxy_connect_job.h"

#include <memory>
#include <utility>

#include "base/bind.h"
#include "base/callback.h"
#include "base/cxx17_backports.h"
#include "base/metrics/field_trial.h"
#include "base/metrics/field_trial_params.h"
#include "base/metrics/histogram_functions.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/threading/thread_task_runner_handle.h"
#include "base/values.h"
#include "build/build_config.h"
#include "http_proxy_client_socket.h"
#include "net/base/host_port_pair.h"
#include "net/base/http_user_agent_settings.h"
#include "net/base/net_errors.h"
#include "net/log/net_log_source_type.h"
#include "net/log/net_log_with_source.h"
#include "net/nqe/network_quality_estimator.h"
#include "net/quic/quic_http_utils.h"
#include "net/quic/quic_proxy_client_socket.h"
#include "net/quic/quic_stream_factory.h"
#include "net/socket/client_socket_handle.h"
#include "net/socket/next_proto.h"
#include "net/socket/ssl_client_socket.h"
#include "net/socket/ssl_connect_job.h"
#include "net/socket/transport_client_socket_pool.h"
#include "net/socket/transport_connect_job.h"
#include "net/spdy/spdy_proxy_client_socket.h"
#include "net/spdy/spdy_session.h"
#include "net/spdy/spdy_session_pool.h"
#include "net/spdy/spdy_stream.h"
#include "net/ssl/ssl_cert_request_info.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "third_party/abseil-cpp/absl/types/variant.h"
#include "url/gurl.h"
#include "url/scheme_host_port.h"

namespace net {

namespace {

// HttpProxyConnectJobs will time out after this many seconds.  Note this is in
// addition to the timeout for the transport socket.
#if BUILDFLAG(IS_ANDROID) || BUILDFLAG(IS_IOS)
constexpr base::TimeDelta kHttpProxyConnectJobTunnelTimeout = base::Seconds(10);
#else
constexpr base::TimeDelta kHttpProxyConnectJobTunnelTimeout = base::Seconds(30);
#endif

class HttpProxyTimeoutExperiments {
 public:
  HttpProxyTimeoutExperiments() { Init(); }

  ~HttpProxyTimeoutExperiments() = default;

  void Init() {
    min_proxy_connection_timeout_ =
        base::Seconds(GetInt32Param("min_proxy_connection_timeout_seconds", 8));
    max_proxy_connection_timeout_ = base::Seconds(
        GetInt32Param("max_proxy_connection_timeout_seconds", 30));
    ssl_http_rtt_multiplier_ = GetInt32Param("ssl_http_rtt_multiplier", 10);
    non_ssl_http_rtt_multiplier_ =
        GetInt32Param("non_ssl_http_rtt_multiplier", 5);

    DCHECK_LT(0, ssl_http_rtt_multiplier_);
    DCHECK_LT(0, non_ssl_http_rtt_multiplier_);
    DCHECK_LE(base::TimeDelta(), min_proxy_connection_timeout_);
    DCHECK_LE(base::TimeDelta(), max_proxy_connection_timeout_);
    DCHECK_LE(min_proxy_connection_timeout_, max_proxy_connection_timeout_);
  }

  base::TimeDelta min_proxy_connection_timeout() const {
    return min_proxy_connection_timeout_;
  }
  base::TimeDelta max_proxy_connection_timeout() const {
    return max_proxy_connection_timeout_;
  }
  int32_t ssl_http_rtt_multiplier() const { return ssl_http_rtt_multiplier_; }
  int32_t non_ssl_http_rtt_multiplier() const {
    return non_ssl_http_rtt_multiplier_;
  }

 private:
  // Returns the value of the parameter |param_name| for the field trial
  // "NetAdaptiveProxyConnectionTimeout". If the value of the parameter is
  // unavailable, then |default_value| is available.
  static int32_t GetInt32Param(const std::string& param_name,
                               int32_t default_value) {
    int32_t param;
    if (!base::StringToInt(base::GetFieldTrialParamValue(
                               "NetAdaptiveProxyConnectionTimeout", param_name),
                           &param)) {
      return default_value;
    }
    return param;
  }

  // For secure proxies, the connection timeout is set to
  // |ssl_http_rtt_multiplier_| times the HTTP RTT estimate. For insecure
  // proxies, the connection timeout is set to |non_ssl_http_rtt_multiplier_|
  // times the HTTP RTT estimate. In either case, the connection timeout
  // is clamped to be between |min_proxy_connection_timeout_| and
  // |max_proxy_connection_timeout_|.
  base::TimeDelta min_proxy_connection_timeout_;
  base::TimeDelta max_proxy_connection_timeout_;
  int32_t ssl_http_rtt_multiplier_;
  int32_t non_ssl_http_rtt_multiplier_;
};

HttpProxyTimeoutExperiments* GetProxyTimeoutExperiments() {
  static HttpProxyTimeoutExperiments proxy_timeout_experiments;
  return &proxy_timeout_experiments;
}

}  // namespace

HttpProxySocketParams::HttpProxySocketParams(
    scoped_refptr<TransportSocketParams> transport_params,
    scoped_refptr<SSLSocketParams> ssl_params,
    bool is_quic,
    const HostPortPair& endpoint,
    bool tunnel,
    const NetworkTrafficAnnotationTag traffic_annotation,
    const NetworkIsolationKey& network_isolation_key)
    : transport_params_(std::move(transport_params)),
      ssl_params_(std::move(ssl_params)),
      is_quic_(is_quic),
      endpoint_(endpoint),
      tunnel_(tunnel),
      network_isolation_key_(network_isolation_key),
      traffic_annotation_(traffic_annotation) {
  // This is either a connection to an HTTP proxy or an SSL/QUIC proxy.
  DCHECK(transport_params_ || ssl_params_);
  DCHECK(!transport_params_ || !ssl_params_);

  // If connecting to a QUIC proxy, and |ssl_params_| must be valid. This also
  // implies |transport_params_| is null, per the above DCHECKs.
  if (is_quic_)
    DCHECK(ssl_params_);

  // Only supports proxy endpoints without scheme for now.
  // TODO(crbug.com/1206799): Handle scheme.
  if (transport_params_) {
    DCHECK(absl::holds_alternative<HostPortPair>(
        transport_params_->destination()));
  } else {
    DCHECK(absl::holds_alternative<HostPortPair>(
        ssl_params_->GetDirectConnectionParams()->destination()));
  }
}

HttpProxySocketParams::~HttpProxySocketParams() = default;

std::unique_ptr<HttpProxyConnectJob> HttpProxyConnectJob::Factory::Create(
    RequestPriority priority,
    const SocketTag& socket_tag,
    const CommonConnectJobParams* common_connect_job_params,
    scoped_refptr<HttpProxySocketParams> params,
    ConnectJob::Delegate* delegate,
    const NetLogWithSource* net_log) {
  return std::make_unique<HttpProxyConnectJob>(
      priority, socket_tag, common_connect_job_params, std::move(params),
      delegate, net_log);
}

HttpProxyConnectJob::HttpProxyConnectJob(
    RequestPriority priority,
    const SocketTag& socket_tag,
    const CommonConnectJobParams* common_connect_job_params,
    scoped_refptr<HttpProxySocketParams> params,
    ConnectJob::Delegate* delegate,
    const NetLogWithSource* net_log)
    : ConnectJob(priority,
                 socket_tag,
                 base::TimeDelta() /* The socket takes care of timeouts */,
                 common_connect_job_params,
                 delegate,
                 net_log,
                 NetLogSourceType::HTTP_PROXY_CONNECT_JOB,
                 NetLogEventType::HTTP_PROXY_CONNECT_JOB_CONNECT),
      params_(std::move(params)),
      http_auth_controller_(
          params_->tunnel()
              ? base::MakeRefCounted<HttpAuthController>(
                    HttpAuth::AUTH_PROXY,
                    GURL((params_->ssl_params() ? "https://" : "http://") +
                         GetDestination().ToString()),
                    params_->network_isolation_key(),
                    common_connect_job_params->http_auth_cache,
                    common_connect_job_params->http_auth_handler_factory,
                    host_resolver())
              : nullptr) {}

HttpProxyConnectJob::~HttpProxyConnectJob() = default;

const RequestPriority HttpProxyConnectJob::kH2QuicTunnelPriority =
    DEFAULT_PRIORITY;

LoadState HttpProxyConnectJob::GetLoadState() const {
  switch (next_state_) {
    case STATE_TRANSPORT_CONNECT_COMPLETE:
      return nested_connect_job_->GetLoadState();
    case STATE_HTTP_PROXY_CONNECT:
    case STATE_HTTP_PROXY_CONNECT_COMPLETE:
    case STATE_SPDY_PROXY_CREATE_STREAM:
    case STATE_SPDY_PROXY_CREATE_STREAM_COMPLETE:
    case STATE_QUIC_PROXY_CREATE_SESSION:
    case STATE_QUIC_PROXY_CREATE_STREAM:
    case STATE_QUIC_PROXY_CREATE_STREAM_COMPLETE:
    case STATE_RESTART_WITH_AUTH:
    case STATE_RESTART_WITH_AUTH_COMPLETE:
      return LOAD_STATE_ESTABLISHING_PROXY_TUNNEL;
    // This state shouldn't be possible to be called in.
    case STATE_TRANSPORT_CONNECT:
      NOTREACHED();
      [[fallthrough]];
    case STATE_BEGIN_CONNECT:
    case STATE_NONE:
      // May be possible for this method to be called after an error, shouldn't
      // be called after a successful connect.
      break;
  }
  return LOAD_STATE_IDLE;
}

bool HttpProxyConnectJob::HasEstablishedConnection() const {
  if (has_established_connection_)
    return true;

  // It's possible the nested connect job has established a connection, but
  // hasn't completed yet (For example, an SSLConnectJob may be negotiating
  // SSL).
  if (nested_connect_job_)
    return nested_connect_job_->HasEstablishedConnection();
  return false;
}

ResolveErrorInfo HttpProxyConnectJob::GetResolveErrorInfo() const {
  return resolve_error_info_;
}

bool HttpProxyConnectJob::IsSSLError() const {
  return ssl_cert_request_info_ != nullptr;
}

scoped_refptr<SSLCertRequestInfo> HttpProxyConnectJob::GetCertRequestInfo() {
  return ssl_cert_request_info_;
}

void HttpProxyConnectJob::OnConnectJobComplete(int result, ConnectJob* job) {
  DCHECK_EQ(nested_connect_job_.get(), job);
  DCHECK_EQ(next_state_, STATE_TRANSPORT_CONNECT_COMPLETE);
  OnIOComplete(result);
}

void HttpProxyConnectJob::OnNeedsProxyAuth(
    const HttpResponseInfo& response,
    HttpAuthController* auth_controller,
    base::OnceClosure restart_with_auth_callback,
    ConnectJob* job) {
  // None of the nested ConnectJob used by this class can encounter auth
  // challenges. Instead, the challenges are returned by the ProxyClientSocket
  // implementations after nested_connect_job_ has already established a
  // connection.
  NOTREACHED();
}

base::TimeDelta HttpProxyConnectJob::AlternateNestedConnectionTimeout(
    const HttpProxySocketParams& params,
    const NetworkQualityEstimator* network_quality_estimator) {
  base::TimeDelta default_alternate_timeout;

  // On Android and iOS, a default proxy connection timeout is used instead of
  // the actual TCP/SSL timeouts of nested jobs.
#if BUILDFLAG(IS_ANDROID) || BUILDFLAG(IS_IOS)
  default_alternate_timeout = kHttpProxyConnectJobTunnelTimeout;
#endif  // !BUILDFLAG(IS_ANDROID) && !BUILDFLAG(IS_IOS)

  bool is_https = params.ssl_params() != nullptr;
  // HTTP proxy connections can't be on top of proxy connections.
  DCHECK(!is_https ||
         params.ssl_params()->GetConnectionType() == SSLSocketParams::DIRECT);

  if (!network_quality_estimator)
    return default_alternate_timeout;

  absl::optional<base::TimeDelta> http_rtt_estimate =
      network_quality_estimator->GetHttpRTT();
  if (!http_rtt_estimate)
    return default_alternate_timeout;

  int32_t multiplier =
      is_https ? GetProxyTimeoutExperiments()->ssl_http_rtt_multiplier()
               : GetProxyTimeoutExperiments()->non_ssl_http_rtt_multiplier();
  base::TimeDelta timeout = multiplier * http_rtt_estimate.value();
  // Ensure that connection timeout is between
  // |min_proxy_connection_timeout_| and |max_proxy_connection_timeout_|.
  return base::clamp(
      timeout, GetProxyTimeoutExperiments()->min_proxy_connection_timeout(),
      GetProxyTimeoutExperiments()->max_proxy_connection_timeout());
}

base::TimeDelta HttpProxyConnectJob::TunnelTimeoutForTesting() {
  return kHttpProxyConnectJobTunnelTimeout;
}

void HttpProxyConnectJob::UpdateFieldTrialParametersForTesting() {
  GetProxyTimeoutExperiments()->Init();
}

int HttpProxyConnectJob::ConnectInternal() {
  DCHECK_EQ(next_state_, STATE_NONE);
  next_state_ = STATE_BEGIN_CONNECT;
  return DoLoop(OK);
}

ProxyServer::Scheme HttpProxyConnectJob::GetProxyServerScheme() const {
  if (params_->is_quic())
    return ProxyServer::SCHEME_QUIC;

  if (params_->transport_params())
    return ProxyServer::SCHEME_HTTP;

  return ProxyServer::SCHEME_HTTPS;
}

void HttpProxyConnectJob::OnIOComplete(int result) {
  int rv = DoLoop(result);
  if (rv != ERR_IO_PENDING) {
    // May delete |this|.
    NotifyDelegateOfCompletion(rv);
  }
}

void HttpProxyConnectJob::RestartWithAuthCredentials() {
  DCHECK(transport_socket_);
  DCHECK_EQ(STATE_NONE, next_state_);

  // Always do this asynchronously, to avoid re-entrancy.
  next_state_ = STATE_RESTART_WITH_AUTH;
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE, base::BindOnce(&HttpProxyConnectJob::OnIOComplete,
                                weak_ptr_factory_.GetWeakPtr(), net::OK));
}

int HttpProxyConnectJob::DoLoop(int result) {
  DCHECK_NE(next_state_, STATE_NONE);

  int rv = result;
  do {
    State state = next_state_;
    next_state_ = STATE_NONE;
    switch (state) {
      case STATE_BEGIN_CONNECT:
        DCHECK_EQ(OK, rv);
        rv = DoBeginConnect();
        break;
      case STATE_TRANSPORT_CONNECT:
        DCHECK_EQ(OK, rv);
        rv = DoTransportConnect();
        break;
      case STATE_TRANSPORT_CONNECT_COMPLETE:
        rv = DoTransportConnectComplete(rv);
        break;
      case STATE_HTTP_PROXY_CONNECT:
        DCHECK_EQ(OK, rv);
        rv = DoHttpProxyConnect();
        break;
      case STATE_HTTP_PROXY_CONNECT_COMPLETE:
        rv = DoHttpProxyConnectComplete(rv);
        break;
      case STATE_SPDY_PROXY_CREATE_STREAM:
        DCHECK_EQ(OK, rv);
        rv = DoSpdyProxyCreateStream();
        break;
      case STATE_SPDY_PROXY_CREATE_STREAM_COMPLETE:
        rv = DoSpdyProxyCreateStreamComplete(rv);
        break;
      case STATE_QUIC_PROXY_CREATE_SESSION:
        DCHECK_EQ(OK, rv);
        rv = DoQuicProxyCreateSession();
        break;
      case STATE_QUIC_PROXY_CREATE_STREAM:
        rv = DoQuicProxyCreateStream(rv);
        break;
      case STATE_QUIC_PROXY_CREATE_STREAM_COMPLETE:
        rv = DoQuicProxyCreateStreamComplete(rv);
        break;
      case STATE_RESTART_WITH_AUTH:
        DCHECK_EQ(OK, rv);
        rv = DoRestartWithAuth();
        break;
      case STATE_RESTART_WITH_AUTH_COMPLETE:
        rv = DoRestartWithAuthComplete(rv);
        break;
      default:
        NOTREACHED() << "bad state";
        rv = ERR_FAILED;
        break;
    }
  } while (rv != ERR_IO_PENDING && next_state_ != STATE_NONE);

  return rv;
}

int HttpProxyConnectJob::DoBeginConnect() {
  connect_start_time_ = base::TimeTicks::Now();
  ResetTimer(
      AlternateNestedConnectionTimeout(*params_, network_quality_estimator()));
  switch (GetProxyServerScheme()) {
    case ProxyServer::SCHEME_QUIC:
      next_state_ = STATE_QUIC_PROXY_CREATE_SESSION;
      // QUIC connections are always considered to have been established.
      // |has_established_connection_| is only used to start retries if a
      // connection hasn't been established yet, and QUIC has its own connection
      // establishment logic.
      has_established_connection_ = true;
      break;
    case ProxyServer::SCHEME_HTTP:
    case ProxyServer::SCHEME_HTTPS:
      next_state_ = STATE_TRANSPORT_CONNECT;
      break;
    default:
      NOTREACHED();
  }
  return OK;
}

int HttpProxyConnectJob::DoTransportConnect() {
  ProxyServer::Scheme scheme = GetProxyServerScheme();
  if (scheme == ProxyServer::SCHEME_HTTP) {
    nested_connect_job_ = std::make_unique<TransportConnectJob>(
        priority(), socket_tag(), common_connect_job_params(),
        params_->transport_params(), this, &net_log());
  } else {
    DCHECK_EQ(scheme, ProxyServer::SCHEME_HTTPS);
    DCHECK(params_->ssl_params());
    // Skip making a new connection if we have an existing HTTP/2 session.
    if (params_->tunnel() &&
        common_connect_job_params()->spdy_session_pool->FindAvailableSession(
            CreateSpdySessionKey(), /*enable_ip_based_pooling=*/false,
            /*is_websocket=*/false, net_log())) {
      next_state_ = STATE_SPDY_PROXY_CREATE_STREAM;
      return OK;
    }

    nested_connect_job_ = std::make_unique<SSLConnectJob>(
        priority(), socket_tag(), common_connect_job_params(),
        params_->ssl_params(), this, &net_log());
  }

  next_state_ = STATE_TRANSPORT_CONNECT_COMPLETE;
  return nested_connect_job_->Connect();
}

int HttpProxyConnectJob::DoTransportConnectComplete(int result) {
  resolve_error_info_ = nested_connect_job_->GetResolveErrorInfo();
  ProxyServer::Scheme scheme = GetProxyServerScheme();
  if (result != OK) {
    base::UmaHistogramMediumTimes(
        scheme == ProxyServer::SCHEME_HTTP
            ? "Net.HttpProxy.ConnectLatency.Insecure.Error"
            : "Net.HttpProxy.ConnectLatency.Secure.Error",
        base::TimeTicks::Now() - connect_start_time_);

    if (IsCertificateError(result)) {
      DCHECK_EQ(ProxyServer::SCHEME_HTTPS, scheme);
      // TODO(rch): allow the user to deal with proxy cert errors in the
      // same way as server cert errors.
      return ERR_PROXY_CERTIFICATE_INVALID;
    }

    if (result == ERR_SSL_CLIENT_AUTH_CERT_NEEDED) {
      DCHECK_EQ(ProxyServer::SCHEME_HTTPS, scheme);
      ssl_cert_request_info_ = nested_connect_job_->GetCertRequestInfo();
      DCHECK(ssl_cert_request_info_);
      ssl_cert_request_info_->is_proxy = true;
      return result;
    }

    return ERR_PROXY_CONNECTION_FAILED;
  }

  base::UmaHistogramMediumTimes(
      scheme == ProxyServer::SCHEME_HTTP
          ? "Net.HttpProxy.ConnectLatency.Insecure.Success"
          : "Net.HttpProxy.ConnectLatency.Secure.Success",
      base::TimeTicks::Now() - connect_start_time_);

  has_established_connection_ = true;

  if (!params_->tunnel()) {
    // If not tunneling, this is an HTTP URL being fetched directly over the
    // proxy. Return the underlying socket directly. The caller will handle the
    // ALPN protocol, etc., from here. Clear the DNS aliases to match the other
    // proxy codepaths.
    SetSocket(nested_connect_job_->PassSocket(),
              /*dns_aliases=*/std::set<std::string>());
    return result;
  }

  // Establish a tunnel over the proxy by making a CONNECT request. HTTP/1.1 and
  // HTTP/2 handle CONNECT differently.
  if (nested_connect_job_->socket()->GetNegotiatedProtocol() == kProtoHTTP2) {
    DCHECK_EQ(ProxyServer::SCHEME_HTTPS, scheme);
    next_state_ = STATE_SPDY_PROXY_CREATE_STREAM;
  } else {
    next_state_ = STATE_HTTP_PROXY_CONNECT;
  }
  return result;
}

int HttpProxyConnectJob::DoHttpProxyConnect() {
  DCHECK(params_->tunnel());
  next_state_ = STATE_HTTP_PROXY_CONNECT_COMPLETE;

  // Reset the timer to just the length of time allowed for HttpProxy handshake
  // so that a fast TCP connection plus a slow HttpProxy failure doesn't take
  // longer to timeout than it should.
  ResetTimer(kHttpProxyConnectJobTunnelTimeout);

  // Add a HttpProxy connection on top of the tcp socket.
  transport_socket_ = std::make_unique<HttpProxyClientSocket>(
      nested_connect_job_->PassSocket(), GetUserAgent(), params_->endpoint(),
      ProxyServer(GetProxyServerScheme(), GetDestination()),
      http_auth_controller_, common_connect_job_params()->proxy_delegate,
      params_->traffic_annotation());
  nested_connect_job_.reset();
  return transport_socket_->Connect(base::BindOnce(
      &HttpProxyConnectJob::OnIOComplete, base::Unretained(this)));
}

int HttpProxyConnectJob::DoHttpProxyConnectComplete(int result) {
  // Always inform caller of auth requests asynchronously.
  if (result == ERR_PROXY_AUTH_REQUESTED) {
    base::ThreadTaskRunnerHandle::Get()->PostTask(
        FROM_HERE, base::BindOnce(&HttpProxyConnectJob::OnAuthChallenge,
                                  weak_ptr_factory_.GetWeakPtr()));
    return ERR_IO_PENDING;
  }

  if (result == ERR_HTTP_1_1_REQUIRED)
    return ERR_PROXY_HTTP_1_1_REQUIRED;

  // In TLS 1.2 with False Start or TLS 1.3, alerts from the server rejecting
  // our client certificate are received at the first Read(), not Connect(), so
  // the error mapping in DoTransportConnectComplete does not apply. Repeat the
  // mapping here.
  if (result == ERR_BAD_SSL_CLIENT_AUTH_CERT)
    return ERR_PROXY_CONNECTION_FAILED;

  if (result == OK) {
    SetSocket(std::move(transport_socket_), /*dns_aliases=*/absl::nullopt);
  }

  return result;
}

int HttpProxyConnectJob::DoSpdyProxyCreateStream() {
  DCHECK(params_->tunnel());
  DCHECK(params_->ssl_params());

  // Reset the timer to just the length of time allowed for HttpProxy handshake
  // so that a fast TCP connection plus a slow HttpProxy failure doesn't take
  // longer to timeout than it should.
  ResetTimer(kHttpProxyConnectJobTunnelTimeout);

  SpdySessionKey key = CreateSpdySessionKey();
  base::WeakPtr<SpdySession> spdy_session =
      common_connect_job_params()->spdy_session_pool->FindAvailableSession(
          key, /* enable_ip_based_pooling = */ false,
          /* is_websocket = */ false, net_log());
  // It's possible that a session to the proxy has recently been created
  if (spdy_session) {
    nested_connect_job_.reset();
  } else {
    // Create a session direct to the proxy itself
    spdy_session = common_connect_job_params()
                       ->spdy_session_pool->CreateAvailableSessionFromSocket(
                           key, nested_connect_job_->PassSocket(),
                           nested_connect_job_->connect_timing(), net_log());
    DCHECK(spdy_session);
    nested_connect_job_.reset();
  }

  next_state_ = STATE_SPDY_PROXY_CREATE_STREAM_COMPLETE;
  spdy_stream_request_ = std::make_unique<SpdyStreamRequest>();
  return spdy_stream_request_->StartRequest(
      SPDY_BIDIRECTIONAL_STREAM, spdy_session,
      GURL("https://" + params_->endpoint().ToString()),
      false /* no early data */, kH2QuicTunnelPriority, socket_tag(),
      spdy_session->net_log(),
      base::BindOnce(&HttpProxyConnectJob::OnIOComplete,
                     base::Unretained(this)),
      params_->traffic_annotation());
}

int HttpProxyConnectJob::DoSpdyProxyCreateStreamComplete(int result) {
  if (result < 0) {
    // See the comment in DoHttpProxyConnectComplete(). HTTP/2 proxies will
    // typically also fail here, as a result of SpdyProxyClientSocket::Connect()
    // below, but the error may surface out of SpdyStreamRequest if there were
    // enough requests in parallel that stream creation became asynchronous.
    if (result == ERR_BAD_SSL_CLIENT_AUTH_CERT)
      result = ERR_PROXY_CONNECTION_FAILED;

    spdy_stream_request_.reset();
    return result;
  }

  next_state_ = STATE_HTTP_PROXY_CONNECT_COMPLETE;
  base::WeakPtr<SpdyStream> stream = spdy_stream_request_->ReleaseStream();
  spdy_stream_request_.reset();
  DCHECK(stream.get());
  // |transport_socket_| will set itself as |stream|'s delegate.
  transport_socket_ = std::make_unique<SpdyProxyClientSocket>(
      stream, ProxyServer(GetProxyServerScheme(), GetDestination()),
      GetUserAgent(), params_->endpoint(), net_log(), http_auth_controller_,
      common_connect_job_params()->proxy_delegate);
  return transport_socket_->Connect(base::BindOnce(
      &HttpProxyConnectJob::OnIOComplete, base::Unretained(this)));
}

int HttpProxyConnectJob::DoQuicProxyCreateSession() {
  SSLSocketParams* ssl_params = params_->ssl_params().get();
  DCHECK(ssl_params);
  DCHECK(params_->tunnel());
  DCHECK(!common_connect_job_params()->quic_supported_versions->empty());

  // Reset the timer to just the length of time allowed for HttpProxy handshake
  // so that a fast QUIC connection plus a slow tunnel setup doesn't take longer
  // to timeout than it should.
  ResetTimer(kHttpProxyConnectJobTunnelTimeout);

  next_state_ = STATE_QUIC_PROXY_CREATE_STREAM;
  const HostPortPair& proxy_server = GetDestination();
  quic_stream_request_ = std::make_unique<QuicStreamRequest>(
      common_connect_job_params()->quic_stream_factory);

  // Use default QUIC version, which is the version listed supported version.
  quic::ParsedQuicVersion quic_version =
      common_connect_job_params()->quic_supported_versions->front();
  return quic_stream_request_->Request(
      // TODO(crbug.com/1206799) Pass the destination directly once it's
      // converted to contain scheme.
      url::SchemeHostPort(url::kHttpsScheme, proxy_server.host(),
                          proxy_server.port()),
      quic_version, ssl_params->privacy_mode(), kH2QuicTunnelPriority,
      socket_tag(), params_->network_isolation_key(),
      ssl_params->GetDirectConnectionParams()->secure_dns_policy(),
      /*use_dns_aliases=*/false, /*require_dns_https_alpn=*/false,
      ssl_params->ssl_config().GetCertVerifyFlags(),
      GURL("https://" + proxy_server.ToString()), net_log(),
      &quic_net_error_details_,
      /*failed_on_default_network_callback=*/CompletionOnceCallback(),
      base::BindOnce(&HttpProxyConnectJob::OnIOComplete,
                     base::Unretained(this)));
}

int HttpProxyConnectJob::DoQuicProxyCreateStream(int result) {
  if (result < 0) {
    quic_stream_request_.reset();
    return result;
  }

  next_state_ = STATE_QUIC_PROXY_CREATE_STREAM_COMPLETE;
  quic_session_ = quic_stream_request_->ReleaseSessionHandle();
  quic_stream_request_.reset();

  return quic_session_->RequestStream(
      false,
      base::BindOnce(&HttpProxyConnectJob::OnIOComplete,
                     base::Unretained(this)),
      params_->traffic_annotation());
}

int HttpProxyConnectJob::DoQuicProxyCreateStreamComplete(int result) {
  if (result < 0)
    return result;

  next_state_ = STATE_HTTP_PROXY_CONNECT_COMPLETE;
  std::unique_ptr<QuicChromiumClientStream::Handle> quic_stream =
      quic_session_->ReleaseStream();

  spdy::SpdyPriority spdy_priority =
      ConvertRequestPriorityToQuicPriority(kH2QuicTunnelPriority);
  spdy::SpdyStreamPrecedence precedence(spdy_priority);
  quic_stream->SetPriority(precedence);

  transport_socket_ = std::make_unique<QuicProxyClientSocket>(
      std::move(quic_stream), std::move(quic_session_),
      ProxyServer(GetProxyServerScheme(), GetDestination()), GetUserAgent(),
      params_->endpoint(), net_log(), http_auth_controller_,
      common_connect_job_params()->proxy_delegate);
  return transport_socket_->Connect(base::BindOnce(
      &HttpProxyConnectJob::OnIOComplete, base::Unretained(this)));
}

int HttpProxyConnectJob::DoRestartWithAuth() {
  DCHECK(transport_socket_);

  // Start the timeout timer again.
  ResetTimer(kHttpProxyConnectJobTunnelTimeout);

  next_state_ = STATE_RESTART_WITH_AUTH_COMPLETE;
  return transport_socket_->RestartWithAuth(base::BindOnce(
      &HttpProxyConnectJob::OnIOComplete, base::Unretained(this)));
}

int HttpProxyConnectJob::DoRestartWithAuthComplete(int result) {
  DCHECK_NE(ERR_IO_PENDING, result);

  if (result == OK && !transport_socket_->IsConnected())
    result = ERR_UNABLE_TO_REUSE_CONNECTION_FOR_PROXY_AUTH;

  // If the connection could not be reused to attempt to send proxy auth
  // credentials, try reconnecting. Do not reset the HttpAuthController in this
  // case; the server may, for instance, send "Proxy-Connection: close" and
  // expect that each leg of the authentication progress on separate
  // connections.
  bool reconnect = result == ERR_UNABLE_TO_REUSE_CONNECTION_FOR_PROXY_AUTH;

  // If auth credentials were sent but the connection was closed, the server may
  // have timed out while the user was selecting credentials. Retry once.
  if (!has_restarted_ &&
      (result == ERR_CONNECTION_CLOSED || result == ERR_CONNECTION_RESET ||
       result == ERR_CONNECTION_ABORTED ||
       result == ERR_SOCKET_NOT_CONNECTED)) {
    reconnect = true;
    has_restarted_ = true;

    // Release any auth state bound to the connection. The new connection will
    // start the current scheme and identity from scratch.
    if (http_auth_controller_)
      http_auth_controller_->OnConnectionClosed();
  }

  if (reconnect) {
    // Attempt to create a new one.
    transport_socket_.reset();
    next_state_ = STATE_BEGIN_CONNECT;
    return OK;
  }

  // If not reconnecting, treat the result as the result of establishing a
  // tunnel through the proxy. This is important in the case another auth
  // challenge is seen.
  next_state_ = STATE_HTTP_PROXY_CONNECT_COMPLETE;
  return result;
}

void HttpProxyConnectJob::ChangePriorityInternal(RequestPriority priority) {
  // Do not set the priority on |spdy_stream_request_| or
  // |quic_stream_request_|, since those should always use
  // kH2QuicTunnelPriority.
  if (nested_connect_job_)
    nested_connect_job_->ChangePriority(priority);

  if (transport_socket_)
    transport_socket_->SetStreamPriority(priority);
}

void HttpProxyConnectJob::OnTimedOutInternal() {
  if (next_state_ == STATE_TRANSPORT_CONNECT_COMPLETE) {
    base::UmaHistogramMediumTimes(
        GetProxyServerScheme() == ProxyServer::SCHEME_HTTP
            ? "Net.HttpProxy.ConnectLatency.Insecure.TimedOut"
            : "Net.HttpProxy.ConnectLatency.Secure.TimedOut",
        base::TimeTicks::Now() - connect_start_time_);
  }
}

void HttpProxyConnectJob::OnAuthChallenge() {
  // Stop timer while potentially waiting for user input.
  ResetTimer(base::TimeDelta());

  NotifyDelegateOfProxyAuth(
      *transport_socket_->GetConnectResponseInfo(),
      transport_socket_->GetAuthController().get(),
      base::BindOnce(&HttpProxyConnectJob::RestartWithAuthCredentials,
                     weak_ptr_factory_.GetWeakPtr()));
}

const HostPortPair& HttpProxyConnectJob::GetDestination() const {
  const TransportSocketParams* transport_params;
  if (params_->transport_params()) {
    transport_params = params_->transport_params().get();
  } else {
    transport_params = params_->ssl_params()->GetDirectConnectionParams().get();
  }

  // TODO(crbug.com/1206799): Handle proxy destination with scheme.
  DCHECK(
      absl::holds_alternative<HostPortPair>(transport_params->destination()));
  return absl::get<HostPortPair>(transport_params->destination());
}

std::string HttpProxyConnectJob::GetUserAgent() const {
  if (!http_user_agent_settings())
    return std::string();
  return http_user_agent_settings()->GetUserAgent();
}

SpdySessionKey HttpProxyConnectJob::CreateSpdySessionKey() const {
  return SpdySessionKey(
      GetDestination(), ProxyServer::Direct(), PRIVACY_MODE_DISABLED,
      SpdySessionKey::IsProxySession::kTrue, socket_tag(),
      params_->network_isolation_key(),
      params_->ssl_params()->GetDirectConnectionParams()->secure_dns_policy());
}

}  // namespace net
