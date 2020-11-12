//OpenRevolution RtAudio player (brstm_rt)
//Copyright (C) 2020 IC
#include <iostream>
#include <fstream>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>

#include <unistd.h>
#include <termios.h>
#include <pthread.h>

#include "RtAudio.h"
#include "../lib/brstm.h"

#define OUTPUT_BUFSIZE 256

struct player_state_t {
    //Thread lock
    bool lock = 0;
    //Track switching and mixing
    bool track_mixing_enabled = 0;
    //Toggles for which tracks are playing when using the track mixer.
    bool tracks_enabled[9] = {1,0,0,0,0,0,0,0,0};
    //Current track in normal track switching
    unsigned int current_track=0;
    
    //Playback
    unsigned long playback_current_sample=0;
    bool stop_playing=0;
    bool paused=0;
    
    //File data and BRSTM struct
    unsigned char* memblock;
    std::ifstream file;
    Brstm* brstm;
    
    //Memory mode
    signed char memoryMode = -1;
    //-1 - Automatically picked
    // 0 - Load file into memory and decode in real time
    // 1 - Stream file from disk and decode in real time
    // 2 - Decode all audio data into memory before playing
    
    //Mixing/playback buffer
    int16_t* brstmbuffer[2];
};

#include "utils.h"
#include "ui.h"

//Get the buffer in different ways depending on the memory mode
void getBufferHelper(player_state_t* state, unsigned long sampleOffset, unsigned int bufferSize) {
    switch(state->memoryMode) {
        //Realtime decoding from memory
        case 0:
        brstm_getbuffer(state->brstm, state->memblock, sampleOffset, bufferSize);
        return;
        
        //Streaming data from disk and realtime decoding
        case 1:
        brstm_fstream_getbuffer(state->brstm, state->file, sampleOffset, bufferSize);
        return;
        
        //Full decoding
        case 2:
        Brstm* brstm = state->brstm;
        for(unsigned int c=0;c<brstm->num_channels;c++) {
            delete[] brstm->PCM_buffer[c];
            brstm->PCM_buffer[c] = new int16_t[bufferSize];
            for(unsigned int i=0;i<bufferSize;i++) {
                brstm->PCM_buffer[c][i] = brstm->PCM_samples[c][sampleOffset+i];
            }
        }
        return;
    }
}

void mixTracks(player_state_t* state, unsigned int bufferSize) {
    int16_t** brstmbuffer = state->brstmbuffer;
    Brstm* brstm = state->brstm;
    
    //Clear out the mixing buffer
    for(unsigned int s=0; s<bufferSize; s++) {
        brstmbuffer[0][s] = 0;
        brstmbuffer[1][s] = 0;
    }
    
    for(unsigned int t=0; t<brstm->num_tracks; t++) {
        if(!state->tracks_enabled[t]) continue;
        
        unsigned char ch1id = brstm->track_lchannel_id [t];
        unsigned char ch2id = brstm->track_num_channels[t] == 2 ? brstm->track_rchannel_id[t] : ch1id;
        double track_volume = (brstm->track_desc_type == 0 ? 1 : (double)brstm->track_volume[t]/127);
        
        for(unsigned int s=0; s<bufferSize; s++) {
            brstmbuffer[0][s] = brstm_clamp16( ((int32_t)brstmbuffer[0][s] + brstm->PCM_buffer[ch1id][s]*track_volume) );
            brstmbuffer[1][s] = brstm_clamp16( ((int32_t)brstmbuffer[1][s] + brstm->PCM_buffer[ch2id][s]*track_volume) );
        }
    }
}

//RtAudio callback
int RtAudioCb( void *outputBuffer, void *inputBuffer, unsigned int nBufferFrames, double streamTime, RtAudioStreamStatus status, void *userData) {
    unsigned int i = 0;
    int ioffset = 0;
    int16_t *buffer = (int16_t*) outputBuffer;
    //if(status) std::cout << "Stream underflow detected!\n";
    
    //State
    player_state_t* state = (player_state_t*)userData;
    Brstm* brstm = state->brstm;
    int16_t** brstmbuffer = state->brstmbuffer;
    unsigned long &playback_current_sample = state->playback_current_sample;
    
    //Unlock playback state
    while(state->lock) usleep(100);
    state->lock = 1;
    
    if(!state->paused) {
        //Avoid reading garbage outside the file
        unsigned int samplesToGet = brstm->total_samples - playback_current_sample < nBufferFrames ? brstm->total_samples - playback_current_sample : nBufferFrames;
        getBufferHelper(state, playback_current_sample, samplesToGet);
        
        //Channel IDs / track mixing
        unsigned char ch1id = 0, ch2id = 0;
        if(!state->track_mixing_enabled) {
            ch1id = brstm->track_lchannel_id [state->current_track];
            ch2id = brstm->track_num_channels[state->current_track] == 2 ? brstm->track_rchannel_id[state->current_track] : ch1id;
            brstmbuffer[0] = brstm->PCM_buffer[ch1id];
            brstmbuffer[1] = brstm->PCM_buffer[ch2id];
        } else {
            mixTracks(state, samplesToGet);
        }
        
        for (i=0;i<nBufferFrames;i+=1) {
            *buffer++ = brstmbuffer[0][i+ioffset];
            *buffer++ = brstmbuffer[1][i+ioffset];
            
            playback_current_sample++;
            if(playback_current_sample >= brstm->total_samples) {
                
                if(brstm->loop_flag) {
                    playback_current_sample = brstm->loop_start;
                    
                    //Refill buffer, using same safety as before.
                    samplesToGet = brstm->total_samples - playback_current_sample < nBufferFrames-i ? brstm->total_samples - playback_current_sample : nBufferFrames-i;
                    getBufferHelper(state, playback_current_sample, samplesToGet);
                    
                    //Set up brstmbuffer
                    if(state->track_mixing_enabled) {mixTracks(state, samplesToGet);}
                    else {
                        brstmbuffer[0] = brstm->PCM_buffer[ch1id];
                        brstmbuffer[1] = brstm->PCM_buffer[ch2id];
                    }
                    
                    //1 is added because i will be incremented before next sample.
                    ioffset=0-(i+1);
                } else {
                    state->paused = 1;
                    playback_current_sample = 0;
                    //Break out to go to paused buffer code and fill the remaining buffer with silence.
                    break;
                }
            }
        }
    }
    if(state->paused) {
        //Player is paused
        for(;i<nBufferFrames;i+=1) {
            *buffer++ = 0;
            *buffer++ = 0;
        }
    }
    
    state->lock = 0;
    
    return 0;
}

//-------------------######### STRINGS

const char* helpString = "OpenRevolution audio player\nCopyright (C) 2020 I.C.\nThis program is free software, see the license file for more information.\nUsage:\nbrstm_rt [file to open] [options...]\nOptions:\n-v - Verbose output\n-q - Quiet output (no player UI)\n--force-sample-rate [sample rate] - Force playback sample rate\n--enable-mixer - Enable track mixing for multi-track files\n--classic-ui - Classic brstm_rt appearance\n\nMemory modes:\n-m - Load the file into memory and decode it in real time\n-s - Stream the audio data from disk (lower memory usage, recommended for large files)\n-d - Decode the entire file before playing it (high memory usage, not recommended)\nDefault mode is chosen depending on the file size.\n";

const char* opts[] = {"-v","-m","-s","-d","-force-sample-rate","-q","-enable-mixer","-classic-ui"};
const char* opts_alt[] = {"--verbose","--memory","--streaming","--decode","--force-sample-rate","--quiet","--enable-mixer","--classic-ui"};
const unsigned int optcount = 8;
const bool optrequiredarg[optcount] = {0,0,0,0,1,0,0,0};
bool  optused  [optcount];
char* optargstr[optcount];
//____________________________________

int main(int argc, char** args) {
    if(argc<2 || strcmp(args[1],"--help") == 0) {
        std::cout << helpString;
        return 0;
    }
    
    if(strcmp(args[1],"--version") == 0) {
        std::cout << brstm_getVersionString() << '\n';
        exit(0);
    }
    
    //Parse command line args
    for(int a=2;a<argc;a++) {
        int vOpt = -1;
        //Compare cmd arg against each known option
        for(unsigned int o=0;o<optcount;o++) {
            if( strcmp(args[a], opts[o]) == 0 || strcmp(args[a], opts_alt[o]) == 0 ) {
                //Matched
                vOpt = o;
                break;
            }
        }
        //No match
        if(vOpt < 0) {std::cout << "Unknown option '" << args[a] << "'.\n"; exit(255);}
        //Mark the options as used
        optused[vOpt] = 1;
        //Read the argument for the option if it requires it
        if(optrequiredarg[vOpt]) {
            if(a+1 < argc) {
                optargstr[vOpt] = args[++a];
            } else {
                std::cout << "Option " << opts[vOpt] << " requires an argument\n";
                exit(255);
            }
        }
    }
    
    //Create player and UI states
    player_state_t* player_state = new player_state_t;
    uinput_state_t* uinput_state = new uinput_state_t;
    uoutput_state_t* uoutput_state = new uoutput_state_t;
    uinput_state->playback_state = player_state;
    uinput_state->output_state = uoutput_state;
    uoutput_state->playback_state = player_state;
    
    //Apply the options
    bool verb=0;
    unsigned long forcedSampleRate = 0;
    if(optused[0]) verb=1;
    if(optused[1]) player_state->memoryMode=0;
    if(optused[2]) player_state->memoryMode=1;
    if(optused[3]) player_state->memoryMode=2;
    if(optused[4]) forcedSampleRate = atoi(optargstr[4]);
    if(optused[5]) uoutput_state->quietOutput=1;
    if(optused[6]) player_state->track_mixing_enabled=1;
    if(optused[7]) uoutput_state->classic_noflush=1;
    
    //Allocate brstm struct
    Brstm* brstm = new Brstm;
    player_state->brstm = brstm;
    
    //Read the file
    std::streampos fsize;
    player_state->file.open(args[1], std::ios::in|std::ios::binary|std::ios::ate);
    if(player_state->file.is_open()) {
        fsize = player_state->file.tellg();
        player_state->file.seekg (0, std::ios::beg);
        //Pick default memory mode
        if(player_state->memoryMode == -1) {
            //Get base file information
            unsigned char res = brstm_fstream_getBaseInformation(brstm, player_state->file, 0);
            if(res>127) exit(res);
            if(brstm->file_format == 4) {
                //Always use full decoding for BWAV
                player_state->memoryMode = 2;
            } else {
                //Streaming for >15MB files
                if(fsize > 15 * 1000000) player_state->memoryMode = 1;
                //Default realtime decoding mode
                else player_state->memoryMode = 0;
            }
        }
        //Don't read the file in mode 1 (Streaming)
        if(player_state->memoryMode != 1) {
            player_state->memblock = new unsigned char [fsize];
            player_state->file.seekg(0);
            player_state->file.read ((char*)player_state->memblock, fsize);
            player_state->file.close();
        }
    } else {
        //File open error
        perror(args[1]);
        exit(255);
    }
    
    if(verb) switch(player_state->memoryMode) {
        case 0: std::cout << "Realtime decoding mode\n"; break;
        case 1: std::cout << "Disk stream mode\n"; break;
        case 2: std::cout << "Full decode mode\n"; break;
    }
    
    //Read the BRSTM headers
    if(player_state->memoryMode != 1) {
        //Use normal brstm read functions
        unsigned char result=brstm_read(brstm, player_state->memblock, verb,
            //Decode the audio data if memory mode is 2 (Full decoding)
            player_state->memoryMode == 2 ? true : false
        );
        //The file data will not be needed anymore in full decoding mode
        if(player_state->memoryMode == 2) {delete[] player_state->memblock;}
        if(result>127) {
            std::cout << "File read error. (" << (int)result << ")\n";
            return result;
        }
    } else {
        //Disk streaming mode
        unsigned char result = brstm_fstream_read(brstm, player_state->file, verb);
        if(result>127) {
            std::cout << "File read error. (" << (int)result << ")\n";
            return result;
        }
    }
    
    if(player_state->track_mixing_enabled && brstm->num_tracks >= 9) {
        std::cout << "Too many tracks for mixing.\n";
        player_state->track_mixing_enabled = 0;
    }
    if(player_state->track_mixing_enabled && brstm->num_tracks == 1) {
        player_state->track_mixing_enabled = 0;
    }
    
    //Allocate mixing buffer
    if(player_state->track_mixing_enabled) {
        player_state->brstmbuffer[0] = new int16_t[OUTPUT_BUFSIZE];
        player_state->brstmbuffer[1] = new int16_t[OUTPUT_BUFSIZE];
    }
    
    //Print basic file information in non-verbose mode
    if(verb == 0 && uoutput_state->quietOutput == 0) {
        const char* loopstr = (brstm->loop_flag ? (brstm->loop_start > 0 ? "Looping" : "E to S") : "No loop");
        printf("%s | %s | %luHz | %uch/%utr | %s\n", brstm_getShortFormatString(brstm), brstm_getCodecString(brstm), brstm->sample_rate, brstm->num_channels, brstm->num_tracks, loopstr);
    }
    
    //Initialize RtAudio
    RtAudio dac;
    if (dac.getDeviceCount()<1) {
        std::cout << "No audio device found.\n";
        exit(255);
    }
    RtAudio::StreamParameters parameters;
    RtAudio::StreamOptions options;
    parameters.deviceId = dac.getDefaultOutputDevice();
    parameters.nChannels = 2;
    parameters.firstChannel = 0;
    options.streamName = "OpenRevolution brstm_rt";
    unsigned int sampleRate = forcedSampleRate ? forcedSampleRate : brstm->sample_rate;
    unsigned int bufferFrames = OUTPUT_BUFSIZE;
    
    try {
        dac.openStream(&parameters, NULL, RTAUDIO_SINT16, sampleRate, &bufferFrames, &RtAudioCb, (void*)player_state, &options);
        dac.startStream();
    } catch(RtAudioError& e) {
        e.printMessage();
        exit(255);
    }
    
    
    //Calculate total seconds
    uoutput_state->total_seconds = brstm->total_samples / brstm->sample_rate;
    secondsToMString(uoutput_state->total_seconds_string, 10, uoutput_state->total_seconds);
    
    //Initialize user interface thread
    pthread_t uiThread;
    {
        int pthread_res = 0;
        pthread_res = pthread_create(&uiThread, NULL, uinput_thread, (void*)uinput_state);
        if(pthread_res) {
            printf("The UI thread could not be created. (%d)\n", pthread_res);
            exit(255);
        }
    }
    
    //Calculate UI refresh rate (4 times per second)
    uoutput_state->ui_counter_l = (1000 / 10) / 4;
    uoutput_state->ui_counter = -1;
    
    //Main UI loop
    while(player_state->stop_playing == 0) {
        drawPlayerUI(uoutput_state);
        usleep(10000);
    }
    
    std::cout << '\n';
    
    try {
        //Stop the stream
        dac.stopStream();
    } catch(RtAudioError& e) {
        e.printMessage();
    }
    if(dac.isStreamOpen()) dac.closeStream();
    
    //Free mixing buffer
    if(player_state->track_mixing_enabled) {
        delete[] player_state->brstmbuffer[0];
        delete[] player_state->brstmbuffer[1];
    }
    
    pthread_join(uiThread, NULL);
    
    brstm_close(brstm);
    delete brstm;
    delete player_state;
    delete uinput_state;
    delete uoutput_state;
    
    return 0;
}
