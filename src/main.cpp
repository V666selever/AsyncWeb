#include <Geode/Geode.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/modify/CCHttpClient.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace geode::prelude;

class MyCCHttpRequest : public CCHttpRequest {
public:
    void setProgress(int progress) {
        _downloadProgress = progress;
    }
    bool shouldCancel() {
        return _shouldCancel;
    }
};

namespace {
    // v5: no more $objectModify / alphas_geode_utils. Per-request state lives in a
    // map keyed by a unique id instead of in fields on CCHttpRequest.
    // Only ever touched from the main thread.
    struct PendingRequest {
        CCHttpRequest* request = nullptr;
        async::TaskHolder<web::WebResponse> holder;
    };

    std::unordered_map<uint64_t, std::unique_ptr<PendingRequest>> s_pending;
    uint64_t s_nextId = 0;

    // Drops the entry (destroying the TaskHolder aborts the task if it is still
    // running) and releases the request exactly once, like the old code did.
    void finishRequest(uint64_t id) {
        auto it = s_pending.find(id);
        if (it == s_pending.end()) return;

        CCHttpRequest* request = it->second->request;
        s_pending.erase(it);
        request->release();
    }
}

class $modify(MyCCHttpClient, CCHttpClient) {

    void send(CCHttpRequest* request) {

        uint64_t id = ++s_nextId;
        auto& entry = *(s_pending[id] = std::make_unique<PendingRequest>());
        entry.request = request;

        web::WebRequest req;

        auto start = reinterpret_cast<uint8_t*>(request->getRequestData());
        std::vector<uint8_t> bytes(start, start + request->getRequestDataSize());

        if (!bytes.empty()) {
            req.body(bytes);
        }

        req.userAgent("");
        req.version(web::HttpVersion::VERSION_2_0);

        // Progress callbacks don't run on the main thread anymore, so hop over
        // before touching the request, and make sure it is still alive.
        req.onProgress([id](web::WebProgress const& progress) {
            auto pr = progress.downloadProgress();
            queueInMainThread([id, pr] {
                auto it = s_pending.find(id);
                if (it == s_pending.end()) return;

                auto myRequest = static_cast<MyCCHttpRequest*>(it->second->request);
                if (myRequest->shouldCancel()) {
                    finishRequest(id);
                    return;
                }
                if (pr.has_value()) {
                    myRequest->setProgress(static_cast<int>(pr.value()));
                }
            });
        });

        Ref<CCHttpResponse> response = new CCHttpResponse(request);
        CCHttpClient* client = this;
        std::string url = request->getUrl();

        auto task = [&] {
            switch (request->getRequestType()) {
                case CCHttpRequest::kHttpGet:
                    return req.get(url);
                case CCHttpRequest::kHttpPut:
                    return req.put(url);
                case CCHttpRequest::kHttpPost:
                default:
                    return req.post(url);
            }
        }();

        // The callback runs on the main thread.
        entry.holder.spawn(std::move(task), [id, client, response](web::WebResponse res) {
            auto it = s_pending.find(id);
            if (it == s_pending.end()) return;

            auto myRequest = static_cast<MyCCHttpRequest*>(it->second->request);

            response->setSucceed(res.ok());
            response->setResponseCode(res.code());

            gd::vector<uint8_t> data = res.data();
            response->setResponseData(reinterpret_cast<gd::vector<char>*>(&data));

            SEL_HttpResponse pSelector = myRequest->getSelector();
            CCObject* pTarget = myRequest->getTarget();

            if (pTarget && pSelector) {
                (pTarget->*pSelector)(client, response);
            }

            // Don't destroy the TaskHolder from inside its own callback.
            queueInMainThread([id] {
                finishRequest(id);
            });
        });
    }
};