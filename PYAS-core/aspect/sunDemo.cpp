#define MAX_THREADS 20
#define SAVE_LOCATION "/mnt/disk2/" // location for saving full images locally
#define REPORT_FOCUS false

//Default camera settings
#define CAMERA_EXPOSURE 15000 // microseconds, was 4500 microseconds in first Sun test
#define CAMERA_ANALOGGAIN 400 // camera defaults to 143, but we are changing it
#define CAMERA_PREAMPGAIN -3 // camera defaults to +6, but we are changing it
#define CAMERA_XSIZE 1296 // full frame is 1296
#define CAMERA_YSIZE 966 //full frame is 966
#define CAMERA_XOFFSET 0
#define CAMERA_YOFFSET 0

//Sleep settings (seconds)
#define SLEEP_SOLUTION         1 // period for providing solutions to CTL
#define SLEEP_SAVE             5 // period for saving full images locally
#define SLEEP_LOG_TEMPERATURE 10 // period for logging temperature locally
#define SLEEP_CAMERA_CONNECT   1 // waits for errors while connecting to camera
#define SLEEP_KILL             2 // waits when killing all threads

//Sleep settings (microseconds)
#define USLEEP_CMD_SEND     5000 // period for popping off the command queue
#define USLEEP_TM_SEND     50000 // period for popping off the telemetry queue
#define USLEEP_TM_GENERIC 250000 // period for adding generic telemetry packets to queue

#define SAS1_MAC_ADDRESS "00:20:9d:23:26:b9"
#define SAS2_MAC_ADDRESS "00:20:9d:23:5c:9e"

//IP addresses
#define IP_FDR      "192.168.2.4"   // will be 192.168.1.1 in flight
#define IP_CTL      "192.168.2.4"   // will be 192.168.1.2 in flight
#define IP_SAS1     "192.168.2.221" // will be 192.168.1.32 in flight
#define IP_SAS2     "192.168.16.16" // not yet implemented
#define IP_PYAS     "192.168.4.4"   // not yet implemented
#define IP_RAS      "192.168.8.8"   // not yet implemented

#define IP_LOOPBACK "127.0.0.1"   // should not change

//UDP ports, aside from PORT_IMAGE, which is TCP
#define PORT_CMD      2000 // commands, FDR (receive) and CTL (send/receive)
#define PORT_TM       2002 // send telemetry to FDR (except images)
#define PORT_IMAGE    2013 // send images to FDR, TCP port
#define PORT_SBC_INFO 3456 //

//HEROES target ID for commands, source ID for telemetry
#define TARGET_ID_CTL 0x01
#define SOURCE_ID_SAS 0x30

//HEROES telemetry types
#define TM_ACK_RECEIPT 0x01
#define TM_ACK_PROCESS 0xE1
#define TM_SAS_GENERIC 0x70
#define TM_SAS_IMAGE   0x82
#define TM_SAS_TAG     0x83

//HEROES commands, CTL/FDR to SAS
#define HKEY_CTL_START_TRACKING  0x1000
#define HKEY_CTL_STOP_TRACKING   0x1001
#define HKEY_FDR_SAS_CMD         0x10FF

//HEROES commands, SAS to CTL
#define HKEY_SAS_TRACKING_IS_ON  0x1100
#define HKEY_SAS_TRACKING_IS_OFF 0x1101
#define HKEY_SAS_SOLUTION        0x1102
#define HKEY_SAS_ERROR           0x1103
#define HKEY_SAS_TIMESTAMP       0x1104

//Operation commands
#define SKEY_OP_DUMMY            0x0000
#define SKEY_KILL_WORKERS        0x0010
#define SKEY_RESTART_THREADS     0x0020
#define SKEY_START_OUTPUTTING    0x0030
#define SKEY_STOP_OUTPUTTING     0x0040

//Setting commands
#define SKEY_SET_TARGET          0x0112
#define SKEY_SET_EXPOSURE        0x0151
#define SKEY_SET_ANALOGGAIN      0x0181
#define SKEY_SET_PREAMPGAIN      0x0191

//Getting commands
#define SKEY_REQUEST_IMAGE       0x0210

#include <cstring>
#include <stdio.h>      /* for printf() and fprintf() */
#include <pthread.h>    /* for multithreading */
#include <stdlib.h>     /* for atoi() and exit() */
#include <unistd.h>     /* for sleep()  */
#include <signal.h>     /* for signal() */
#include <math.h>       /* for testing only, remove when done */
#include <ctime>        /* time_t, struct tm, time, gmtime */
#include <opencv.hpp>
#include <iostream>
#include <string>

#include "UDPSender.hpp"
#include "UDPReceiver.hpp"
#include "Command.hpp"
#include "Telemetry.hpp"
#include "Image.hpp"
#include "Transform.hpp"
#include "types.hpp"
#include "TCPSender.hpp"
#include "ImperxStream.hpp"
#include "processing.hpp"
#include "compression.hpp"
#include "utilities.hpp"

// global declarations
uint16_t command_sequence_number = 0;
uint16_t latest_heroes_command_key = 0x0000;
uint16_t latest_sas_command_key = 0x0000;
uint16_t latest_sas_command_vars[15];
uint32_t tm_frame_sequence_number = 0;
uint16_t solution_sequence_number = 0;

bool isTracking = false; // does CTL want solutions?
bool isOutputting = false; // is this SAS supposed to be outputting solutions?
bool acknowledgedCTL = true; // have we acknowledged the last command from CTL?

CommandQueue recvd_command_queue;
TelemetryPacketQueue tm_packet_queue;
CommandPacketQueue cm_packet_queue;
ImagePacketQueue im_packet_queue;

// related to threads
unsigned int stop_message[MAX_THREADS];
pthread_t threads[MAX_THREADS];
bool started[MAX_THREADS];
int tid_listen = 0;
pthread_attr_t attr;
pthread_mutex_t mutexImage;
pthread_mutex_t mutexProcess;

struct Thread_data{
    int  thread_id;
    uint16_t command_key;
    uint8_t command_num_vars;
    uint16_t command_vars[15];
};
struct Thread_data thread_data[MAX_THREADS];

sig_atomic_t volatile g_running = 1;

int sas_id;

cv::Mat frame;

Aspect aspect;
AspectCode runResult;
Transform solarTransform;

uint8_t frameMin, frameMax;
cv::Point2f pixelCenter, screenCenter, error;
CoordList limbs, pixelFiducials, screenFiducials;
IndexList ids;
std::vector<float> mapping;

HeaderData keys;

bool staleFrame;
Flag procReady, saveReady;
int runtime = 10;
uint16_t exposure = CAMERA_EXPOSURE;
uint16_t analogGain = CAMERA_ANALOGGAIN;
int16_t preampGain = CAMERA_PREAMPGAIN;

timespec frameRate = {0,100000000L};
int cameraReady = 0;

timespec frameTime;
long int frameCount = 0;

float camera_temperature;
int8_t sbc_temperature;
float sbc_v105, sbc_v25, sbc_v33, sbc_v50, sbc_v120;

//Function declarations
void sig_handler(int signum);
void kill_all_threads( void ); //kills all threads
void kill_all_workers( void ); //kills all threads except the one that listens for commands
void identifySAS();
void *CameraStreamThread( void * threadargs);
void *ImageProcessThread(void *threadargs);
void *TelemetrySenderThread(void *threadargs);
void *SBCInfoThread(void *threadargs);
void *SaveTemperaturesThread(void *threadargs);
void *SaveImageThread(void *threadargs);
void *TelemetryPackagerThread(void *threadargs);
void *listenForCommandsThread(void *threadargs);
void *CommandSenderThread( void *threadargs );
void *CommandPackagerThread( void *threadargs );
void queue_cmd_proc_ack_tmpacket( uint16_t error_code );
uint16_t cmd_send_image_to_ground( int camera_id );
void *commandHandlerThread(void *threadargs);
void cmd_process_heroes_command(uint16_t heroes_command);
void cmd_process_sas_command(uint16_t sas_command, Command &command);
void start_all_workers( void );
void start_thread(void *(*start_routine) (void *), const Thread_data *tdata);

void sig_handler(int signum)
{
    if (signum == SIGINT)
    {
        g_running = 0;
    }
}

void kill_all_workers( void ){
    for(int i = 0; i < MAX_THREADS; i++ ){
        if ((i != tid_listen) && started[i]) {
            stop_message[i] = 1;
        }
    }
    sleep(SLEEP_KILL);
    for(int i = 0; i < MAX_THREADS; i++ ){
        if ((i != tid_listen) && started[i]) {
            printf("Quitting thread %i, quitting status is %i\n", i, pthread_cancel(threads[i]));
            started[i] = false;
        }
    }
}

void kill_all_threads( void){
    if (started[tid_listen]) {
        stop_message[tid_listen] = 1;
        kill_all_workers();
        printf("Quitting thread %i, quitting status is %i\n", tid_listen, pthread_cancel(threads[tid_listen]));
        started[tid_listen] = false;
    }
}

void identifySAS()
{
    FILE *in;
    char buff[128];

    if(!(in = popen("ifconfig sbc | grep ether", "r"))) {
        std::cout << "Error identifying computer, defaulting to SAS-1\n";
        sas_id = 1;
        return;
    }

    fgets(buff, sizeof(buff), in);

    if(strstr(buff, SAS1_MAC_ADDRESS) != NULL) {
        std::cout << "SAS-1 identified\n";
        sas_id = 1;
    } else if(strstr(buff, SAS2_MAC_ADDRESS) != NULL) {
        std::cout << "SAS-2 identified\n";
        sas_id = 2;
    } else {
        std::cout << "Unknown computer, defaulting to SAS-1\n";
        sas_id = 1;
    }

    pclose(in);
}

void *CameraStreamThread( void * threadargs)
{    
    long tid = (long)((struct Thread_data *)threadargs)->thread_id;
    printf("CameraStream thread #%ld!\n", tid);

    ImperxStream camera;

    cv::Mat localFrame;
    timespec preExposure, postExposure, timeElapsed, duration;
    int width, height;
    int failcount = 0;

    uint16_t localExposure = exposure;
    int16_t localPreampGain = preampGain;
    uint16_t localAnalogGain = analogGain;

    cameraReady = 0;
    staleFrame = true;
    while(1)
    {
        if (stop_message[tid] == 1)
        {
            printf("CameraStream thread #%ld exiting\n", tid);
            camera.Stop();
            camera.Disconnect();
            started[tid] = false;
            pthread_exit( NULL );
        }
        else if (cameraReady == false)
        {
            if (camera.Connect() != 0)
            {
                std::cout << "Error connecting to camera!\n";
                sleep(SLEEP_CAMERA_CONNECT);
                continue;
            }
            else
            {
                camera.ConfigureSnap();
                camera.SetROISize(CAMERA_XSIZE,CAMERA_YSIZE);
                camera.SetROIOffset(CAMERA_XOFFSET,CAMERA_YOFFSET);
                camera.SetExposure(localExposure);
                camera.SetAnalogGain(localAnalogGain);
                camera.SetPreAmpGain(localPreampGain);

                width = camera.GetROIWidth();
                height = camera.GetROIHeight();
                localFrame.create(height, width, CV_8UC1);
                if(camera.Initialize() != 0)
                {
                    std::cout << "Error initializing camera!\n";
                    //may need disconnect here
                    sleep(SLEEP_CAMERA_CONNECT);
                    continue;
                }
                cameraReady = 1;
                frameCount = 0;
            }
        }
        else
        {
            if (localExposure != exposure) {
                localExposure = exposure;
                camera.SetExposure(localExposure);
            }
            
            if (localPreampGain != preampGain) {
                localPreampGain = preampGain;
                camera.SetPreAmpGain(localPreampGain);
            }
            
            if (localAnalogGain != analogGain) {
                localAnalogGain = analogGain;
                camera.SetAnalogGain(analogGain);
            }

            clock_gettime(CLOCK_REALTIME, &preExposure);

            if(!camera.Snap(localFrame))
            {
                failcount = 0;
                procReady.raise();
                saveReady.raise();

                //printf("CameraStreamThread: trying to lock\n");
                pthread_mutex_lock(&mutexImage);
                //printf("CameraStreamThread: got lock, copying over\n");
                localFrame.copyTo(frame);
                frameTime = preExposure;
                //printf("%d\n", frame.at<uint8_t>(0,0));
                frameCount++;
                pthread_mutex_unlock(&mutexImage);
                staleFrame = false;

                //printf("camera temp is %lld\n", camera.getTemperature());
                camera_temperature = camera.getTemperature();
            }
            else
            {
                failcount++;
                std::cout << "Frame failure count = " << failcount << std::endl;
                if (failcount >= 10)
                {
                    camera.Stop();
                    camera.Disconnect();
                    cameraReady = false;
                    staleFrame = true;
                    std::cout << "*********************** RESETTING CAMERA ***********************************" << std::endl;
                    continue;
                }
            }
            clock_gettime(CLOCK_REALTIME, &postExposure);
            timeElapsed = TimespecDiff(preExposure, postExposure);
            duration.tv_sec = frameRate.tv_sec - timeElapsed.tv_sec;
            duration.tv_nsec = frameRate.tv_nsec - timeElapsed.tv_nsec;
//            std::cout << timeElapsed.tv_sec << " " << timeElapsed.tv_nsec << "\n";
            nanosleep(&duration, NULL);
        }
    }
}

void *ImageProcessThread(void *threadargs)
{
    long tid = (long)((struct Thread_data *)threadargs)->thread_id;
    printf("ImageProcess thread #%ld!\n", tid);

    CoordList localLimbs, localPixelFiducials, localScreenFiducials;
    IndexList localIds;
    uint8_t localMin, localMax;
    std::vector<float> localMapping;
    cv::Point2f localPixelCenter, localScreenCenter, localError;
    timespec waittime;

    waittime.tv_sec = frameRate.tv_sec/10;
    waittime.tv_nsec = frameRate.tv_nsec/10;
    
    while(1)
    {
        if (stop_message[tid] == 1)
        {
            printf("ImageProcess thread #%ld exiting\n", tid);
            started[tid] = false;
            pthread_exit( NULL );
        }
        
        if (cameraReady)
        {
            while(1)
            {
                if(procReady.check())
                {
                    procReady.lower();
                    break;
                }
                else
                {
                    nanosleep(&waittime, NULL);
                }
            }
    
            //printf("ImageProcessThread: trying to lock\n");
            if (pthread_mutex_trylock(&mutexImage) == 0)
            {
                //printf("ImageProcessThread: got lock\n");
                if(!frame.empty())
                {
                    aspect.LoadFrame(frame);

                    pthread_mutex_unlock(&mutexImage);

                    runResult = aspect.Run();
                    
                    switch(GeneralizeError(runResult))
                    {
                        case NO_ERROR:
                            aspect.GetScreenFiducials(localScreenFiducials);
                            aspect.GetScreenCenter(localScreenCenter);
                            aspect.GetMapping(localMapping);

                        case MAPPING_ERROR:
                            aspect.GetFiducialIDs(localIds);

                        case ID_ERROR:
                            aspect.GetPixelFiducials(localPixelFiducials);

                        case FIDUCIAL_ERROR:
                            aspect.GetPixelCenter(localPixelCenter);
                            aspect.GetPixelError(localError);

                        case CENTER_ERROR:
                            aspect.GetPixelCrossings(localLimbs);
                            if (REPORT_FOCUS) aspect.ReportFocus();

                        case LIMB_ERROR:
                        case RANGE_ERROR:
                            aspect.GetPixelMinMax(localMin, localMax);
                            break;
                        default:
                            std::cout << "Nothing worked\n";
                    }

                    pthread_mutex_lock(&mutexProcess);
                    switch(GeneralizeError(runResult))
                    {
                        case NO_ERROR:
                            screenFiducials = localScreenFiducials;
                            screenCenter = localScreenCenter;
                            mapping = localMapping;
                        case MAPPING_ERROR:
                            ids = localIds;

                        case ID_ERROR:
                            pixelFiducials = localPixelFiducials;

                        case FIDUCIAL_ERROR:
                            pixelCenter = localPixelCenter;  
                            error = localError;

                        case CENTER_ERROR:
                            limbs = localLimbs;

                        case LIMB_ERROR:
                        case RANGE_ERROR:
                            frameMin = localMin;
                            frameMax = localMax;
                            break;
                        default:
                            break;
                    }
                    pthread_mutex_unlock(&mutexProcess);
                }
                else
                {
                    //std::cout << "Frame empty!" << std::endl;
                }

                /*
                  std::cout << ids.size() << " fiducials found:";
                  for(uint8_t i = 0; i < ids.size() && i < 20; i++) std::cout << pixelFiducials[i];
                  std::cout << std::endl;

                  for(uint8_t i = 0; i < ids.size() && i < 20; i++) std::cout << ids[i];
                  std::cout << std::endl;

                  for(uint8_t i = 0; i < ids.size() && i < 20; i++) std::cout << screenFiducials[i];
                  std::cout << std::endl;

                  std::cout << "Sun center (pixels): " << pixelCenter << ", Sun center (screen): " << screenCenter << std::endl;
                */
            }
        }
    }
}

void *TelemetrySenderThread(void *threadargs)
{    
    long tid = (long)((struct Thread_data *)threadargs)->thread_id;
    printf("TelemetrySender thread #%ld!\n", tid);

    TelemetrySender telSender(IP_FDR, (unsigned short) PORT_TM);

    while(1)    // run forever
    {
        usleep(USLEEP_TM_SEND);

        if( !tm_packet_queue.empty() ){
            TelemetryPacket tp(NULL);
            tm_packet_queue >> tp;
            telSender.send( &tp );
            //std::cout << "TelemetrySender:" << tp << std::endl;
        }

        if (stop_message[tid] == 1){
            printf("TelemetrySender thread #%ld exiting\n", tid);
            started[tid] = false;
            pthread_exit( NULL );
        }
    }
}

void *SBCInfoThread(void *threadargs)
{
    long tid = (long)((struct Thread_data *)threadargs)->thread_id;
    printf("SBCInfo thread #%ld!\n", tid);

    UDPReceiver receiver(PORT_SBC_INFO);
    receiver.init_connection();

    uint16_t packet_length;
    uint8_t *array;

    while(1)
    {
        if (stop_message[tid] == 1)
        {
            printf("SBCInfo thread #%ld exiting\n", tid);
            started[tid] = false;
            pthread_exit( NULL );
        }

        //This call will block forever if the service is not running
        packet_length = receiver.listen();
        array = new uint8_t[packet_length];
        receiver.get_packet(array);

        Packet packet( array, packet_length );
        packet >> sbc_temperature >> sbc_v105 >> sbc_v25 >> sbc_v33 >> sbc_v50 >> sbc_v120;
    }
}

void *SaveTemperaturesThread(void *threadargs)
{
    long tid = (long)((struct Thread_data *)threadargs)->thread_id;
    printf("SaveTemperatures thread #%ld!\n", tid);

    char stringtemp[80];
    char obsfilespec[128];
    FILE *file;
    time_t ltime;
    struct tm *times;

    time(&ltime);
    times = localtime(&ltime);
    strftime(stringtemp,40,"%y%m%d_%H%M%S",times);
    sprintf(obsfilespec, "%stemp_data_%s.dat", SAVE_LOCATION, stringtemp);
    obsfilespec[128 - 1] = '\0';
    printf("Creating file %s \n",obsfilespec);

    if((file = fopen(obsfilespec, "w")) == NULL){
        printf("Cannot open file\n");
        started[tid] = false;
        pthread_exit( NULL );
    } else {
        fprintf(file, "time, camera temp, cpu temp\n");
        sleep(SLEEP_LOG_TEMPERATURE);
        while(1)
        {
            char current_time[25];
            if (stop_message[tid] == 1)
            {
                printf("SaveTemperatures thread #%ld exiting\n", tid);
                fclose(file);
                started[tid] = false;
                pthread_exit( NULL );
            }
            sleep(SLEEP_LOG_TEMPERATURE);

            time(&ltime);
            times = localtime(&ltime);
            strftime(current_time,25,"%y/%m/%d %H:%M:%S",times);
            fprintf(file, "%s, %f, %d\n", current_time, camera_temperature, sbc_temperature);
            printf("%s, %f, %d\n", current_time, camera_temperature, sbc_temperature);
        }
    }
}

void *SaveImageThread(void *threadargs)
{
    long tid = (long)((struct Thread_data *)threadargs)->thread_id;
    printf("SaveImage thread #%ld!\n", tid);

    cv::Mat localFrame;
    long int localFrameCount;
    std::string fitsfile;
    timespec waittime = {1,0};
    //timespec thetimenow;
    while(1)
    {
        if (stop_message[tid] == 1)
        {
            printf("SaveImage thread #%ld exiting\n", tid);
            started[tid] = false;
            pthread_exit( NULL );
        }
        if (cameraReady)
        {
            while(1)
            {
                if(saveReady.check())
                {
                    saveReady.lower();
                    break;
                }
                else
                {
                    nanosleep(&waittime, NULL);
                }
            }

            //printf("SaveImageThread: trying to lock\n");
            if (pthread_mutex_trylock(&mutexImage) == 0)
            {
                //printf("ImageProcessThread: got lock\n");
                if(!frame.empty())
                {
                    localFrameCount = frameCount;
                    frame.copyTo(localFrame);
                    keys.captureTime = frameTime;
                    keys.frameCount = frameCount;
                    pthread_mutex_unlock(&mutexImage);

                    keys.exposureTime = exposure;

                    char stringtemp[80];
                    char obsfilespec[128];
                    time_t ltime;
                    struct tm *times;

                    //Use clock_gettime instead?
                    time(&ltime);
                    times = localtime(&ltime);
                    strftime(stringtemp,40,"%y%m%d_%H%M%S",times);

                    sprintf(obsfilespec, "%simage_%s_%02d.fits", SAVE_LOCATION, stringtemp, (int)localFrameCount);

                    printf("Saving image %s: exposure %d us, analog gain %d, preamp gain %d\n", obsfilespec, exposure, analogGain, preampGain);
                    writeFITSImage(localFrame, keys, obsfilespec);

                    sleep(SLEEP_SAVE);
                }
                else
                {
                    pthread_mutex_unlock(&mutexImage);
                }
            }
        }
    }
}

void *TelemetryPackagerThread(void *threadargs)
{
    long tid = (long)((struct Thread_data *)threadargs)->thread_id;
    printf("TelemetryPackager thread #%ld!\n", tid);

    unsigned char localMin, localMax;
    CoordList localLimbs, localFiducials;
    std::vector<float> localMapping;
    cv::Point2f localCenter, localError;

    while(1)    // run forever
    {
        usleep(USLEEP_TM_GENERIC);
        tm_frame_sequence_number++;

        TelemetryPacket tp(TM_SAS_GENERIC, SOURCE_ID_SAS);
        tp.setSAS(sas_id);
        tp << tm_frame_sequence_number;
        tp << command_sequence_number;
        tp << latest_sas_command_key;

        if(pthread_mutex_trylock(&mutexProcess) == 0)
        {
            localMin = frameMin;
            localMax = frameMax;
            localLimbs = limbs;
            localCenter = pixelCenter;
            localError = error;
            localFiducials = pixelFiducials;
            localMapping = mapping;

            std::cout << "Telemetry packet with Sun center (pixels): " << localCenter;
            if(localMapping.size() == 4) {
                std::cout << ", mapping is";
                for(uint8_t l = 0; l < 4; l++) std::cout << " " << localMapping[l];
                solarTransform.set_conversion(Pair(localMapping[0],localMapping[2]),Pair(localMapping[1],localMapping[3]));
            }
            std::cout << std::endl;

            std::cout << "Offset: " << solarTransform.calculateOffset(Pair(localCenter.x,localCenter.y)) << std::endl;

            pthread_mutex_unlock(&mutexProcess);
        } else {
            std::cout << "Using stale information for telemetry packet" << std::endl;
        }

        //Housekeeping fields, two of them
        tp << Float2B(camera_temperature);
        tp << (uint16_t)sbc_temperature;

        //Sun center and error
        tp << Pair3B(localCenter.x, localCenter.y);
        tp << Pair3B(localError.x, localError.y);

        //Predicted Sun center and error
        tp << Pair3B(0, 0);
        tp << Pair3B(0, 0);

        //Number of limb crossings
        tp << (uint16_t)localLimbs.size();

        //Limb crossings (currently 8)
        for(uint8_t j = 0; j < 8; j++) {
            if (j < localLimbs.size()) {
                tp << Pair3B(localLimbs[j].x, localLimbs[j].y);
            } else {
                tp << Pair3B(0, 0);
            }
        }

        //Number of fiducials
        tp << (uint16_t)localFiducials.size();

        //Fiduicals (currently 6)
        for(uint8_t k = 0; k < 6; k++) {
            if (k < localFiducials.size()) {
                tp << Pair3B(localFiducials[k].x, localFiducials[k].y);
            } else {
                tp << Pair3B(0, 0);
            }
        }

        //Pixel to screen conversion
        if(localMapping.size() == 4) {
            tp << localMapping[0]; //X intercept
            tp << localMapping[1]; //X slope
            tp << localMapping[2]; //Y intercept
            tp << localMapping[3]; //Y slope
        } else {
            tp << (float)-3000; //X intercept
            tp << (float)6; //X slope
            tp << (float)3000; //Y intercept
            tp << (float)-6; //Y slope
        }

        //Image max and min
        tp << (uint8_t) localMax; //max
        tp << (uint8_t) localMin; //min

        //Tacking on the offset numbers intended for CTL
        tp << solarTransform.calculateOffset(Pair(localCenter.x,localCenter.y));

        //add telemetry packet to the queue
        tm_packet_queue << tp;
            
        if (stop_message[tid] == 1){
            printf("TelemetryPackager thread #%ld exiting\n", tid);
            started[tid] = false;
            pthread_exit( NULL );
        }
    }

    /* NEVER REACHED */
    return NULL;
}

void *listenForCommandsThread(void *threadargs)
{  
    long tid = (long)((struct Thread_data *)threadargs)->thread_id;
    printf("listenForCommands thread #%ld!\n", tid);

    tid_listen = tid;

    CommandReceiver comReceiver( (unsigned short) PORT_CMD);
    comReceiver.init_connection();
    
    while(1)    // run forever
    {
        unsigned int packet_length;
    
        packet_length = comReceiver.listen( );
        printf("listenForCommandsThread: %i\n", packet_length);
        uint8_t *packet;
        packet = new uint8_t[packet_length];
        comReceiver.get_packet( packet );
    
        CommandPacket command_packet( packet, packet_length );

        if (command_packet.valid()){
            printf("listenForCommandsThread: good command packet\n");

            command_sequence_number = command_packet.getSequenceNumber();

            // add command ack packet
            TelemetryPacket ack_tp(TM_ACK_RECEIPT, SOURCE_ID_SAS);
            ack_tp << command_sequence_number;
            tm_packet_queue << ack_tp;

            // update the command count
            printf("command sequence number to %i\n", command_sequence_number);

            try { recvd_command_queue.add_packet(command_packet); }
            catch (std::exception& e) {
                std::cerr << e.what() << std::endl;
            }

        } else {
            printf("listenForCommandsThread: bad command packet\n");
        }

        if (stop_message[tid] == 1){
            printf("listenForCommands thread #%ld exiting\n", tid);
            comReceiver.close_connection();
            started[tid] = false;
            pthread_exit( NULL );
        }
    }

    /* NEVER REACHED */
    return NULL;
}

void *CommandSenderThread( void *threadargs )
{
    long tid = (long)((struct Thread_data *)threadargs)->thread_id;
    printf("CommandSender thread #%ld!\n", tid);

    CommandSender comSender(IP_CTL, PORT_CMD);

    while(1)    // run forever
    {
        usleep(USLEEP_CMD_SEND);
    
        if( !cm_packet_queue.empty() ){
            CommandPacket cp(NULL);
            cm_packet_queue >> cp;
            comSender.send( &cp );
            //std::cout << "CommandSender: " << cp << std::endl;
        }

        if (stop_message[tid] == 1){
            printf("CommandSender thread #%ld exiting\n", tid);
            started[tid] = false;
            pthread_exit( NULL );
        }
    }
  
}

void *CommandPackagerThread( void *threadargs )
{
    long tid = (long)((struct Thread_data *)threadargs)->thread_id;
    printf("CommandPackager thread #%ld!\n", tid);

    cv::Point2f localCenter, localError;

    while(1)    // run forever
    {
        sleep(SLEEP_SOLUTION);

        if (isOutputting) {
            solution_sequence_number++;
            CommandPacket cp(TARGET_ID_CTL, solution_sequence_number);

            if (isTracking) {
                if (!acknowledgedCTL) {
                    cp << (uint16_t)HKEY_SAS_TRACKING_IS_ON;
                    acknowledgedCTL = true;
                } else {
                    if(pthread_mutex_trylock(&mutexProcess) == 0)
                    {
                        localCenter = pixelCenter;
                        localError = error;

                        pthread_mutex_unlock(&mutexProcess);
                    } else {
                        std::cout << "Using stale information for solution packet" << std::endl;
                    }

                    cp << (uint16_t)HKEY_SAS_SOLUTION;
                    cp << solarTransform.calculateOffset(Pair(localCenter.x,localCenter.y));
                    cp << (double)0; // roll offset
                    cp << (double)0.003; // error
                    cp << (uint32_t)0; //seconds
                    cp << (uint16_t)0; //milliseconds
                }
            } else { // isTracking is false
                if (!acknowledgedCTL) {
                    cp << (uint16_t)HKEY_SAS_TRACKING_IS_OFF;
                    acknowledgedCTL = true;
                }
            } // isTracking

            //Add packet to the queue if any commands have been inserted to the packet
            if(cp.remainingBytes() > 0) {
                cm_packet_queue << cp;
            }
        } // isOutputting

        if (stop_message[tid] == 1){
            printf("CommandPackager thread #%ld exiting\n", tid);
            started[tid] = false;
            pthread_exit( NULL );
        }
    }

    /* NEVER REACHED */
    return NULL;
}

void queue_cmd_proc_ack_tmpacket( uint16_t error_code )
{
    TelemetryPacket ack_tp(TM_ACK_PROCESS, SOURCE_ID_SAS);
    ack_tp << command_sequence_number;
    ack_tp << latest_sas_command_key;
    ack_tp << error_code;
    tm_packet_queue << ack_tp;
}

uint16_t cmd_send_image_to_ground( int camera_id )
{
    // camera_id refers to 0 PYAS, 1 is RAS (if valid)
    uint16_t error_code = 0;
    cv::Mat localFrame;
    HeaderData localKeys;

    TCPSender tcpSndr(IP_FDR, (unsigned short) PORT_IMAGE);
    int ret = tcpSndr.init_connection();
    if (ret > 0){
        if (pthread_mutex_trylock(&mutexImage) == 0){
            if( !frame.empty() ){
                frame.copyTo(localFrame);
                localKeys = keys;
            }
            pthread_mutex_unlock(&mutexImage);
        }
        if( !localFrame.empty() ){
            //1 for SAS-1/PYAS, 2 for SAS-2/PYAS, 6 for SAS-2/RAS
            uint8_t camera = sas_id+4*camera_id;

            uint16_t numXpixels = localFrame.cols;
            uint16_t numYpixels = localFrame.rows;
            uint32_t totalpixels = numXpixels*numYpixels;

            //Copy localFrame to a C array
            uint8_t *array = new uint8_t[totalpixels];

            uint16_t rows = (localFrame.isContinuous() ? 1 : localFrame.rows);
            uint32_t cols = totalpixels/rows;

            for (int j = 0; j < rows; j++) {
                memcpy(array+j*cols, localFrame.ptr<uint8_t>(j), cols);
            }

            im_packet_queue.add_array(camera, numXpixels, numYpixels, array);

            delete array;

            //Add FITS header tags
            uint32_t temp = localKeys.exposureTime;
            im_packet_queue << ImageTagPacket(camera, &temp, TLONG, "EXPOSURE", "Exposure time (msec)");

            //Make sure to synchronize all the timestamps
            im_packet_queue.synchronize();

            std::cout << "Sending " << im_packet_queue.size() << " packets\n";

            ImagePacket im(NULL);
            while(!im_packet_queue.empty()) {
                im_packet_queue >> im;
                tcpSndr.send_packet( &im );
            }

        }
        tcpSndr.close_connection();
        error_code = 1;
    } else { error_code = 2; }
    return error_code;
}
        
void *commandHandlerThread(void *threadargs)
{
    long tid = (long)((struct Thread_data *)threadargs)->thread_id;
    struct Thread_data *my_data;
    uint16_t error_code = 0;
    my_data = (struct Thread_data *) threadargs;

    switch( my_data->command_key & 0x0FFF)
    {
        case SKEY_REQUEST_IMAGE:
            {
                error_code = cmd_send_image_to_ground( 0 );
                queue_cmd_proc_ack_tmpacket( error_code );
            }
            break;
        case SKEY_SET_EXPOSURE:    // set exposure time
            {
                if( (my_data->command_vars[0] > 0) && (my_data->command_num_vars == 1)) exposure = my_data->command_vars[0];
                std::cout << "Requested exposure time is: " << exposure << std::endl;
                queue_cmd_proc_ack_tmpacket( error_code );
            }
            break;
        case SKEY_SET_PREAMPGAIN:    // set preamp gain
            {
                if( my_data->command_num_vars == 1) preampGain = (int16_t)my_data->command_vars[0];
                std::cout << "Requested preamp gain is: " << preampGain << std::endl;
                queue_cmd_proc_ack_tmpacket( error_code );
            }
            break;
        case SKEY_SET_ANALOGGAIN:    // set analog gain
            {
                if( my_data->command_num_vars == 1) analogGain = my_data->command_vars[0];
                std::cout << "Requested analog gain is: " << analogGain << std::endl;
                queue_cmd_proc_ack_tmpacket( error_code );
            }
            break;
        case SKEY_SET_TARGET:    // set new solar target
            solarTransform.set_solar_target(Pair((int16_t)my_data->command_vars[0], (int16_t)my_data->command_vars[1]));
            break;
        case SKEY_START_OUTPUTTING:
            {
                isOutputting = true;
            }
            break;
        case SKEY_STOP_OUTPUTTING:
            {
                isOutputting = false;
            }
            break;
        default:
            {
                error_code = 0xffff;            // unknown command!
                queue_cmd_proc_ack_tmpacket( error_code );
            }
    }

    started[tid] = false;
    return NULL;
}

void cmd_process_heroes_command(uint16_t heroes_command)
{
    if ((heroes_command & 0xFF00) == 0x1000) {
        switch(heroes_command) {
            case HKEY_CTL_START_TRACKING: // start tracking
                isTracking = true;
                acknowledgedCTL = false;
                // need to send 0x1100 command packet
                break;
            case HKEY_CTL_STOP_TRACKING: // stop tracking
                isTracking = false;
                acknowledgedCTL = false;
                // need to send 0x1101 command packet
                break;
            case HKEY_FDR_SAS_CMD: // SAS command, so do nothing here
                break;
            default:
                printf("Unknown HEROES command\n");
        }
    } else printf("Not a HEROES-to-SAS command\n");
}

void start_thread(void *(*routine) (void *), const Thread_data *tdata)
{
    int i = 0;
    while (started[i] == true) {
        i++;
        if (i == MAX_THREADS) return; //should probably thrown an exception
    }

    //Copy the thread data to a global to prevent deallocation
    if (tdata != NULL) memcpy(&thread_data[i], tdata, sizeof(Thread_data));
    thread_data[i].thread_id = i;

    stop_message[i] = 0;

    int rc = pthread_create(&threads[i], NULL, routine, &thread_data[i]);
    if (rc != 0) {
        printf("ERROR; return code from pthread_create() is %d\n", rc);
    } else started[i] = true;

    return;
}

void cmd_process_sas_command(uint16_t sas_command, Command &command)
{
    Thread_data tdata;

    if ((sas_command & (sas_id << 12)) != 0) {
        tdata.command_key = sas_command;
        tdata.command_num_vars = sas_command & 0x000F;

        for(int i = 0; i < tdata.command_num_vars; i++){
            try {
              command >> tdata.command_vars[i];
            } catch (std::exception& e) {
               std::cerr << e.what() << std::endl;
            }
        }

        switch( sas_command & 0x0FFF){
            case SKEY_OP_DUMMY:     // test, do nothing
                queue_cmd_proc_ack_tmpacket( 1 );
                break;
            case SKEY_KILL_WORKERS:    // kill all worker threads
                {
                    kill_all_workers();
                    queue_cmd_proc_ack_tmpacket( 1 );
                }
                break;
            case SKEY_RESTART_THREADS:    // (re)start all worker threads
                {
                    kill_all_threads();

                    start_thread(listenForCommandsThread, NULL);
                    start_all_workers();
                    queue_cmd_proc_ack_tmpacket( 1 );
                }
                break;
            default:
                {
                    start_thread(commandHandlerThread, &tdata);
                }
        } //switch
    } else printf("Not the intended SAS for this command\n");
}

void start_all_workers( void ){
    start_thread(TelemetryPackagerThread, NULL);
    start_thread(CommandPackagerThread, NULL);
    start_thread(TelemetrySenderThread, NULL);
    start_thread(CommandSenderThread, NULL);
    start_thread(CameraStreamThread, NULL);
    start_thread(ImageProcessThread, NULL);
    start_thread(SaveImageThread, NULL);
    start_thread(SaveTemperaturesThread, NULL);
    start_thread(SBCInfoThread, NULL);
}

int main(void)
{  
    // to catch a Ctrl-C and clean up
    signal(SIGINT, &sig_handler);

    identifySAS();
    if (sas_id == 1) isOutputting = true;

    pthread_mutex_init(&mutexImage, NULL);
    pthread_mutex_init(&mutexProcess, NULL);

    /* Create worker threads */
    printf("In main: creating threads\n");

    for(int i = 0; i < MAX_THREADS; i++ ){
        started[0] = false;
    }

    // start the listen for commands thread right away
    start_thread(listenForCommandsThread, NULL);
    start_all_workers();

    while(g_running){
        // check if new command have been added to command queue and service them
        if (!recvd_command_queue.empty()){
            printf("size of queue: %zu\n", recvd_command_queue.size());
            Command command;
            command = Command();
            recvd_command_queue >> command;

            latest_heroes_command_key = command.get_heroes_command();
            latest_sas_command_key = command.get_sas_command();
            printf("Received command key 0x%x/0x%x\n", latest_heroes_command_key, latest_sas_command_key);

            cmd_process_heroes_command(latest_heroes_command_key);
            if(latest_heroes_command_key == HKEY_FDR_SAS_CMD) {
                cmd_process_sas_command(latest_sas_command_key, command);
            }
        }
    }

    /* Last thing that main() should do */
    printf("Quitting and cleaning up.\n");
    /* wait for threads to finish */
    kill_all_threads();
    pthread_mutex_destroy(&mutexImage);
    pthread_mutex_destroy(&mutexProcess);
    pthread_exit(NULL);

    return 0;
}

