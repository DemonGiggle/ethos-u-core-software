/*
 * Copyright (c) 2020 Arm Limited. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the License); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an AS IS BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <message_process.hpp>

#include "cmsis_compiler.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <inttypes.h>

using namespace std;
using namespace InferenceProcess;

namespace MessageProcess {

QueueImpl::QueueImpl(ethosu_core_queue &_queue) : queue(_queue) {
    cleanHeaderData();
}

bool QueueImpl::empty() const {
    return queue.header.read == queue.header.write;
}

size_t QueueImpl::available() const {
    size_t avail = queue.header.write - queue.header.read;

    if (queue.header.read > queue.header.write) {
        avail += queue.header.size;
    }

    return avail;
}

size_t QueueImpl::capacity() const {
    return queue.header.size - available();
}

bool QueueImpl::read(uint8_t *dst, uint32_t length) {
    const uint8_t *end = dst + length;
    uint32_t rpos      = queue.header.read;

    invalidateHeaderData();

    if (length > available()) {
        return false;
    }

    while (dst < end) {
        *dst++ = queue.data[rpos];
        rpos   = (rpos + 1) % queue.header.size;
    }

    queue.header.read = rpos;

    cleanHeader();

    return true;
}

bool QueueImpl::write(const Vec *vec, size_t length) {
    size_t total = 0;

    for (size_t i = 0; i < length; i++) {
        total += vec[i].length;
    }

    invalidateHeader();

    if (total > capacity()) {
        return false;
    }

    uint32_t wpos = queue.header.write;

    for (size_t i = 0; i < length; i++) {
        const uint8_t *src = reinterpret_cast<const uint8_t *>(vec[i].base);
        const uint8_t *end = src + vec[i].length;

        while (src < end) {
            queue.data[wpos] = *src++;
            wpos             = (wpos + 1) % queue.header.size;
        }
    }

    // Update the write position last
    queue.header.write = wpos;

    cleanHeaderData();

    return true;
}

bool QueueImpl::write(const uint32_t type, const void *src, uint32_t length) {
    ethosu_core_msg msg = {ETHOSU_CORE_MSG_MAGIC, type, length};
    Vec vec[2]          = {{&msg, sizeof(msg)}, {src, length}};

    return write(vec, 2);
}

// Skip to magic or end of queue
void QueueImpl::reset() {
    invalidateHeader();
    queue.header.read = queue.header.write;
    cleanHeader();
}

void QueueImpl::cleanHeader() const {
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    SCB_CleanDCache_by_Addr(reinterpret_cast<uint32_t *>(&queue.header), sizeof(queue.header));
#endif
}

void QueueImpl::cleanHeaderData() const {
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    SCB_CleanDCache_by_Addr(reinterpret_cast<uint32_t *>(&queue.header), sizeof(queue.header));
    uintptr_t queueDataPtr = reinterpret_cast<uintptr_t>(&queue.data[0]);
    SCB_CleanDCache_by_Addr(reinterpret_cast<uint32_t *>(queueDataPtr & ~3), queue.header.size + (queueDataPtr & 3));
#endif
}

void QueueImpl::invalidateHeader() const {
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    SCB_InvalidateDCache_by_Addr(reinterpret_cast<uint32_t *>(&queue.header), sizeof(queue.header));
#endif
}

void QueueImpl::invalidateHeaderData() const {
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    SCB_InvalidateDCache_by_Addr(reinterpret_cast<uint32_t *>(&queue.header), sizeof(queue.header));
    uintptr_t queueDataPtr = reinterpret_cast<uintptr_t>(&queue.data[0]);
    SCB_InvalidateDCache_by_Addr(reinterpret_cast<uint32_t *>(queueDataPtr & ~3),
                                 queue.header.size + (queueDataPtr & 3));
#endif
}

MessageProcess::MessageProcess(ethosu_core_queue &in,
                               ethosu_core_queue &out,
                               Mailbox::Mailbox &mbox,
                               ::InferenceProcess::InferenceProcess &_inferenceProcess) :
    queueIn(in),
    queueOut(out), mailbox(mbox), inferenceProcess(_inferenceProcess) {
    mailbox.registerCallback(mailboxCallback, reinterpret_cast<void *>(this));
}

void MessageProcess::run() {
    while (true) {
        // Handle all messages in queue
        while (handleMessage())
            ;

        // Wait for event
        __WFE();
    }
}

void MessageProcess::handleIrq() {
    __SEV();
}

bool MessageProcess::handleMessage() {
    ethosu_core_msg msg;

    if (queueIn.available() == 0) {
        return false;
    }

    // Read msg header
    // Only process a complete message header, else send error message
    // and reset queue
    if (!queueIn.read(msg)) {
        sndErrorRspAndResetQueue(ETHOSU_CORE_MSG_ERR_INVALID_SIZE, "Failed to read a complete header");
        return false;
    }

    printf("Msg: header magic=%" PRIX32 ", type=%" PRIu32 ", length=%" PRIu32 "\n", msg.magic, msg.type, msg.length);

    if (msg.magic != ETHOSU_CORE_MSG_MAGIC) {
        sndErrorRspAndResetQueue(ETHOSU_CORE_MSG_ERR_INVALID_MAGIC, "Invalid magic");
        return false;
    }

    switch (msg.type) {
    case ETHOSU_CORE_MSG_PING:
        printf("Msg: Ping\n");
        sendPong();
        break;
    case ETHOSU_CORE_MSG_ERR: {
        struct ethosu_core_msg_err error = {0};
        if (!queueIn.read(error)) {
            printf("ERROR: Msg: Failed to receive error message\n");
        } else {
            printf("Msg: Received an error response, type=%" PRIu32 ", msg=\"%s\"\n", error.type, error.msg);
        }
        queueIn.reset();
        return false;
    }
    case ETHOSU_CORE_MSG_VERSION_REQ:
        printf("Msg: Version request\n");
        sendVersionRsp();
        break;
    case ETHOSU_CORE_MSG_INFERENCE_REQ: {
        ethosu_core_inference_req req;

        if (!queueIn.read(req)) {
            sndErrorRspAndResetQueue(ETHOSU_CORE_MSG_ERR_INVALID_PAYLOAD, "InferenceReq. Failed to read payload");
            return false;
        }

        printf("Msg: InferenceReq. user_arg=0x%" PRIx64 ", network={0x%" PRIx32 ", %" PRIu32 "}",
               req.user_arg,
               req.network.ptr,
               req.network.size);

        printf(", ifm_count=%" PRIu32 ", ifm=[", req.ifm_count);
        for (uint32_t i = 0; i < req.ifm_count; ++i) {
            if (i > 0) {
                printf(", ");
            }

            printf("{0x%" PRIx32 ", %" PRIu32 "}", req.ifm[i].ptr, req.ifm[i].size);
        }
        printf("]");

        printf(", ofm_count=%" PRIu32 ", ofm=[", req.ofm_count);
        for (uint32_t i = 0; i < req.ofm_count; ++i) {
            if (i > 0) {
                printf(", ");
            }

            printf("{0x%" PRIx32 ", %" PRIu32 "}", req.ofm[i].ptr, req.ofm[i].size);
        }
        printf("]\n");

        DataPtr networkModel(reinterpret_cast<void *>(req.network.ptr), req.network.size);

        vector<DataPtr> ifm;
        for (uint32_t i = 0; i < req.ifm_count; ++i) {
            ifm.push_back(DataPtr(reinterpret_cast<void *>(req.ifm[i].ptr), req.ifm[i].size));
        }

        vector<DataPtr> ofm;
        for (uint32_t i = 0; i < req.ofm_count; ++i) {
            ofm.push_back(DataPtr(reinterpret_cast<void *>(req.ofm[i].ptr), req.ofm[i].size));
        }

        vector<DataPtr> expectedOutput;

        vector<uint8_t> pmuEventConfig(ETHOSU_CORE_PMU_MAX);
        for (uint32_t i = 0; i < ETHOSU_CORE_PMU_MAX; i++) {
            pmuEventConfig[i] = req.pmu_event_config[i];
        }

        InferenceJob job(
            "job", networkModel, ifm, ofm, expectedOutput, -1, pmuEventConfig, req.pmu_cycle_counter_enable);
        job.invalidate();

        bool failed = inferenceProcess.runJob(job);
        job.clean();

        sendInferenceRsp(req.user_arg,
                         job.output,
                         failed,
                         job.pmuEventConfig,
                         job.pmuCycleCounterEnable,
                         job.pmuEventCount,
                         job.pmuCycleCounterCount);
        break;
    }
    default: {
        char errMsg[128] = {0};
        snprintf(&errMsg[0],
                 sizeof(errMsg),
                 "Msg: Unknown type: %" PRIu32 " with payload length %" PRIu32 " bytes\n",
                 msg.type,
                 msg.length);
        sndErrorRspAndResetQueue(ETHOSU_CORE_MSG_ERR_UNSUPPORTED_TYPE, errMsg);
        return false;
    }
    }
    return true;
}

void MessageProcess::sendPong() {
    if (!queueOut.write(ETHOSU_CORE_MSG_PONG)) {
        printf("ERROR: Msg: Failed to write pong response. No mailbox message sent\n");
    } else {
        mailbox.sendMessage();
    }
}

void MessageProcess::sendVersionRsp() {
    struct ethosu_core_msg_version ver = {.major     = ETHOSU_CORE_MSG_VERSION_MAJOR,
                                          .minor     = ETHOSU_CORE_MSG_VERSION_MINOR,
                                          .patch     = ETHOSU_CORE_MSG_VERSION_PATCH,
                                          ._reserved = 0};

    if (!queueOut.write(ETHOSU_CORE_MSG_VERSION_RSP, ver)) {
        printf("ERROR: Failed to write version response. No mailbox message sent\n");
    } else {
        mailbox.sendMessage();
    }
}

void MessageProcess::sndErrorRspAndResetQueue(ethosu_core_msg_err_type type, const char *message) {
    ethosu_core_msg_err error = {0};
    error.type                = type;
    unsigned int i            = 0;

    if (message) {
        for (; i < (sizeof(error.msg) - 1) && message[i]; i++) {
            error.msg[i] = message[i];
        }
    }
    printf("ERROR: Msg: \"%s\"\n", message);
    if (!queueOut.write(ETHOSU_CORE_MSG_ERR, &error)) {
        printf("ERROR: Msg: Failed to write error response. No mailbox message sent\n");
        return;
    }
    queueIn.reset();
    mailbox.sendMessage();
}

void MessageProcess::sendInferenceRsp(uint64_t userArg,
                                      vector<DataPtr> &ofm,
                                      bool failed,
                                      vector<uint8_t> &pmuEventConfig,
                                      uint32_t pmuCycleCounterEnable,
                                      vector<uint32_t> &pmuEventCount,
                                      uint64_t pmuCycleCounterCount) {
    ethosu_core_inference_rsp rsp = {
        .pmu_event_count =
            {
                0,
            },
    };

    rsp.user_arg  = userArg;
    rsp.ofm_count = ofm.size();
    rsp.status    = failed ? ETHOSU_CORE_STATUS_ERROR : ETHOSU_CORE_STATUS_OK;

    for (size_t i = 0; i < ofm.size(); ++i) {
        rsp.ofm_size[i] = ofm[i].size;
    }

    for (size_t i = 0; i < pmuEventConfig.size(); i++) {
        rsp.pmu_event_config[i] = pmuEventConfig[i];
    }
    rsp.pmu_cycle_counter_enable = pmuCycleCounterEnable;
    for (size_t i = 0; i < pmuEventCount.size(); i++) {
        rsp.pmu_event_count[i] = pmuEventCount[i];
    }
    rsp.pmu_cycle_counter_count = pmuCycleCounterCount;

    printf("Sending inference response. userArg=0x%" PRIx64 ", ofm_count=%" PRIu32 ", status=%" PRIu32 "\n",
           rsp.user_arg,
           rsp.ofm_count,
           rsp.status);

    if (!queueOut.write(ETHOSU_CORE_MSG_INFERENCE_RSP, rsp)) {
        printf("ERROR: Msg: Failed to write inference response. No mailbox message sent\n");
    } else {
        mailbox.sendMessage();
    }
}

void MessageProcess::mailboxCallback(void *userArg) {
    MessageProcess *_this = reinterpret_cast<MessageProcess *>(userArg);
    _this->handleIrq();
}

} // namespace MessageProcess
