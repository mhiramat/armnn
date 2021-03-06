//
// Copyright © 2019 Arm Ltd. All rights reserved.
// SPDX-License-Identifier: MIT
//

#include "FileOnlyProfilingConnection.hpp"
#include "PacketVersionResolver.hpp"

#include <armnn/Exceptions.hpp>
#include <common/include/Constants.hpp>

#include <algorithm>
#include <boost/numeric/conversion/cast.hpp>
#include <iostream>
#include <thread>

namespace armnn
{

namespace profiling
{

FileOnlyProfilingConnection::~FileOnlyProfilingConnection()
{
    Close();
}

bool FileOnlyProfilingConnection::IsOpen() const
{
    // This type of connection is always open.
    return true;
}

void FileOnlyProfilingConnection::Close()
{
    // Dump any unread packets out of the queue.
    size_t initialSize = m_PacketQueue.size();
    for (size_t i = 0; i < initialSize; ++i)
    {
        m_PacketQueue.pop();
    }
    // dispose of the processing thread
    m_KeepRunning.store(false);
    if (m_LocalHandlersThread.joinable())
    {
        // make sure the thread wakes up and sees it has to stop
        m_ConditionPacketReadable.notify_one();
        m_LocalHandlersThread.join();
    }
}

bool FileOnlyProfilingConnection::WaitForStreamMeta(const unsigned char* buffer, uint32_t length)
{
    IgnoreUnused(length);

    // The first word, stream_metadata_identifer, should always be 0.
    if (ToUint32(buffer, TargetEndianness::BeWire) != 0)
    {
        Fail("Protocol error. The stream_metadata_identifer was not 0.");
    }

    // Before we interpret the length we need to read the pipe_magic word to determine endianness.
    if (ToUint32(buffer + 8, TargetEndianness::BeWire) == armnnProfiling::PIPE_MAGIC)
    {
        m_Endianness = TargetEndianness::BeWire;
    }
    else if (ToUint32(buffer + 8, TargetEndianness::LeWire) == armnnProfiling::PIPE_MAGIC)
    {
        m_Endianness = TargetEndianness::LeWire;
    }
    else
    {
        Fail("Protocol read error. Unable to read PIPE_MAGIC value.");
    }
    return true;
}

void FileOnlyProfilingConnection::SendConnectionAck()
{
    if (!m_QuietOp)
    {
        std::cout << "Sending connection acknowledgement." << std::endl;
    }
    std::unique_ptr<unsigned char[]> uniqueNullPtr = nullptr;
    {
        std::lock_guard<std::mutex> lck(m_PacketAvailableMutex);
        m_PacketQueue.push(Packet(0x10000, 0, uniqueNullPtr));
    }
    m_ConditionPacketAvailable.notify_one();
}

bool FileOnlyProfilingConnection::SendCounterSelectionPacket()
{
    uint32_t uint16_t_size = sizeof(uint16_t);
    uint32_t uint32_t_size = sizeof(uint32_t);

    uint32_t offset   = 0;
    uint32_t bodySize = uint32_t_size + boost::numeric_cast<uint32_t>(m_IdList.size()) * uint16_t_size;

    auto uniqueData     = std::make_unique<unsigned char[]>(bodySize);
    unsigned char* data = reinterpret_cast<unsigned char*>(uniqueData.get());

    // Copy capturePeriod
    WriteUint32(data, offset, m_Options.m_CapturePeriod);

    // Copy m_IdList
    offset += uint32_t_size;
    for (const uint16_t& id : m_IdList)
    {
        WriteUint16(data, offset, id);
        offset += uint16_t_size;
    }

    {
        std::lock_guard<std::mutex> lck(m_PacketAvailableMutex);
        m_PacketQueue.push(Packet(0x40000, bodySize, uniqueData));
    }
    m_ConditionPacketAvailable.notify_one();

    return true;
}

bool FileOnlyProfilingConnection::WritePacket(const unsigned char* buffer, uint32_t length)
{
    ARMNN_ASSERT(buffer);
    Packet packet = ReceivePacket(buffer, length);

    // Read Header and determine case
    uint32_t outgoingHeaderAsWords[2];
    PackageActivity packageActivity = GetPackageActivity(packet, outgoingHeaderAsWords);

    switch (packageActivity)
    {
        case PackageActivity::StreamMetaData:
        {
            if (!WaitForStreamMeta(buffer, length))
            {
                return EXIT_FAILURE;
            }

            SendConnectionAck();
            break;
        }
        case PackageActivity::CounterDirectory:
        {
            std::unique_ptr<unsigned char[]> uniqueCounterData = std::make_unique<unsigned char[]>(length - 8);

            std::memcpy(uniqueCounterData.get(), buffer + 8, length - 8);

            Packet directoryPacket(outgoingHeaderAsWords[0], length - 8, uniqueCounterData);

            armnn::profiling::PacketVersionResolver packetVersionResolver;
            DirectoryCaptureCommandHandler directoryCaptureCommandHandler(
                0, 2, packetVersionResolver.ResolvePacketVersion(0, 2).GetEncodedValue());
            directoryCaptureCommandHandler.operator()(directoryPacket);
            const ICounterDirectory& counterDirectory = directoryCaptureCommandHandler.GetCounterDirectory();
            for (auto& category : counterDirectory.GetCategories())
            {
                // Remember we need to translate the Uid's from our CounterDirectory instance to the parent one.
                std::vector<uint16_t> translatedCounters;
                for (auto const& copyUid : category->m_Counters)
                {
                    translatedCounters.emplace_back(directoryCaptureCommandHandler.TranslateUIDCopyToOriginal(copyUid));
                }
                m_IdList.insert(std::end(m_IdList), std::begin(translatedCounters), std::end(translatedCounters));
            }
            SendCounterSelectionPacket();
            break;
        }
        default:
        {
            break;
        }
    }
    ForwardPacketToHandlers(packet);
    return true;
}

Packet FileOnlyProfilingConnection::ReadPacket(uint32_t timeout)
{
    std::unique_lock<std::mutex> lck(m_PacketAvailableMutex);

    // Here we are using m_PacketQueue.empty() as a predicate variable
    // The conditional variable will wait until packetQueue is not empty or until a timeout
    if(!m_ConditionPacketAvailable.wait_for(lck,
                                            std::chrono::milliseconds(timeout),
                                            [&]{return !m_PacketQueue.empty();}))
    {
        throw armnn::TimeoutException("Thread has timed out as per requested time limit");
    }

    Packet returnedPacket = std::move(m_PacketQueue.front());
    m_PacketQueue.pop();
    return returnedPacket;
}

PackageActivity FileOnlyProfilingConnection::GetPackageActivity(const Packet& packet, uint32_t headerAsWords[2])
{
    headerAsWords[0] = packet.GetHeader();
    headerAsWords[1] = packet.GetLength();
    if (headerAsWords[0] == 0x20000)    // Packet family = 0 Packet Id = 2
    {
        return PackageActivity::CounterDirectory;
    }
    else if (headerAsWords[0] == 0)    // Packet family = 0 Packet Id = 0
    {
        return PackageActivity::StreamMetaData;
    }
    else
    {
        return PackageActivity::Unknown;
    }
}

uint32_t FileOnlyProfilingConnection::ToUint32(const unsigned char* data, TargetEndianness endianness)
{
    // Extract the first 4 bytes starting at data and push them into a 32bit integer based on the
    // specified endianness.
    if (endianness == TargetEndianness::BeWire)
    {
        return static_cast<uint32_t>(data[0]) << 24 | static_cast<uint32_t>(data[1]) << 16 |
               static_cast<uint32_t>(data[2]) << 8 | static_cast<uint32_t>(data[3]);
    }
    else
    {
        return static_cast<uint32_t>(data[3]) << 24 | static_cast<uint32_t>(data[2]) << 16 |
               static_cast<uint32_t>(data[1]) << 8 | static_cast<uint32_t>(data[0]);
    }
}

void FileOnlyProfilingConnection::Fail(const std::string& errorMessage)
{
    Close();
    throw RuntimeException(errorMessage);
}

/// Adds a local packet handler to the FileOnlyProfilingConnection. Invoking this will start
/// a processing thread that will ensure that processing of packets will happen on a separate
/// thread from the profiling services send thread and will therefore protect against the
/// profiling message buffer becoming exhausted because packet handling slows the dispatch.
void FileOnlyProfilingConnection::AddLocalPacketHandler(ILocalPacketHandlerSharedPtr localPacketHandler)
{
    m_PacketHandlers.push_back(std::move(localPacketHandler));
    ILocalPacketHandlerSharedPtr localCopy = m_PacketHandlers.back();
    localCopy->SetConnection(this);
    if (localCopy->GetHeadersAccepted().empty())
    {
        //this is a universal handler
        m_UniversalHandlers.push_back(localCopy);
    }
    else
    {
        for (uint32_t header : localCopy->GetHeadersAccepted())
        {
            auto iter = m_IndexedHandlers.find(header);
            if (iter == m_IndexedHandlers.end())
            {
                std::vector<ILocalPacketHandlerSharedPtr> handlers;
                handlers.push_back(localCopy);
                m_IndexedHandlers.emplace(std::make_pair(header, handlers));
            }
            else
            {
                iter->second.push_back(localCopy);
            }
        }
    }
}

void FileOnlyProfilingConnection::StartProcessingThread()
{
    // check if the thread has already started
    if (m_IsRunning.load())
    {
        return;
    }
    // make sure if there was one running before it is joined
    if (m_LocalHandlersThread.joinable())
    {
        m_LocalHandlersThread.join();
    }
    m_IsRunning.store(true);
    m_KeepRunning.store(true);
    m_LocalHandlersThread = std::thread(&FileOnlyProfilingConnection::ServiceLocalHandlers, this);
}

void FileOnlyProfilingConnection::ForwardPacketToHandlers(Packet& packet)
{
    if (m_PacketHandlers.empty())
    {
        return;
    }
    if (m_KeepRunning.load() == false)
    {
        return;
    }
    {
        std::unique_lock<std::mutex> readableListLock(m_ReadableMutex);
        if (m_KeepRunning.load() == false)
        {
            return;
        }
        m_ReadableList.push(std::move(packet));
    }
    m_ConditionPacketReadable.notify_one();
}

void FileOnlyProfilingConnection::ServiceLocalHandlers()
{
    do
    {
        Packet returnedPacket;
        bool readPacket = false;
        {   // only lock while we are taking the packet off the incoming list
            std::unique_lock<std::mutex> lck(m_ReadableMutex);
            if (m_Timeout < 0)
            {
                m_ConditionPacketReadable.wait(lck,
                                               [&] { return !m_ReadableList.empty(); });
            }
            else
            {
                m_ConditionPacketReadable.wait_for(lck,
                                                   std::chrono::milliseconds(std::max(m_Timeout, 1000)),
                                                   [&] { return !m_ReadableList.empty(); });
            }
            if (m_KeepRunning.load())
            {
                if (!m_ReadableList.empty())
                {
                    returnedPacket = std::move(m_ReadableList.front());
                    m_ReadableList.pop();
                    readPacket = true;
                }
            }
            else
            {
                ClearReadableList();
            }
        }
        if (m_KeepRunning.load() && readPacket)
        {
            DispatchPacketToHandlers(returnedPacket);
        }
    } while (m_KeepRunning.load());
    // make sure the readable list is cleared
    ClearReadableList();
    m_IsRunning.store(false);
}

void FileOnlyProfilingConnection::ClearReadableList()
{
    // make sure the incoming packet queue gets emptied
    size_t initialSize = m_ReadableList.size();
    for (size_t i = 0; i < initialSize; ++i)
    {
        m_ReadableList.pop();
    }
}

void FileOnlyProfilingConnection::DispatchPacketToHandlers(const Packet& packet)
{
    for (auto& delegate : m_UniversalHandlers)
    {
        delegate->HandlePacket(packet);
    }
    auto iter = m_IndexedHandlers.find(packet.GetHeader());
    if (iter != m_IndexedHandlers.end())
    {
        for (auto &delegate : iter->second)
        {
            delegate->HandlePacket(packet);
        }
    }
}

}    // namespace profiling

}    // namespace armnn
