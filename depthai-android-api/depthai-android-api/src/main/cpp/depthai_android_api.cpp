#include <chrono>
#include <string>

// g++ -DPIPELINE_LOCAL_TEST -I/mnt/btrfs-data/venvs/ml-tutorials/repos/depthai-core/include -I/mnt/btrfs-data/venvs/ml-tutorials/repos/depthai-shared/include -I/mnt/btrfs-data/venvs/ml-tutorials/repos/depthai-shared/libnop/include -I/mnt/btrfs-data/venvs/ml-tutorials/repos/depthai-shared/json/include -I/mnt/btrfs-data/venvs/ml-tutorials/repos/depthai-android-unity-example/depthai-android-api/depthai-android-api/src/main/include depthai_android_api.cpp -o depth -L. -ldepthai

#ifndef PIPELINE_LOCAL_TEST
#include <android/log.h>

#include <libusb/libusb/libusb.h>
#include <jni.h>
#endif

#include "depthai/depthai.hpp"

#include <iostream>
#include <cstdarg>
#include <sstream> 
#include <fstream>
#include <chrono>

#include "utils.h"
#include "date.h"

#define LOG_TAG "depthaiAndroid"
#define log(...) __android_log_print(ANDROID_LOG_INFO,LOG_TAG, __VA_ARGS__)

#ifndef PIPELINE_LOCAL_TEST
JNIEnv* jni_env = NULL;
JavaVM* JVM;

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved)
{
    // Ref: https://stackoverflow.com/a/49947762/13706271
    JVM = vm;
    if (vm->GetEnv((void**)&jni_env, JNI_VERSION_1_6) != JNI_OK)
        log("ERROR: GetEnv failed");

    return JNI_VERSION_1_6;
}

extern "C"
{
#endif
    using namespace std;

    std::shared_ptr<dai::Device> device;
    std::shared_ptr<dai::DataOutputQueue> qRgb, qDisparity, qDepth;

    std::vector<u_char> rgbImageBuffer, colorDisparityBuffer;

    const int disparityWidth = 640;
    const int disparityHeight = 400;

    // Closer-in minimum depth, disparity range is doubled (from 95 to 190):
    std::atomic<bool> extended_disparity{true};
    auto maxDisparity = extended_disparity ? 190.0f :95.0f;

    // Better accuracy for longer distance, fractional disparity 32-levels:
    std::atomic<bool> subpixel{false};
    // Better handling for occlusions:
    std::atomic<bool> lr_check{false};


    //std::string external_storage_path = "/storage/emulated/0/";
    //std::string external_storage_path = "/storage/emulated/0/Android/data/com.DepthaiAndroid.UnityDepthaiAndroidExample/files/";

    struct video_info
    {
        std::shared_ptr<dai::DataOutputQueue> outQ1;
        std::shared_ptr<dai::DataOutputQueue> outQ2;
        std::shared_ptr<dai::DataOutputQueue> outQ3;

        std::ofstream videoFile1;
        std::ofstream videoFile2;
        std::ofstream videoFile3;

        std::ofstream logfile;

	std::uint64_t frame_counter = 0;
    };

    video_info v_info;

    void api_stop_device()
    {
        v_info.logfile.close();
    }

    void api_log(const char *format, ...)
    {
	#ifndef PIPELINE_LOCAL_TEST
        log("0. format: %s", format);
	#endif
	std::stringstream ss;
        /*
        va_list args;
        va_start(args, fmt);
        while (*fmt != '\0')
	{
            if (*fmt == 'd')
	    {
                int i = va_arg(args, int);
                ss << i << ", ";
            }
	    else if (*fmt == 's')
	    {
                log("1. fmt: %s", 's');
                char * s = va_arg(args, char*);
                log("2. s: %s", s);
                ss << s << ", ";
            }
	    else if (*fmt == 'l')
	    {
                int64_t l = va_arg(args, int);
                ss << l << ", ";
            }
	    else if (*fmt == 'u')
	    {
                uint64_t ul = va_arg(args, int);
                ss << ul << ", ";
            }
            ++fmt;
        }
        */

	char buffer[4096];
	va_list args;
	va_start(args, format);
	vsnprintf(buffer, 4095, format, args);

	auto now = std::chrono::system_clock::now();
	ss << date::format("%d/%m/%Y %H:%M:%S -- ", now) << buffer;

	va_end (args);


	#ifndef PIPELINE_LOCAL_TEST
        log("%s", ss.str().c_str());
	#endif
        v_info.logfile << ss.str() << std::endl;
     
        va_end(args);
    }

    void api_open_logfile(std::string external_storage_path)
    {
        std::string logfile_fname = external_storage_path + "/depthai-android-api.log";
        v_info.logfile.open(logfile_fname, std::ios::app);
        api_log("Opening logfile: %s", logfile_fname.c_str());
        v_info.logfile << logfile_fname + " starting..." << std::endl;
    }

    void api_start_device(int rgbWidth, int rgbHeight, unsigned char* external_storage_path)
    {
	api_open_logfile(std::string(reinterpret_cast<char const*>(external_storage_path)));

	#ifndef PIPELINE_LOCAL_TEST
        // libusb
        auto r = libusb_set_option(nullptr, LIBUSB_OPTION_ANDROID_JNIENV, jni_env);
        api_log("libusb_set_option ANDROID_JAVAVM: %s", libusb_strerror(r));
	#endif

        // Create pipeline
        dai::Pipeline pipeline;

        // Define source and output
        auto camRgb = pipeline.create<dai::node::ColorCamera>();
        auto monoLeft = pipeline.create<dai::node::MonoCamera>();
        auto monoRight = pipeline.create<dai::node::MonoCamera>();
        auto stereo = pipeline.create<dai::node::StereoDepth>();

        auto xoutRgb = pipeline.create<dai::node::XLinkOut>();
        auto xoutDisparity = pipeline.create<dai::node::XLinkOut>();
        auto xoutDepth = pipeline.create<dai::node::XLinkOut>();

        xoutRgb->setStreamName("rgb");
        xoutDisparity->setStreamName("disparity");
        xoutDepth->setStreamName("depth");

        // Properties
        camRgb->setPreviewSize(rgbWidth, rgbHeight);
        camRgb->setBoardSocket(dai::CameraBoardSocket::RGB);
        camRgb->setResolution(dai::ColorCameraProperties::SensorResolution::THE_1080_P);
        camRgb->setInterleaved(true);
        camRgb->setColorOrder(dai::ColorCameraProperties::ColorOrder::RGB);

        monoLeft->setResolution(dai::MonoCameraProperties::SensorResolution::THE_400_P);
        monoLeft->setBoardSocket(dai::CameraBoardSocket::LEFT);
        monoRight->setResolution(dai::MonoCameraProperties::SensorResolution::THE_400_P);
        monoRight->setBoardSocket(dai::CameraBoardSocket::RIGHT);

        stereo->initialConfig.setConfidenceThreshold(245);
        // Options: MEDIAN_OFF, KERNEL_3x3, KERNEL_5x5, KERNEL_7x7 (default)
        stereo->initialConfig.setMedianFilter(dai::MedianFilter::KERNEL_7x7);
        stereo->setLeftRightCheck(lr_check);
        stereo->setExtendedDisparity(extended_disparity);
        stereo->setSubpixel(subpixel);

        // Linking
        camRgb->preview.link(xoutRgb->input);
        monoLeft->out.link(stereo->left);
        monoRight->out.link(stereo->right);
        stereo->disparity.link(xoutDisparity->input);
        stereo->depth.link(xoutDepth->input);

        // Connect to device and start pipeline
        device = std::make_shared<dai::Device>(pipeline, dai::UsbSpeed::SUPER);

        auto device_info = device->getDeviceInfo();
	#ifndef PIPELINE_LOCAL_TEST
        api_log("%s",device_info.toString().c_str());
	#endif

        // Output queue will be used to get the rgb frames from the output defined above
        qRgb = device->getOutputQueue("rgb", 4, false);
        qDisparity = device->getOutputQueue("disparity", 4, false);
        qDepth = device->getOutputQueue("depth", 4, false);

        // Resize image buffers
        rgbImageBuffer.resize(rgbWidth*rgbHeight*4);
        colorDisparityBuffer.resize(disparityWidth*disparityHeight*4);

        api_log("Device Connected!");
        v_info.logfile << "Device Connected!" << std::endl;
    }

    unsigned int api_get_rgb_image(unsigned char* unityImageBuffer)
    {
        auto inRgb = qRgb->get<dai::ImgFrame>();
        auto imgData = inRgb->getData();

        // Convert from RGB to RGBA for Unity
        int rgb_index = 0;
        int argb_index = 0;
        for (int y = 0; y < inRgb->getHeight(); y ++)
        {
            for (int x = 0; x < inRgb->getWidth(); x ++)
            {
                rgbImageBuffer[argb_index++] = imgData[rgb_index++]; // red
                rgbImageBuffer[argb_index++] = imgData[rgb_index++]; // green
                rgbImageBuffer[argb_index++] = imgData[rgb_index++]; // blue
                rgbImageBuffer[argb_index++] = 255; // alpha

            }
        }

        // Copy the new image to the Unity image buffer
        std::copy(rgbImageBuffer.begin(), rgbImageBuffer.end(), unityImageBuffer);

        // Return the image number
        return inRgb->getSequenceNum();
    }

    unsigned int api_get_color_disparity_image(unsigned char* unityImageBuffer)
    {
        auto inDisparity = qDisparity->get<dai::ImgFrame>();;
        auto disparityData = inDisparity->getData();
        uint8_t colorPixel[3];

        // Convert Disparity to RGBA format for Unity
        int argb_index = 0;
        for (int i = 0; i < disparityWidth*disparityHeight; i++)
        {
            // Convert the disparity to color
            colorDisparity(colorPixel, disparityData[i], maxDisparity);

            colorDisparityBuffer[argb_index++] = colorPixel[0]; // red
            colorDisparityBuffer[argb_index++] = colorPixel[1]; // green
            colorDisparityBuffer[argb_index++] = colorPixel[2]; // blue
            colorDisparityBuffer[argb_index++] = 255; // alpha
        }

        // Copy the new image to the Unity image buffer
        std::copy(colorDisparityBuffer.begin(), colorDisparityBuffer.end(), unityImageBuffer);

        // Return the image number
        return inDisparity->getSequenceNum();
    }


    void api_start_device_record_video(unsigned char* external_storage_path)
    {
   	std::string ext_storage_path = std::string(reinterpret_cast<char const*>(external_storage_path));
	api_open_logfile(ext_storage_path);

        //std::string fname_prefix = std::string(cstr_fname_prefix) - TODO: de-hardcode fname
        std::string fname_prefix = ext_storage_path + "/depthai-video-";

        // Create pipeline
        dai::Pipeline pipeline;
        api_log("Pipeline started");
    
        // Define sources and outputs
        auto camRgb = pipeline.create<dai::node::ColorCamera>();
        auto monoLeft = pipeline.create<dai::node::MonoCamera>();
        auto monoRight = pipeline.create<dai::node::MonoCamera>();

        auto ve1 = pipeline.create<dai::node::VideoEncoder>();
        auto ve2 = pipeline.create<dai::node::VideoEncoder>();
        auto ve3 = pipeline.create<dai::node::VideoEncoder>();
    
        auto ve1Out = pipeline.create<dai::node::XLinkOut>();
        auto ve2Out = pipeline.create<dai::node::XLinkOut>();
        auto ve3Out = pipeline.create<dai::node::XLinkOut>();
    
        ve1Out->setStreamName("ve1Out");
        ve2Out->setStreamName("ve2Out");
        ve3Out->setStreamName("ve3Out");
    
        // Properties
        camRgb->setBoardSocket(dai::CameraBoardSocket::RGB);
        monoLeft->setBoardSocket(dai::CameraBoardSocket::LEFT);
        monoRight->setBoardSocket(dai::CameraBoardSocket::RIGHT);

        // Create encoders, one for each camera, consuming the frames and encoding them using H.264 / H.265 encoding
        ve1->setDefaultProfilePreset(30, dai::VideoEncoderProperties::Profile::H264_MAIN);      // left
        ve2->setDefaultProfilePreset(30, dai::VideoEncoderProperties::Profile::H265_MAIN);      // RGB
        ve3->setDefaultProfilePreset(30, dai::VideoEncoderProperties::Profile::H264_MAIN);      // right
        api_log("H.264/H.265 video encoders created");
    
        // Linking
        monoLeft->out.link(ve1->input);
        camRgb->video.link(ve2->input);
        monoRight->out.link(ve3->input);

        api_log("H.264/H.265 video encoders linking done");

        ve1->bitstream.link(ve1Out->input);
        ve2->bitstream.link(ve2Out->input);
        ve3->bitstream.link(ve3Out->input);

        api_log("H.264/H.265 video encoders bitstream linking done");
    
        // Connect to device and start pipeline
        dai::Device device(pipeline);

        api_log("DepthAI device created");
    
        // Output queues will be used to get the encoded data from the output defined above
        auto outQ1 = device.getOutputQueue("ve1Out", 30, true);
        auto outQ2 = device.getOutputQueue("ve2Out", 30, true);
        auto outQ3 = device.getOutputQueue("ve3Out", 30, true);

        api_log("Output queues created");
    
        // The .h264 / .h265 files are raw stream files (not playable yet)
	std::string left_fn  = fname_prefix + std::string("left.h264" );
	std::string color_fn = fname_prefix + std::string("color.h265");
	std::string right_fn = fname_prefix + std::string("right.h264");
        v_info.videoFile1 = std::ofstream(left_fn , std::ios::binary);
        v_info.videoFile2 = std::ofstream(color_fn, std::ios::binary);
        v_info.videoFile3 = std::ofstream(right_fn, std::ios::binary);
        api_log("Output files opened (%s - %s - %s)", left_fn.c_str(), color_fn.c_str(), right_fn.c_str());

        v_info.outQ1 = outQ1;
        v_info.outQ2 = outQ2;
        v_info.outQ3 = outQ3;

	/*    
        v_info.videoFile1 = videoFile1;
        v_info.videoFile2 = videoFile2;
        v_info.videoFile3 = videoFile3;
	*/

        //cout << "Press Ctrl+C to stop encoding..." << endl;
        /*
        while(alive)
        {
            auto out1 = outQ1->get<dai::ImgFrame>();
            videoFile1.write((char*)out1->getData().data(), out1->getData().size());
            auto out2 = outQ2->get<dai::ImgFrame>();
            videoFile2.write((char*)out2->getData().data(), out2->getData().size());
            auto out3 = outQ3->get<dai::ImgFrame>();
            videoFile3.write((char*)out3->getData().data(), out3->getData().size());
        }
    
        cout << "To view the encoded data, convert the stream file (.h264/.h265) into a video file (.mp4), using a command below:" << endl;
        cout << "ffmpeg -framerate 30 -i mono1.h264 -c copy mono1.mp4" << endl;
        cout << "ffmpeg -framerate 30 -i mono2.h264 -c copy mono2.mp4" << endl;
        cout << "ffmpeg -framerate 30 -i color.h265 -c copy color.mp4" << endl;
        */
    }
    unsigned long api_get_video_frames()
    {
            auto out1 = v_info.outQ1->get<dai::ImgFrame>();
            v_info.videoFile1.write((char*)out1->getData().data(), out1->getData().size());
            auto out2 = v_info.outQ2->get<dai::ImgFrame>();
            v_info.videoFile2.write((char*)out2->getData().data(), out2->getData().size());
            auto out3 = v_info.outQ3->get<dai::ImgFrame>();
            v_info.videoFile3.write((char*)out3->getData().data(), out3->getData().size());

	    v_info.frame_counter++;
	    if (v_info.frame_counter % 1000)
 	           api_log("Read video frame no.: %lu", v_info.frame_counter);

            return v_info.frame_counter++;
    }
#ifndef PIPELINE_LOCAL_TEST
}
#endif



