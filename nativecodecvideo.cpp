#include "nativecodecvideo.h"

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

#include <OMXAL/OpenMAXAL.h>
#include <OMXAL/OpenMAXAL_Android.h>



using std::string;

NativeCodecReader::NativeCodecReader(QString filename)
    :QObject(nullptr)
{
    mFilename = filename;
    mTotalTimeBuffer = -1;

    prepareDecoder();

    Q_ASSERT(mExtractor != nullptr);
    size_t numberOfTracks = AMediaExtractor_getTrackCount(mExtractor);
    qDebug() << "Found " << numberOfTracks << " tracks.";

    qDebug() << "Selecting track "<< mTrackIndex;
    media_status_t ret = AMediaExtractor_selectTrack(mExtractor, mTrackIndex);
    if(ret != AMEDIA_OK){
        qWarning() << "AMediaExtractor_selectTrack failed.";
    }

}

NativeCodecReader::~NativeCodecReader(){
    releaseDecoder();
    AMediaFormat_delete(mFormat);
}

cv::Mat NativeCodecReader::performRead(){

    //qDebug() << "performRead";

    if(mSize.empty() || mSize.width == -1 || mSize.height == -1){
        int frameWidth = -1;
        int frameHeight = -1;
        bool ok = AMediaFormat_getInt32(mFormat, AMEDIAFORMAT_KEY_WIDTH, &frameWidth);
        ok = ok && AMediaFormat_getInt32(mFormat, AMEDIAFORMAT_KEY_HEIGHT, &frameHeight);
        if(!ok){
            qWarning() << "Asking format for frame width / height failed.";
            return cv::Mat();
        }
        else{
            qDebug() << "Setting OpenCv Buffer size" << frameWidth << "x" << frameHeight;
            mSize = cv::Size(frameWidth, frameHeight);
        }
    }



    if (mTrackIndex >=0) {

        ssize_t bufidx;

        bufidx = AMediaCodec_dequeueInputBuffer(mCodec, TIMEOUT_USEC);
        //qDebug() << "AMediaCodec_dequeueInputBuffer: " << bufidx;

        //ALOGV("track %d, input buffer %zd", t, bufidx);
        if (bufidx >= 0) {
            size_t bufsize;

            uint8_t *buf = AMediaCodec_getInputBuffer(mCodec, bufidx, &bufsize);
            //qDebug() << "AMediaCodec_getInputBuffer size" << bufsize;
            int sampleSize = AMediaExtractor_readSampleData(mExtractor, buf, bufsize);
            //qDebug() << "AMediaExtractor_readSampleData: " << sampleSize;
            if (sampleSize < 0) {
                sampleSize = 0;
                sawInputEOS = true;
                qDebug() << "Extracting EOS";
                return cv::Mat();
            }


            int64_t presentationTimeUs = AMediaExtractor_getSampleTime(mExtractor);
            AMediaCodec_queueInputBuffer(mCodec, bufidx, 0, sampleSize, presentationTimeUs, sawInputEOS ? AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM : 0);
            //qDebug() << "Pushing new input sample: " << bufidx << " @ " << presentationTimeUs;
            bool ok = AMediaExtractor_advance(mExtractor);
            if(!ok){
                //qWarning() << "Unable to advance extractor";
                // This actually happens regularly and is nothig to worry about
            }
        }
        else{
            qWarning() << "AMediaCodec_dequeueInputBuffer returned invalid buffer idx";
        }
    } else {
        qDebug() << "@@@@ no more input samples";
        if (!sawInputEOS) {
            // we ran out of samples without ever signaling EOS to the codec,
            // so do that now
            int bufidx;
            bufidx = AMediaCodec_dequeueInputBuffer(mCodec, TIMEOUT_USEC);
            if (bufidx >= 0) {
                AMediaCodec_queueInputBuffer(mCodec, bufidx, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                sawInputEOS = true;
                return cv::Mat();
            }

        }

    }



    // check all codecs for available data
    AMediaCodecBufferInfo info;
    AMediaFormat *outputFormat;
    if (!sawOutputEOS) {
        int status;

        status = AMediaCodec_dequeueOutputBuffer(mCodec, &info, 1);

        if (status >= 0) {
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
                qDebug() << "EOS on track";
                sawOutputEOS = true;
            }
            //qDebug() << "got decoded buffer for track";
            cv::Mat colImg;
            if (info.size > 0) {
                size_t bufsize;
                uint8_t *buf = AMediaCodec_getOutputBuffer(mCodec, status, &bufsize);

                //qDebug() << "Converting frame data to OpenCV::Mat (" << bufsize << ")";
                //qDebug() << "Allocating OpenCV buffer " << mSize.width << "x" << mSize.height;
                cv::Mat YUVframe(cv::Size(mSize.width, mSize.height*1.5), CV_8UC1);
                colImg = cv::Mat(mSize, CV_8UC3);
                //qDebug() << "Copying frame data";
                memcpy(YUVframe.data, buf, bufsize);

                //qDebug() << "Color conversion";
                cv::cvtColor(YUVframe, colImg, CV_YUV2BGR_I420, 3);
                //qDebug() << "Conversion done.";
                // right here we have the raw frame data available!


                //int adler = checksum(buf, info.size, mFormat);
                //sizes.add(adler);
            }
            AMediaCodec_releaseOutputBuffer(mCodec, status, false);
            return colImg;
        } else if (status == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            qDebug() << "output buffers changed";
        } else if (status == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            if (mFormat != nullptr) {
                AMediaFormat_delete(mFormat);
            }

            mFormat = AMediaCodec_getOutputFormat(mCodec);
            qDebug() << "format changed " << AMediaFormat_toString(mFormat);

            //int colorCode = -1;
            //AMediaFormat_getInt32(mFormat, AMEDIAFORMAT_KEY_COLOR_FORMAT, &colorCode);
            //qDebug() << "Color code was: " << colorCode;

        } else if (status == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            qWarning() << "no output buffer right now";
        } else {
            qWarning() << "unexpected info code" << status;
        }
    } else {
        qDebug() << "already at EOS";
    }


    return cv::Mat();
}



void  NativeCodecReader::prepareDecoder(){
    mExtractor = AMediaExtractor_new();
    mTotalTimeBuffer = -1;
    if(mExtractor == nullptr){
        qWarning() << "Unable to get a media extractor!";
    }

    //PsshInfo* info = AMediaExtractor_getPsshInfo(mExtractor);
    QString inputPath = mFilename;

    QFile infile(inputPath);
    //if(!infile.open(QIODevice::ReadOnly)){
    //    qWarning() << "Cannot open video file " << inputPath;
    //}


    //qDebug() << "AMediaExtractor_setDataSourceFd "<< 0 << " " << infile.size();
    //media_status_t status = AMediaExtractor_setDataSourceFd(mExtractor, infile.handle(), 0, infile.size());
    media_status_t status = AMediaExtractor_setDataSource(mExtractor, inputPath.toStdString().c_str());

    if(status != AMEDIA_OK){
        qWarning() << "AMediaExtractor_setDataSourceFd failed: " << status;
    }

    int numtracks = AMediaExtractor_getTrackCount(mExtractor);
    if(numtracks != 1){
        qWarning() << "Strange number of tracks";
    }
    mTrackIndex = 0;

    sawInputEOS = false;
    sawOutputEOS = false;


    mFormat = AMediaExtractor_getTrackFormat(mExtractor, mTrackIndex);
    qDebug() << "Media format detected: " << AMediaFormat_toString(mFormat);

    // find out the vuideo duration here (it is not possible later on!)
    qDebug() << "Recalculating totalTime";
    const char* formatDescription = AMediaFormat_toString(mFormat);
    QStringList entries = QString::fromLocal8Bit(formatDescription).split(", ");
    qDebug() << formatDescription;
    for(int i=0; i < entries.size(); i++){
        if(entries[i].startsWith("durationUs:")){
            qDebug() << "Found duration";
            QString crop = entries[i].right(entries[i].length()- QString("durationUs: int64(").length()).chopped(1);
            qDebug() << "Parsing " << entries[i] << " to " << crop << " result " << crop.toLong();
            mTotalTimeBuffer = crop.toLong() /1000;
            break;
        }
    }


    const char *mime;
    if (!AMediaFormat_getString(mFormat, AMEDIAFORMAT_KEY_MIME, &mime)) {
        qWarning() << "Mime type cannot be determined!";
    } else
        if (!strncmp(mime, "video/", 6)) {
            mCodec = AMediaCodec_createDecoderByType(mime);

            media_status_t err =AMediaCodec_configure(mCodec, mFormat, nullptr /* surface */, nullptr /* crypto */, 0);
            if(err != AMEDIA_OK){
                qWarning() << "Error occurred: " << err;
            }

            err =AMediaCodec_start(mCodec);
            if(err != AMEDIA_OK){
                qWarning() << "Error occurred: " << err;
            }
            sawInputEOS = false;
            sawOutputEOS = false;
        } else {
            qWarning() << "expected audio or video mime type, got "<< mime;

        }

    qDebug() << "Decoder ready!";
}



/**
     * Releases decoder resources.  May be called after partial / failed initialization.
     */
void NativeCodecReader::releaseDecoder() {
    qDebug() << "releasing encoder objects";
    if (mCodec != nullptr) {
        AMediaCodec_stop(mCodec);
        AMediaCodec_delete(mCodec);
        mCodec = nullptr;
    }
    /*
    if (mInputSurface != null) {
    mInputSurface.release();
    mInputSurface = null;
    }
    */
    if (mExtractor != nullptr) {
        AMediaExtractor_delete(mExtractor);
        mExtractor = nullptr;
    }
}

int64 NativeCodecReader::nFrames(){
    return totalTime() * dst_fps / 1000;

}

int64 NativeCodecReader::currentFrame(){
    return currentTime() * dst_fps / 1000;
}

int64 NativeCodecReader::currentTime(){
    Q_ASSERT(mExtractor != nullptr);
    int64 time = AMediaExtractor_getSampleTime(mExtractor);
    return time / 1000;
}

int64 NativeCodecReader::totalTime(){


    return mTotalTimeBuffer;

}

bool NativeCodecReader::seek(cv::Mat& mat, int64 frameNumber){
    int64 pos = frame2Time(frameNumber);
    AMediaExtractor_seekTo(mExtractor, pos, SeekMode::AMEDIAEXTRACTOR_SEEK_CLOSEST_SYNC);
    //For decoders that do not support adaptive playback (including when not decoding onto a Surface)
    // In order to start decoding data that is not adjacent to previously submitted data (i.e. after a seek) you MUST flush the decoder.
    AMediaCodec_flush(mCodec);
}

bool NativeCodecReader::read(cv::Mat& mat){
    mat =  performRead();
    return !mat.empty();
}

int64 NativeCodecReader::frame2Time(int64 frameNo){
    return (frameNo*1000000)/dst_fps;
}



NativeCodecWriter::NativeCodecWriter(QString filename, const int fps, const cv::Size& size)
    :QObject(nullptr),
      mFilename(filename),
      mFPS(fps),
      mSize(size),
      isRunning(false)
{
}

NativeCodecWriter::~NativeCodecWriter(){

}

bool NativeCodecWriter::write(const cv::Mat& mat, const long long timestamp){
    // Feed any pending encoder output into the muxer.
    drainEncoder(false);

    if(mat.empty()) return false;

    // Generate a new frame of input.

    /**
                  * Get the index of the next available input buffer. An app will typically use this with
                  * getInputBuffer() to get a pointer to the buffer, then copy the data to be encoded or decoded
                  * into the buffer before passing it to the codec.
                  */
    ssize_t inBufferIdx = AMediaCodec_dequeueInputBuffer(mEncoder, TIMEOUT_USEC);

    /**
                  * Get an input buffer. The specified buffer index must have been previously obtained from
                  * dequeueInputBuffer, and not yet queued.
                  */
    size_t out_size;
    uint8_t* inBuffer = AMediaCodec_getInputBuffer(mEncoder, inBufferIdx, &out_size);

    // Make sure the image is colorful (later on we can try encoding grayscale somehow...)
    cv::Mat colorImg;

    cv::cvtColor(mat, colorImg, CV_BGR2YUV_I420); // COLOR_FormatYUV420SemiPlanar
    // All video codecs support flexible YUV 4:2:0 buffers since Build.VERSION_CODES.LOLLIPOP_MR1.
    /*
        if(mat.channels() == 3){
            cv::cvtColor(mat, colorImg, CV_BGR2YUV_I420);
            }
        else{
            cv::cvtColor(mat, colorImg, CV_GRAY2BGR);
            cv::cvtColor(colorImg, colorImg, CV_BGR2YUV_I420);
        }
        */
    //    colorImg = mat;

    // here we actually copy the data.
    memcpy(inBuffer, colorImg.data, out_size);

    /**
          * Send the specified buffer to the codec for processing.
          */
    //int64_t presentationTimeNs = timestamp;
    int64_t presentationTimeNs = computePresentationTimeNsec();

    media_status_t status = AMediaCodec_queueInputBuffer(mEncoder, inBufferIdx, 0, out_size, presentationTimeNs, mat.empty() ? AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM : 0);

    if(status == AMEDIA_OK){
        //qDebug() << "Successfully pushed frame to input buffer";
    }
    else{
        qWarning() << "Something went wrong while pushing frame to input buffer";
        return false;
    }

    // Submit it to the encoder.  The eglSwapBuffers call will block if the input
    // is full, which would be bad if it stayed full until we dequeued an output
    // buffer (which we can't do, since we're stuck here).  So long as we fully drain
    // the encoder before supplying additional input, the system guarantees that we
    // can supply another frame without blocking.
    //qDebug() << "sending frame " << i << " to encoder";
    //AMediaCodec_flush(mEncoder);
    return true;
}


void NativeCodecWriter::end(){
    qDebug() << "End of recording called!";
    // Send the termination frame
    ssize_t inBufferIdx = AMediaCodec_dequeueInputBuffer(mEncoder, TIMEOUT_USEC);
    size_t out_size;
    uint8_t* inBuffer = AMediaCodec_getInputBuffer(mEncoder, inBufferIdx, &out_size);
    int64_t presentationTimeNs = computePresentationTimeNsec();
    qDebug() << "Sending EOS";
    media_status_t status = AMediaCodec_queueInputBuffer(mEncoder, inBufferIdx, 0, out_size, presentationTimeNs, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
    // send end-of-stream to encoder, and drain remaining output

    drainEncoder(true);

    releaseEncoder();

    // To test the result, open the file with MediaExtractor, and get the format.  Pass
    // that into the MediaCodec decoder configuration, along with a SurfaceTexture surface,
    // and examine the output with glReadPixels.
}


void NativeCodecWriter::prepareEncoder(){


    AMediaFormat* format = AMediaFormat_new();
    AMediaFormat_setInt32(format,AMEDIAFORMAT_KEY_WIDTH,mSize.width);
    AMediaFormat_setInt32(format,AMEDIAFORMAT_KEY_HEIGHT,mSize.height);

    AMediaFormat_setString(format,AMEDIAFORMAT_KEY_MIME,"video/avc"); // H.264 Advanced Video Coding
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, 21); // #21 COLOR_FormatYUV420SemiPlanar (NV12)
    AMediaFormat_setInt32(format,AMEDIAFORMAT_KEY_BIT_RATE,500000);
    AMediaFormat_setFloat(format,AMEDIAFORMAT_KEY_FRAME_RATE,mFPS);
    AMediaFormat_setInt32(format,AMEDIAFORMAT_KEY_I_FRAME_INTERVAL,5);


    //AMediaFormat_setInt32(format,AMEDIAFORMAT_KEY_STRIDE,mSize.width);
    //AMediaFormat_setInt32(format,AMEDIAFORMAT_KEY_M  AX_WIDTH,mSize.width);
    //AMediaFormat_setInt32(format,AMEDIAFORMAT_KEY_MAX_HEIGHT,mSize.height);

    mEncoder = AMediaCodec_createEncoderByType("video/avc");
    if(mEncoder == nullptr){
        qWarning() << "Unable to create encoder";
    }


    media_status_t err = AMediaCodec_configure(mEncoder, format, NULL, NULL, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    if(err != AMEDIA_OK){
        qWarning() << "Error occurred: " << err;
    }

    err = AMediaCodec_start(mEncoder);
    if(err != AMEDIA_OK){
        qWarning() << "Error occurred: " << err;
    }

    if(err != AMEDIA_OK){
        qWarning() << "Error occurred: " << err;
    }


    QFile outFile(mFilename);
    if(!outFile.open(QIODevice::WriteOnly)){
        qWarning() << "Cannot open file: " << mFilename;
    }
    else{
        qDebug() << "Writing video to file:" << mFilename;
    }

    // Create a MediaMuxer.  We can't add the video track and start() the muxer here,
    // because our MediaFormat doesn't have the Magic Goodies.  These can only be
    // obtained from the encoder after it has started processing data.
    //
    // We're not actually interested in multiplexing audio.  We just want to convert
    // the raw H.264 elementary stream we get from MediaCodec into a .mp4 file.
    mMuxer = AMediaMuxer_new(outFile.handle(), AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);

    if(mMuxer == nullptr){
        qWarning() << "Unable to create Muxer";
    }

    mTrackIndex = -1;
    mMuxerStarted = false;
    mFrameCounter = 0;
    isRunning = true;
    qDebug() << "Encoder ready!";
}

/**
     * Extracts all pending data from the encoder.
     * <p>
     * If endOfStream is not set, this returns when there is no more data to drain.  If it
     * is set, we send EOS to the encoder, and then iterate until we see EOS on the output.
     * Calling this with endOfStream set should be done once, right before stopping the muxer.
     */
void NativeCodecWriter::drainEncoder(bool endOfStream) {

    if (endOfStream) {
        qDebug() << "Draining encoder to EOS";
        // only API >= 26
        // Send an empty frame with the end-of-stream flag set.
        // AMediaCodec_signalEndOfInputStream();
        // Instead, we construct that frame manually.
    }




    while (true) {
        ssize_t encoderStatus = AMediaCodec_dequeueOutputBuffer(mEncoder, &mBufferInfo, TIMEOUT_USEC);


        if (encoderStatus == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            // no output available yet
            if (!endOfStream) {
                return;
                //break;      // out of while
            }
            if(endOfStream){
                qDebug() << "no output available, spinning to await EOS";
                return;
            }

        } else if (encoderStatus == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            // not expected for an encoder
        } else if (encoderStatus == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            // should happen before receiving buffers, and should only happen once
            if (mMuxerStarted) {
                qWarning() << "ERROR: format changed twice";
            }
            AMediaFormat* newFormat = AMediaCodec_getOutputFormat(mEncoder);

            if(newFormat == nullptr){
                qWarning() << "Unable to set new format.";
            }

            qDebug() << "encoder output format changed: " + QString::fromStdString(AMediaFormat_toString(newFormat));

            // now that we have the Magic Goodies, start the muxer
            mTrackIndex = AMediaMuxer_addTrack(mMuxer, newFormat);
            media_status_t err = AMediaMuxer_start(mMuxer);

            if(err != AMEDIA_OK){
                qWarning() << "Error occurred: " << err;
            }

            mMuxerStarted = true;
        } else if (encoderStatus < 0) {
            qWarning() << "unexpected result from encoder.dequeueOutputBuffer: " + QString::number(encoderStatus);
            // let's ignore it
        } else {

            size_t out_size;
            uint8_t* encodedData = AMediaCodec_getOutputBuffer(mEncoder, encoderStatus, &out_size);

            if(out_size <= 0){
                qWarning() << "Encoded data of size 0.";
            }

            if (encodedData == nullptr) {
                qWarning() << "encoderOutputBuffer " + QString::number(encoderStatus) + " was null";
            }


            if ((mBufferInfo.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) != 0) {
                // The codec config data was pulled out and fed to the muxer when we got
                // the INFO_OUTPUT_FORMAT_CHANGED status.  Ignore it.
                qDebug() << "ignoring BUFFER_FLAG_CODEC_CONFIG";
                mBufferInfo.size = 0;
            }

            if (mBufferInfo.size != 0) {
                if (!mMuxerStarted) {
                    qWarning() << "muxer hasn't started";
                }


                // adjust the ByteBuffer values to match BufferInfo (not needed?)
                //encodedData.position(mBufferInfo.offset);
                //encodedData.limit(mBufferInfo.offset + mBufferInfo.size);

                AMediaMuxer_writeSampleData(mMuxer, mTrackIndex, encodedData, &mBufferInfo);
                //qDebug() << "sent " + QString::number(mBufferInfo.size) + " bytes to muxer";
            }
            else{
                qWarning()<< "mBufferInfo empty " << mBufferInfo.size;
            }

            AMediaCodec_releaseOutputBuffer(mEncoder, encoderStatus, false);

            if ((mBufferInfo.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0) {
                if (!endOfStream) {
                    qWarning() << "reached end of stream unexpectedly";
                } else {
                    qDebug() << "end of stream reached";

                }
                break;      // out of while
            }
        }
    }
}

/**
 * Releases encoder resources.  May be called after partial / failed initialization.
 */
void NativeCodecWriter::releaseEncoder() {
    qDebug() << "releasing encoder objects";
    if (mEncoder != nullptr) {
        AMediaCodec_stop(mEncoder);
    }

    if (mMuxer != nullptr) {
        AMediaMuxer_stop(mMuxer);
    }

    if (mEncoder != nullptr) {
        AMediaCodec_delete(mEncoder);
        mEncoder = nullptr;
    }

    if (mMuxer != nullptr) {
        AMediaMuxer_delete(mMuxer);
        mMuxer = nullptr;
    }

    isRunning = false;
    emit recordingFinished();
}

/**
         * Generates the presentation time for frame N, in nanoseconds.
         */

long long NativeCodecWriter::computePresentationTimeNsec() {
    mFrameCounter++;
    double timePerFrame = 1000000.0/mFPS;
    return static_cast<long long>(mFrameCounter*timePerFrame);
}


