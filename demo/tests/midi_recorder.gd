extends RefCounted
# A MIDI target for tests: implements the two methods every MidiDriver target has, and records
# each call. Plain script, so it proves the protocol works across the extension boundary.

var events: Array[Dictionary] = []
var stops: Array[Dictionary] = []


func midi_event(time_us: int, source: int, kind: int, channel: int, index: int, value: int, note_id: int) -> void:
	events.append({"time_us": time_us, "source": source, "kind": kind, "channel": channel, "index": index, "value": value, "note_id": note_id})


func midi_stop(time_us: int, source: int, mode: int) -> void:
	stops.append({"time_us": time_us, "source": source, "mode": mode})


var transports: Array[Dictionary] = []


func midi_transport(time_us: int, source: int, playing: bool, bpm: float, song_beat: float, loop_start: float, loop_end: float) -> void:
	transports.append({"time_us": time_us, "source": source, "playing": playing, "bpm": bpm, "song_beat": song_beat, "loop_start": loop_start, "loop_end": loop_end})


func of(kind: int) -> Array[Dictionary]:
	var out: Array[Dictionary] = []
	for e in events:
		if e.kind == kind:
			out.append(e)
	return out
