#include <jack/jack.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <sstream>
#include <thread>
#include <cmath>
#include <set>
#include <map>
#include <algorithm>
#include <mutex>
#include <chrono>
#include <cctype>
#include <iomanip>
#include <atomic>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---- USER CONFIG ----
constexpr double BPM = 200.0;
constexpr double SIXTEENTH = 60.0 / BPM / 4.0;
constexpr double VOLUME = 0.15;
constexpr double DRUM_VOL = 0.19;
constexpr double ATTACK = 0.01;
constexpr double DECAY = 0.07;
constexpr double SUSTAIN = 0.7;
constexpr double RELEASE = 0.2;
constexpr double DRUM_ATTACK = 0.002;
constexpr double DRUM_DECAY = 0.09;
constexpr double DRUM_RELEASE = 0.12;
constexpr double MAX_SUSTAIN = 10.0;
constexpr int SAMPLE_RATE = 48000;
const std::string MIDA_FILENAME = "mida_file.txt";

// ---- Note name to MIDI ----
int noteNameToMidi(const std::string& s) {
    static const std::vector<std::string> names = {
        "C", "C#", "D", "D#", "E", "F",
        "F#", "G", "G#", "A", "A#", "B"
    };
    if (s.empty()) return -1;
    std::string base = s.substr(0, 1);
    int idx = -1;
    size_t pos = 1;
    if (s.size() > 2 && (s[1] == '#' || s[1] == 'b')) {
        base = s.substr(0, 2);
        pos = 2;
    }
    for (size_t i = 0; i < names.size(); ++i) {
        if (names[i] == base) { idx = i; break; }
    }
    if (idx == -1) return -1;
    int octave = std::stoi(s.substr(pos));
    return 12 * (octave + 1) + idx;
}

double midiToFreq(int midi) {
    return 440.0 * std::pow(2.0, (midi - 69) / 12.0);
}

// ---- MIDA Parsing ----
using Timeline = std::vector<std::vector<std::string>>; // [step][tokens]

// Utility: trim whitespace
std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    size_t end = s.find_last_not_of(" \t\r\n");
    return (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
}

// Split on delimiter, preserving empty tokens
std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        out.push_back(trim(item));
    }
    return out;
}

// ---- Layer 7 Melodic Audicle Parsing ----
Timeline parse_layer7_audicle(const std::string& audicle) {
    Timeline timeline;
    std::string body = audicle;
    if (body.front() == '*') body = body.substr(1);
    if (body.back() == '*') body.pop_back();
    std::vector<std::string> tokens = split(body, ' ');
    std::vector<std::string> prev_notes;
    for (const auto& tok : tokens) {
        if (tok.empty() || tok == "|") continue;
        if (tok == ".") {
            timeline.push_back({});
            prev_notes.clear();
        }
        else if (tok == "-") {
            if (!prev_notes.empty()) {
                timeline.push_back({ "-" });
            }
            else {
                timeline.push_back({});
            }
        }
        else {
            std::vector<std::string> notes = split(tok, '~');
            timeline.push_back(notes);
            prev_notes = notes;
        }
    }
    return timeline;
}

// ---- Layer 5 Drum Audicle Parsing ----
Timeline parse_layer5_audicle(const std::string& line) {
    Timeline timeline;
    std::string body = line;
    if (!body.empty() && body.front() == '(' && body.back() == ')')
        body = body.substr(1, body.size() - 2);

    std::vector<std::string> tokens;
    std::string token;
    bool in_group = false;
    std::string group_content;
    for (size_t i = 0; i < body.size(); ++i) {
        char c = body[i];
        if (c == '{') {
            in_group = true;
            group_content.clear();
        }
        else if (c == '}') {
            in_group = false;
            std::vector<std::string> group_tokens = split(group_content, ' ');
            timeline.push_back(group_tokens);
        }
        else if (in_group) {
            group_content += c;
        }
        else if (std::isspace(c)) {
            if (!token.empty()) {
                timeline.push_back({ token });
                token.clear();
            }
        }
        else {
            token += c;
        }
    }
    if (!token.empty()) timeline.push_back({ token });
    return timeline;
}

// ---- File Parsing ----
struct Audicle {
    Timeline timeline;
    bool is_drum;
    std::string name; // For debugging/logging
};

std::vector<Audicle> parse_mida_file(const std::string& corpus) {
    std::vector<Audicle> audicles;
    std::istringstream iss(corpus);
    std::string line;
    while (std::getline(iss, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '/') continue;
        if (line.front() == '*' && line.back() == '*') {
            audicles.push_back({ parse_layer7_audicle(line), false, "" });
        }
        else if (line.front() == '(' && line.back() == ')') {
            audicles.push_back({ parse_layer5_audicle(line), true, "" });
        }
    }
    return audicles;
}

// ---- JACK Synth Engine ----
struct Voice {
    int audicle = 0;
    int midi = -1;
    double freq = 0;
    double phase = 0;
    double gain = 0;
    double start_time = 0;
    bool active = false;
    bool released = false;
    double release_time = 0;
    double env_level = 0;
};

struct DrumVoice {
    int audicle = 0;
    std::string type;
    double start_time = 0;
    double gain = 1.0;
    double env_level = 1.0;
    bool active = false;
};

std::mutex synth_mutex;
std::vector<Voice> voices;
std::vector<DrumVoice> drum_voices;

// Improved oscillator: sine + triangle + saw
double improved_osc(double phase) {
    double sine = std::sin(phase);
    double tri = 2.0 * std::abs(2.0 * (phase / (2 * M_PI) - std::floor(phase / (2 * M_PI) + 0.5))) - 1.0;
    double saw = 2.0 * (phase / (2 * M_PI) - std::floor(phase / (2 * M_PI) + 0.5));
    return 0.6 * sine + 0.2 * tri + 0.2 * saw;
}

// Envelope for pitched synths
double envelope(const Voice& v, double t) {
    if (!v.released) {
        if (t < ATTACK) return (t / ATTACK);
        else if (t < ATTACK + DECAY) return 1.0 - (1.0 - SUSTAIN) * ((t - ATTACK) / DECAY);
        else return SUSTAIN;
    }
    else {
        double rel_t = t - (v.release_time - v.start_time);
        double env = v.env_level * ((1.0 - rel_t / RELEASE) > 0.0 ? (1.0 - rel_t / RELEASE) : 0.0);
        if (rel_t > RELEASE) return 0.0;
        return env;
    }
}

// Envelope for drums
double drum_env(double t) {
    if (t < DRUM_ATTACK) return t / DRUM_ATTACK;
    else if (t < DRUM_ATTACK + DRUM_DECAY) return 1.0 - (t - DRUM_ATTACK) / DRUM_DECAY;
    else return 0.0;
}

// Drum synthesis: different type set symbols get different timbres
double drum_sample(const DrumVoice& v, double t) {
    double env = drum_env(t) * v.gain;
    if (v.type == "*|") {
        double noise = ((rand() % 2000) / 1000.0 - 1.0) * env;
        double click = std::sin(2 * M_PI * 200.0 * t) * env * 0.5;
        return noise * 0.6 + click * 0.4;
    }
    if (v.type == "^|") {
        double noise = ((rand() % 2000) / 1000.0 - 1.0) * env * 1.5;
        double click = std::sin(2 * M_PI * 320.0 * t) * env * 0.8;
        return noise * 0.7 + click * 0.6;
    }
    if (v.type == "v|") {
        double noise = ((rand() % 2000) / 1000.0 - 1.0) * env * 0.5;
        double click = std::sin(2 * M_PI * 120.0 * t) * env * 0.2;
        return noise * 0.8 + click * 0.2;
    }
    double noise = ((rand() % 2000) / 1000.0 - 1.0) * env;
    double click = std::sin(2 * M_PI * 200.0 * t) * env * 0.5;
    return noise * 0.6 + click * 0.4;
}

// ---- JACK callback with atomic playhead ----
std::atomic<size_t> global_playhead_samples{ 0 };

int jack_callback(jack_nframes_t nframes, void* arg) {
    float* out = (float*)jack_port_get_buffer((jack_port_t*)arg, nframes);
    static double sample_rate = SAMPLE_RATE;
    std::lock_guard<std::mutex> lock(synth_mutex);
    for (jack_nframes_t i = 0; i < nframes; ++i) {
        double t = global_playhead_samples.load() / sample_rate;
        double sample = 0.0;
        for (size_t vi = 0; vi < voices.size(); ++vi) {
            Voice& v = voices[vi];
            if (v.active) {
                double rel_t = t - v.start_time;
                double env = envelope(v, rel_t);
                sample += improved_osc(v.phase) * v.gain * env;
                v.phase += 2 * M_PI * v.freq / sample_rate;
                if (!v.released && rel_t > MAX_SUSTAIN) {
                    v.released = true;
                    v.release_time = t;
                    v.env_level = envelope(v, rel_t);
                }
                if ((!v.released && env <= 0.0) || (v.released && env <= 0.0))
                    v.active = false;
            }
        }
        for (size_t vi = 0; vi < drum_voices.size(); ++vi) {
            DrumVoice& v = drum_voices[vi];
            if (v.active) {
                double rel_t = t - v.start_time;
                double env = drum_env(rel_t) * v.gain;
                sample += drum_sample(v, rel_t);
                if (env <= 0.0)
                    v.active = false;
            }
        }
        out[i] = static_cast<float>(sample);
        global_playhead_samples++;
    }
    voices.erase(std::remove_if(voices.begin(), voices.end(),
        [](const Voice& v) { return !v.active; }), voices.end());
    drum_voices.erase(std::remove_if(drum_voices.begin(), drum_voices.end(),
        [](const DrumVoice& v) { return !v.active; }), drum_voices.end());
    return 0;
}

void trigger_note(int audicle, int midi, double freq, double start_time) {
    std::lock_guard<std::mutex> lock(synth_mutex);
    Voice v;
    v.audicle = audicle;
    v.midi = midi;
    v.freq = freq;
    v.phase = 0;
    v.gain = VOLUME;
    v.active = true;
    v.released = false;
    v.start_time = start_time;
    voices.push_back(v);
}

void release_note(int audicle, int midi, double rel_time) {
    std::lock_guard<std::mutex> lock(synth_mutex);
    for (size_t vi = 0; vi < voices.size(); ++vi) {
        Voice& v = voices[vi];
        if (v.active && !v.released && v.audicle == audicle && v.midi == midi) {
            v.released = true;
            v.release_time = rel_time;
            double t = rel_time - v.start_time;
            v.env_level = envelope(v, t);
        }
    }
}

void trigger_drum(int audicle, const std::string& type, double start_time) {
    std::lock_guard<std::mutex> lock(synth_mutex);
    DrumVoice v;
    v.audicle = audicle;
    v.type = type;
    v.start_time = start_time;
    v.gain = (type == "^|") ? 1.6 : (type == "v|") ? 0.5 : 1.0;
    v.active = true;
    drum_voices.push_back(v);
}

// ---- Event Scheduling ----
struct ScheduledEvent {
    size_t sample_index;
    enum Type { NOTE_ON, NOTE_OFF, DRUM_ON, LOG_ROW } type;
    int midi;
    int audicle_idx;
    double freq;
    std::string drum_type;
    std::vector<std::string> log_cells; // Only for LOG_ROW
};

void schedule_events_and_log(
    const std::vector<Audicle>& audicles,
    std::vector<ScheduledEvent>& events,
    size_t& total_samples
) {
    size_t n_aud = audicles.size();
    // max_steps: the longest timeline, in 16ths, among all audicles
    size_t max_steps = 0;
    for (size_t i = 0; i < n_aud; ++i) {
        size_t steps = audicles[i].is_drum ? audicles[i].timeline.size() * 2 : audicles[i].timeline.size();
        if (steps > max_steps) max_steps = steps;
    }
    total_samples = static_cast<size_t>(std::ceil(max_steps * SIXTEENTH * SAMPLE_RATE));

    // Prepare log grid and schedule events
    std::vector<std::vector<std::string>> log_grid(max_steps, std::vector<std::string>(n_aud));
    for (size_t a = 0; a < n_aud; ++a) {
        const Timeline& tl = audicles[a].timeline;
        bool is_drum = audicles[a].is_drum;
        std::set<int> prev_midi;
        std::vector<std::string> prev_notes;
        if (is_drum) {
            // VISUALLY UPSAMPLE: repeat each drum cell for two 16th rows
            for (size_t drum_step = 0; drum_step < tl.size(); ++drum_step) {
                // Construct cell
                std::string cell;
                const auto& notes = tl[drum_step];
                if (notes.empty())
                    cell = "_";
                else if (notes.size() == 1)
                    cell = notes[0];
                else {
                    cell = "{";
                    for (size_t n = 0; n < notes.size(); ++n) {
                        if (n) cell += " ";
                        cell += notes[n];
                    }
                    cell += "}";
                }
                // Repeat for both 16th rows
                size_t row1 = 2 * drum_step;
                size_t row2 = 2 * drum_step + 1;
                if (row1 < max_steps) log_grid[row1][a] = cell;
                if (row2 < max_steps) log_grid[row2][a] = cell;
                // Schedule drum audio event ONLY at row1 (even step)
                double t = row1 * SIXTEENTH;
                size_t sample_idx = static_cast<size_t>(std::round(t * SAMPLE_RATE));
                for (size_t n = 0; n < notes.size(); ++n) {
                    if (notes[n] != "_") {
                        events.push_back({ sample_idx, ScheduledEvent::DRUM_ON, -1, (int)a, 0.0, notes[n], {} });
                    }
                }
            }
            // Fill any remaining log rows with "_"
            for (size_t step = 2 * tl.size(); step < max_steps; ++step) {
                log_grid[step][a] = "_";
            }
        }
        else {
            // Melodic: as usual
            for (size_t step = 0; step < max_steps; ++step) {
                std::string cell;
                double t = step * SIXTEENTH;
                size_t sample_idx = static_cast<size_t>(std::round(t * SAMPLE_RATE));
                if (step < tl.size()) {
                    const auto& notes = tl[step];
                    if (notes.empty()) cell = ".";
                    else if (notes.size() == 1 && notes[0] == "-") cell = "-";
                    else if (notes.size() == 1) cell = notes[0];
                    else {
                        for (size_t n = 0; n < notes.size(); ++n) {
                            if (n) cell += "~";
                            cell += notes[n];
                        }
                    }
                    std::set<int> current_midi;
                    if (notes.size() == 1 && notes[0] == "-") {
                        for (size_t i = 0; i < prev_notes.size(); ++i) {
                            int midi = noteNameToMidi(prev_notes[i]);
                            if (midi > 0) current_midi.insert(midi);
                        }
                    }
                    else {
                        for (size_t i = 0; i < notes.size(); ++i) {
                            int midi = noteNameToMidi(notes[i]);
                            if (midi > 0) current_midi.insert(midi);
                        }
                        prev_notes = notes;
                    }
                    for (auto midi : current_midi) {
                        if (prev_midi.count(midi) == 0) {
                            events.push_back({ sample_idx, ScheduledEvent::NOTE_ON, midi, (int)a, midiToFreq(midi), "", {} });
                        }
                    }
                    for (auto midi : prev_midi) {
                        if (current_midi.count(midi) == 0) {
                            events.push_back({ sample_idx, ScheduledEvent::NOTE_OFF, midi, (int)a, midiToFreq(midi), "", {} });
                        }
                    }
                    prev_midi = current_midi;
                }
                else {
                    cell = ".";
                }
                log_grid[step][a] = cell;
            }
            // Schedule note offs at the end
            if (tl.size() > 0) {
                size_t sample_idx = static_cast<size_t>(std::round(tl.size() * SIXTEENTH * SAMPLE_RATE));
                for (auto midi : prev_midi) {
                    events.push_back({ sample_idx, ScheduledEvent::NOTE_OFF, midi, (int)a, midiToFreq(midi), "", {} });
                }
            }
        }
    }
    // Schedule log rows
    for (size_t step = 0; step < max_steps; ++step) {
        size_t sample_idx = static_cast<size_t>(std::round(step * SIXTEENTH * SAMPLE_RATE));
        events.push_back({ sample_idx, ScheduledEvent::LOG_ROW, -1, -1, 0.0, "", log_grid[step] });
    }
    std::sort(events.begin(), events.end(), [](const ScheduledEvent& a, const ScheduledEvent& b) {
        if (a.sample_index != b.sample_index) return a.sample_index < b.sample_index;
        return a.type < b.type;
        });
}

// ---- Unified playback and log scheduler ----
void playback_and_log(const std::vector<ScheduledEvent>& events, size_t total_samples) {
    size_t event_idx = 0;
    size_t n_audicles = 0;
    if (!events.empty()) {
        for (const auto& ev : events) {
            if (ev.type == ScheduledEvent::LOG_ROW) {
                n_audicles = ev.log_cells.size();
                break;
            }
        }
    }
    // Print header
    for (size_t a = 0; a < n_audicles; ++a) std::cout << "A" << (a + 1) << " ";
    std::cout << std::endl;

    while (event_idx < events.size()) {
        size_t playhead = global_playhead_samples.load();
        while (event_idx < events.size() && events[event_idx].sample_index <= playhead) {
            const ScheduledEvent& ev = events[event_idx];
            if (ev.type == ScheduledEvent::NOTE_ON) {
                trigger_note(ev.audicle_idx, ev.midi, ev.freq, ev.sample_index / double(SAMPLE_RATE));
            }
            else if (ev.type == ScheduledEvent::NOTE_OFF) {
                release_note(ev.audicle_idx, ev.midi, ev.sample_index / double(SAMPLE_RATE));
            }
            else if (ev.type == ScheduledEvent::DRUM_ON) {
                trigger_drum(ev.audicle_idx, ev.drum_type, ev.sample_index / double(SAMPLE_RATE));
            }
            else if (ev.type == ScheduledEvent::LOG_ROW) {
                for (size_t a = 0; a < ev.log_cells.size(); ++a) {
                    std::cout << std::setw(3) << ev.log_cells[a];
                }
                std::cout << " <" << std::endl;
            }
            ++event_idx;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // Wait for tail of audio to finish
    while (global_playhead_samples.load() < total_samples + static_cast<size_t>(RELEASE * SAMPLE_RATE)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

int main() {
    std::ifstream infile(MIDA_FILENAME);
    if (!infile) {
        std::cerr << "Could not open file: " << MIDA_FILENAME << "\n";
        return 1;
    }
    std::string corpus((std::istreambuf_iterator<char>(infile)), std::istreambuf_iterator<char>());

    std::vector<Audicle> audicles = parse_mida_file(corpus);

    std::vector<ScheduledEvent> events;
    size_t total_samples = 0;
    schedule_events_and_log(audicles, events, total_samples);

    jack_client_t* client = jack_client_open("mida", JackNullOption, nullptr);
    if (!client) { std::cerr << "Could not open JACK client.\n"; return 1; }
    jack_port_t* output_port = jack_port_register(client, "out", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    if (!output_port) { std::cerr << "Could not register JACK port.\n"; return 1; }
    jack_set_process_callback(client, [](jack_nframes_t nframes, void* arg) {
        return jack_callback(nframes, arg);
        }, output_port);
    if (jack_activate(client)) { std::cerr << "Cannot activate JACK client.\n"; return 1; }

    const char** ports = jack_get_ports(client, nullptr, JACK_DEFAULT_AUDIO_TYPE, JackPortIsPhysical | JackPortIsInput);
    if (ports && ports[0]) {
        if (jack_connect(client, jack_port_name(output_port), ports[0]) != 0)
            std::cerr << "Failed to connect to " << ports[0] << "\n";
        if (ports[1])
            if (jack_connect(client, jack_port_name(output_port), ports[1]) != 0)
                std::cerr << "Failed to connect to " << ports[1] << "\n";
    }
    else {
        std::cerr << "No physical playback ports found for auto-connect.\n";
    }
    if (ports) jack_free((void*)ports);

    playback_and_log(events, total_samples);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    jack_client_close(client);
    return 0;
}
