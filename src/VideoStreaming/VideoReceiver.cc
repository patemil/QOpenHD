/****************************************************************************
 *
 *   (c) 2009-2016 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/


/**
 * @file
 *   @brief QGC Video Receiver
 *   @author Gus Grubba <mavlink@grubba.com>
 */

#include "VideoReceiver.h"
#ifdef QGC_GST_TAISYNC_ENABLED
#include "TaisyncHandler.h"
#endif
#include <QDebug>
#include <QUrl>
#include <QDir>
#include <QDateTime>
#include <QSysInfo>


#if defined(QGC_GST_STREAMING)

static const char* kVideoExtensions[] =
{
    "mkv",
    "mov",
    "mp4"
};

static const char* kVideoMuxes[] =
{
    "matroskamux",
    "qtmux",
    "mp4mux"
};

#define NUM_MUXES (sizeof(kVideoMuxes) / sizeof(char*))

#endif


VideoReceiver::VideoReceiver(QObject* parent)
    : QObject(parent)
#if defined(QGC_GST_STREAMING)
    , _running(false)
    , _recording(false)
    , _streaming(false)
    , _starting(false)
    , _stopping(false)
    , _sink(nullptr)
    , _tee(nullptr)
    , _pipeline(nullptr)
    , _pipelineStopRec(nullptr)
    , _videoSink(nullptr)
    , _socket(nullptr)
    , _serverPresent(false)
    , _rtspTestInterval_ms(5000)
    , _udpReconnect_us(5000000)
#endif
    , _videoSurface(nullptr)
    , _videoRunning(false)
    , _showFullScreen(false)
    //, _videoSettings(nullptr)
{
    _videoSurface = new VideoSurface;
    //_videoSettings = qgcApp()->toolbox()->settingsManager()->videoSettings();
#if defined(QGC_GST_STREAMING)
    _setVideoSink(_videoSurface->videoSink());
    _timer.setSingleShot(true);
    connect(&_timer, &QTimer::timeout, this, &VideoReceiver::_timeout);
    connect(this, &VideoReceiver::msgErrorReceived, this, &VideoReceiver::_handleError);
    connect(this, &VideoReceiver::msgEOSReceived, this, &VideoReceiver::_handleEOS);
    connect(this, &VideoReceiver::msgStateChangedReceived, this, &VideoReceiver::_handleStateChanged);
    connect(&_frameTimer, &QTimer::timeout, this, &VideoReceiver::_updateTimer);
    _frameTimer.start(1000);
#endif
}

VideoReceiver::~VideoReceiver()
{
#if defined(QGC_GST_STREAMING)
    stop();
    if(_socket) {
        delete _socket;
    }
    if (_videoSink) {
        gst_object_unref(_videoSink);
    }
#endif
    if(_videoSurface)
        delete _videoSurface;
}

#if defined(QGC_GST_STREAMING)
void
VideoReceiver::_setVideoSink(GstElement* sink)
{
    if (_videoSink) {
        gst_object_unref(_videoSink);
        _videoSink = nullptr;
    }
    if (sink) {
        _videoSink = sink;
        gst_object_ref_sink(_videoSink);
    }
}
#endif

//-----------------------------------------------------------------------------
void
VideoReceiver::grabImage(QString imageFile)
{
    _imageFile = imageFile;
    emit imageFileChanged();
}

//-----------------------------------------------------------------------------
#if defined(QGC_GST_STREAMING)
static void
newPadCB(GstElement* element, GstPad* pad, gpointer data)
{
    gchar* name;
    name = gst_pad_get_name(pad);
    //g_print("A new pad %s was created\n", name);
    GstCaps* p_caps = gst_pad_get_pad_template_caps (pad);
    gchar* description = gst_caps_to_string(p_caps);
    qDebug() << p_caps << ", " << description;
    g_free(description);
    GstElement* sink = GST_ELEMENT(data);
    if(gst_element_link_pads(element, name, sink, "sink") == false)
        qCritical() << "newPadCB : failed to link elements\n";
    g_free(name);
}
#endif

//-----------------------------------------------------------------------------
#if defined(QGC_GST_STREAMING)
void
VideoReceiver::_connected()
{
    //-- Server showed up. Now we start the stream.
    _timer.stop();
    _socket->deleteLater();
    _socket = nullptr;
    //if(_videoSettings->streamEnabled()->rawValue().toBool()) {
        _serverPresent = true;
        start();
    //}
}
#endif

//-----------------------------------------------------------------------------
#if defined(QGC_GST_STREAMING)
void
VideoReceiver::_socketError(QAbstractSocket::SocketError socketError)
{
    Q_UNUSED(socketError);
    _socket->deleteLater();
    _socket = nullptr;
    //-- Try again in a while
    //if(_videoSettings->streamEnabled()->rawValue().toBool()) {
        _timer.start(_rtspTestInterval_ms);
    //}
}
#endif

//-----------------------------------------------------------------------------
#if defined(QGC_GST_STREAMING)
void
VideoReceiver::_timeout()
{
    //-- If socket is live, we got no connection nor a socket error
    if(_socket) {
        delete _socket;
        _socket = nullptr;
    }
    //if(_videoSettings->streamEnabled()->rawValue().toBool()) {
        //-- RTSP will try to connect to the server. If it cannot connect,
        //   it will simply give up and never try again. Instead, we keep
        //   attempting a connection on this timer. Once a connection is
        //   found to be working, only then we actually start the stream.
        QUrl url(_uri);
        //-- If RTSP and no port is defined, set default RTSP port (554)
        if(_uri.contains("rtsp://") && url.port() <= 0) {
            url.setPort(554);
        }
        _socket = new QTcpSocket;
        //QNetworkProxy tempProxy;
        //tempProxy.setType(QNetworkProxy::DefaultProxy);
        //_socket->setProxy(tempProxy);
        connect(_socket, static_cast<void (QTcpSocket::*)(QAbstractSocket::SocketError)>(&QTcpSocket::error), this, &VideoReceiver::_socketError);
        connect(_socket, &QTcpSocket::connected, this, &VideoReceiver::_connected);
        _socket->connectToHost(url.host(), static_cast<uint16_t>(url.port()));
        _timer.start(_rtspTestInterval_ms);
    //}
}
#endif

//-----------------------------------------------------------------------------
// When we finish our pipeline will look like this:
//
//                                   +-->queue-->decoder-->_videosink
//                                   |
//    datasource-->demux-->parser-->tee
//
//                                   ^
//                                   |
//                                   +-Here we will later link elements for recording
void
VideoReceiver::start()
{
#if defined(QGC_GST_STREAMING)
    _stop = false;
    qDebug() << "start()";

#if  defined(QGC_GST_TAISYNC_ENABLED) && (defined(__android__) || defined(__ios__))
    //-- Taisync on iOS or Android sends a raw h.264 stream
    bool isTaisyncUSB = qgcApp()->toolbox()->videoManager()->isTaisync();
#else
    bool isTaisyncUSB = false;
#endif
    bool isUdp      = _uri.contains("udp://")  && !isTaisyncUSB;
    bool isRtsp     = _uri.contains("rtsp://") && !isTaisyncUSB;
    bool isTCP      = _uri.contains("tcp://")  && !isTaisyncUSB;
    bool isMPEGTS   = _uri.contains("mpegts://")  && !isTaisyncUSB;
    bool isTestSrc  = _uri.contains("videotestsrc://")  && !isTaisyncUSB;

    if (!isTaisyncUSB && _uri.isEmpty()) {
        qCritical() << "VideoReceiver::start() failed because URI is not specified";
        return;
    }
    if (_videoSink == nullptr) {
        qCritical() << "VideoReceiver::start() failed because video sink is not set";
        return;
    }
    if(_running) {
        qDebug() << "Already running!";
        return;
    }

    _starting = true;

    //-- For RTSP and TCP, check to see if server is there first
    if(!_serverPresent && (isRtsp || isTCP)) {
        _timer.start(100);
        return;
    }

    bool running    = false;
    bool pipelineUp = false;

    GstElement*     dataSource  = nullptr;
    GstCaps*        caps        = nullptr;
    GstElement*     demux       = nullptr;
    GstElement*     parser      = nullptr;
    GstElement*     queue       = nullptr;
    GstElement*     decoder     = nullptr;
    GstElement*     queue1      = nullptr;

    do {
        if ((_pipeline = gst_pipeline_new("receiver")) == nullptr) {
            qCritical() << "VideoReceiver::start() failed. Error with gst_pipeline_new()";
            break;
        }

        if(isUdp || isMPEGTS || isTaisyncUSB) {
            dataSource = gst_element_factory_make("udpsrc", "udp-source");
        } else if(isTCP) {
            dataSource = gst_element_factory_make("tcpclientsrc", "tcpclient-source");
        }  else if(isTestSrc) {
            dataSource = gst_element_factory_make("videotestsrc", "test-source");
        } else {
            dataSource = gst_element_factory_make("rtspsrc", "rtsp-source");
        }

        if (!dataSource) {
            qCritical() << "VideoReceiver::start() failed. Error with data source for gst_element_factory_make()";
            break;
        }

        if(isUdp) {
            if ((caps = gst_caps_from_string("application/x-rtp, media=(string)video, clock-rate=(int)90000, encoding-name=(string)H264")) == nullptr) {
                qCritical() << "VideoReceiver::start() failed. Error with gst_caps_from_string()";
                break;
            }
            g_object_set(static_cast<gpointer>(dataSource), "uri", qPrintable(_uri), "caps", caps, nullptr);
#if  defined(QGC_GST_TAISYNC_ENABLED) && (defined(__android__) || defined(__ios__))
        } else if(isTaisyncUSB) {
            QString uri = QString("0.0.0.0:%1").arg(TAISYNC_VIDEO_UDP_PORT);
            qDebug() << "Taisync URI:" << uri;
            g_object_set(static_cast<gpointer>(dataSource), "port", TAISYNC_VIDEO_UDP_PORT, nullptr);
#endif
        } else if(isTCP) {
            QUrl url(_uri);
            g_object_set(static_cast<gpointer>(dataSource), "host", qPrintable(url.host()), "port", url.port(), nullptr );
        } else if(isMPEGTS) {
            QUrl url(_uri);
            g_object_set(static_cast<gpointer>(dataSource), "port", url.port(), nullptr);
        } else if (isTestSrc) {

        } else {
            g_object_set(static_cast<gpointer>(dataSource), "location", qPrintable(_uri), "latency", 17, "udp-reconnect", 1, "timeout", _udpReconnect_us, NULL);
        }

        if (isTCP || isMPEGTS) {
            if ((demux = gst_element_factory_make("tsdemux", "mpeg-ts-demuxer")) == nullptr) {
                qCritical() << "VideoReceiver::start() failed. Error with gst_element_factory_make('tsdemux')";
                break;
            }
        } else {
            if(!isTaisyncUSB) {
                if ((demux = gst_element_factory_make("rtph264depay", "rtp-h264-depacketizer")) == nullptr) {
                   qCritical() << "VideoReceiver::start() failed. Error with gst_element_factory_make('rtph264depay')";
                    break;
                }
            }
        }

        if ((parser = gst_element_factory_make("h264parse", "h264-parser")) == nullptr) {
            qCritical() << "VideoReceiver::start() failed. Error with gst_element_factory_make('h264parse')";
            break;
        }

        if((_tee = gst_element_factory_make("tee", nullptr)) == nullptr)  {
            qCritical() << "VideoReceiver::start() failed. Error with gst_element_factory_make('tee')";
            break;
        }

        if((queue = gst_element_factory_make("queue", nullptr)) == nullptr)  {
            // TODO: We may want to add queue2 max-size-buffers=1 to get lower latency
            //       We should compare gstreamer scripts to QGroundControl to determine the need
            qCritical() << "VideoReceiver::start() failed. Error with gst_element_factory_make('queue')";
            break;
        }

        if ((decoder = gst_element_factory_make("avdec_h264", "h264-decoder")) == nullptr) {
            qCritical() << "VideoReceiver::start() failed. Error with gst_element_factory_make('avdec_h264')";
            break;
        }

        if ((queue1 = gst_element_factory_make("queue", nullptr)) == nullptr) {
            qCritical() << "VideoReceiver::start() failed. Error with gst_element_factory_make('queue') [1]";
            break;
        }

        if(isTaisyncUSB) {
            gst_bin_add_many(GST_BIN(_pipeline), dataSource, parser, _tee, queue, decoder, queue1, _videoSink, nullptr);
        } else if (isTestSrc) {
            gst_bin_add_many(GST_BIN(_pipeline), dataSource, queue1, _videoSink, nullptr);
        } else {
            gst_bin_add_many(GST_BIN(_pipeline), dataSource, demux, parser, _tee, queue, decoder, queue1, _videoSink, nullptr);
        }
        pipelineUp = true;

        if(isUdp) {
            // Link the pipeline in front of the tee
            if(!gst_element_link_many(dataSource, demux, parser, _tee, queue, decoder, queue1, _videoSink, nullptr)) {
                qCritical() << "Unable to link UDP elements.";
                break;
            }
        } else if (isTestSrc) {
            if(!gst_element_link_many(dataSource, queue1, _videoSink, nullptr)) {
                qCritical() << "Unable to link VideoTestSrc elements.";
                break;
            }
        } else if(isTaisyncUSB) {
            // Link the pipeline in front of the tee
            if(!gst_element_link_many(dataSource, parser, _tee, queue, decoder, queue1, _videoSink, nullptr)) {
                qCritical() << "Unable to link Taisync USB elements.";
                break;
            }
        } else if (isTCP || isMPEGTS) {
            if(!gst_element_link(dataSource, demux)) {
                qCritical() << "Unable to link TCP/MPEG-TS dataSource to Demux.";
                break;
            }
            if(!gst_element_link_many(parser, _tee, queue, decoder, queue1, _videoSink, nullptr)) {
                qCritical() << "Unable to link TCP/MPEG-TS pipline to parser.";
                break;
            }
            g_signal_connect(demux, "pad-added", G_CALLBACK(newPadCB), parser);
        } else {
            g_signal_connect(dataSource, "pad-added", G_CALLBACK(newPadCB), demux);
            if(!gst_element_link_many(demux, parser, _tee, queue, decoder, _videoSink, nullptr)) {
                qCritical() << "Unable to link RTSP elements.";
                break;
            }
        }

        dataSource = demux = parser = queue = decoder = queue1 = nullptr;

        GstBus* bus = nullptr;

        if ((bus = gst_pipeline_get_bus(GST_PIPELINE(_pipeline))) != nullptr) {
            gst_bus_enable_sync_message_emission(bus);
            g_signal_connect(bus, "sync-message", G_CALLBACK(_onBusMessage), this);
            gst_object_unref(bus);
            bus = nullptr;
        }

        GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "pipeline-paused");
        running = gst_element_set_state(_pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;

    } while(0);

    if (caps != nullptr) {
        gst_caps_unref(caps);
        caps = nullptr;
    }

    if (!running) {
        qCritical() << "VideoReceiver::start() failed";

        // In newer versions, the pipeline will clean up all references that are added to it
        if (_pipeline != nullptr) {
            gst_object_unref(_pipeline);
            _pipeline = nullptr;
        }

        // If we failed before adding items to the pipeline, then clean up
        if (!pipelineUp) {
            if (decoder != nullptr) {
                gst_object_unref(decoder);
                decoder = nullptr;
            }

            if (parser != nullptr) {
                gst_object_unref(parser);
                parser = nullptr;
            }

            if (demux != nullptr) {
                gst_object_unref(demux);
                demux = nullptr;
            }

            if (dataSource != nullptr) {
                gst_object_unref(dataSource);
                dataSource = nullptr;
            }

            if (_tee != nullptr) {
                gst_object_unref(_tee);
                dataSource = nullptr;
            }

            if (queue != nullptr) {
                gst_object_unref(queue);
                dataSource = nullptr;
            }
        }

        _running = false;
    } else {
        GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "pipeline-playing");
        _running = true;
        //qDebug() << "Running";
    }
    _starting = false;
#endif
}

//-----------------------------------------------------------------------------
void
VideoReceiver::stop()
{
#if defined(QGC_GST_STREAMING)
    _stop = true;
    qDebug() << "stop()";
    if(!_streaming) {
        _shutdownPipeline();
    } else if (_pipeline != nullptr && !_stopping) {
        qDebug() << "Stopping _pipeline";
        gst_element_send_event(_pipeline, gst_event_new_eos());
        _stopping = true;
        GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(_pipeline));
        GstMessage* message = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE, (GstMessageType)(GST_MESSAGE_EOS|GST_MESSAGE_ERROR));
        gst_object_unref(bus);
        if(GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            _shutdownPipeline();
            qCritical() << "Error stopping pipeline!";
        } else if(GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
            _handleEOS();
        }
        gst_message_unref(message);
    }
#endif
}

//-----------------------------------------------------------------------------
void
VideoReceiver::setUri(const QString & uri)
{
    _uri = uri;
}

//-----------------------------------------------------------------------------
#if defined(QGC_GST_STREAMING)
void
VideoReceiver::_shutdownPipeline() {
    if(!_pipeline) {
        qDebug() << "No pipeline";
        return;
    }
    GstBus* bus = nullptr;
    if ((bus = gst_pipeline_get_bus(GST_PIPELINE(_pipeline))) != nullptr) {
        gst_bus_disable_sync_message_emission(bus);
        gst_object_unref(bus);
        bus = nullptr;
    }
    gst_element_set_state(_pipeline, GST_STATE_NULL);
    gst_bin_remove(GST_BIN(_pipeline), _videoSink);
    gst_object_unref(_pipeline);
    _pipeline = nullptr;
    delete _sink;
    _sink = nullptr;
    _serverPresent = false;
    _streaming = false;
    _recording = false;
    _stopping = false;
    _running = false;
    emit recordingChanged();
}
#endif

//-----------------------------------------------------------------------------
#if defined(QGC_GST_STREAMING)
void
VideoReceiver::_handleError() {
    qDebug() << "Gstreamer error!";
    stop();
    start();
}
#endif

//-----------------------------------------------------------------------------
#if defined(QGC_GST_STREAMING)
void
VideoReceiver::_handleEOS() {
    if(_stopping) {
        _shutdownPipeline();
        qDebug() << "Stopped";
    } else if(_recording && _sink->removing) {
        _shutdownRecordingBranch();
    } else {
        qWarning() << "VideoReceiver: Unexpected EOS!";
        stop();
        start();
    }
}
#endif

//-----------------------------------------------------------------------------
#if defined(QGC_GST_STREAMING)
void
VideoReceiver::_handleStateChanged() {
    if(_pipeline) {
        _streaming = GST_STATE(_pipeline) == GST_STATE_PLAYING;
        //qDebug() << "State changed, _streaming:" << _streaming;
    }
}
#endif

//-----------------------------------------------------------------------------
#if defined(QGC_GST_STREAMING)
gboolean
VideoReceiver::_onBusMessage(GstBus* bus, GstMessage* msg, gpointer data)
{
    Q_UNUSED(bus)
    Q_ASSERT(msg != nullptr && data != nullptr);
    VideoReceiver* pThis = (VideoReceiver*)data;

    switch(GST_MESSAGE_TYPE(msg)) {
    case(GST_MESSAGE_ERROR): {
        gchar* debug;
        GError* error;
        gst_message_parse_error(msg, &error, &debug);
        g_free(debug);
        qCritical() << error->message;
        g_error_free(error);
        pThis->msgErrorReceived();
    }
        break;
    case(GST_MESSAGE_EOS):
        pThis->msgEOSReceived();
        break;
    case(GST_MESSAGE_STATE_CHANGED):
        pThis->msgStateChangedReceived();
        break;
    default:
        break;
    }

    return TRUE;
}
#endif

//-----------------------------------------------------------------------------
#if defined(QGC_GST_STREAMING)
void
VideoReceiver::_cleanupOldVideos()
{

}
#endif

//-----------------------------------------------------------------------------
// When we finish our pipeline will look like this:
//
//                                   +-->queue-->decoder-->_videosink
//                                   |
//    datasource-->demux-->parser-->tee
//                                   |
//                                   |    +--------------_sink-------------------+
//                                   |    |                                      |
//   we are adding these elements->  +->teepad-->queue-->matroskamux-->_filesink |
//                                        |                                      |
//                                        +--------------------------------------+
void
VideoReceiver::startRecording(const QString &videoFile)
{

}

//-----------------------------------------------------------------------------
void
VideoReceiver::stopRecording(void)
{

}

//-----------------------------------------------------------------------------
// This is only installed on the transient _pipelineStopRec in order
// to finalize a video file. It is not used for the main _pipeline.
// -EOS has appeared on the bus of the temporary pipeline
// -At this point all of the recoring elements have been flushed, and the video file has been finalized
// -Now we can remove the temporary pipeline and its elements
#if defined(QGC_GST_STREAMING)
void
VideoReceiver::_shutdownRecordingBranch()
{

}
#endif

//-----------------------------------------------------------------------------
// -Unlink the recording branch from the tee in the main _pipeline
// -Create a second temporary pipeline, and place the recording branch elements into that pipeline
// -Setup watch and handler for EOS event on the temporary pipeline's bus
// -Send an EOS event at the beginning of that pipeline
#if defined(QGC_GST_STREAMING)
void
VideoReceiver::_detachRecordingBranch(GstPadProbeInfo* info)
{
    Q_UNUSED(info)
}
#endif

//-----------------------------------------------------------------------------
#if defined(QGC_GST_STREAMING)
GstPadProbeReturn
VideoReceiver::_unlinkCallBack(GstPad* pad, GstPadProbeInfo* info, gpointer user_data)
{
    Q_UNUSED(pad);
    if(info != nullptr && user_data != nullptr) {
        VideoReceiver* pThis = static_cast<VideoReceiver*>(user_data);
        // We will only act once
        if(g_atomic_int_compare_and_exchange(&pThis->_sink->removing, FALSE, TRUE)) {
            pThis->_detachRecordingBranch(info);
        }
    }
    return GST_PAD_PROBE_REMOVE;
}
#endif

//-----------------------------------------------------------------------------
#if defined(QGC_GST_STREAMING)
GstPadProbeReturn
VideoReceiver::_keyframeWatch(GstPad* pad, GstPadProbeInfo* info, gpointer user_data)
{
    Q_UNUSED(pad);
    if(info != nullptr && user_data != nullptr) {
        GstBuffer* buf = gst_pad_probe_info_get_buffer(info);
        if(GST_BUFFER_FLAG_IS_SET(buf, GST_BUFFER_FLAG_DELTA_UNIT)) { // wait for a keyframe
            return GST_PAD_PROBE_DROP;
        } else {
            VideoReceiver* pThis = static_cast<VideoReceiver*>(user_data);
            // reset the clock
            GstClock* clock = gst_pipeline_get_clock(GST_PIPELINE(pThis->_pipeline));
            GstClockTime time = gst_clock_get_time(clock);
            gst_object_unref(clock);
            gst_element_set_base_time(pThis->_pipeline, time); // offset pipeline timestamps to start at zero again
            buf->dts = 0; // The offset will not apply to this current buffer, our first frame, timestamp is zero
            buf->pts = 0;
            qDebug() << "Got keyframe, stop dropping buffers";
        }
    }

    return GST_PAD_PROBE_REMOVE;
}
#endif

//-----------------------------------------------------------------------------
void
VideoReceiver::_updateTimer()
{
#if defined(QGC_GST_STREAMING)
    if(_videoSurface) {
        if(stopping() || starting()) {
            return;
        }
        if(streaming()) {
            if(!_videoRunning) {
                _videoSurface->setLastFrame(0);
                _videoRunning = true;
                emit videoRunningChanged();
            }
        } else {
            if(_videoRunning) {
                _videoRunning = false;
                emit videoRunningChanged();
            }
        }
        if(_videoRunning) {
            uint32_t timeout = 10;
            //if(qgcApp()->toolbox() && qgcApp()->toolbox()->settingsManager()) {
            //    timeout = _videoSettings->rtspTimeout()->rawValue().toUInt();
            //}
            time_t elapsed = 0;
            time_t lastFrame = _videoSurface->lastFrame();
            if(lastFrame != 0) {
                elapsed = time(nullptr) - _videoSurface->lastFrame();
            }
            if(elapsed > static_cast<time_t>(timeout) && _videoSurface) {
                stop();
                // We want to start it back again with _updateTimer
                _stop = false;
            }
        } else {
            if(!_stop && !running() && !_uri.isEmpty()/* && _videoSettings->streamEnabled()->rawValue().toBool()*/) {
                start();
            }
        }
    }
#endif
}

