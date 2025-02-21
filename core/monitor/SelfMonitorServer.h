/*
 * Copyright 2024 iLogtail Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <string>

#include "collection_pipeline/CollectionPipeline.h"

namespace logtail {

class SelfMonitorServer {
public:
    SelfMonitorServer(const SelfMonitorServer&) = delete;
    SelfMonitorServer& operator=(const SelfMonitorServer&) = delete;
    static SelfMonitorServer* GetInstance();

    void Init();
    void Monitor();
    void Stop();

    void UpdateMetricPipeline(CollectionPipelineContext* ctx, SelfMonitorMetricRules* rules);
    void RemoveMetricPipeline();
    void UpdateAlarmPipeline(CollectionPipelineContext* ctx); // Todo
private:
    SelfMonitorServer();
    ~SelfMonitorServer() = default;

    std::future<void> mThreadRes;
    std::mutex mThreadRunningMux;
    bool mIsThreadRunning = true;
    std::condition_variable mStopCV;

    void SendMetrics();
    bool ProcessSelfMonitorMetricEvent(SelfMonitorMetricEvent& event, const SelfMonitorMetricRule& rule);
    void PushSelfMonitorMetricEvents(std::vector<SelfMonitorMetricEvent>& events);
    void ReadAsPipelineEventGroup(PipelineEventGroup& pipelineEventGroup);

    CollectionPipelineContext* mMetricPipelineCtx = nullptr;
    SelfMonitorMetricRules* mSelfMonitorMetricRules = nullptr;
    SelfMonitorMetricEventMap mSelfMonitorMetricEventMap;
    mutable ReadWriteLock mMetricPipelineLock;

    void SendAlarms();

    CollectionPipelineContext* mAlarmPipelineCtx;
    std::mutex mAlarmPipelineMux;
#ifdef APSARA_UNIT_TEST_MAIN
    friend class InputInternalMetricsUnittest;
#endif
};

} // namespace logtail
