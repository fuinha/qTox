/*
    Copyright © 2015 by The qTox Project

    This file is part of qTox, a Qt-based graphical interface for Tox.

    qTox is libre software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    qTox is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with qTox.  If not, see <http://www.gnu.org/licenses/>.
*/

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}
#include <QMutexLocker>
#include <QDebug>
#include <QtConcurrent/QtConcurrentRun>
#include <memory>
#include <functional>
#include "src/persistence/settings.h"
#include "camerasource.h"
#include "cameradevice.h"
#include "videoframe.h"

/**
@class CameraSource
@brief This class is a wrapper to share a camera's captured video frames

It allows objects to suscribe and unsuscribe to the stream, starting
the camera and streaming new video frames only when needed.
This is a singleton, since we can only capture from one
camera at the same time without thread-safety issues.
The source is lazy in the sense that it will only keep the video
device open as long as there are subscribers, the source can be
open but the device closed if there are zero subscribers.
*/

/**
@var QVector<std::weak_ptr<VideoFrame>> CameraSource::freelist
@brief Frames that need freeing before we can safely close the device

@var QFuture<void> CameraSource::streamFuture
@brief Future of the streaming thread

@var QString CameraSource::deviceName
@brief Short name of the device for CameraDevice's open(QString)

@var CameraDevice* CameraSource::device
@brief Non-owning pointer to an open CameraDevice, or nullptr. Not atomic, synced with memfences when becomes null.

@var VideoMode CameraSource::mode
@brief What mode we tried to open the device in, all zeros means default mode

@var AVCodecContext* CameraSource::cctx
@brief Codec context of the camera's selected video stream

@var AVCodecContext* CameraSource::cctxOrig
@brief Codec context of the camera's selected video stream

@var int CameraSource::videoStreamIndex
@brief A camera can have multiple streams, this is the one we're decoding

@var QMutex CameraSource::biglock
@brief True when locked. Faster than mutexes for video decoding.

@var QMutex CameraSource::freelistLock
@brief True when locked. Faster than mutexes for video decoding.

@var std::atomic_bool CameraSource::streamBlocker
@brief Holds the streaming thread still when true

@var std::atomic_int CameraSource::subscriptions
@brief Remember how many times we subscribed for RAII
*/

CameraSource* CameraSource::instance{nullptr};

CameraSource::CameraSource()
    : deviceName{"none"}, device{nullptr}, mode(VideoMode()),
      cctx{nullptr}, cctxOrig{nullptr}, videoStreamIndex{-1},
      _isOpen{false}, streamBlocker{false}, subscriptions{0}
{
    subscriptions = 0;
    av_register_all();
    avdevice_register_all();
}

/**
@brief Returns the singleton instance.
*/
CameraSource& CameraSource::getInstance()
{
    if (!instance)
        instance = new CameraSource();
    return *instance;
}

void CameraSource::destroyInstance()
{
    if (instance)
    {
        delete instance;
        instance = nullptr;
    }
}

/**
@brief Opens the source for the camera device.
@note If a device is already open, the source will seamlessly switch to the new device.

Opens the source for the camera device in argument, in the settings, or the system default.
*/
void CameraSource::open()
{
    open(CameraDevice::getDefaultDeviceName());
}

void CameraSource::open(const QString& deviceName)
{
    bool isScreen = CameraDevice::isScreen(deviceName);
    VideoMode mode = VideoMode(Settings::getInstance().getScreenRegion());
    if (!isScreen)
    {
        mode = VideoMode(Settings::getInstance().getCamVideoRes());
        mode.FPS = Settings::getInstance().getCamVideoFPS();
    }

    open(deviceName, mode);
}

void CameraSource::open(const QString& DeviceName, VideoMode Mode)
{
    streamBlocker = true;
    QMutexLocker l{&biglock};

    if (DeviceName == deviceName && Mode == mode)
    {
        streamBlocker = false;
        return;
    }

    if (subscriptions)
        closeDevice();

    deviceName = DeviceName;
    mode = Mode;
    _isOpen = (deviceName != "none");

    if (subscriptions && _isOpen)
        openDevice();

    streamBlocker = false;
}

/**
@brief Stops streaming.

Equivalent to opening the source with the video device "none".
*/
void CameraSource::close()
{
    open("none");
}

bool CameraSource::isOpen()
{
    return _isOpen;
}

CameraSource::~CameraSource()
{
    QMutexLocker l{&biglock};

    if (!_isOpen)
        return;

    // Free all remaining VideoFrame
    // Locking must be done precisely this way to avoid races
    for (int i = 0; i < freelist.size(); i++)
    {
        std::shared_ptr<VideoFrame> vframe = freelist[i].lock();
        if (!vframe)
            continue;
        vframe->releaseFrame();
    }

    if (cctx)
        avcodec_free_context(&cctx);
    if (cctxOrig)
        avcodec_close(cctxOrig);

    if (device)
    {
        for (int i = 0; i < subscriptions; i++)
            device->close();

        device = nullptr;
    }

    // Memfence so the stream thread sees a nullptr device
    std::atomic_thread_fence(std::memory_order_release);
    l.unlock();

    // Synchronize with our stream thread
    while (streamFuture.isRunning())
        QThread::yieldCurrentThread();
}

bool CameraSource::subscribe()
{
    QMutexLocker l{&biglock};

    if (!_isOpen)
    {
        ++subscriptions;
        return true;
    }

    if (openDevice())
    {
        ++subscriptions;
        return true;
    }
    else
    {
        while (device && !device->close()) {}
        device = nullptr;
        cctx = cctxOrig = nullptr;
        videoStreamIndex = -1;
        // Memfence so the stream thread sees a nullptr device
        std::atomic_thread_fence(std::memory_order_release);
        return false;
    }

}

void CameraSource::unsubscribe()
{
    streamBlocker = true;
    QMutexLocker l{&biglock};
    streamBlocker = false;

    if (!_isOpen)
    {
        --subscriptions;
        return;
    }

    if (!device)
    {
        qWarning() << "Unsubscribing with zero subscriber";
        return;
    }

    if (subscriptions - 1 == 0)
    {
        closeDevice();
        l.unlock();

        // Synchronize with our stream thread
        while (streamFuture.isRunning())
            QThread::yieldCurrentThread();
    }
    else
    {
        device->close();
    }
    subscriptions--;
}

/**
@brief Opens the video device and starts streaming.
@note Callers must own the biglock.
@return True if success, false otherwise.
*/
bool CameraSource::openDevice()
{
    qDebug() << "Opening device " << deviceName;

    if (device)
    {
        device->open();
        return true;
    }

    // We need to create a new CameraDevice
    AVCodec* codec;
    device = CameraDevice::open(deviceName, mode);

    if (!device)
    {
        qWarning() << "Failed to open device!";
        return false;
    }

    // We need to open the device as many time as we already have subscribers,
    // otherwise the device could get closed while we still have subscribers
    for (int i = 0; i < subscriptions; i++)
        device->open();

    // Find the first video stream
    for (unsigned i = 0; i < device->context->nb_streams; i++)
    {
        if (device->context->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            videoStreamIndex = i;
            break;
        }
    }

    if (videoStreamIndex == -1)
    {
        qWarning() << "Video stream not found";
        return false;
    }

    // Get a pointer to the codec context for the video stream
    cctxOrig = device->context->streams[videoStreamIndex]->codec;
    codec = avcodec_find_decoder(cctxOrig->codec_id);
    if (!codec)
    {
        qWarning() << "Codec not found";
        return false;
    }

    // Copy context, since we apparently aren't allowed to use the original
    cctx = avcodec_alloc_context3(codec);
    if (avcodec_copy_context(cctx, cctxOrig) != 0)
    {
        qWarning() << "Can't copy context";
        return false;
    }

    cctx->refcounted_frames = 1;

    // Open codec
    if (avcodec_open2(cctx, codec, nullptr)<0)
    {
        qWarning() << "Can't open codec";
        avcodec_free_context(&cctx);
        return false;
    }

    if (streamFuture.isRunning())
        qDebug() << "The stream thread is already running! Keeping the current one open.";
    else
        streamFuture = QtConcurrent::run(std::bind(&CameraSource::stream, this));

    // Synchronize with our stream thread
    while (!streamFuture.isRunning())
        QThread::yieldCurrentThread();

    emit deviceOpened();

    return true;
}

/**
@brief Closes the video device and stops streaming.
@note Callers must own the biglock.
*/
void CameraSource::closeDevice()
{
    qDebug() << "Closing device "<<deviceName;

    // Free all remaining VideoFrame
    // Locking must be done precisely this way to avoid races
    for (int i = 0; i < freelist.size(); i++)
    {
        std::shared_ptr<VideoFrame> vframe = freelist[i].lock();
        if (!vframe)
            continue;

        vframe->releaseFrame();
    }
    freelist.clear();
    freelist.squeeze();

    // Free our resources and close the device
    videoStreamIndex = -1;
    avcodec_free_context(&cctx);
    avcodec_close(cctxOrig);
    cctxOrig = nullptr;
    while (device && !device->close()) {}
    device = nullptr;
    // Memfence so the stream thread sees a nullptr device
    std::atomic_thread_fence(std::memory_order_release);
}

/**
@brief Blocking. Decodes video stream and emits new frames.
@note Designed to run in its own thread.
*/
void CameraSource::stream()
{
    auto streamLoop = [=]()
    {
        AVFrame* frame = av_frame_alloc();
        if (!frame)
            return;

        frame->opaque = nullptr;

        AVPacket packet;
        if (av_read_frame(device->context, &packet) < 0)
            return;

        // Only keep packets from the right stream;
        if (packet.stream_index == videoStreamIndex)
        {
            // Decode video frame
            int frameFinished;
            avcodec_decode_video2(cctx, frame, &frameFinished, &packet);
            if (!frameFinished)
                return;

            freelistLock.lock();

            int freeFreelistSlot = getFreelistSlotLockless();
            auto frameFreeCb = std::bind(&CameraSource::freelistCallback, this, freeFreelistSlot);
            std::shared_ptr<VideoFrame> vframe = std::make_shared<VideoFrame>(frame, frameFreeCb);
            freelist.append(vframe);
            freelistLock.unlock();
            emit frameAvailable(vframe);
        }

      // Free the packet that was allocated by av_read_frame
      av_packet_unref(&packet);
    };

    forever
    {
        biglock.lock();

        // When a thread makes device null, it releases it, so we acquire here
        std::atomic_thread_fence(std::memory_order_acquire);
        if (!device)
        {
            biglock.unlock();
            return;
        }

        streamLoop();

        // Give a chance to other functions to pick up the lock if needed
        biglock.unlock();
        while (streamBlocker)
            QThread::yieldCurrentThread();

        QThread::yieldCurrentThread();
    }
}

/**
@brief CameraSource::freelistCallback
@param freelistIndex

All VideoFrames must be deleted or released before we can close the device
or the device will forcibly free them, and then ~VideoFrame() will double free.
In theory very careful coding from our users could ensure all VideoFrames
die before unsubscribing, even the ones currently in flight in the metatype system.
But that's just asking for trouble and mysterious crashes, so we'll just
maintain a freelist and have all VideoFrames tell us when they die so we can forget them.
*/
void CameraSource::freelistCallback(int freelistIndex)
{
    QMutexLocker l{&freelistLock};
    freelist[freelistIndex].reset();
}

/**
@brief Get the index of a free slot in the freelist.
@note Callers must hold the freelistLock.
@return Index of a free slot.
*/
int CameraSource::getFreelistSlotLockless()
{
    int size = freelist.size();
    for (int i = 0; i < size; ++i)
        if (freelist[i].expired())
            return i;

    freelist.resize(size + (size>>1) + 4); // Arbitrary growth strategy, should work well
    return size;
}
