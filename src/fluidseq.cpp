#include "MidiFile.h"
#include "Options.h"
#include <iostream>
#include <iomanip>
#include <jack/jack.h>
#include <jack/midiport.h>
#include <unistd.h>
#include <cmath>
#include <cstring>
#include <fluidsynth.h>
#include <lo/lo.h>
#include <lo/lo_cpp.h>
#include <csignal>
#include <thread>
#include <atomic>

using namespace std;
using namespace smf;

#define JACK_CLIENT_NAME            "sequencer"
#define JACK_AUDIO_OUTPUT_PORT_L    "out_L"
#define JACK_AUDIO_OUTPUT_PORT_R    "out_R"
#define JACK_MIDI_OUTPUT_PORT       "midi_out"
#define JACK_SYSTEM_PLAYBACK_L      "system:playback_1"
#define JACK_SYSTEM_PLAYBACK_R      "system:playback_2"

#define SOUND_FONT                  "/home/marius/Téléchargements/studio_dubroom_general_midi.sf2"
// #define SOUND_FONT                  "/usr/share/sounds/sf2/FluidR3_GM.sf2"

// midi file
MidiFile midifile;
bool init = false;
bool midi_file_loaded = false;
int event_index = -1;

jack_client_t *m_jack_client = nullptr;
jack_port_t *midi_output_port = nullptr;
static jack_port_t *audio_output_port_l = nullptr;
static jack_port_t *audio_output_port_r = nullptr;
static fluid_settings_t *settings = nullptr;
static fluid_synth_t *synth = nullptr;
bool stopping = false;

// transport master
float time_beats_per_bar = 4.0;
float time_beat_type = 4.0;
double time_ticks_per_beat = 1920.0;
double time_beats_per_minute = 120.0;
double current_bpm = time_beats_per_minute;
int time_reset = 1;
double last_tick;

lo::ServerThread *m_osc_server = nullptr;
bool m_osc_server_initialized = false;

lo::Address *m_osc_client = nullptr;
bool m_osc_client_initialized = false;

// position
std::atomic<jack_nframes_t> play_frame{0};
std::atomic<bool> running{false};

void seek_midi_event(jack_position_t pos)
{
    event_index = 0;

    // locate from jack BBT on current bar
    double current_jack_ticks = (pos.beat - 1) * pos.ticks_per_beat + pos.tick;

    // convert jack midi tick_per_beat -> midifile tick_per_beat
    double current_midi_ticks = current_jack_ticks * midifile.getTicksPerQuarterNote() / pos.ticks_per_beat;
    printf("Seeking jack: %f midi:%f tpqn: %d tpb %f \n", current_jack_ticks, current_midi_ticks, midifile.getTicksPerQuarterNote(), pos.ticks_per_beat);

    // fprintf(stderr, "WARNING: track count must be 1. joinTrack must be called");

    for(int track = 0; track < midifile.getTrackCount(); track++){
        for (int event = 0; event < midifile[track].size(); event++)
        {
            if (!midifile[track][event].isNoteOn())
                continue;

            if (midifile[track][event].tick >= current_midi_ticks)
            {
                event_index = event;
                break;
            }
        }
    }

    printf("Finding event based on midi tick: %f => event at index: %d\n", current_jack_ticks, event_index);
}

void next_midi_event(){
    event_index++;
    if(event_index >= midifile[0].size()){
        event_index = 0;
    }
}

void position_thread()
{
    while (!stopping)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if(!running){
            continue;
        }
        double ticks_per_second = midifile.getTicksPerQuarterNote() * current_bpm / 60.0;
        double frames_per_tick = 48000.0 / ticks_per_second;
        double midi_duration_in_frames = midifile.getFileDurationInTicks() * frames_per_tick;

        jack_nframes_t frame = play_frame.load(std::memory_order_relaxed);
        double fframe = fmod((double)frame, midi_duration_in_frames);

        lo::Message message;
        message.add_double(fframe * 100.0 / midi_duration_in_frames);
        m_osc_client->send("/sequencer/midi_pos", message);

    }
}

int process(jack_nframes_t nframes, void *)
{
    if(stopping)
        return 0;

    // do not run if midi file is not yet loaded
    if(!midi_file_loaded)
        return 0;

    jack_position_t pos;
    jack_transport_state_t state = jack_transport_query(m_jack_client, &pos);
    float *left  = (float *)jack_port_get_buffer(audio_output_port_l, nframes);
    float *right = (float *)jack_port_get_buffer(audio_output_port_r, nframes);
    void *midi_buffer = jack_port_get_buffer(midi_output_port, nframes);
    jack_nframes_t period_start = pos.frame;
    jack_nframes_t period_end   = period_start + nframes;

    if (state != JackTransportRolling || !(pos.valid & JackPositionBBT))
    {
        return 0;
    }

    jack_midi_clear_buffer(midi_buffer);

    if(!init || current_bpm != pos.beats_per_minute){
        // this will sync midi file to jack tick
        seek_midi_event(pos);
        init = true;
        current_bpm = pos.beats_per_minute;
        running = true;
    }

    // TODO = this works because we are the master!!!!!
    //send position
    play_frame.store(pos.frame, std::memory_order_relaxed);

    double current_jack_tick = ((pos.beat - 1) * pos.ticks_per_beat) + pos.tick_double;
    double frames_per_tick = (double)pos.frame_rate * 60.0 / pos.beats_per_minute / pos.ticks_per_beat;

    jack_nframes_t cursor = 0;
    while(1){

        MidiEvent &msg = midifile[0][event_index];
        // convert midi to jack tpqn
        double next_jack_tick = msg.tick * pos.ticks_per_beat / midifile.getTicksPerQuarterNote();

        //compute delta
        while(next_jack_tick >= (pos.beats_per_bar * pos.ticks_per_beat))
            next_jack_tick -= (pos.beats_per_bar * pos.ticks_per_beat);
        double delta_tick = next_jack_tick - current_jack_tick;

        // printf("next: %0.20f current: %.20f %.20f\n",next_jack_tick, current_jack_tick, delta_tick);

        // handle loop boundaries
        if (std::abs(delta_tick) < 1e-6)
            delta_tick = 0.0;
        else if(delta_tick < 0.0)
            delta_tick += pos.ticks_per_beat * pos.beats_per_bar;
        // printf("=> next: %0.20f current: %.20f %.20f\n",next_jack_tick, current_jack_tick, delta_tick);


        double delta_frame = delta_tick * frames_per_tick;
        jack_nframes_t offset = (jack_nframes_t)llround(delta_frame);

        if(offset >= nframes)
            break;

        if (offset >= 0)
        {
            printf("index:%d beat:%d tick:%f offset=%d\n", event_index, pos.beat, pos.tick_double, offset);
            jack_midi_event_write(midi_buffer, offset, msg.data(), msg.size());

            //--------------------------------------------------------------
            // Render until the event
            //--------------------------------------------------------------
            jack_nframes_t chunk = offset - cursor;

            if (chunk)
            {
                fluid_synth_write_float(synth, chunk, left + cursor, 0, 1, right + cursor, 0, 1);
                cursor += chunk;
            }

            if(msg.isNoteOn())
            {
                fluid_synth_noteon(synth, msg.getChannel(), msg.getKeyNumber(), msg.getVelocity());
            }
            else if(msg.isNoteOff())
            {
                fluid_synth_noteoff(synth, msg.getChannel(), msg.getKeyNumber());
            }

            // update event index
            next_midi_event();
        }
    }
    //------------------------------------------------------------------
    // Render remainder of JACK period
    //------------------------------------------------------------------
    if (cursor < nframes)
    {
        fluid_synth_write_float(synth, nframes - cursor, left + cursor, 0, 1, right + cursor, 0, 1);
    }

    return 0;
}

void timebase_callback(jack_transport_state_t state, jack_nframes_t nframes,
                       jack_position_t *pos, int new_pos, void *arg)
{
    double min;    /* minutes since frame 0 */
    long abs_tick; /* ticks since frame 0 */
    long abs_beat; /* beats since frame 0 */

    if (new_pos || time_reset)
    {

        pos->valid = JackPositionBBT;
        pos->beats_per_bar = time_beats_per_bar;
        pos->beat_type = time_beat_type;
        pos->ticks_per_beat = time_ticks_per_beat;
        pos->beats_per_minute = time_beats_per_minute;

        time_reset = 0; /* time change complete */

        /* Compute BBT info from frame number.  This is relatively
         * simple here, but would become complex if we supported tempo
         * or time signature changes at specific locations in the
         * transport timeline.
         */

        min = pos->frame / ((double)pos->frame_rate * 60.0);
        abs_tick = min * pos->beats_per_minute * pos->ticks_per_beat;
        abs_beat = abs_tick / pos->ticks_per_beat;

        pos->bar = abs_beat / pos->beats_per_bar;
        pos->beat = abs_beat - (pos->bar * pos->beats_per_bar) + 1;
        last_tick = abs_tick - (abs_beat * pos->ticks_per_beat);
        pos->bar_start_tick = pos->bar * pos->beats_per_bar *
                              pos->ticks_per_beat;
        pos->bar++; /* adjust start to bar 1 */
    }
    else
    {
        /* Compute BBT info based on previous period. */
        last_tick +=
            nframes * pos->ticks_per_beat * pos->beats_per_minute / (pos->frame_rate * 60);

        while (last_tick >= pos->ticks_per_beat)
        {
            last_tick -= pos->ticks_per_beat;
            if (++pos->beat > pos->beats_per_bar)
            {
                pos->beat = 1;
                ++pos->bar;
                pos->bar_start_tick +=
                    pos->beats_per_bar * pos->ticks_per_beat;
            }
        }
    }

    pos->tick = (int)(last_tick + 0.5);
	pos->valid = static_cast<jack_position_bits_t>(pos->valid | JackTickDouble);
	pos->tick_double = last_tick;
}

int connect_ports()
{
    const char **ports  = jack_get_ports(m_jack_client, JACK_CLIENT_NAME, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput);
    if (ports == NULL)
    {
        return 0;
    }

    if (jack_connect(m_jack_client, ports[0], JACK_SYSTEM_PLAYBACK_L))
    {
        std::cout << "cannot connect output ports" << std::endl;
        return -1;
    }

    if (jack_connect(m_jack_client, ports[1], JACK_SYSTEM_PLAYBACK_R))
    {
        std::cout << "cannot connect output ports" << std::endl;
        return -1;
    }
    jack_free(ports);

    return 0;
}

float slider_to_db(int value)
{
    if (value <= 0)
        return -60.0f;  // mute

    // 1..100 maps to -60dB..0dB
    return -60.0f + (value / 100.0f) * 60.0f;
}

float slider_to_gain(int value)
{
    float db = slider_to_db(value);

    return powf(10.0f, db / 20.0f);
}

void set_bpm(int bpm)
{
    cout << "jack bpm : " << bpm << endl;
    if (bpm > 20 && bpm < 150 && bpm != time_beats_per_minute)
    {
        time_beats_per_minute = bpm;
        time_reset = 1;
    }
}

void set_state(int play){

}

void set_volume(int volume){
    fluid_synth_set_gain(synth, volume/10.0);
}

void load_midi_test(){
    midifile.setTicksPerQuarterNote(192);
    midifile.addTempo(0, 0, 120);
    midifile.addTimeSignature(0, 0, 4, 4);
    midifile.addTrack();
    midifile.addNoteOn( 0, 0, 9, 48, 127);
    midifile.addNoteOff(0, 192/4, 48, 1);
    midifile.addNoteOn( 0, 192, 9, 48, 100);
    midifile.addNoteOff(0, 192 + 192/4,48, 1);
    midifile.addNoteOn( 0, 384, 9, 48, 100);
    midifile.addNoteOff(0, 384 + 192/4,48, 1);
    midifile.addNoteOn( 0, 576, 9, 48, 100);
    midifile.addNoteOff(0, 576 + 192/4,48, 1);
    // midifile.addTrack();
    // midifile.addNoteOn( 1, 0, 1, 48, 100);
    // midifile.addNoteOff(1, 192/4, 48, 1);
    // midifile.addNoteOn( 1, 192, 1, 48, 100);
    // midifile.addNoteOff(1, 192 + 192/4,48, 1);
    // midifile.addNoteOn( 1, 384, 1, 48, 100);
    // midifile.addNoteOff(1, 384 + 192/4,48, 1);
    // midifile.addNoteOn( 1, 576, 1, 48, 100);
    // midifile.addNoteOff(1, 576 + 192/4,48, 1);
    // midifile.addNoteOn( 1, 768, 1, 48, 100);
    // midifile.addNoteOff(1, 768 + 192/4,48, 1);
    midi_file_loaded = true;
}

void load_midi_file(std::string file_path)
{

    midi_file_loaded = false;
    running = false;

    //TODO: check that file exists
    midifile.read("/home/marius/.midi/" + file_path);
    // load_midi_test();
    midifile.doTimeAnalysis();
    midifile.linkNotePairs();

    int tracks = midifile.getTrackCount();

    cout << "**********************" << endl;
    cout << "MIDI FILE: " << file_path << endl;
    std::cout << "Ticks: " << midifile.getFileDurationInTicks() << '\n';
    cout << "TPQ: " << midifile.getTicksPerQuarterNote() << endl;
    if (tracks > 1)
        cout << "TRACKS: " << tracks << endl;
    for (int track = 0; track < tracks; track++)
    {
        if (tracks > 1)
            cout << "\nTrack " << track << endl;
        cout << "Index\tTick\tSeconds\tDur\tMessage" << endl;
        for (int event = 0; event < midifile[track].size(); event++)
        {
            cout << dec << event;
            cout << '\t' << dec << midifile[track][event].tick;
            cout << '\t' << dec << midifile[track][event].seconds;
            cout << '\t';
            cout << midifile[track][event].getDurationInSeconds();
            cout << '\t' << hex;
            for (int i = 0; i < midifile[track][event].size(); i++)
                cout << (int)midifile[track][event][i] << ' ';
            cout << endl;
        }
    }
    cout << "**********************" << endl;
    midifile.joinTracks();

    // compute duration
    double dur = 0.0;
    int total_event = midifile[0].size();
    if(midifile[0][total_event-1].isEndOfTrack()){
        cout << "MIDI DURATION : " << std::to_string(midifile.getFileDurationInTicks()) << endl;
        dur = midifile.getFileDurationInSeconds();
    }else{

        int numerator = 0;
        int denominator = 0;

        for (int track = 0; track < midifile.getTrackCount(); track++)
        {
            for (int i = 0; i < midifile[track].size(); i++)
            {
                MidiEvent& event = midifile[track][i];

                if (event.isMeta() && event.getMetaType() == 0x58)
                {
                    numerator = event[3];
                    denominator = 1 << event[4];
                    break;
                }
            }
        }

        if(numerator && denominator){
            int one_bar = (int)( midifile.getTicksPerQuarterNote() * denominator);
            int number_of_bars = (int)ceil((double)midifile.getFileDurationInTicks() /  one_bar);
            int number_of_beats = number_of_bars * numerator;
            dur = number_of_beats * 60.0 / 120.0;
            printf("one bar is %d ticks\n", one_bar);
            printf("number of bar: %d\n", number_of_bars);
            printf("=> duration: %f s\n", dur);
        }
    }
    midi_file_loaded = true;
    init = false;

    lo::Message message;
    message.add_string(file_path);
    message.add_double(dur);
    m_osc_client->send("/sequencer/midi_file_info", message);
}

void ping(std::string return_path){
    lo::Message message;
    m_osc_client->send(return_path, message);
}

void init_osc(){
    m_osc_server = new lo::ServerThread(9999);
    if (!m_osc_server->is_valid())
    {
        std::cout << "Unable to create osc_server." << std::endl;
        m_osc_server_initialized = false;
    }
    else
    {
        m_osc_server_initialized = true;
    }

    m_osc_server->add_method("/sequencer/ping", "ss", [](lo_arg **argv, int i)
                                { ping(&argv[1]->s); });
    m_osc_server->add_method("/sequencer/bpm", "i", [](lo_arg **argv, int i)
                                { set_bpm(argv[0]->i); });
    m_osc_server->add_method("/sequencer/state", "i", [](lo_arg **argv, int i)
                                { set_state(argv[0]->i); });
    m_osc_server->add_method("/sequencer/load_midi_file", "sss", [](lo_arg **argv, int i)
                                { load_midi_file(&argv[0]->s); });
    m_osc_server->add_method("/sequencer/volume", "i", [](lo_arg **argv, int i)
                                { set_volume(argv[0]->i); });
    m_osc_server->start();

    m_osc_client = new lo::Address("localhost", 9998);
    if (m_osc_client == NULL)
    {
        std::cout << "[LOOPER] Unable to create osc client" << std::endl;
        m_osc_client_initialized = false;
    }else{
        m_osc_client_initialized = true;
    }
}

int start(void)
{
    std::cout << "Init Jack" << std::endl;

    int i, err;
    m_jack_client = jack_client_open("sequencer", JackNullOption, NULL);
    if (m_jack_client == NULL)
    {
        std::cout << "Could not connect to the JACK server; run jackd first?" << std::endl;
        return -1;
    }

    // jack ports
    audio_output_port_l = jack_port_register(m_jack_client, JACK_AUDIO_OUTPUT_PORT_L, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    if (audio_output_port_l == NULL)
    {
        std::cout << "Could not register JACK audio output port" << std::endl;
        return -1;
    }
    audio_output_port_r = jack_port_register(m_jack_client, JACK_AUDIO_OUTPUT_PORT_R, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    if (audio_output_port_r == NULL)
    {
        std::cout << "Could not register JACK audio output port" << std::endl;
        return -1;
    }
    midi_output_port = jack_port_register(m_jack_client, JACK_MIDI_OUTPUT_PORT, JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);
    if (midi_output_port == NULL)
    {
        std::cout << "Could not register JACK midi output" << std::endl;
        return -1;
    }

    // process callback
    err = jack_set_process_callback(m_jack_client, process, NULL);
    if (err)
    {
        std::cout << "Could not register JACK process callback." << std::endl;
        return -1;
    }

    // timebase master callback
    err = jack_set_timebase_callback(m_jack_client, 0, timebase_callback, NULL);
    if (err)
    {
        std::cout << "Could not register JACK timebase callback %d." << std::endl;
        return -1;
    }

    // fluidsynth
    settings = new_fluid_settings();
    fluid_settings_setnum(settings, "synth.sample-rate", jack_get_sample_rate(m_jack_client));
    synth = new_fluid_synth(settings);
    if (fluid_synth_sfload(synth, SOUND_FONT, 1) == FLUID_FAILED)
    {
        std::cerr << "Cannot load SoundFont\n";
        return 1;
    }

    if (jack_activate(m_jack_client))
    {
        std::cout << "Cannot activate JACK client." << std::endl;
        return -1;
    }

    if(connect_ports() != 0)
        return -1;

    jack_transport_locate(m_jack_client, 0);
    jack_transport_start(m_jack_client);

    std::cout << "Jack initialized" << std::endl;

    return 0;
}

void stop(){
    for (int ch = 0; ch < 16; ch++)
    {
        fluid_synth_all_notes_off(synth, ch);
        fluid_synth_all_sounds_off(synth, ch);
    }

    jack_deactivate(m_jack_client);
    jack_client_close(m_jack_client);
    delete_fluid_synth(synth);
    delete_fluid_settings(settings);

    synth = nullptr;
    settings = nullptr;
    m_jack_client = nullptr;
}

void signal_callback(int sig)
{
    stopping = true;
}

int main(int argc, char **argv)
{
    signal(SIGINT, signal_callback);

    init_osc();
    while(start() != 0)
    {
        sleep(2);
    }

    std::thread pos_thread(position_thread);
    while (!stopping)
    {
        sleep(1);
    }

    pos_thread.join();
    stop();

    return 0;
}