//
// Copyright © 2020 Arm Ltd. All rights reserved.
// SPDX-License-Identifier: MIT
//

#pragma once

#include <armnn/profiling/ILocalPacketHandler.hpp>
#include <armnn/profiling/ITimelineDecoder.hpp>
#include <Packet.hpp>

#include "ProfilingUtils.hpp"
#include "TimelineCaptureCommandHandler.hpp"
#include "TimelineDirectoryCaptureCommandHandler.hpp"
#include "TimelineModel.hpp"

#include <condition_variable>
#include <map>
#include <mutex>
#include <vector>

namespace armnn
{

namespace profiling
{

// forward declaration of class
class TestTimelinePacketHandler;
class TimelineMessageDecoder : public ITimelineDecoder
{
public:
    TimelineMessageDecoder(TimelineModel& model) : m_PacketHandler(nullptr), m_TimelineModel(model) {}
    virtual TimelineStatus CreateEntity(const Entity&) override;
    virtual TimelineStatus CreateEventClass(const EventClass&) override;
    virtual TimelineStatus CreateEvent(const Event&) override;
    virtual TimelineStatus CreateLabel(const Label&) override;
    virtual TimelineStatus CreateRelationship(const Relationship&) override;
    void SetPacketHandler(TestTimelinePacketHandler* packetHandler) {m_PacketHandler = packetHandler;};
private:
    TestTimelinePacketHandler* m_PacketHandler;
    TimelineModel& m_TimelineModel;
};

class TestTimelinePacketHandler : public ILocalPacketHandler
{
public:
    TestTimelinePacketHandler() :
        m_Connection(nullptr),
        m_InferenceCompleted(false),
        m_DirectoryHeader(CreateTimelinePacketHeader(1, 0, 0, 0, 0, 0).first),
        m_MessageHeader(CreateTimelinePacketHeader(1, 0, 1, 0, 0, 0).first),
        m_MessageDecoder(m_TimelineModel),
        m_Decoder(1, 1, 0, m_MessageDecoder),
        m_DirectoryDecoder(1, 0, 0, m_Decoder, true)
    { m_MessageDecoder.SetPacketHandler(this); }

    virtual std::vector<uint32_t> GetHeadersAccepted() override; // ILocalPacketHandler

    virtual void HandlePacket(const Packet& packet) override; // ILocalPacketHandler

    void Stop();

    void WaitOnInferenceCompletion(unsigned int timeout);
    void SetInferenceComplete();

    const TimelineModel& GetTimelineModel() const {return m_TimelineModel;}

    virtual void SetConnection(IProfilingConnection* profilingConnection) override // ILocalPacketHandler
    {
        m_Connection = profilingConnection;
    }

private:
    void ProcessDirectoryPacket(const Packet& packet);
    void ProcessMessagePacket(const Packet& packet);
    IProfilingConnection* m_Connection;
    std::mutex m_InferenceCompletedMutex;
    std::condition_variable m_InferenceCompletedConditionVariable;
    bool m_InferenceCompleted;
    TimelineModel m_TimelineModel;
    uint32_t m_DirectoryHeader;
    uint32_t m_MessageHeader;
    TimelineMessageDecoder m_MessageDecoder;
    timelinedecoder::TimelineCaptureCommandHandler m_Decoder;
    timelinedecoder::TimelineDirectoryCaptureCommandHandler m_DirectoryDecoder;
};

} // namespace profiling

} // namespace armnn