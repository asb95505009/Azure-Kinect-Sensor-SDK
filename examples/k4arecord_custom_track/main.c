// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <k4a/k4a.h>
#include <k4arecord/record.h>

#define VERIFY(result)                                                                                                 \
    if (result != K4A_RESULT_SUCCEEDED)                                                                                \
    {                                                                                                                  \
        printf("%s \n - (File: %s, Function: %s, Line: %d)\n", #result " failed", __FILE__, __FUNCTION__, __LINE__);   \
        exit(1);                                                                                                       \
    }

#define FOURCC(cc) ((cc)[0] | (cc)[1] << 8 | (cc)[2] << 16 | (cc)[3] << 24)

// Codec context struct for Codec ID: "V_MS/VFW/FOURCC"
// See https://docs.microsoft.com/en-us/windows/desktop/wmdm/-bitmapinfoheader
typedef struct
{
    uint32_t biSize;
    uint32_t biWidth;
    uint32_t biHeight;
    uint16_t biPlanes;
    uint16_t biBitCount;
    uint32_t biCompression;
    uint32_t biSizeImage;
    uint32_t biXPelsPerMeter;
    uint32_t biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
} BITMAPINFOHEADER;

void fill_bitmap_header(uint32_t width, uint32_t height, BITMAPINFOHEADER *out);
void fill_bitmap_header(uint32_t width, uint32_t height, BITMAPINFOHEADER *out)
{
    out->biSize = sizeof(BITMAPINFOHEADER);
    out->biWidth = width;
    out->biHeight = height;
    out->biPlanes = 1;
    out->biBitCount = 16;
    out->biCompression = FOURCC("YUY2");
    out->biSizeImage = sizeof(uint16_t) * width * height;
    out->biXPelsPerMeter = 0;
    out->biYPelsPerMeter = 0;
    out->biClrUsed = 0;
    out->biClrImportant = 0;
}
//------------------------------------------------------------------------------------codec tuned?
int main(int argc, char **argv)                
{
    if (argc < 2)                                               //it seems not necessary
    {
        printf("Usage: k4arecord_custom_track output.mkv\n");
        exit(0);
    }

    char *recording_filename = argv[1];                         //argv stores recording filename

    k4a_device_configuration_t device_config = K4A_DEVICE_CONFIG_INIT_DISABLE_ALL;      //setup config
    device_config.depth_mode = K4A_DEPTH_MODE_NFOV_UNBINNED;
    device_config.camera_fps = K4A_FRAMES_PER_SECOND_30;

    k4a_device_t device;
    VERIFY(k4a_device_open(0, &device));                        //open device
    VERIFY(k4a_device_start_cameras(device, &device_config));   //start camera

    printf("Device started\n");

    k4a_record_t recording;                                                                         //1.initialize recording sample
    if (K4A_FAILED(k4a_record_create(recording_filename, device, device_config, &recording)))       //2.create recording sample
    {
        printf("Unable to create recording file: %s\n", recording_filename);
        return 1;
    }

    // Add a hello_world.txt attachment to the recording
    const char *attachment_data = "Hello, World!\n";
    VERIFY(k4a_record_add_attachment(recording,
                                     "hello_world.txt",
                                     (const uint8_t *)attachment_data,
                                     strlen(attachment_data)));

    // Add a custom recording tag
    VERIFY(k4a_record_add_tag(recording, "CUSTOM_TAG", "Hello, World!"));

    // Add a custom video track to store processed depth images.
    // Read the depth resolution from the camera configuration so we can create our custom track with the same size.
    k4a_calibration_t sensor_calibration;                                                           //3.initialize custom video track
    VERIFY(k4a_device_get_calibration(device, device_config.depth_mode, K4A_COLOR_RESOLUTION_OFF, &sensor_calibration));//4.create custom video track
    uint32_t depth_width = (uint32_t)sensor_calibration.depth_camera_calibration.resolution_width;    //5.setting custom video track
    uint32_t depth_height = (uint32_t)sensor_calibration.depth_camera_calibration.resolution_height;

    BITMAPINFOHEADER codec_header;                                      //6. initialize codec_header  
    fill_bitmap_header(depth_width, depth_height, &codec_header);       //7. create header

    k4a_record_video_settings_t video_settings;                         //8. initialize record format setting
    video_settings.width = depth_width;                                 //9. setting record format setting
    video_settings.height = depth_height;
    video_settings.frame_rate = 30; // Should be the same rate as device_config.camera_fps

    // Add the video track to the recording.                            //think recording as one carrier = video + tag +...
    VERIFY(k4a_record_add_custom_video_track(recording,                 //10. add video track to the recording
                                             "PROCESSED_DEPTH",         
                                             "V_MS/VFW/FOURCC",
                                             (uint8_t *)(&codec_header),
                                             sizeof(codec_header),
                                             &video_settings));

    // Write the recording header now that all the track metadata is set up.   //11.write header 
    VERIFY(k4a_record_write_header(recording));                                //setup completed

    
    
    
    
    
    
    // Start reading 100 depth frames (~3 seconds at 30 fps) from the camera.   //reading frames from camera to recording
    for (int frame = 0; frame < 100; frame++)
    {
        k4a_capture_t capture;                                                  //1. initialize capture
        k4a_wait_result_t get_capture_result = k4a_device_get_capture(device, &capture, K4A_WAIT_INFINITE);//2. create capture
        if (get_capture_result == K4A_WAIT_RESULT_SUCCEEDED)
        {
            // Write the capture to the built-in tracks
            VERIFY(k4a_record_write_capture(recording, capture));               //3. write capture to recording

            // Get the depth image from the capture so we can write a processed copy to our custom track.
            k4a_image_t depth_image = k4a_capture_get_depth_image(capture);     //4. get depth image from capture --> put into recording
            if (depth_image)                                                    
            {
                // The YUY2 image format is the same stride as the 16-bit depth image, so we can modify it in-place.
                uint8_t *depth_buffer = k4a_image_get_buffer(depth_image);      //5.change depth image to buffer
                size_t depth_buffer_size = k4a_image_get_size(depth_image);     //6.get size of depth image
                for (size_t i = 0; i < depth_buffer_size; i += 2)               //7. "for-loop" turn depth value into color format
                {
                    // Convert the depth value (16-bit, in millimeters) to the YUY2 color format.
                    // The YUY2 format should be playable in video players such as VLC.
                    uint16_t depth = (uint16_t)(depth_buffer[i + 1] << 8 | depth_buffer[i]);    //depth to color format
                    // Clamp the depth range to ~1 meter and scale it to fit in the Y channel of the image (8-bits).
                    if (depth > 0x3FF)                                                          
                    {
                        depth_buffer[i] = 0xFF;
                    }
                    else
                    {
                        depth_buffer[i] = (uint8_t)(depth >> 2);
                    }
                    // Set the U/V channel to 128 (i.e. grayscale).
                    depth_buffer[i + 1] = 128;
                }

                VERIFY(k4a_record_write_custom_track_data(recording,            //8.write YUY2 color video format into recording
                                                          "PROCESSED_DEPTH",
                                                          k4a_image_get_device_timestamp_usec(depth_image),
                                                          depth_buffer,
                                                          (uint32_t)depth_buffer_size));

                k4a_image_release(depth_image);                 //9.release depth image at the end of depth image handling
            }

            k4a_capture_release(capture);                       //10.release capture
        }
        else if (get_capture_result == K4A_WAIT_RESULT_TIMEOUT)
        {
            // TIMEOUT should never be returned when K4A_WAIT_INFINITE is set.
            printf("k4a_device_get_capture() timed out!\n");
            break;
        }
        else
        {
            printf("k4a_device_get_capture() returned error: %d\n", get_capture_result);
            break;
        }
    }

    k4a_device_stop_cameras(device);                            //11.stop camera

    printf("Saving recording...\n");
    VERIFY(k4a_record_flush(recording));                        //12.flush data into recording
    k4a_record_close(recording);                                //13.release recording

    printf("Done\n");
    k4a_device_close(device);                                   //14.release device

    return 0;
}
