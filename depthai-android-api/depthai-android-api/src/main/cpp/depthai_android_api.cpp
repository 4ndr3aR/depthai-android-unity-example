#include <chrono>
#include <string>

#ifndef PIPELINE_LOCAL_TEST
#include <android/log.h>

#include <libusb/libusb/libusb.h>
#include <jni.h>
#endif

#include <typeinfo>     // for typeid

#include <iostream>
#include <cstdarg>
#include <sstream> 
#include <fstream>
#include <chrono>

#include "depthai/depthai.hpp"

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


    int color_fps = 15;
    int mono_fps = 15;

    // Closer-in minimum depth, disparity range is doubled (from 95 to 190):
    std::atomic<bool> extended_disparity{false};
    auto maxDisparity = extended_disparity ? 190.0f :95.0f;

    // Better accuracy for longer distance, fractional disparity 32-levels:
    std::atomic<bool> subpixel{false};
    // Better handling for occlusions:
    std::atomic<bool> lr_check{false};

    int recv_img_debug_verbosity = 1000;

    struct video_info
    {
        std::shared_ptr<dai::DataOutputQueue> outQRgb;
        std::ofstream videoFileRgb;

	#ifdef ENCODE_RGB_L_R
        std::shared_ptr<dai::DataOutputQueue> outQLeft;
        std::ofstream videoFileLeft;
        std::shared_ptr<dai::DataOutputQueue> outQRight;
        std::ofstream videoFileRight;
	#else
        std::shared_ptr<dai::DataOutputQueue> outQDisp;
        std::ofstream videoFileDisp;
        std::shared_ptr<dai::DataOutputQueue> outQRectRight;
        std::ofstream videoFileRectRight;
	#endif

        std::shared_ptr<dai::DataOutputQueue> qRgb;
        std::shared_ptr<dai::DataOutputQueue> qDisparity;
        std::shared_ptr<dai::DataOutputQueue> qDepth;

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
	std::stringstream ss;

	char buffer[4096];
	va_list args;
	va_start(args, format);
	vsnprintf(buffer, 4095, format, args);

	auto now = std::chrono::system_clock::now();
	ss << date::format("%d/%m/%Y %H:%M:%S -- ", now) << buffer;

	va_end (args);

	#ifndef PIPELINE_LOCAL_TEST
        log("%s", ss.str().c_str());
	#else
	std::cout << std::fixed << std::setprecision(3) << ss.str().c_str() << std::endl;		// no effect on date-time format
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

    unsigned int api_get_rgb_image(unsigned char* unityImageBuffer)
    {
        auto inRgb = qRgb->get<dai::ImgFrame>();
        auto frame_no = inRgb->getSequenceNum();
        auto imgData = inRgb->getData();

	if (inRgb.get() and frame_no % recv_img_debug_verbosity == 0)
	{
		api_log("api_get_rgb_image() received inRgb ptr: %p", inRgb.get());
		api_log("api_get_rgb_image() received image with size: %d x %d", inRgb->getWidth(), inRgb->getHeight());
		api_log("api_get_rgb_image() received frame no: %d - frame imgData size: %d", frame_no, imgData.size());
	}

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
        return frame_no;
    }

    unsigned int api_get_color_disparity_image(unsigned char* unityImageBuffer)
    {
        auto inDisparity = qDisparity->get<dai::ImgFrame>();;
        auto frame_no = inDisparity->getSequenceNum();
        auto disparityData = inDisparity->getData();
        uint8_t colorPixel[3];

	if (inDisparity and frame_no % recv_img_debug_verbosity == 0)
	{
		api_log("api_get_color_disparity_image() received inDisparity ptr: %p", inDisparity.get());
		api_log("api_get_color_disparity_image() received image with size: %d x %d", inDisparity->getWidth(), inDisparity->getHeight());
		api_log("api_get_color_disparity_image() received frame no: %d - frame disparityData size: %d", frame_no, disparityData.size());
	}

        // Convert Disparity to RGBA format for Unity
        int argb_index = 0;
        for (int i = 0; i < inDisparity->getWidth()*inDisparity->getHeight(); i++)
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
        return frame_no;
    }


    int api_start_device_record_video(int rgbWidth, int rgbHeight, int disparityWidth, int disparityHeight, const char* external_storage_path)
    {
   	std::string ext_storage_path = std::string(external_storage_path);
	api_open_logfile(ext_storage_path);

        api_log("api_start_device_record_video() - received images RGB size from Unity: %d x %d", rgbWidth, rgbHeight);
        api_log("api_start_device_record_video() - received images disparity size from Unity: %d x %d", disparityWidth, disparityHeight);

	#ifndef PIPELINE_LOCAL_TEST
        // libusb
        auto r = libusb_set_option(nullptr, LIBUSB_OPTION_ANDROID_JNIENV, jni_env);
        api_log("libusb_set_option ANDROID_JAVAVM: %s", libusb_strerror(r));
	#endif

        std::string fname_prefix = ext_storage_path + "/depthai-video-";

        // Create pipeline
        dai::Pipeline pipeline;
        api_log("Pipeline started");
    
        // Define sources and outputs
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
        camRgb->setBoardSocket(dai::CameraBoardSocket::RGB);
        monoLeft->setBoardSocket(dai::CameraBoardSocket::LEFT);
        monoRight->setBoardSocket(dai::CameraBoardSocket::RIGHT);

        // Properties
        camRgb->setPreviewSize(rgbWidth, rgbHeight);
        camRgb->setResolution(dai::ColorCameraProperties::SensorResolution::THE_1080_P);
        camRgb->setFps(color_fps);
        camRgb->setInterleaved(true);
        camRgb->setColorOrder(dai::ColorCameraProperties::ColorOrder::RGB);


        monoLeft->setResolution(dai::MonoCameraProperties::SensorResolution::THE_400_P);
        monoLeft->setFps(mono_fps);
        monoRight->setResolution(dai::MonoCameraProperties::SensorResolution::THE_400_P);
        monoRight->setFps(mono_fps);

        stereo->initialConfig.setConfidenceThreshold(245);

	#ifndef ENCODE_RGB_L_R
	auto xoutRectRight = pipeline.create<dai::node::XLinkOut>();
	xoutRectRight->setStreamName("rectRight");
	stereo->rectifiedRight.link(xoutRectRight->input);
	#endif


        // Options: MEDIAN_OFF, KERNEL_3x3, KERNEL_5x5, KERNEL_7x7 (default)
        stereo->initialConfig.setMedianFilter(dai::MedianFilter::KERNEL_7x7);
        stereo->setLeftRightCheck(lr_check);
        stereo->setRectifyEdgeFillColor(0);
        stereo->setExtendedDisparity(extended_disparity);
        stereo->setSubpixel(subpixel);

        // Linking
        camRgb->preview.link(xoutRgb->input);
        monoLeft->out.link(stereo->left);
        monoRight->out.link(stereo->right);
        stereo->disparity.link(xoutDisparity->input);
        stereo->depth.link(xoutDepth->input);


	#ifdef ENCODE_RGB_L_R
        auto veLeft = pipeline.create<dai::node::VideoEncoder>();
        auto veRgb = pipeline.create<dai::node::VideoEncoder>();
        auto veRight = pipeline.create<dai::node::VideoEncoder>();
    
        auto veLeftOut = pipeline.create<dai::node::XLinkOut>();
        auto veRgbOut = pipeline.create<dai::node::XLinkOut>();
        auto veRightOut = pipeline.create<dai::node::XLinkOut>();

        // Linking
        monoLeft->out.link(veLeft->input);
        camRgb->video.link(veRgb->input);
        monoRight->out.link(veRight->input);

        api_log("H.264/H.265 video encoders linking done");

        veLeft->bitstream.link(veLeftOut->input);
        veRgb->bitstream.link(veRgbOut->input);
        veRight->bitstream.link(veRightOut->input);

        api_log("H.264/H.265 video encoders bitstream linking done");


        veLeftOut->setStreamName("veLeftOut");
        veRgbOut->setStreamName("veRgbOut");
        veRightOut->setStreamName("veRightOut");
    
        // Create encoders, one for each camera, consuming the frames and encoding them using H.264 / H.265 encoding
        veLeft->setDefaultProfilePreset(monoLeft->getFps(),   dai::VideoEncoderProperties::Profile::H265_MAIN);      // left
        veRgb->setDefaultProfilePreset(camRgb->getFps(),      dai::VideoEncoderProperties::Profile::H265_MAIN);      // RGB
        veRight->setDefaultProfilePreset(monoRight->getFps(), dai::VideoEncoderProperties::Profile::H265_MAIN);      // right
        api_log("H.264/H.265 video encoders created - RGB-FPS: %f - L-FPS: %f - R-FPS: %f", camRgb->getFps(), monoLeft->getFps(), monoRight->getFps());
	#else
        auto veDisp = pipeline.create<dai::node::VideoEncoder>();
        auto veRgb = pipeline.create<dai::node::VideoEncoder>();
        auto veRectRight = pipeline.create<dai::node::VideoEncoder>();
    
        auto veDispOut = pipeline.create<dai::node::XLinkOut>();
        auto veRgbOut = pipeline.create<dai::node::XLinkOut>();
        auto veRectRightOut = pipeline.create<dai::node::XLinkOut>();

        // Linking
        stereo->disparity.link(veDisp->input);
        camRgb->video.link(veRgb->input);
        monoRight->out.link(veRectRight->input);

        api_log("H.264/H.265 video encoders linking done");

        veDisp->bitstream.link(veDispOut->input);
        veRgb->bitstream.link(veRgbOut->input);
        veRectRight->bitstream.link(veRectRightOut->input);

        api_log("H.264/H.265 video encoders bitstream linking done");


        veDispOut->setStreamName("veDispOut");
        veRgbOut->setStreamName("veRgbOut");
        veRectRightOut->setStreamName("veRectRightOut");
    
        // Create encoders, one for each camera, consuming the frames and encoding them using H.264 / H.265 encoding
        veDisp->setDefaultProfilePreset(monoLeft->getFps(),       dai::VideoEncoderProperties::Profile::H265_MAIN);      // disp
        veRgb->setDefaultProfilePreset(camRgb->getFps(),          dai::VideoEncoderProperties::Profile::H265_MAIN);      // RGB
        veRectRight->setDefaultProfilePreset(monoRight->getFps(), dai::VideoEncoderProperties::Profile::H265_MAIN);      // rectRight
        api_log("H.264/H.265 video encoders created - RGB-FPS: %f - D-FPS: %f - RR-FPS: %f", camRgb->getFps(), monoLeft->getFps(), monoRight->getFps());
	#endif
    
	bool exception_thrown = false;
	try
	{
	        // Connect to device and start pipeline
	        device = std::make_shared<dai::Device>(pipeline, dai::UsbSpeed::SUPER);
	}
	catch (const std::overflow_error & err)
	{
		exception_thrown = true;
        	api_log("Overflow error exception: «%s». Exiting...", err.what());
	} // this executes if f() throws std::overflow_error (same type rule)
	catch (const std::runtime_error & err)
	{
		exception_thrown = true;
        	api_log("Runtime error exception: «%s». Exiting...", err.what());
	} // this executes if f() throws std::underflow_error (base class rule)
	catch (const std::exception & err)
	{
		exception_thrown = true;
        	api_log("Unknown exception: «%s». Exiting...", err.what());
	} // this executes if f() throws std::logic_error (base class rule)
	catch (...)
	{
		exception_thrown = true;
        	api_log("Unknown exception. Exiting...");
	} // this executes if f() throws std::string or int or any other unrelated type
	if (exception_thrown)
		return -1;

        api_log("DepthAI device created");
        auto device_info = device->getDeviceInfo();
	#ifndef PIPELINE_LOCAL_TEST
        api_log("Device info: %s", device_info.toString().c_str());
	#endif


    
	#ifdef ENCODE_RGB_L_R
        // Output queues will be used to get the encoded data from the output defined above
        auto outQLeft = device->getOutputQueue("veLeftOut", 1, false);
        auto outQRgb = device->getOutputQueue("veRgbOut", 1, false);
        auto outQRight = device->getOutputQueue("veRightOut", 1, false);

        api_log("Output queues created");

	std::stringstream curr_date_time;
	auto now = return_next_full_second();
	curr_date_time << std::fixed << std::setprecision(2) << date::format("%Y%m%d-%H%M%S", now);		// No effect sadly :(
	auto curr_date_time_str = curr_date_time.str();
	auto pos = curr_date_time_str.find("."); 

        // The H.264/H.265 files are raw stream files (not playable yet)
	std::string left_fn  = fname_prefix + curr_date_time_str.substr(0,pos) + std::string("-left.h265" );
	std::string color_fn = fname_prefix + curr_date_time_str.substr(0,pos) + std::string("-color.h265");
	std::string right_fn = fname_prefix + curr_date_time_str.substr(0,pos) + std::string("-right.h265");
        v_info.videoFileLeft = std::ofstream(left_fn , std::ios::binary);
        v_info.videoFileRgb = std::ofstream(color_fn, std::ios::binary);
        v_info.videoFileRight = std::ofstream(right_fn, std::ios::binary);
        api_log("Output files opened (%s - %s - %s)", left_fn.c_str(), color_fn.c_str(), right_fn.c_str());

        v_info.outQLeft = outQLeft;
        v_info.outQRgb = outQRgb;
        v_info.outQRight = outQRight;
	#else
        // Output queues will be used to get the encoded data from the output defined above
        auto outQDisp = device->getOutputQueue("veDispOut", 1, false);
        auto outQRgb = device->getOutputQueue("veRgbOut", 1, false);
        auto outQRectRight = device->getOutputQueue("veRectRightOut", 1, false);

        api_log("Output queues created");

	std::stringstream curr_date_time;
	auto now = return_next_full_second();
	curr_date_time << std::fixed << std::setprecision(2) << date::format("%Y%m%d-%H%M%S", now);		// No effect sadly :(
	auto curr_date_time_str = curr_date_time.str();
	auto pos = curr_date_time_str.find("."); 

        // The H.264/H.265 files are raw stream files (not playable yet)
	std::string disp_fn  = fname_prefix + curr_date_time_str.substr(0,pos) + std::string("-disp.h265" );
	std::string color_fn = fname_prefix + curr_date_time_str.substr(0,pos) + std::string("-color.h265");
	std::string rright_fn = fname_prefix + curr_date_time_str.substr(0,pos) + std::string("-rright.h265");
        v_info.videoFileDisp = std::ofstream(disp_fn , std::ios::binary);
        v_info.videoFileRgb = std::ofstream(color_fn, std::ios::binary);
        v_info.videoFileRectRight = std::ofstream(rright_fn, std::ios::binary);
        api_log("Output files opened (%s - %s - %s)", disp_fn.c_str(), color_fn.c_str(), rright_fn.c_str());

        v_info.outQDisp = outQDisp;
        v_info.outQRgb = outQRgb;
        v_info.outQRectRight = outQRectRight;
	#endif


        // Output queue will be used to get the rgb frames from the output defined above
        qRgb = device->getOutputQueue("rgb", 1, false);
        qDisparity = device->getOutputQueue("disparity", 1, false);
        qDepth = device->getOutputQueue("depth", 1, false);

        v_info.qRgb = qRgb;
        v_info.qDisparity = qDisparity;
        v_info.qDepth = qDepth;


        // Resize image buffers
        rgbImageBuffer.resize(rgbWidth*rgbHeight*4);
        colorDisparityBuffer.resize(disparityWidth*disparityHeight*4);

        api_log("Device Connected!");

	return 0;
    }
    unsigned long api_write_one_video_frame(std::shared_ptr<dai::DataOutputQueue> outQ, std::ofstream & videoFile)
    {
        auto out = outQ->get<dai::ImgFrame>();
        videoFile.write((char*)out->getData().data(), out->getData().size());

        return out->getData().size();   // compressed size of the H.264/H.265 frame
    }
    unsigned long api_get_video_frames()
    {
            api_write_one_video_frame(v_info.outQRgb, v_info.videoFileRgb);
	    #ifdef ENCODE_RGB_L_R
            api_write_one_video_frame(v_info.outQLeft, v_info.videoFileLeft);
            api_write_one_video_frame(v_info.outQRight, v_info.videoFileRight);
	    #else
            api_write_one_video_frame(v_info.outQDisp, v_info.videoFileDisp);
            api_write_one_video_frame(v_info.outQRectRight, v_info.videoFileRectRight);
	    #endif

	    v_info.frame_counter++;
	    if (v_info.frame_counter % 1000 == 0)
	    {
 	           api_log("Read video frame no.: %lu", v_info.frame_counter);
	    }

            return v_info.frame_counter;
    }
#ifndef PIPELINE_LOCAL_TEST
}
#endif


#ifdef PIPELINE_LOCAL_TEST
int main()
{
        uint64_t frame_no = 0;
        uint64_t rgbFrameNum = 0;
        uint64_t dispFrameNum = 0;
        int RGBWidth  = 640;
        int RGBHeight = 480;
	int disparityWidth  = 1280;
	int disparityHeight = 720;
        std::string external_storage_path = "/tmp/";

        int retval = api_start_device_record_video(RGBWidth, RGBHeight, disparityWidth, disparityHeight, external_storage_path.c_str());
	if (retval == -1)
	{
		return retval;
	}

        unsigned char* _rgbImgPtr  = new unsigned char[rgbImageBuffer.size()];
        unsigned char* _dispImgPtr = new unsigned char[colorDisparityBuffer.size()];
        std::cout << "rgbImageBuffer.size(): " << rgbImageBuffer.size() << std::endl;
        std::cout << "colorDisparityBuffer.size(): " << colorDisparityBuffer.size() << std::endl;

        while (true)
        {
                frame_no = api_get_video_frames();
                std::cout << "Received frame: " << frame_no << std::endl;
                rgbFrameNum = api_get_rgb_image(_rgbImgPtr);
                dispFrameNum = api_get_color_disparity_image(_dispImgPtr);

        }
}
#endif

