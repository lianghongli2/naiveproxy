// Copyright 2018 The Chromium Authors. All rights reserved.
// Copyright 2018 klzgrad <kizdiv@gmail.com>. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstdlib>
#include <limits>
#include <memory>
#include <string>

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/message_loop/message_loop.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/sys_info.h"
#include "base/task/task_scheduler/task_scheduler.h"
#include "base/values.h"
#include "build/build_config.h"
#include "components/version_info/version_info.h"
#include "net/base/auth.h"
#include "net/dns/host_resolver.h"
#include "net/dns/mapped_host_resolver.h"
#include "net/http/http_auth.h"
#include "net/http/http_auth_cache.h"
#include "net/http/http_network_session.h"
#include "net/http/http_transaction_factory.h"
#include "net/log/file_net_log_observer.h"
#include "net/log/net_log.h"
#include "net/log/net_log_capture_mode.h"
#include "net/log/net_log_entry.h"
#include "net/log/net_log_source.h"
#include "net/log/net_log_util.h"
#include "net/proxy_resolution/proxy_config.h"
#include "net/proxy_resolution/proxy_config_service_fixed.h"
#include "net/proxy_resolution/proxy_config_with_annotation.h"
#include "net/proxy_resolution/proxy_resolution_service.h"
#include "net/socket/client_socket_pool_manager.h"
#include "net/socket/ssl_client_socket.h"
#include "net/socket/tcp_server_socket.h"
#include "net/ssl/ssl_key_logger_impl.h"
#include "net/tools/naive/naive_proxy.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "net/url_request/url_request_context.h"
#include "net/url_request/url_request_context_builder.h"
#include "url/gurl.h"
#include "url/scheme_host_port.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"

#if defined(OS_MACOSX)
#include "base/mac/scoped_nsautorelease_pool.h"
#endif

namespace {

constexpr int kListenBackLog = 512;
constexpr int kDefaultMaxSocketsPerPool = 256;
constexpr int kDefaultMaxSocketsPerGroup = 255;
constexpr int kExpectedMaxUsers = 8;
constexpr char kDefaultHostName[] = "example";
constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("naive", "");

struct Params {
  std::string listen_addr;
  int listen_port;
  net::NaiveProxy::Protocol protocol;
  bool is_quic;
  bool use_proxy;
  std::string proxy_url;
  bool is_quic_proxy;
  std::string proxy_user;
  std::string proxy_pass;
  std::string host_resolver_rules;
  logging::LoggingSettings log_settings;
  base::FilePath net_log_path;
  base::FilePath ssl_key_path;
  base::FilePath certificate_file;
  base::FilePath key_file;
};

std::unique_ptr<base::Value> GetConstants(
    const base::CommandLine::StringType& command_line_string) {
  auto constants_dict = net::GetNetConstants();
  DCHECK(constants_dict);

  // Add a dictionary with the version of the client and its command line
  // arguments.
  auto dict = std::make_unique<base::DictionaryValue>();

  // We have everything we need to send the right values.
  std::string os_type = base::StringPrintf(
      "%s: %s (%s)", base::SysInfo::OperatingSystemName().c_str(),
      base::SysInfo::OperatingSystemVersion().c_str(),
      base::SysInfo::OperatingSystemArchitecture().c_str());
  dict->SetString("os_type", os_type);
  dict->SetString("command_line", command_line_string);

  constants_dict->Set("clientInfo", std::move(dict));

  return std::move(constants_dict);
}

// Builds a URLRequestContext assuming there's only a single loop.
std::unique_ptr<net::URLRequestContext> BuildURLRequestContext(
    const Params& params,
    net::NetLog* net_log) {
  net::URLRequestContextBuilder builder;

  builder.DisableHttpCache();
  builder.set_net_log(net_log);

  net::ProxyConfig proxy_config;
  if (params.use_proxy) {
    std::string proxy_url = params.proxy_url;
    if (params.is_quic_proxy) {
      proxy_url = base::StrCat({"quic://", proxy_url.substr(sizeof("https://") - 1)});
    }
    proxy_config.proxy_rules().ParseFromString(proxy_url);
  }
  auto proxy_service = net::ProxyResolutionService::CreateWithoutProxyResolver(
      std::make_unique<net::ProxyConfigServiceFixed>(
          net::ProxyConfigWithAnnotation(proxy_config, kTrafficAnnotation)),
      net_log);
  proxy_service->ForceReloadProxyConfig();
  builder.set_proxy_resolution_service(std::move(proxy_service));

  if (!params.host_resolver_rules.empty()) {
    auto remapped_resolver = std::make_unique<net::MappedHostResolver>(
        net::HostResolver::CreateDefaultResolver(net_log));
    remapped_resolver->SetRulesFromString(params.host_resolver_rules);
    builder.set_host_resolver(std::move(remapped_resolver));
  }

  auto context = builder.Build();

  if (params.use_proxy && !params.proxy_user.empty() && !params.proxy_pass.empty()) {
    net::HttpNetworkSession* session =
        context->http_transaction_factory()->GetSession();
    net::HttpAuthCache* auth_cache = session->http_auth_cache();
    GURL auth_origin(params.proxy_url);
    net::AuthCredentials credentials(base::ASCIIToUTF16(params.proxy_user),
                                     base::ASCIIToUTF16(params.proxy_pass));
    auth_cache->Add(auth_origin, /*realm=*/std::string(),
                    net::HttpAuth::AUTH_SCHEME_BASIC, /*challenge=*/"Basic",
                    credentials, /*path=*/"/");
  }

  return context;
}

bool ParseCommandLineFlags(Params* params) {
  const base::CommandLine& line = *base::CommandLine::ForCurrentProcess();

  if (line.HasSwitch("h") || line.HasSwitch("help")) {
    LOG(INFO) << "Usage: naive [options]\n"
                 "\n"
                 "Options:\n"
                 "-h, --help                 Show this message\n"
                 "--version                  Print version\n"
                 "--proto=[socks|http|quic]  Protocol to accept (socks)\n"
                 "--addr=<address>           Address to listen on (0.0.0.0)\n"
                 "--port=<port>              Port to listen on (1080)\n"
                 "--proxy=[https|quic]://<user>:<pass>@<hostname>[:<port>]\n"
                 "                           Proxy specification.\n"
                 "--certificate_file=<file>\n"
                 "--key_file=<file>\n"
                 "--log                      Log to stderr, otherwise no log\n"
                 "--log-net-log=<path>       Save NetLog\n"
                 "--ssl-key-log-file=<path>  Save SSL keys for Wireshark\n";
    exit(EXIT_SUCCESS);
    return false;
  }

  if (line.HasSwitch("version")) {
    LOG(INFO) << "Version: " << version_info::GetVersionNumber();
    exit(EXIT_SUCCESS);
    return false;
  }

  params->protocol = net::NaiveProxy::kSocks5;
  params->is_quic = false;
  if (line.HasSwitch("proto")) {
    const auto& proto = line.GetSwitchValueASCII("proto");
    if (proto == "socks") {
      params->protocol = net::NaiveProxy::kSocks5;
    } else if (proto == "http") {
      params->protocol = net::NaiveProxy::kHttp;
    } else if (proto == "quic") {
      params->is_quic = true;
    } else {
      LOG(ERROR) << "Invalid --proto";
      return false;
    }
  }

  params->listen_addr = "0.0.0.0";
  if (line.HasSwitch("addr")) {
    params->listen_addr = line.GetSwitchValueASCII("addr");
  }
  if (params->listen_addr.empty()) {
    LOG(ERROR) << "Invalid --addr";
    return false;
  }

  params->listen_port = 1080;
  if (line.HasSwitch("port")) {
    if (!base::StringToInt(line.GetSwitchValueASCII("port"),
                           &params->listen_port)) {
      LOG(ERROR) << "Invalid --port";
      return false;
    }
    if (params->listen_port <= 0 ||
        params->listen_port > std::numeric_limits<uint16_t>::max()) {
      LOG(ERROR) << "Invalid --port";
      return false;
    }
  }

  params->use_proxy = false;
  params->is_quic_proxy = false;
  std::string proxy_str = line.GetSwitchValueASCII("proxy");
  if (base::StartsWith(proxy_str, "quic://", base::CompareCase::SENSITIVE)) {
    proxy_str = base::StrCat({"https://", proxy_str.substr(sizeof("quic://") - 1)});
    params->is_quic_proxy = true;
  }
  GURL url(proxy_str);
  if (line.HasSwitch("proxy")) {
    params->use_proxy = true;
    if (!url.is_valid()) {
      LOG(ERROR) << "Invalid proxy URL";
      return false;
    }
    if (url.scheme() != "https") {
      LOG(ERROR) << "Must be HTTPS proxy";
      return false;
    }
    params->proxy_url = url::SchemeHostPort(url).Serialize();
    params->proxy_user = url.username();
    params->proxy_pass = url.password();
  }

  if (line.HasSwitch("host-resolver-rules")) {
    params->host_resolver_rules =
        line.GetSwitchValueASCII("host-resolver-rules");
  } else {
    // SNI should only contain DNS hostnames not IP addresses per RFC 6066.
    if (url.HostIsIPAddress()) {
      GURL::Replacements replacements;
      replacements.SetHostStr(kDefaultHostName);
      params->proxy_url =
          url::SchemeHostPort(url.ReplaceComponents(replacements)).Serialize();
      LOG(INFO) << "Using '" << kDefaultHostName << "' as the hostname for "
                << url.host();
      params->host_resolver_rules =
          std::string("MAP ") + kDefaultHostName + " " + url.host();
    }
  }

  if (line.HasSwitch("log")) {
    params->log_settings.logging_dest = logging::LOG_TO_SYSTEM_DEBUG_LOG;
  } else {
    params->log_settings.logging_dest = logging::LOG_NONE;
  }

  if (line.HasSwitch("log-net-log")) {
    params->net_log_path = line.GetSwitchValuePath("log-net-log");
  }

  if (line.HasSwitch("ssl-key-log-file")) {
    params->ssl_key_path = line.GetSwitchValuePath("ssl-key-log-file");
  }

  if (params->is_quic) {
    if (!line.HasSwitch("certificate_file")) {
      LOG(ERROR) << "Missing --certificate_file";
      return false;
    }
    if (!line.HasSwitch("key_file")) {
      LOG(ERROR) << "Missing --key_file";
      return false;
    }
    params->certificate_file = line.GetSwitchValuePath("certificate_file");
    params->key_file = line.GetSwitchValuePath("key_file");
  }

  return true;
}

// NetLog::ThreadSafeObserver implementation that simply prints events
// to the logs.
class PrintingLogObserver : public net::NetLog::ThreadSafeObserver {
 public:
  PrintingLogObserver() = default;

  ~PrintingLogObserver() override {
    // This is guaranteed to be safe as this program is single threaded.
    net_log()->RemoveObserver(this);
  }

  // NetLog::ThreadSafeObserver implementation:
  void OnAddEntry(const net::NetLogEntry& entry) override {
    switch (entry.type()) {
      case net::NetLogEventType::SOCKET_POOL_STALLED_MAX_SOCKETS:
      case net::NetLogEventType::SOCKET_POOL_STALLED_MAX_SOCKETS_PER_GROUP:
      case net::NetLogEventType::
          HTTP2_SESSION_STREAM_STALLED_BY_SESSION_SEND_WINDOW:
      case net::NetLogEventType::
          HTTP2_SESSION_STREAM_STALLED_BY_STREAM_SEND_WINDOW:
      case net::NetLogEventType::HTTP2_SESSION_STALLED_MAX_STREAMS:
      case net::NetLogEventType::HTTP2_STREAM_FLOW_CONTROL_UNSTALLED:
        break;
      default:
        return;
    }
    const char* const source_type =
        net::NetLog::SourceTypeToString(entry.source().type);
    const char* const event_type = net::NetLog::EventTypeToString(entry.type());
    const char* const event_phase =
        net::NetLog::EventPhaseToString(entry.phase());
    auto params = entry.ParametersToValue();
    std::string params_str;
    if (params.get()) {
      base::JSONWriter::Write(*params, &params_str);
      params_str.insert(0, ": ");
    }

    LOG(INFO) << source_type << "(" << entry.source().id << "): " << event_type
              << ": " << event_phase << params_str;
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(PrintingLogObserver);
};

}  // namespace

int main(int argc, char* argv[]) {
  base::TaskScheduler::CreateAndStartWithDefaultParams("naive");
  base::AtExitManager exit_manager;
  base::MessageLoopForIO main_loop;

#if defined(OS_MACOSX)
  base::mac::ScopedNSAutoreleasePool pool;
#endif

  base::CommandLine::Init(argc, argv);

  Params params;
  if (!ParseCommandLineFlags(&params)) {
    return EXIT_FAILURE;
  }

  net::ClientSocketPoolManager::set_max_sockets_per_pool(
      net::HttpNetworkSession::NORMAL_SOCKET_POOL,
      kDefaultMaxSocketsPerPool * kExpectedMaxUsers);
  net::ClientSocketPoolManager::set_max_sockets_per_proxy_server(
      net::HttpNetworkSession::NORMAL_SOCKET_POOL,
      kDefaultMaxSocketsPerPool * kExpectedMaxUsers);
  net::ClientSocketPoolManager::set_max_sockets_per_group(
      net::HttpNetworkSession::NORMAL_SOCKET_POOL,
      kDefaultMaxSocketsPerGroup * kExpectedMaxUsers);

  CHECK(logging::InitLogging(params.log_settings));

  if (!params.ssl_key_path.empty()) {
    net::SSLClientSocket::SetSSLKeyLogger(
        std::make_unique<net::SSLKeyLoggerImpl>(params.ssl_key_path));
  }

  // The declaration order for net_log and printing_log_observer is
  // important. The destructor of PrintingLogObserver removes itself
  // from net_log, so net_log must be available for entire lifetime of
  // printing_log_observer.
  net::NetLog net_log;
  std::unique_ptr<net::FileNetLogObserver> observer;
  if (!params.net_log_path.empty()) {
    const auto& cmdline =
        base::CommandLine::ForCurrentProcess()->GetCommandLineString();
    observer = net::FileNetLogObserver::CreateUnbounded(params.net_log_path,
                                                        GetConstants(cmdline));
    observer->StartObserving(&net_log, net::NetLogCaptureMode::Default());
  }
  PrintingLogObserver printing_log_observer;
  net_log.AddObserver(&printing_log_observer,
                      net::NetLogCaptureMode::Default());

  auto context = BuildURLRequestContext(params, &net_log);
  auto* session = context->http_transaction_factory()->GetSession();

  if (params->is_quic) {
    auto backend = std::make_unique<net::QuicNaiveProxyBackend>(session);
    quic::QuicConfig config;
    auto proof_source = std::make_unique<net::ProofSourceChromium>();
    CHECK(proof_source->Initialize(params.certificate_file, params.key_file, base::FilePath()));
    net::QuicSimpleServer server(
        std::move(proof_source),
        config, quic::QuicCryptoServerConfig::ConfigOptions(),
        quic::AllSupportedVersions(), backend.get());

    net::IPAddress ip;
    int result = net::ERR_ADDRESS_INVALID;
    if (ip.AssignFromIPLiteral(params.listen_addr)) {
      result = server.Listen(net::IPEndPoint(ip, params.listen_port));
    }
    if (result != net::OK) {
      LOG(ERROR) << "Failed to listen: " << result;
      return EXIT_FAILURE;
    }

    base::RunLoop().Run();

    return EXIT_SUCCESS;
  }

  auto listen_socket =
      std::make_unique<net::TCPServerSocket>(&net_log, net::NetLogSource());

  int result = listen_socket->ListenWithAddressAndPort(
      params.listen_addr, params.listen_port, kListenBackLog);
  if (result != net::OK) {
    LOG(ERROR) << "Failed to listen: " << result;
    return EXIT_FAILURE;
  }

  net::NaiveProxy naive_proxy(
      std::move(listen_socket), params.protocol, params.use_proxy,
      session, kTrafficAnnotation);

  base::RunLoop().Run();

  return EXIT_SUCCESS;
}
