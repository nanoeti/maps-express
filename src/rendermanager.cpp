#include "rendermanager.h"

#include <fstream>

#include <glog/logging.h>

#include "subtiler.h"


static bool ParseStyleInfo(const std::string& name, const Json::Value& jstyle_info, StyleInfo& style_info) {
    style_info.name = name;
    if (style_info.name.empty()) {
        LOG(ERROR) << "Invalid style node name: " << name;
        return false;
    }

    const Json::Value& jmap_path = jstyle_info["map"];
    if (!jmap_path.isString()) {
        if (jmap_path.isNull()) {
            LOG(ERROR) << "No map path for style " << style_info.name << " provided!";
        } else {
            LOG(ERROR) << "Map path should have string type!";
        }
        return false;
    }
    style_info.path = jmap_path.asString();

    const Json::Value& jutfgrid_allowed = jstyle_info["allow_utfgrid"];
    if (!jutfgrid_allowed.isBool()) {
        style_info.allow_grid_render = jstyle_info["allow_utfgrid"].asBool();
    } else {
        LOG(WARNING) << "allow_utfgrid should have bool type!";
    }
    const Json::Value& jversion = jstyle_info["version"];
    if (jversion.isUInt()) {
        style_info.version = jversion.asUInt();
    }
    return true;
}


static bool ParseStyles(const Json::Value& jstyles, std::vector<StyleInfo>& styles) {
    if (!jstyles.isObject()) {
        return false;
    }
    for (Json::ValueConstIterator jstyle_itr = jstyles.begin(); jstyle_itr != jstyles.end(); ++jstyle_itr) {
        const std::string& style_name = jstyle_itr.name();
        styles.emplace_back();
        if (!ParseStyleInfo(style_name, *jstyle_itr, styles.back())) {
            styles.pop_back();
            return false;
        }
    }
    return true;
}


void StyleUpdateObserver::OnUpdate(std::shared_ptr<Json::Value> value) {
    assert(value);
    rm_.PostStyleUpdate(std::move(value));
}


RenderManager::RenderManager(Config& config) :
        style_names_(std::make_shared<std::unordered_set<std::string>>()),
        update_observer_(*this),
        config_(config)
{
    std::shared_ptr<const Json::Value> jqueue_limit_ptr = config.GetValue("render/queue_limit");
    assert(jqueue_limit_ptr);
    const Json::Value& jqueue_limit = *jqueue_limit_ptr;
    uint queue_limit = jqueue_limit.isIntegral() ? jqueue_limit.asUInt() : 1000u;
    render_pool_.SetQueueLimit(queue_limit);

    std::shared_ptr<std::vector<StyleInfo>> styles;
    std::shared_ptr<const Json::Value> jstyles = config.GetValue("render/styles", &update_observer_);
    if (jstyles->isObject()) {
        styles = std::make_shared<std::vector<StyleInfo>>();
        for (Json::ValueConstIterator jstyle_itr = jstyles->begin(); jstyle_itr != jstyles->end(); ++jstyle_itr) {
            const std::string& style_name = jstyle_itr.name();
            styles->emplace_back();
            if (!ParseStyleInfo(style_name, *jstyle_itr, styles->back())) {
                styles->pop_back();
                continue;
            }
            if (style_names_->find(style_name) != style_names_->end()) {
                LOG(ERROR) << "Duplicate style name: " << style_name;
                styles->pop_back();
                continue;
            }
            style_names_->insert(std::move(style_name));
        }
    } else {
        LOG(WARNING) << "No styles provided";
    }

    std::shared_ptr<const Json::Value> jworkers_ptr = config.GetValue("render/workers");
    assert(jworkers_ptr);
    const Json::Value& jworkers = *jworkers_ptr;
    uint num_workers = jworkers.isIntegral() ? jworkers.asUInt() : std::thread::hardware_concurrency();
    for (uint i = 0; i < num_workers; ++i) {
        auto render_worker = std::make_unique<RenderWorker>(styles);
        render_pool_.PushWorker(std::move(render_worker));
    }

    // Check if we already have style updates
    inited_ = true;
    TryProcessStyleUpdate();
}

std::shared_ptr<RenderTask> RenderManager::Render(std::unique_ptr<RenderRequest> request,
                                                  std::function<void (render_result_t&&)> success_callback,
                                                  std::function<void ()> error_callback) {
    assert(request);
    auto task = std::make_shared<RenderTask>(std::move(success_callback), std::move(error_callback), true);
    if (!has_style(request->style_name)) {
        task->NotifyError();
        return task;
    }
    render_pool_.PostTask(TileWorkTask{task, std::move(request)});
    return task;
}

bool RenderManager::RenderSync(std::unique_ptr<RenderRequest>, render_result_t& output) {
    // TODO
    return false;
}

std::shared_ptr<RenderTask> RenderManager::MakeSubtile(std::unique_ptr<SubtileRequest> request,
                                                       std::function<void (render_result_t&&)> success_callback,
                                                       std::function<void ()> error_callback) {
    assert(request);
    auto task = std::make_shared<RenderTask>(std::move(success_callback), std::move(error_callback), true);
    if (!(request->mvt_tile.id.Valid() && request->tile_id.Valid())) {
        LOG(ERROR) << "Invalid tile id!";
        task->NotifyError();
        return task;
    }
    render_pool_.PostTask(TileWorkTask{task, std::move(request)});
    return task;
}

void RenderManager::PostStyleUpdate(std::shared_ptr<const Json::Value> jstyles) {
    assert(jstyles);
    std::atomic_store(&styles_update_, std::move(jstyles));
    TryProcessStyleUpdate();
}

void RenderManager::TryProcessStyleUpdate() {
    if (!inited_ || !std::atomic_load(&styles_update_)) {
        return;
    }
    bool expected = false;
    if (!updating_.compare_exchange_strong(expected, true)) {
        return;
    }
    std::shared_ptr<const Json::Value> jstyles = std::atomic_exchange(&styles_update_,
                                                                      std::shared_ptr<const Json::Value>());
    if (!jstyles || !ParseStyles(*jstyles, pending_update_)) {
        FinishUpdate();
        return;
    }

    workers_to_update_ = render_pool_.Workers();
    if (workers_to_update_.empty()) {
        LOG(WARNING) << "Render pool has no workers! Skipping update!";
    }
    render_pool_.ExecuteOnWorker(std::bind(&RenderManager::UpdateWorker, this, std::placeholders::_1),
                                 workers_to_update_.back());
}

void RenderManager::UpdateWorker(RenderWorker& worker) {
    if (!worker.UpdateStyles(pending_update_)) {
        LOG(ERROR) << "Error updating worker " << workers_to_update_.size() << ". Cancelling update!";
        const auto* pending_update_ptr = &pending_update_;
        for (const RenderWorker* rw : updated_workers_) {
            render_pool_.ExecuteOnWorker([pending_update_ptr](RenderWorker& wrk) {
                wrk.CancelUpdate(pending_update_ptr);
            }, rw);
        }
        FinishUpdate();
    } else {
        updated_workers_.push_back(workers_to_update_.back());
        workers_to_update_.pop_back();
        if (workers_to_update_.empty()) {
            // All workers updated
            const auto* pending_update_ptr = &pending_update_;
            for (const RenderWorker* rw : updated_workers_) {
                render_pool_.ExecuteOnWorker([pending_update_ptr](RenderWorker& wrk) {
                    wrk.CommitUpdate(pending_update_ptr);
                }, rw);
            }
            // Update style names set
            auto new_style_names = std::make_shared<std::unordered_set<std::string>>();
            for (const StyleInfo& style_info : pending_update_) {
                new_style_names->insert(style_info.name);
            }
            std::atomic_store(&style_names_, std::move(new_style_names));
            FinishUpdate();
        } else {
            // Update next worker
            render_pool_.ExecuteOnWorker(std::bind(&RenderManager::UpdateWorker, this, std::placeholders::_1),
                                         workers_to_update_.back());
        }
    }
}

void RenderManager::FinishUpdate() {
    workers_to_update_.clear();
    updated_workers_.clear();
    pending_update_.clear();
    updating_ = false;
    TryProcessStyleUpdate();
}
