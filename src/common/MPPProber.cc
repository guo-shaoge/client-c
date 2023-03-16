#include <kvproto/mpp.pb.h>
#include <pingcap/Exception.h>
#include <pingcap/common/MPPProber.h>
#include <pingcap/kv/Cluster.h>

#include <thread>

namespace pingcap
{
namespace common
{

bool MPPProber::isRecovery(const std::string & store_addr, const std::chrono::seconds & recovery_ttl)
{
    FailedStoreMap copy_failed_stores;
    {
        std::lock_guard<std::mutex> lock(store_lock);
        copy_failed_stores = failed_stores;
    }
    auto iter = copy_failed_stores.find(store_addr);
    if (iter == copy_failed_stores.end())
        return true;

    {
        std::lock_guard<std::mutex> lock(iter->second->state_lock);
        return iter->second->recovery_timepoint != INVALID_TIME_POINT && getElapsed(iter->second->recovery_timepoint) > recovery_ttl;
    }
}

void MPPProber::add(const std::string & store_addr)
{
    std::lock_guard<std::mutex> lock(store_lock);
    auto iter = failed_stores.find(store_addr);
    if (iter == failed_stores.end())
    {
        failed_stores[store_addr] = std::make_shared<ProbeState>(store_addr, cluster);
    }
    else
    {
        iter->second->last_lookup_timepoint = std::chrono::steady_clock::now();
    }
}

void MPPProber::run()
{
    while (!stopped.load())
    {
        std::this_thread::sleep_for(std::chrono::seconds(scan_interval));
        scan();
    }
}

void MPPProber::stop()
{
    stopped.store(true);
}

void MPPProber::scan()
{
    FailedStoreMap copy_failed_stores;
    {
        std::lock_guard<std::mutex> guard(store_lock);
        copy_failed_stores = failed_stores;
    }
    std::vector<std::string> recovery_stores;
    recovery_stores.reserve(copy_failed_stores.size());

    for (const auto & ele : copy_failed_stores)
    {
        if (!ele.second->state_lock.try_lock())
            continue;

        ele.second->detectAndUpdateState(detect_period, detect_rpc_timeout);

        const auto & recovery_timepoint = ele.second->recovery_timepoint;
        if (recovery_timepoint != INVALID_TIME_POINT)
        {
            // Means this store is now alive.
            if (getElapsed(recovery_timepoint) > std::chrono::duration_cast<std::chrono::seconds>(MAX_RECOVERY_TIME_LIMIT))
            {
                recovery_stores.push_back(ele.first);
            }
        }
        else
        {
            // Store is dead, we want to check if this store has not used for MAX_OBSOLET_TIME.
            if (getElapsed(ele.second->last_lookup_timepoint) > std::chrono::duration_cast<std::chrono::seconds>(MAX_OBSOLET_TIME_LIMIT))
                recovery_stores.push_back(ele.first);
        }
        ele.second->state_lock.unlock();
    }

    {
        std::lock_guard<std::mutex> guard(store_lock);
        for (const auto & store : recovery_stores)
        {
            failed_stores.erase(store);
        }
    }
}

void ProbeState::detectAndUpdateState(const std::chrono::seconds & detect_period, size_t detect_rpc_timeout)
{
    if (getElapsed(last_detect_timepoint) < detect_period)
        return;

    bool is_alive = detectStore(cluster->rpc_client, store_addr, detect_rpc_timeout, log);
    if (!is_alive)
    {
        log->debug("got dead store: " + store_addr);
        recovery_timepoint = INVALID_TIME_POINT;
    }
    else if (recovery_timepoint == INVALID_TIME_POINT)
    {
        // Alive store, and first recovery, set its recovery time.
        recovery_timepoint = std::chrono::steady_clock::now();
    }
}

bool detectStore(kv::RpcClientPtr & rpc_client, const std::string & store_addr, int rpc_timeout, Logger * log)
{
    kv::RpcCall<::mpp::IsAliveRequest> rpc(std::make_shared<::mpp::IsAliveRequest>());
    try
    {
        rpc_client->sendRequest(store_addr, rpc, rpc_timeout, kv::GRPCMetaData{});
    }
    catch (const Exception & e)
    {
        log->warning("detect failed: " + store_addr + " error: ", e.message());
        return false;
    }

    const auto & resp = rpc.getResp();
    return resp->available();
}

} // namespace common
} // namespace pingcap
