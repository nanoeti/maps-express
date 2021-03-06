#include <csignal>

#include <mapnik/config.hpp>
#include <mapnik/datasource_cache.hpp>

#include <folly/Memory.h>
#include <folly/io/async/EventBaseManager.h>
#include <proxygen/lib/http/HTTPMessage.h>
#include <proxygen/httpserver/HTTPServer.h>

#include "etcd_config.h"
#include "httphandlerfactory.h"
#include "json_config.h"
#include "json_util.h"
#include "nodes_monitor.h"
#include "status_monitor.h"


using folly::EventBase;
using folly::EventBaseManager;
using folly::SocketAddress;

using HTTPServer = proxygen::HTTPServer;
using Protocol = HTTPServer::Protocol;

using json_util::FromJson;

static const double kVersion = 0.4;

static const uint kDefaultPort = 8080;
static const std::string kDefaultIp = "0.0.0.0";

static const std::string kHelpStr = R"help(
Maps Express.

Usage:
    maps-express <host> json <json-config-path>
    maps-express <host> etcd <etcd-host>
)help";

namespace {

class SignalHandler {
public:
    SignalHandler(std::shared_ptr<StatusMonitor> monitor, HTTPServer& server, NodesMonitor* nodes_monitor) :
            monitor_(std::move(monitor)),
            server_(server),
            nodes_monitor_(nodes_monitor) {}

    void HandleSighup() {
        StatusMonitor::Status prev_status = monitor_->exchange_status(StatusMonitor::Status::maintenance);
        if (prev_status == StatusMonitor::Status::maintenance) {
            return;
        }
        LOG(INFO) << "Switching to maintenance mode!";
        if (nodes_monitor_) {
            nodes_monitor_->Unregister();
        }
        std::this_thread::sleep_for(std::chrono::seconds(10));
        LOG(INFO) << "Stopping server!";
        server_.stop();
    }

private:
    std::shared_ptr<StatusMonitor> monitor_;
    HTTPServer& server_;
    NodesMonitor* nodes_monitor_;
};


class EtcdHelper {
public:
    // EtcdClient owns event base thread. EtcdConfig's and NodesMonitor's loops run in that thread.
    EtcdHelper(const std::string& etcd_host, const std::string& server_host, uint server_port) :
            client_(std::make_shared<EtcdClient>(etcd_host, 2379u, 3)),
            config(client_),
            nodes_monitor(server_host, server_port, client_) {}

    ~EtcdHelper() {
        // Stop EtcdClient, EtcdConfig's and NodesMonitor's loops will be stopped to.
        client_->Shutdown();
    }

private:
    std::shared_ptr<EtcdClient> client_;

public:
    EtcdConfig config;
    NodesMonitor nodes_monitor;
};

std::atomic<SignalHandler*> gSignalHandler{nullptr};

} // ns anonymous

void signal_handler(int signal) {
    SignalHandler* handler_ptr = gSignalHandler;
    if (handler_ptr != nullptr) {
        handler_ptr->HandleSighup();
    }
}


static void PrintHelpAndExit() {
    std::cout << kHelpStr << std::endl;
    std::exit(1);
}


int main(int argc, char* argv[]) {
    std::signal(SIGHUP, signal_handler);
    google::InitGoogleLogging(argv[0]);
    google::InstallFailureSignalHandler();

    if (argc < 4) {
        PrintHelpAndExit();
    }
    const std::string host = argv[1];
    const std::string config_type = argv[2];

    std::unique_ptr<JsonConfig> json_config;
    std::unique_ptr<EtcdHelper> etcd_helper;
    Config* config = nullptr;
    if (config_type == "json") {
        json_config = std::make_unique<JsonConfig>(argv[3]);
        config = json_config.get();
    } else if (config_type == "etcd") {
        etcd_helper = std::make_unique<EtcdHelper>(argv[3], host, kDefaultPort);
        config = &etcd_helper->config;
    } else {
        std::cout << "Invlid config type: " << config_type << "\n" << std::endl;
        PrintHelpAndExit();
    }

    assert(config);
    if (!config->Valid()) {
        LOG(FATAL) << "Unable to load config!";
        return -1;
    }

    auto dscache = mapnik::datasource_cache::instance;
    if(!dscache().register_datasources(MAPNIK_PLUGINDIR)){
        LOG(FATAL) << "could not register postgis plugin";
    }

    std::shared_ptr<const Json::Value> japp_ptr = config->GetValue("app");
    assert(japp_ptr);
    const Json::Value& japp = *japp_ptr;
    FLAGS_log_dir = japp["log_dir"].asString().c_str();

    std::shared_ptr<const Json::Value> jserver_ptr = config->GetValue("server");
    assert(jserver_ptr);
    const Json::Value& jserver = *jserver_ptr;

    uint http_port = FromJson<uint>(jserver["port"], kDefaultPort);
    uint internal_http_port = FromJson<uint>(jserver["internal_port"], kDefaultPort + 1);

    std::vector<HTTPServer::IPConfig> IPs = {
        {SocketAddress(host, static_cast<std::uint16_t>(http_port), true), Protocol::HTTP},
        {SocketAddress(host, static_cast<std::uint16_t>(internal_http_port), true), Protocol::HTTP},
    };

    auto monitor = std::make_shared<StatusMonitor>();
    NodesMonitor* nodes_monitor = nullptr;
    if (etcd_helper) {
        nodes_monitor = &etcd_helper->nodes_monitor;
    }

    proxygen::HTTPServerOptions options;
    options.threads = std::thread::hardware_concurrency();
    options.idleTimeout = std::chrono::milliseconds(60000);
    options.shutdownOn = {SIGINT, SIGTERM};
    options.enableContentCompression = true;
    options.contentCompressionLevel = 5;
    options.handlerFactories = proxygen::RequestHandlerChain()
        .addThen<HttpHandlerFactory>(*config, monitor, nodes_monitor)
        .build();

    LOG(INFO) << "starting... " << japp["name"].asString() << " " << japp["version"].asString() << std::endl;

    HTTPServer server(std::move(options));
    server.bind(IPs);


    // Start HTTPServer mainloop in a separate thread
    std::thread t([&] () {
    LOG(INFO) << "running... " << japp["name"].asString() << " " << japp["version"].asString() << std::endl;
        server.start();
    });

    SignalHandler sh(monitor, server, nodes_monitor);
    gSignalHandler.store(&sh);

    t.join();
    std::signal(SIGHUP, SIG_DFL);

    return 0;
}
