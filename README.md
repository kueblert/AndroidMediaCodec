# AndroidMediaCodec
Android Media codec for video encoding and decoding from C++ for use with QT.
By using the native codec we have the possibility of leveraging hardware acceleration for compression and decoding, if available on the device.

*Not fully tested yet, ergo not stable!*

I am using API 23, lower versions (>18?) are probably possible.
I am not able to provide any support but thought this might help some people to get through the partially quite rough documeentation for the ndk.


NativeCodecReader opens and decodes a media file (such as mp4 with h264 or webm)
You can query OpenCV's cv::Mat via read()
Frame counts are calculated from video duration / current sample timestamps and the framerate.
TODO Might be worth having a look at the asynchronous functions so that we can actually push the frames out instead of querying them, which might allow for faster playback without a buffering layer.


NativeCodecWriter encodes frames to a video and muxes them to a media file (such as mp4 with h264 or webm)
You can push OpenCV's cv::Mat via write()
After recording you need to call end() to finish the writing process and to flush the remaining buffers. This might take a while.
The object can be deleted once the recordingFinished() signal is emitted (and no earlier!)

You might want to run the encoding in a separate thread, e.g.

QThread* mEncodingThread = new QThread();
NativeCodecWriter* videoWriter = new NativeCodecWriter(videofile, fps, Size(data.input.cols, data.input.rows));
videoWriter->moveToThread(mEncodingThread);
mEncodingThread->start();
QMetaObject::invokeMethod(videoWriter, &NativeCodecWriter::prepareEncoder);
connect(this, &DataRecorder::pushFrame, videoWriter, &NativeCodecWriter::write);


