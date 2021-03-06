#include "httphandlerfactory.h"

#include "couchbase_cacher.h"
#include "json_util.h"
#include "mon_handler.h"
#include "tile_handler.h"
#include "util.h"


using json_util::FromJson;
using json_util::FromJsonOrErr;

using endpoint_t = HttpHandlerFactory::endpoint_t;
using endpoints_map_t = std::unordered_map<std::string, endpoint_t>;


class ServerUpdateObserver : public Config::ConfigObserver {
public:
    ServerUpdateObserver(HttpHandlerFactory& hhf) : hhf_(hhf) {}

private:
    void OnUpdate(std::shared_ptr<Json::Value> value) override {
        hhf_.UpdateConfig(std::move(value));
    }

    HttpHandlerFactory& hhf_;
};


static std::shared_ptr<endpoints_map_t> ParseEndpoints(const Json::Value jendpoints) {
    if (!jendpoints.isObject()) {
        return nullptr;
    }

    auto endpoints_map = std::make_shared<endpoints_map_t>(jendpoints.size());
    for (auto itr = jendpoints.begin() ; itr != jendpoints.end() ; ++itr) {
        const std::string endpoint_path = itr.key().asString();
        if (endpoints_map->find(endpoint_path) != endpoints_map->end()) {
            LOG(ERROR) << "Duplicate endpoint path: " << endpoint_path;
            continue;
        }
        endpoint_t endpoint;
        const Json::Value& jendpoint = *itr;
        for (const auto& jparams : jendpoint) {
            auto params = std::make_shared<EndpointParams>();
            params->minzoom = FromJson<int>(jparams["minzoom"], 0);
            params->maxzoom = FromJson<int>(jparams["maxzoom"], 19);
            params->zoom_offset = FromJson<int>(jparams["data_zoom_offset"], 0);
            params->provider_name = FromJson<std::string>(jparams["data_provider"], "");
            params->style_name = FromJson<std::string>(jparams["style"], "");
            params->allow_layers_query = FromJson<bool>(jparams["allow_layers_query"], false);
            std::string type = FromJson<std::string>(jparams["type"], "static");
            if (type == "static") {
                params->type = EndpointType::static_files;
                if (params->provider_name.empty()) {
                    LOG(ERROR) << "No loader name for endpoint '" << endpoint_path << "' provided!";
                    continue;
                }
            } else if (type == "render") {
                params->type = EndpointType::render;
                params->allow_utf_grid = FromJson<bool>(jparams["allow_utfgrid"], false);
                params->utfgrid_key = FromJson<std::string>(jparams["utfgrid_key"], "");
                if (params->allow_utf_grid && params->utfgrid_key.empty()) {
                    LOG(ERROR) << "No utfgrid key for endpoint '" << endpoint_path << "' provided!";
                    params->allow_utf_grid = false;
                }
                if (params->style_name.empty()) {
                    LOG(ERROR) << "No style name for endpoint '" << endpoint_path << "' provided!";
                    continue;
                }
            } else if (type == "mvt") {
                params->type = EndpointType::mvt;
                if (params->provider_name.empty()) {
                    LOG(ERROR) << "No loader name for endpoint '" << endpoint_path << "' provided!";
                    continue;
                }
                const std::string filter_map_path = FromJson<std::string>(jparams["filter_map"], "");
                if (!filter_map_path.empty()) {
                    params->filter_table = std::make_shared<FilterTable>(filter_map_path, params->maxzoom);
                }
            } else {
                LOG(ERROR) << "Invalid type '" << type << "' for endpoint '" << endpoint_path << "' provided!";
                continue;
            }
            const Json::Value& jmetatile_size = jparams["metatile_size"];
            if (jmetatile_size.isString()) {
                if (jmetatile_size.asString() == "auto") {
                    if (params->provider_name.empty()) {
                        LOG(ERROR) << "Auto metatile size can be used only with data provider!";
                    } else {
                        params->auto_metatile_size = true;
                    }
                }
            } else if (jmetatile_size.isUInt()) {
                uint metatile_size = jmetatile_size.asUInt();
                params->metatile_height = metatile_size;
                params->metatile_width = metatile_size;
            } else {
                params->metatile_height = FromJson<uint>(jparams["metatile_height"], 1);
                params->metatile_width = FromJson<uint>(jparams["metatile_width"], 1);
            }
            endpoint.push_back(std::move(params));
        }
        (*endpoints_map)[endpoint_path] = std::move(endpoint);
    }
    return endpoints_map;
}

HttpHandlerFactory::HttpHandlerFactory(Config& config, std::shared_ptr<StatusMonitor> monitor,
                                       NodesMonitor* nodes_monitor) :
        monitor_(std::move(monitor)),
        render_manager_(config),
        data_manager_(config),
        config_(config),
        nodes_monitor_(nodes_monitor)
{
    update_observer_ = std::make_unique<ServerUpdateObserver>(*this);
    std::shared_ptr<const Json::Value> jserver_ptr = config.GetValue("server", update_observer_.get());
    assert(jserver_ptr);
    const Json::Value& jserver = *jserver_ptr;

    const Json::Value& jendpoints = jserver["endpoints"];
    auto endpoints_ptr = ParseEndpoints(jendpoints);
    if (!endpoints_ptr || endpoints_ptr->empty()) {
        LOG(WARNING) << "No endpoints provided";
    }
    std::atomic_store(&endpoints_, endpoints_ptr);

    auto jcacher_ptr = config.GetValue("cacher");
    if (jcacher_ptr) {
        const Json::Value& jcacher = *jcacher_ptr;
        const Json::Value& jhosts = jcacher["hosts"];
        if (jhosts.isArray()) {
            std::vector<std::string> hosts;
            for (const Json::Value& jhost : jhosts) {
                if (!jhost.isString()) {
                    LOG(ERROR) << "Couchbase hostname must be string!";
                    continue;
                }
                hosts.push_back(jhost.asString());
            }
            std::string user = FromJson<std::string>(jcacher["user"], "");
            std::string password = FromJson<std::string>(jcacher["password"], "");
            uint num_workers = FromJson<uint>(jcacher["workers"], 2);
            cacher_ = std::make_unique<CouchbaseCacher>(hosts, user, password, num_workers);
        };
    }
    if (!cacher_) {
        LOG(INFO) << "Starting without cacher";
    }
}

HttpHandlerFactory::~HttpHandlerFactory() {}

void HttpHandlerFactory::onServerStart(folly::EventBase* evb) noexcept {
    if (nodes_monitor_) {
        nodes_monitor_->Register();
    }
}

void HttpHandlerFactory::onServerStop() noexcept {
    if (nodes_monitor_) {
        nodes_monitor_->Unregister();
    }
}

proxygen::RequestHandler* HttpHandlerFactory::onRequest(proxygen::RequestHandler*,
                                                        proxygen::HTTPMessage* msg) noexcept {
    const std::string& path = msg->getPath();
//    if (path.back() != '/') {
//        path.push_back('/');
//    }
    const auto method = msg->getMethod();
    using proxygen::HTTPMethod;
    if (method == HTTPMethod::GET && path == "/mon") {
        return new MonHandler(monitor_);
    }
    auto endpoints = std::atomic_load(&endpoints_);
    return new TileHandler(render_manager_, data_manager_, endpoints, cacher_.get());
}


bool HttpHandlerFactory::UpdateConfig(std::shared_ptr<Json::Value> update) {
    auto endpoints_map = ParseEndpoints((*update)["endpoints"]);
    if (!endpoints_map) {
        return false;
    }
    std::atomic_store(&endpoints_, endpoints_map);
    return true;
}
