import mido
import tkinter as tk
from tkinter import filedialog, messagebox
import sys
from collections import defaultdict

def is_drum_track(track):
    for msg in track:
        if msg.type in ('note_on', 'note_off'):
            if hasattr(msg, 'channel') and msg.channel == 9:
                return True
    return False

def midi_note_to_mida(note):
    note_names = ['C', 'C#', 'D', 'D#', 'E', 'F',
                  'F#', 'G', 'G#', 'A', 'A#', 'B']
    octave = (note // 12) - 1
    name = note_names[note % 12]
    return f"{name}{octave}"

def quantize_ticks(ticks_per_beat, eighth=True):
    return ticks_per_beat // 2 if eighth else ticks_per_beat

def velocity_to_typeset(velocity):
    if velocity >= 110:
        return '^|'
    elif velocity <= 40:
        return 'v|'
    else:
        return '*|'

def extract_drum_audicles(mid, ticks_per_beat):
    # Map: note -> list of events (abs_time, velocity)
    drum_events = defaultdict(list)
    for track in mid.tracks:
        abs_time = 0
        for msg in track:
            abs_time += msg.time
            if msg.type == 'note_on' and hasattr(msg, 'channel') and msg.channel == 9 and msg.velocity > 0:
                drum_events[msg.note].append((abs_time, msg.velocity))
    if not drum_events:
        return []

    eighth_ticks = quantize_ticks(ticks_per_beat, eighth=True)
    # Find the total length in ticks
    total_ticks = 0
    for note, events in drum_events.items():
        if events:
            total_ticks = max(total_ticks, max(e[0] for e in events))
    slot_count = ((total_ticks + eighth_ticks - 1) // eighth_ticks) + 1

    audicles = []
    for note in sorted(drum_events):
        # Build a timeline for this note
        timeline = [[] for _ in range(slot_count)]
        for abs_time, velocity in drum_events[note]:
            slot = abs_time // eighth_ticks
            timeline[slot].append(velocity)
        # Convert to type set tokens
        tokens = []
        for velocities in timeline:
            if not velocities:
                tokens.append('_')
            elif len(velocities) == 1:
                tokens.append(velocity_to_typeset(velocities[0]))
            else:
                # If multiple hits in one slot, output as a type set group
                group = ' '.join(velocity_to_typeset(v) for v in velocities)
                tokens.append('{' + group + '}')
        # Trim trailing rests
        while tokens and tokens[-1] == '_':
            tokens.pop()
        if not tokens:
            continue
        audicles.append('(' + ' '.join(tokens) + ')')
    return audicles

def track_to_audicle(track, ticks_per_beat):
    events = []
    abs_time = 0
    sixteenth_ticks = ticks_per_beat // 4
    for msg in track:
        abs_time += msg.time
        if msg.type == 'note_on' and msg.velocity > 0:
            events.append((abs_time, msg.note, 'on'))
        elif (msg.type == 'note_off') or (msg.type == 'note_on' and msg.velocity == 0):
            events.append((abs_time, msg.note, 'off'))
    if not events:
        return None
    total_ticks = max([e[0] for e in events]) if events else 0
    slot_count = ((total_ticks + sixteenth_ticks - 1) // sixteenth_ticks) + 1
    timeline = []
    active_notes = set()
    event_idx = 0
    for slot in range(slot_count):
        slot_start = slot * sixteenth_ticks
        slot_end = slot_start + sixteenth_ticks
        while event_idx < len(events) and events[event_idx][0] < slot_end:
            tick, note, evtype = events[event_idx]
            if evtype == 'on':
                active_notes.add(note)
            elif evtype == 'off':
                active_notes.discard(note)
            event_idx += 1
        timeline.append(set(active_notes))
    mida_tokens = []
    prev_notes = set()
    for notes in timeline:
        if not notes:
            mida_tokens.append('.')
        else:
            notes_sorted = sorted(notes)
            token = '~'.join(midi_note_to_mida(n) for n in notes_sorted)
            if notes == prev_notes and notes:
                mida_tokens.append('-')
            else:
                mida_tokens.append(token)
        prev_notes = set(notes)
    while mida_tokens and mida_tokens[-1] == '.':
        mida_tokens.pop()
    if not mida_tokens:
        return None
    return f"*{' '.join(mida_tokens)}*"

def main():
    root = tk.Tk()
    root.withdraw()
    midi_path = filedialog.askopenfilename(
        title="Select MIDI file",
        filetypes=[("MIDI files", "*.mid *.midi"), ("All files", "*.*")]
    )
    if not midi_path:
        messagebox.showinfo("No file selected", "No MIDI file was selected. Exiting.")
        sys.exit(0)

    try:
        mid = mido.MidiFile(midi_path)
    except Exception as e:
        messagebox.showerror("Error", f"Failed to load MIDI file:\n{e}")
        sys.exit(1)

    ticks_per_beat = mid.ticks_per_beat
    audicles = []
    drum_audicles = extract_drum_audicles(mid, ticks_per_beat)
    for track in mid.tracks:
        channel_map = set()
        for msg in track:
            if hasattr(msg, 'channel'):
                channel_map.add(msg.channel)
        if is_drum_track(track):
            continue
        audicle = track_to_audicle(track, ticks_per_beat)
        if audicle:
            audicles.append(audicle)

    output = []
    if drum_audicles:
        output.extend(drum_audicles)
    if not audicles and not drum_audicles:
        output.append("// No tracks found.")
    elif len(audicles) + len(drum_audicles) == 1:
        output.append(audicles[0] if audicles else drum_audicles[0])
    else:
        output.insert(0, "`~#")
        output.insert(1, "‘")
        output.extend(audicles)
        output.append("‘")

    result = '\n'.join(output)
    root.clipboard_clear()
    root.clipboard_append(result)
    messagebox.showinfo("MIDA Output", "MIDA code copied to clipboard!\n\n" +
                        "You can now paste it into your editor.\n\n" +
                        "Preview:\n" + result[:500] + ("..." if len(result) > 500 else ""))

if __name__ == "__main__":
    main()
