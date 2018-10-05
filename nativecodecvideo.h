#ifndef NATIVECODECVIDEO_H
#define NATIVECODECVIDEO_H

/*
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * NativeCodecReader opens and decodes a media file (such as mp4 with h264 or webm)
 * You can query OpenCV's cv::Mat via read()
 * Frame counts are calculated from video duration / current sample timestamps and the framerate.
 * TODO Might be worth having a look at the asynchronous functions so that we can actually push the frames out instead of querying them, which might allow for faster playback without a buffering layer.
 */

/**
 * NativeCodecWriter encodes frames to a video and muxes them to a media file (such as mp4 with h264 or webm)
 * You can push OpenCV's cv::Mat via write()
 * After recording you need to call end() to finish the writing process and to flush the remaining buffers. This might take a while.
 * The object can be deleted once the recordingFinished() signal is emitted (and no earlier!)
 *
 * You might want to run the encoding in a separate thread, e.g.
 *           QThread* mEncodingThread = new QThread();
 *           NativeCodecWriter* videoWriter = new NativeCodecWriter(videofile, fps, Size(data.input.cols, data.input.rows));
 *           videoWriter->moveToThread(mEncodingThread);
 *           mEncodingThread->start();
 *           QMetaObject::invokeMethod(videoWriter, &NativeCodecWriter::prepareEncoder);
 *           connect(this, &DataRecorder::pushFrame, videoWriter, &NativeCodecWriter::write);
 */



// Adapted to C++ from https://bigflake.com/mediacodec/EncodeAndMuxTest.java.txt
// With some lookup from https://android.googlesource.com/platform/cts/+/master/tests/tests/media/libmediandkjni/native-media-jni.cpp
// Also interesting https://android.googlesource.com/platform/cts/+/master/tests/tests/media/libmediandkjni/native_media_encoder_jni.cpp

#include <opencv2/opencv.hpp>
#include <QDebug>
#include <QString>
#include <QFile>
#include <QStandardPaths>


#include "media/NdkMediaCrypto.h"
#include "media/NdkMediaCodec.h"
#include "media/NdkMediaError.h"
#include "media/NdkMediaFormat.h"
#include "media/NdkMediaMuxer.h"
#include "media/NdkMediaExtractor.h"


using std::string;


class NativeCodecReader : public QObject
{
    Q_OBJECT

public:
    NativeCodecReader(QString filename);
    ~NativeCodecReader();

    int64 nFrames();
    int64 currentFrame();
    int64 currentTime();
    int64 totalTime();

    bool seek(cv::Mat& mat, int64 frameNumber);
    bool read(cv::Mat& mat);

    const static int dst_fps = 30; //TODO read this from codec


public slots:
    cv::Mat performRead();

private:
    AMediaExtractor* mExtractor;
    AMediaFormat* mFormat;
    AMediaCodec* mCodec;

    int mTrackIndex;
    bool mMuxerStarted;
    const static int TIMEOUT_USEC = 10000;

    int mFPS;
    QString mFilename;
    cv::Size mSize;

    bool sawInputEOS;
    bool sawOutputEOS;

    int64 mTotalTimeBuffer;


    void  prepareDecoder();

    /**
     * Releases decoder resources.  May be called after partial / failed initialization.
     */
    void releaseDecoder();

    int64 frame2Time(int64 frameNo);


};







class NativeCodecWriter: public QObject
{
    Q_OBJECT
public:
    NativeCodecWriter(QString filename, const int fps, const cv::Size& size);
    ~NativeCodecWriter();

public slots:
    bool write(const cv::Mat& mat, const long long timestamp);
    void end();
    void prepareEncoder();

signals:
    void recordingFinished();


private:
    AMediaCodec* mEncoder;
    AMediaMuxer* mMuxer;
    AMediaCodecBufferInfo mBufferInfo;
    int mTrackIndex;
    bool mMuxerStarted;
    const static int TIMEOUT_USEC = 10000;

    QString mFilename;
    int mFPS;
    cv::Size mSize;

    /**
     * @brief mFrameCounter We need to count frames written in order to be able to compute a presentation time for each frame
     */
    int mFrameCounter;

    /**
     * @brief isRunning has the preparation code been run and not reset?
     */
    bool isRunning;




    /**
     * Extracts all pending data from the encoder.
     * <p>
     * If endOfStream is not set, this returns when there is no more data to drain.  If it
     * is set, we send EOS to the encoder, and then iterate until we see EOS on the output.
     * Calling this with endOfStream set should be done once, right before stopping the muxer.
     */
    void drainEncoder(bool endOfStream);

    /**
     * Releases encoder resources.  May be called after partial / failed initialization.
     */
    void releaseEncoder();

    /**
     * Generates the presentation time for frame N, in nanoseconds.
     */
    long long computePresentationTimeNsec();


};

#endif // NATIVECODECVIDEO_H
