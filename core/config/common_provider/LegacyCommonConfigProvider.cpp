// Copyright 2023 iLogtail Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "LegacyCommonConfigProvider.h"

#include <filesystem>
#include <iostream>
#include <random>

#include "json/json.h"

#include "app_config/AppConfig.h"
#include "application/Application.h"
#include "common/EncodingUtil.h"
#include "common/LogtailCommonFlags.h"
#include "common/StringTools.h"
#include "common/TimeUtil.h"
#include "common/http/Constant.h"
#include "common/http/Curl.h"
#include "common/version.h"
#include "logger/Logger.h"
#include "monitor/Monitor.h"

using namespace std;

DEFINE_FLAG_INT32(config_update_interval, "second", 10);

namespace logtail {

const string AGENT = "/Agent";

void LegacyCommonConfigProvider::Init(const string& dir) {
    ConfigProvider::Init(dir);

    const Json::Value& confJson = AppConfig::GetInstance()->GetConfig();

    // configserver path
    if (confJson.isMember("ilogtail_configserver_address") && confJson["ilogtail_configserver_address"].isArray()) {
        for (Json::Value::ArrayIndex i = 0; i < confJson["ilogtail_configserver_address"].size(); ++i) {
            vector<string> configServerAddress
                = SplitString(TrimString(confJson["ilogtail_configserver_address"][i].asString()), ":");

            if (configServerAddress.size() != 2) {
                LOG_WARNING(sLogger,
                            ("ilogtail_configserver_address", "format error")(
                                "wrong address", TrimString(confJson["ilogtail_configserver_address"][i].asString())));
                continue;
            }

            string host = configServerAddress[0];
            int32_t port = atoi(configServerAddress[1].c_str());

            if (port < 1 || port > 65535)
                LOG_WARNING(sLogger, ("ilogtail_configserver_address", "illegal port")("port", port));
            else
                mConfigServerAddresses.push_back(ConfigServerAddress(host, port));
        }

        mConfigServerAvailable = true;
        LOG_INFO(sLogger,
                 ("ilogtail_configserver_address", confJson["ilogtail_configserver_address"].toStyledString()));
    }

    // tags for configserver
    if (confJson.isMember("ilogtail_tags") && confJson["ilogtail_tags"].isObject()) {
        Json::Value::Members members = confJson["ilogtail_tags"].getMemberNames();
        for (Json::Value::Members::iterator it = members.begin(); it != members.end(); it++) {
            mConfigServerTags.push_back(confJson["ilogtail_tags"][*it].asString());
        }
        LOG_INFO(sLogger, ("ilogtail_configserver_tags", confJson["ilogtail_tags"].toStyledString()));
    }

    mThreadRes = async(launch::async, &LegacyCommonConfigProvider::CheckUpdateThread, this);
}

void LegacyCommonConfigProvider::Stop() {
    {
        lock_guard<mutex> lock(mThreadRunningMux);
        mIsThreadRunning = false;
    }
    mStopCV.notify_one();
    if (!mThreadRes.valid()) {
        return;
    }
    future_status s = mThreadRes.wait_for(chrono::seconds(1));
    if (s == future_status::ready) {
        LOG_INFO(sLogger, ("legacy common config provider", "stopped successfully"));
    } else {
        LOG_WARNING(sLogger, ("legacy common config provider", "forced to stopped"));
    }
}

void LegacyCommonConfigProvider::CheckUpdateThread() {
    LOG_INFO(sLogger, ("legacy common config provider", "started"));
    usleep((rand() % 10) * 100 * 1000);
    int32_t lastCheckTime = 0;
    unique_lock<mutex> lock(mThreadRunningMux);
    while (mIsThreadRunning) {
        int32_t curTime = time(NULL);
        if (curTime - lastCheckTime >= INT32_FLAG(config_update_interval)) {
            GetConfigUpdate();
            lastCheckTime = curTime;
        }
        if (mStopCV.wait_for(lock, chrono::seconds(3), [this]() { return !mIsThreadRunning; })) {
            break;
        }
    }
}

LegacyCommonConfigProvider::ConfigServerAddress
LegacyCommonConfigProvider::GetOneConfigServerAddress(bool changeConfigServer) {
    if (0 == mConfigServerAddresses.size()) {
        return ConfigServerAddress("", -1); // No address available
    }

    // Return a random address
    if (changeConfigServer) {
        random_device rd;
        int tmpId = rd() % mConfigServerAddresses.size();
        while (mConfigServerAddresses.size() > 1 && tmpId == mConfigServerAddressId) {
            tmpId = rd() % mConfigServerAddresses.size();
        }
        mConfigServerAddressId = tmpId;
    }
    return ConfigServerAddress(mConfigServerAddresses[mConfigServerAddressId].host,
                               mConfigServerAddresses[mConfigServerAddressId].port);
}

void LegacyCommonConfigProvider::GetConfigUpdate() {
    if (GetConfigServerAvailable()) {
        ConfigServerAddress configServerAddress = GetOneConfigServerAddress(false);
        google::protobuf::RepeatedPtrField<configserver::proto::ConfigCheckResult> checkResults;
        google::protobuf::RepeatedPtrField<configserver::proto::ConfigDetail> configDetails;

        checkResults = SendHeartbeat(configServerAddress);
        if (checkResults.size() > 0) {
            LOG_DEBUG(sLogger, ("fetch pipeline config, config file number", checkResults.size()));
            configDetails = FetchPipelineConfig(configServerAddress, checkResults);
            if (checkResults.size() > 0) {
                UpdateRemoteConfig(checkResults, configDetails);
            } else
                configServerAddress = GetOneConfigServerAddress(true);
        } else
            configServerAddress = GetOneConfigServerAddress(true);
    }
}

google::protobuf::RepeatedPtrField<configserver::proto::ConfigCheckResult>
LegacyCommonConfigProvider::SendHeartbeat(const ConfigServerAddress& configServerAddress) {
    configserver::proto::HeartBeatRequest heartBeatReq;
    configserver::proto::AgentAttributes attributes;
    string requestID = Base64Encode(string("heartbeat").append(to_string(time(NULL))));
    heartBeatReq.set_request_id(requestID);
    heartBeatReq.set_agent_id(Application::GetInstance()->GetInstanceId());
    heartBeatReq.set_agent_type("iLogtail");
    attributes.set_version(ILOGTAIL_VERSION);
    attributes.set_ip(LoongCollectorMonitor::mIpAddr);
    heartBeatReq.mutable_attributes()->MergeFrom(attributes);
    heartBeatReq.mutable_tags()->MergeFrom({GetConfigServerTags().begin(), GetConfigServerTags().end()});
    heartBeatReq.set_running_status("");
    heartBeatReq.set_startup_time(0);
    heartBeatReq.set_interval(INT32_FLAG(config_update_interval));

    google::protobuf::RepeatedPtrField<configserver::proto::ConfigInfo> pipelineConfigs;
    for (unordered_map<string, int64_t>::iterator it = mConfigNameVersionMap.begin(); it != mConfigNameVersionMap.end();
         it++) {
        configserver::proto::ConfigInfo* info = pipelineConfigs.Add();
        info->set_type(configserver::proto::PIPELINE_CONFIG);
        info->set_name(it->first);
        info->set_version(it->second);
    }
    heartBeatReq.mutable_pipeline_configs()->MergeFrom(pipelineConfigs);

    string operation = AGENT;
    operation.append("/").append("HeartBeat");
    map<string, string> httpHeader;
    httpHeader[CONTENT_TYPE] = TYPE_LOG_PROTOBUF;
    string reqBody;
    heartBeatReq.SerializeToString(&reqBody);

    google::protobuf::RepeatedPtrField<configserver::proto::ConfigCheckResult> emptyResult;
    HttpResponse httpResponse;
    if (!logtail::SendHttpRequest(make_unique<HttpRequest>(HTTP_POST,
                                                           false,
                                                           configServerAddress.host,
                                                           configServerAddress.port,
                                                           operation,
                                                           "",
                                                           httpHeader,
                                                           reqBody),
                                  httpResponse)) {
        LOG_WARNING(sLogger,
                    ("SendHeartbeat",
                     "fail")("reqBody", reqBody)("host", configServerAddress.host)("port", configServerAddress.port));
        return emptyResult;
    }

    configserver::proto::HeartBeatResponse heartBeatResp;
    heartBeatResp.ParseFromString(*httpResponse.GetBody<string>());

    if (0 != strcmp(heartBeatResp.request_id().c_str(), requestID.c_str()))
        return emptyResult;

    LOG_DEBUG(sLogger,
              ("SendHeartbeat", "success")("reqBody", reqBody)("requestId", heartBeatResp.request_id())(
                  "statusCode", heartBeatResp.code()));

    return heartBeatResp.pipeline_check_results();
}

google::protobuf::RepeatedPtrField<configserver::proto::ConfigDetail> LegacyCommonConfigProvider::FetchPipelineConfig(
    const ConfigServerAddress& configServerAddress,
    const google::protobuf::RepeatedPtrField<configserver::proto::ConfigCheckResult>& requestConfigs) {
    configserver::proto::FetchPipelineConfigRequest fetchConfigReq;
    string requestID
        = Base64Encode(Application::GetInstance()->GetInstanceId().append("_").append(to_string(time(NULL))));
    fetchConfigReq.set_request_id(requestID);
    fetchConfigReq.set_agent_id(Application::GetInstance()->GetInstanceId());

    google::protobuf::RepeatedPtrField<configserver::proto::ConfigInfo> configInfos;
    for (int i = 0; i < requestConfigs.size(); i++) {
        if (requestConfigs[i].check_status() != configserver::proto::DELETED) {
            configserver::proto::ConfigInfo* info = configInfos.Add();
            info->set_type(configserver::proto::PIPELINE_CONFIG);
            info->set_name(requestConfigs[i].name());
            info->set_version(requestConfigs[i].new_version());
            info->set_context(requestConfigs[i].context());
        }
    }
    fetchConfigReq.mutable_req_configs()->MergeFrom(configInfos);

    string operation = AGENT;
    operation.append("/").append("FetchPipelineConfig");
    map<string, string> httpHeader;
    httpHeader[CONTENT_TYPE] = TYPE_LOG_PROTOBUF;
    string reqBody;
    fetchConfigReq.SerializeToString(&reqBody);

    google::protobuf::RepeatedPtrField<configserver::proto::ConfigDetail> emptyResult;
    HttpResponse httpResponse;
    if (!logtail::SendHttpRequest(make_unique<HttpRequest>(HTTP_POST,
                                                           false,
                                                           configServerAddress.host,
                                                           configServerAddress.port,
                                                           operation,
                                                           "",
                                                           httpHeader,
                                                           reqBody),
                                  httpResponse)) {
        LOG_WARNING(sLogger, ("GetConfigUpdateInfos", "fail")("reqBody", reqBody));
        return emptyResult;
    }

    configserver::proto::FetchPipelineConfigResponse fetchConfigResp;
    fetchConfigResp.ParseFromString(*httpResponse.GetBody<string>());

    if (0 != strcmp(fetchConfigResp.request_id().c_str(), requestID.c_str()))
        return emptyResult;

    LOG_DEBUG(sLogger,
              ("GetConfigUpdateInfos", "success")("reqBody", reqBody)("requestId", fetchConfigResp.request_id())(
                  "statusCode", fetchConfigResp.code()));

    return fetchConfigResp.config_details();
}

void LegacyCommonConfigProvider::UpdateRemoteConfig(
    const google::protobuf::RepeatedPtrField<configserver::proto::ConfigCheckResult>& checkResults,
    const google::protobuf::RepeatedPtrField<configserver::proto::ConfigDetail>& configDetails) {
    error_code ec;
    filesystem::create_directories(mContinuousPipelineConfigDir, ec);
    if (ec) {
        StopUsingConfigServer();
        LOG_ERROR(
            sLogger,
            ("failed to create dir for legacy common configs",
             "stop receiving config from legacy common config server")("dir", mContinuousPipelineConfigDir.string())(
                "error code", ec.value())("error msg", ec.message()));
        return;
    }

    lock_guard<mutex> lock(mContinuousPipelineMux);
    for (const auto& checkResult : checkResults) {
        filesystem::path filePath = mContinuousPipelineConfigDir / (checkResult.name() + ".yaml");
        filesystem::path tmpFilePath = mContinuousPipelineConfigDir / (checkResult.name() + ".yaml.new");
        switch (checkResult.check_status()) {
            case configserver::proto::DELETED:
                mConfigNameVersionMap.erase(checkResult.name());
                filesystem::remove(filePath, ec);
                break;
            case configserver::proto::NEW:
            case configserver::proto::MODIFIED: {
                string configDetail;
                for (const auto& detail : configDetails) {
                    if (detail.name() == checkResult.name()) {
                        configDetail = detail.detail();
                        break;
                    }
                }
                mConfigNameVersionMap[checkResult.name()] = checkResult.new_version();
                ofstream fout(tmpFilePath);
                if (!fout) {
                    LOG_WARNING(sLogger, ("failed to open config file", filePath.string()));
                    continue;
                }
                fout << configDetail;
                fout.close();

                error_code ec;
                filesystem::rename(tmpFilePath, filePath, ec);
                if (ec) {
                    LOG_WARNING(sLogger,
                                ("failed to dump config file",
                                 filePath.string())("error code", ec.value())("error msg", ec.message()));
                    filesystem::remove(tmpFilePath, ec);
                }
                break;
            }
            default:
                break;
        }
    }
}

} // namespace logtail
