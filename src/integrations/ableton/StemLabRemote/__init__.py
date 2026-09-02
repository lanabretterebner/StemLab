from __future__ import absolute_import, print_function, unicode_literals

import json
import os
import socket
import threading
import time
import traceback

try:
    import Live
except ImportError:
    Live = None

try:
    from _Framework.ControlSurface import ControlSurface
except ImportError:
    # Some Live builds expose the same framework from ableton.v2.
    from ableton.v2.control_surface import ControlSurface


HOST = "127.0.0.1"
PORT = 39277
BUFFER_SIZE = 65535

PROTOCOL = "stemlab-ableton-bridge"
ACK_PROTOCOL = "stemlab-ableton-ack"
TEMPO_PROTOCOL = "stemlab-ableton-tempo"
TEMPO_REPLY_PROTOCOL = "stemlab-ableton-tempo-reply"

# Live's own limits on Song.tempo. Sending anything outside them raises out
# of the Live API rather than being clamped for us.
TEMPO_MIN = 20.0
TEMPO_MAX = 999.0

# "fistem" is what this product used to be called: a Set saved against those
# builds still carries the device under the old name, and the track it sits on
# is still the track the stems belong under.
FI_STEM_DEVICE_TOKENS = ("stemlab", "fistem")

# How long the in-flight import guard below believes in a chain that has not
# reported back. A chain is a handful of scheduled callbacks per stem, so this
# is far past any real import; it exists so a chain Live never resumed cannot
# refuse every later import for the rest of the session.
IMPORT_STALE_SECONDS = 60.0


def _normalise_midi_notes(items):
    """Validate the small JSON note contract before touching Live's API."""
    notes = []
    for item in items or ():
        try:
            notes.append(
                {
                    "pitch": max(0, min(127, int(item["pitch"]))),
                    "start": max(0.0, float(item["start"])),
                    "duration": max(0.0001, float(item["duration"])),
                    "velocity": max(1, min(127, int(item["velocity"]))),
                }
            )
        except (KeyError, TypeError, ValueError):
            continue
    return notes


def create_instance(c_instance):
    return StemLabRemote(c_instance)


class StemLabRemote(ControlSurface):
    """Invisible Ableton integration for the StemLab VST3.

    The script owns no MIDI controls and requires no MIDI ports. Its only job
    is to receive requests over localhost and perform the Live Object Model
    operations that a VST3 cannot perform itself. There are four, each one a
    UDP datagram from the plugin answered by a small JSON file it reads back:

      * ``stemlab_get_clip`` - which audio file the selected Arrangement clip
        plays, so Separate can work on what Live is already showing.
      * ``stemlab_ready`` - import the finished stems as Arrangement tracks
        under the source track.
      * ``stemlab_midi_ready`` - turn a transcribed stem into one editable
        Arrangement MIDI clip.
      * ``stemlab_set_tempo`` - put the analysed tempo into the Set, Warp off
        first so the audio it was measured from still plays at its own rate.
    """

    def __init__(self, c_instance):
        ControlSurface.__init__(self, c_instance)

        self._stemlab_running = True
        self._socket = None

        # The manifest whose import chain currently owns the Set, and when it
        # took it. Live's Object Model is mutated one small step per UI tick,
        # so an import is a chain of scheduled callbacks rather than one call:
        # a second chain started while the first is running inserts its tracks
        # at indices the first is still moving, and both end up writing the
        # same ack file. Created here rather than where it is first assigned:
        # a member only the listener thread creates raises AttributeError on
        # the first message that reads it, which is how 0.9.5 broke every clip
        # request.
        self._import_in_flight = None
        self._import_started = 0.0

        self.log_message("StemLabRemote: initializing")

        self._write_status(
            active=True,
            message="StemLab Remote Script active",
        )

        self._start_udp_listener()

        try:
            self.show_message("StemLab Remote Script active")
        except Exception:
            pass

    # ------------------------------------------------------------------
    # Lifetime / networking
    # ------------------------------------------------------------------

    def disconnect(self):
        self._stemlab_running = False

        if self._socket is not None:
            try:
                self._socket.close()
            except Exception:
                pass
            self._socket = None

        self._write_status(
            active=False,
            message="StemLab Remote Script stopped",
        )

        self.log_message("StemLabRemote: disconnected")
        ControlSurface.disconnect(self)

    def _start_udp_listener(self):
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.bind((HOST, PORT))
            sock.settimeout(0.25)
            self._socket = sock
        except Exception as exc:
            self._write_status(
                active=False,
                message="Could not bind UDP port %d: %s" % (PORT, exc),
            )
            self.log_message("StemLabRemote: UDP bind failed: %s" % exc)
            return

        thread = threading.Thread(
            target=self._listener_loop,
            name="StemLabRemoteUDP",
        )
        thread.daemon = True
        thread.start()

        self.log_message("StemLabRemote: listening on %s:%d" % (HOST, PORT))

    def _listener_loop(self):
        while self._stemlab_running:
            try:
                payload, _address = self._socket.recvfrom(BUFFER_SIZE)
            except socket.timeout:
                continue
            except Exception:
                if self._stemlab_running:
                    self.log_message("StemLabRemote: socket error:\n%s" % traceback.format_exc())
                break

            try:
                text = payload.decode("utf-8", errors="replace").strip()
                self._handle_udp_message(text)
            except Exception:
                self.log_message("StemLabRemote: UDP message error:\n%s" % traceback.format_exc())

    def _handle_udp_message(self, text):
        if text.startswith("stemlab_midi_ready "):
            encoded_path = text[len("stemlab_midi_ready ") :].strip()

            try:
                manifest_path = bytes.fromhex(encoded_path).decode(
                    "utf-8",
                    errors="strict",
                )
            except Exception as exc:
                self.log_message("StemLabRemote: invalid MIDI manifest path: %s" % exc)
                return

            self.schedule_message(
                1,
                lambda path=manifest_path: self._import_midi_on_live_thread(path),
            )
            return

        if text.startswith("stemlab_ready "):
            encoded_path = text[len("stemlab_ready ") :].strip()

            try:
                manifest_path = bytes.fromhex(encoded_path).decode(
                    "utf-8",
                    errors="strict",
                )
            except Exception as exc:
                self.log_message("StemLabRemote: invalid manifest path: %s" % exc)
                return

            # Live's Song/Track API must only be touched on Live's main thread.
            self.schedule_message(
                1,
                lambda path=manifest_path: self._import_manifest_on_live_thread(path),
            )
            return

        if text.startswith("stemlab_set_tempo "):
            payload = text[len("stemlab_set_tempo ") :].strip()
            parts = payload.split(" ", 1)

            request_id = parts[0].strip() if parts else ""

            if not request_id or len(parts) < 2 or not parts[1].strip():
                self.log_message("StemLabRemote: malformed tempo request")
                return

            try:
                request_path = bytes.fromhex(parts[1].strip()).decode(
                    "utf-8",
                    errors="strict",
                )
            except Exception as exc:
                self.log_message("StemLabRemote: invalid tempo request path: %s" % exc)
                return

            # Song.tempo and Clip.warping are Live Object Model writes, so
            # like every other write here they have to happen on Live's own
            # thread rather than on this socket thread.
            self.schedule_message(
                1,
                lambda rid=request_id, rp=request_path: self._set_tempo_on_live_thread(rid, rp),
            )
            return

        if text.startswith("stemlab_get_clip "):
            payload = text[len("stemlab_get_clip ") :].strip()
            parts = payload.split(" ", 1)

            request_id = parts[0].strip() if parts else ""

            # Both halves are required, as in the tempo request above. The
            # plugin names the file it will read the answer from, and there is
            # no longer a second, path-less request to keep compatible with:
            # the 0.9.3 fallback that one existed for went with the legacy
            # layers, which is also why no request needs de-duplicating here.
            if not request_id or len(parts) < 2 or not parts[1].strip():
                self.log_message("StemLabRemote: malformed clip request")
                return

            try:
                reply_path = bytes.fromhex(parts[1].strip()).decode(
                    "utf-8",
                    errors="strict",
                )
            except Exception as exc:
                self.log_message("StemLabRemote: invalid clip reply path: %s" % exc)
                return

            self.schedule_message(
                1,
                lambda rid=request_id, rp=reply_path: self._reply_with_source_clip(rid, rp),
            )

    # ------------------------------------------------------------------
    # Tempo
    # ------------------------------------------------------------------

    def _set_tempo_on_live_thread(self, request_id, request_path):
        """Put StemLab's analysed tempo into the Live Set.

        Two writes, in this order and always in this order:

        Warp off first. A warped clip is pinned to the Set's tempo, so
        raising the tempo underneath it speeds the audio up - and the audio
        is the very thing the tempo was measured from. Unwarped it plays at
        the rate it was recorded at, which is the only state in which the
        new tempo describes what you hear. This is the same reason REAPER's
        side of StemLab sets the item to a time timebase before it touches
        the project tempo.

        Then the tempo itself.
        """
        reply_path = ""
        song = None
        undo_open = False

        try:
            request = self._load_json(request_path) or {}

            # Before the protocol check, not after it: a rejected request
            # still has to answer on the path the plugin is watching, or the
            # plugin sits out its whole timeout and reports that nothing came
            # back instead of saying what was wrong with it.
            reply_path = str(request.get("reply_path") or "")

            if str(request.get("protocol") or "") != TEMPO_PROTOCOL:
                raise RuntimeError("Unrecognised StemLab tempo request")

            segments = self._normalise_tempo_segments(request.get("segments"))

            bpm = float(request.get("bpm") or 0.0)

            # The first section is what the track starts at, and the start
            # is what has to line up for anything after it to line up. With
            # no sections at all the single analysed tempo is the answer.
            if segments:
                bpm = float(segments[0]["bpm"])

            if not (TEMPO_MIN <= bpm <= TEMPO_MAX):
                raise RuntimeError(
                    "Analysed tempo %.2f is outside Live's range (%g-%g BPM)"
                    % (bpm, TEMPO_MIN, TEMPO_MAX)
                )

            song = self.song()

            source_path = str(request.get("source_path") or "")

            song.begin_undo_step()
            undo_open = True

            unwarped = self._unwarp_source_clips(song, source_path)

            song.tempo = bpm

            song.end_undo_step()
            undo_open = False

            message = "Live tempo set to %.2f BPM" % bpm

            if unwarped:
                message += " (Warp off on %d clip%s)" % (
                    unwarped,
                    "" if unwarped == 1 else "s",
                )

            # Said here rather than left for the user to discover: Live's
            # Remote Script API has no call that writes Arrangement
            # automation, and the tempo lives on the Main track, so a tempo
            # map cannot be written from a script at all. One tempo is
            # genuinely all that can be applied.
            if len(segments) > 1:
                message += ". %d tempo sections were detected - Live's API takes only one, " % len(
                    segments
                )
                message += "so draw the rest on the Main track's Song Tempo lane"

            self._write_tempo_reply(
                request_id=request_id,
                success=True,
                tempo=bpm,
                segments_total=len(segments),
                message=message,
                reply_path=reply_path,
            )

            self.log_message("StemLabRemote: %s" % message)

        except Exception as exc:
            if undo_open and song is not None:
                try:
                    song.end_undo_step()
                except Exception:
                    pass

            # Guarded, unlike the write on the success path: this is the
            # branch that already has something to report, and a reply that
            # cannot be written must not take the log line saying what
            # actually went wrong down with it.
            try:
                self._write_tempo_reply(
                    request_id=request_id,
                    success=False,
                    message=str(exc),
                    reply_path=reply_path,
                )
            except Exception:
                self.log_message(
                    "StemLabRemote: could not write the tempo reply:\n%s" % traceback.format_exc()
                )

            self.log_message("StemLabRemote: set tempo failed:\n%s" % traceback.format_exc())

    @staticmethod
    def _normalise_tempo_segments(items):
        """Validate the segment contract before any of it reaches Live."""
        segments = []

        for item in items or ():
            try:
                bpm = float(item["bpm"])
                start = float(item.get("start", 0.0))
            except (KeyError, TypeError, ValueError):
                continue

            if TEMPO_MIN <= bpm <= TEMPO_MAX:
                segments.append({"start": start, "bpm": bpm})

        segments.sort(key=lambda entry: entry["start"])

        return segments

    def _unwarp_source_clips(self, song, source_path):
        """Turn Warp off on every clip playing StemLab's source file.

        Matched by file, not by selection: by the time the tempo is set the
        user may well have clicked somewhere else, and the clip that must
        stop following the tempo is the one the tempo was measured from -
        wherever in the Set it happens to sit.
        """
        if not source_path:
            return 0

        try:
            target = os.path.abspath(str(source_path))
        except Exception:
            return 0

        unwarped = 0

        for track in list(song.tracks):
            clips = []

            try:
                clips.extend(list(track.arrangement_clips))
            except Exception:
                pass

            try:
                for slot in list(track.clip_slots):
                    if slot.has_clip:
                        clips.append(slot.clip)
            except Exception:
                pass

            for clip in clips:
                try:
                    if not clip.is_audio_clip:
                        continue

                    if os.path.abspath(str(clip.file_path or "")) != target:
                        continue

                    # Reading it first: assigning warping re-analyses the
                    # clip, so an already-unwarped clip is left alone rather
                    # than nudged.
                    if clip.warping:
                        clip.warping = False
                        unwarped += 1
                except Exception:
                    continue

        return unwarped

    def _write_tempo_reply(
        self,
        request_id,
        success,
        tempo=0.0,
        segments_total=0,
        message="",
        reply_path="",
    ):
        # The plugin puts the path it will watch into the request itself and
        # reads no other file, so an empty one is not a case to fall back for:
        # there is nowhere an answer would ever be read from.
        if not reply_path:
            self.log_message("StemLabRemote: tempo request carried no reply path")
            return

        reply_path = os.path.abspath(str(reply_path))
        folder = os.path.dirname(reply_path)

        if folder and not os.path.isdir(folder):
            os.makedirs(folder)

        self._atomic_write_json(
            reply_path,
            {
                "protocol": TEMPO_REPLY_PROTOCOL,
                "version": 1,
                "request_id": str(request_id),
                "success": bool(success),
                "tempo": float(tempo),
                "segments_total": int(segments_total),
                "message": str(message),
                "timestamp": time.time(),
            },
        )

    # ------------------------------------------------------------------
    # Source clip lookup
    # ------------------------------------------------------------------

    def _reply_with_source_clip(self, request_id, reply_path):
        try:
            song = self.song()
            source_track, clip = self._resolve_selected_audio_clip(song)

            file_path = str(getattr(clip, "file_path", "") or "")

            if not file_path:
                raise RuntimeError("The selected Live clip does not expose an audio file path")

            file_path = os.path.abspath(file_path)

            if not os.path.isfile(file_path):
                raise RuntimeError("Live's source audio file was not found: %s" % file_path)

            try:
                start_beat = float(clip.start_time)
            except Exception:
                start_beat = 0.0

            try:
                clip_name = str(clip.name or "")
            except Exception:
                clip_name = ""

            source_name = self._safe_name(
                source_track,
                "StemLab Source",
            )

            self._write_clip_reply(
                request_id=request_id,
                success=True,
                path=file_path,
                start_beat=start_beat,
                source_track=source_name,
                clip_name=clip_name,
                message="Source ready",
                reply_path=reply_path,
            )

            self.log_message("StemLabRemote: source clip -> %s" % file_path)

        except Exception as exc:
            message = str(exc)

            # Guarded for the reason the tempo reply is: if the reply cannot
            # be written the log line below is the only account of why.
            try:
                self._write_clip_reply(
                    request_id=request_id,
                    success=False,
                    message=message,
                    reply_path=reply_path,
                )
            except Exception:
                self.log_message(
                    "StemLabRemote: could not write the clip reply:\n%s" % traceback.format_exc()
                )

            self.log_message(
                "StemLabRemote: source clip lookup failed:\n%s" % traceback.format_exc()
            )

    @staticmethod
    def _pick_arrangement_clip(song, clips, detail_clip=None):
        """Choose one Arrangement clip out of several, for both resolvers.

        The same order of preference wherever the question is asked: the clip
        the user has open in Detail, then whichever clip Live's playhead is
        inside, then a lone clip. None when none of those answers, because
        what that means differs - one caller still has a fallback to try and
        the other has to tell the user which click it is waiting for.
        """
        if detail_clip is not None:
            for clip in clips:
                try:
                    if clip == detail_clip:
                        return clip
                except Exception:
                    pass

        try:
            song_time = float(song.current_song_time)
        except Exception:
            song_time = -1.0

        if song_time >= 0.0:
            for clip in clips:
                try:
                    if float(clip.start_time) <= song_time < float(clip.end_time):
                        return clip
                except Exception:
                    pass

        if len(clips) == 1:
            return clips[0]

        return None

    def _resolve_selected_audio_clip(self, song):
        # Fast path: use the clip Live already has selected/open in Detail.
        try:
            detail_clip = song.view.detail_clip
        except Exception:
            detail_clip = None

        try:
            selected_track = song.view.selected_track
        except Exception:
            selected_track = None

        if detail_clip is not None:
            file_path = str(getattr(detail_clip, "file_path", "") or "")

            if file_path:
                if selected_track is not None:
                    try:
                        for clip in selected_track.arrangement_clips:
                            if clip == detail_clip:
                                return selected_track, detail_clip
                    except Exception:
                        pass

                # Only scan Arrangement clip ownership if selected_track did
                # not already identify the clip. This avoids recursive device
                # scanning during the normal Use Live Clip path.
                for track in song.tracks:
                    try:
                        for clip in track.arrangement_clips:
                            if clip == detail_clip:
                                return track, detail_clip
                    except Exception:
                        pass

                if selected_track is not None:
                    return selected_track, detail_clip

        if selected_track is not None:
            try:
                clips = list(selected_track.arrangement_clips)
            except Exception:
                clips = []

            # No detail clip handed on: it was tried above, and one that got
            # this far has no audio file behind it to separate.
            clip = self._pick_arrangement_clip(song, clips)

            if clip is not None:
                return selected_track, clip

        # Compatibility fallback only.
        _index, source_track = self._resolve_source_track(song)

        return (
            source_track,
            self._resolve_source_clip(song, source_track),
        )

    def _resolve_source_clip(self, song, source_track):
        try:
            clips = list(source_track.arrangement_clips)
        except Exception:
            clips = []

        if not clips:
            raise RuntimeError("No Arrangement audio clips were found on the StemLab track")

        try:
            detail_clip = song.view.detail_clip
        except Exception:
            detail_clip = None

        clip = self._pick_arrangement_clip(song, clips, detail_clip)

        if clip is not None:
            return clip

        raise RuntimeError(
            "Multiple clips are on this track. Click the clip you want "
            "or place Live's playhead inside it, then press Use Live Clip."
        )

    def _write_clip_reply(
        self,
        request_id,
        success,
        path="",
        start_beat=0.0,
        source_track="",
        clip_name="",
        message="",
        reply_path="",
    ):
        # As with the tempo reply: the plugin names the file it watches, and
        # a clip reply is read from nowhere else.
        if not reply_path:
            self.log_message("StemLabRemote: clip request carried no reply path")
            return

        reply_path = os.path.abspath(str(reply_path))
        folder = os.path.dirname(reply_path)

        if folder and not os.path.isdir(folder):
            os.makedirs(folder)

        payload = {
            "protocol": "stemlab-clip-source",
            "version": 1,
            "request_id": str(request_id),
            "success": bool(success),
            "path": str(path),
            "start_beat": float(start_beat),
            "source_track": str(source_track),
            "clip_name": str(clip_name),
            "message": str(message),
            "timestamp": time.time(),
        }

        self._atomic_write_json(
            reply_path,
            payload,
        )

    # ------------------------------------------------------------------
    # Manifest / source-track resolution
    # ------------------------------------------------------------------

    def _import_midi_on_live_thread(self, manifest_path):
        """Create one editable Arrangement MIDI clip beside the source track."""
        try:
            manifest = self._load_json(manifest_path)
            if manifest.get("protocol") != "stemlab-ableton-midi":
                raise RuntimeError("Unsupported StemLab MIDI protocol")

            notes = _normalise_midi_notes(manifest.get("notes"))
            if not notes:
                raise RuntimeError("StemLab MIDI manifest contains no valid notes")

            song = self.song()
            source_index, source_track = self._resolve_source_track(song)
            target_name = str(manifest.get("target_track") or "").strip()
            target_index = self._find_midi_track(song, target_name)

            state = {
                "manifest_path": manifest_path,
                "notes": notes,
                "start_beat": max(0.0, float(manifest.get("capture_start_ppq", 0.0))),
                "stem": str(manifest.get("source_stem") or "stem"),
                "source_name": self._safe_name(source_track, "StemLab Source"),
            }

            if target_index is None:
                target_index = source_index + 1
                song.create_midi_track(target_index)
                self.schedule_message(
                    1,
                    lambda s=state, index=target_index: self._populate_midi_track(s, index, True),
                )
            else:
                self._populate_midi_track(state, target_index, False)

        except Exception as exc:
            self._finish_midi_import(manifest_path, False, str(exc))

    def _find_midi_track(self, song, target_name):
        if not target_name:
            return None
        for index, track in enumerate(song.tracks):
            try:
                if str(track.name) == target_name and bool(track.has_midi_input):
                    return index
            except Exception:
                pass
        return None

    def _populate_midi_track(self, state, track_index, rename_track):
        try:
            song = self.song()
            tracks = list(song.tracks)
            if track_index < 0 or track_index >= len(tracks):
                raise RuntimeError("Live did not expose the MIDI track")

            track = tracks[track_index]
            pretty_stem = self._title_case(state["stem"])
            if rename_track:
                track.name = "%s - %s MIDI" % (state["source_name"], pretty_stem)

            end_beat = max(note["start"] + note["duration"] for note in state["notes"])
            clip_length = max(0.25, end_beat)
            track.create_midi_clip(float(state["start_beat"]), float(clip_length))

            self.schedule_message(
                1,
                lambda s=state, index=track_index: self._write_midi_notes(s, index),
            )
        except Exception as exc:
            self._finish_midi_import(state["manifest_path"], False, str(exc))

    def _write_midi_notes(self, state, track_index):
        try:
            track = list(self.song().tracks)[track_index]
            clips = list(track.arrangement_clips)
            if not clips:
                raise RuntimeError("Live did not create the Arrangement MIDI clip")

            start_beat = float(state["start_beat"])
            clip = min(clips, key=lambda item: abs(float(item.start_time) - start_beat))
            clip.name = "StemLab %s MIDI" % self._title_case(state["stem"])

            if Live is not None and hasattr(Live.Clip, "MidiNoteSpecification"):
                specifications = tuple(
                    Live.Clip.MidiNoteSpecification(
                        pitch=note["pitch"],
                        start_time=note["start"],
                        duration=note["duration"],
                        velocity=note["velocity"],
                        mute=False,
                    )
                    for note in state["notes"]
                )
                clip.add_new_notes(specifications)
            else:
                # Live 11's Remote Script API accepts the older tuple shape.
                clip.set_notes(
                    tuple(
                        (
                            note["pitch"],
                            note["start"],
                            note["duration"],
                            note["velocity"],
                            False,
                        )
                        for note in state["notes"]
                    )
                )

            message = "Created %s MIDI clip with %d notes" % (
                self._title_case(state["stem"]),
                len(state["notes"]),
            )
            self._finish_midi_import(state["manifest_path"], True, message)
        except Exception as exc:
            self._finish_midi_import(state["manifest_path"], False, str(exc))

    def _finish_midi_import(self, manifest_path, success, message):
        try:
            ack_path = os.path.join(
                os.path.dirname(os.path.abspath(manifest_path)),
                "stemlab_ableton_midi_ack.json",
            )
            self._atomic_write_json(
                ack_path,
                {
                    "protocol": "stemlab-ableton-midi-ack",
                    "version": 1,
                    "success": bool(success),
                    "message": str(message),
                    "timestamp": time.time(),
                },
            )
        except Exception:
            pass

        self._write_status(active=True, message=str(message))
        self.log_message("StemLabRemote: " + str(message))
        try:
            self.show_message("StemLab: " + str(message))
        except Exception:
            pass

    @staticmethod
    def _import_job_dir(manifest_path):
        """The folder holding the one ack file an import of this manifest writes.

        _write_ack names the ack after the folder rather than after the
        manifest, so this is the identity that decides whether two requests
        are answered by the same file - and therefore whether one of them can
        be left to the other.
        """
        try:
            return os.path.normcase(
                os.path.dirname(os.path.abspath(str(manifest_path))),
            )
        except Exception:
            return str(manifest_path)

    def _import_manifest_on_live_thread(self, manifest_path):
        """Validate once, then mutate Live one small step per UI tick.

        Live's Object Model is main-thread-only and some mutations are deferred.
        Creating every track and clip in one callback can make the operation
        brittle. 0.9.4 deliberately yields between track creation, clip
        creation, and clip-property updates.

        Which is exactly why only one of these may run at a time: between two
        of those yields there is nothing stopping a second Send Stems from
        starting its own chain, and the two would then insert tracks at
        indices the other has already moved.
        """
        if (
            self._import_in_flight is not None
            and time.time() - self._import_started < IMPORT_STALE_SECONDS
        ):
            running_job = self._import_job_dir(self._import_in_flight)

            if self._import_job_dir(manifest_path) == running_job:
                # The same job, which is the impatient Retry Import click and
                # the common case. Compared by folder rather than by manifest
                # name because one job directory holds two of them - Send
                # Stems writes stemlab_ableton_selected_manifest.json while
                # Retry falls back to stemlab_ableton_manifest.json - and the
                # single ack file both would be answered by is named after the
                # folder, not after either manifest. So the chain in progress
                # writes exactly the ack this request re-armed the wait for,
                # and refusing it here would put a failure into that same file
                # for the plugin to read before the success lands - a wrong
                # "import failed", and a retry that then does duplicate the
                # tracks this guard exists to keep single.
                self.log_message(
                    "StemLabRemote: this job is already importing, ignoring the repeat request"
                )
            else:
                # A different job: nothing the running chain writes will land
                # in this manifest's folder, so answer where the plugin is
                # watching rather than leaving it to time out with no reason.
                self._write_ack(
                    manifest_path=manifest_path,
                    success=False,
                    imported=0,
                    message=(
                        "Another StemLab import is still running. Wait for it "
                        "to finish, then press Retry Import."
                    ),
                )

            return

        self._import_in_flight = manifest_path
        self._import_started = time.time()

        try:
            manifest = self._load_json(manifest_path)

            if manifest.get("protocol") != PROTOCOL:
                raise RuntimeError("Unsupported StemLab manifest protocol")

            stems = manifest.get("stems") or []

            if not stems:
                raise RuntimeError("StemLab manifest contains no stems")

            start_beat = float(manifest.get("capture_start_ppq", 0.0))

            if start_beat < 0.0:
                start_beat = 0.0

            song = self.song()

            source_index, source_track = self._resolve_source_track(song)

            source_name = self._safe_name(
                source_track,
                "StemLab Source",
            )

            state = {
                "manifest_path": manifest_path,
                "stems": stems,
                "start_beat": start_beat,
                "source_index": source_index,
                "source_name": source_name,
                "next_index": 0,
                "imported": 0,
            }

            self._write_import_progress(
                state,
                "Starting Ableton import",
            )

            self.schedule_message(
                1,
                lambda s=state: self._create_next_stem_track(s),
            )

        except Exception as exc:
            self._finish_import_failure(
                manifest_path=manifest_path,
                imported=0,
                message=str(exc),
            )

    def _create_next_stem_track(self, state):
        try:
            stems = state["stems"]
            next_index = int(state["next_index"])

            if next_index >= len(stems):
                self._finish_import_success(state)
                return

            stem = stems[next_index]
            stem_name = str(stem.get("name") or "stem")

            audio_path = os.path.abspath(str(stem.get("path") or ""))

            if not audio_path or not os.path.isfile(audio_path):
                raise RuntimeError("Missing stem file: %s" % audio_path)

            insert_index = int(state["source_index"]) + 1 + int(state["imported"])

            self._write_import_progress(
                state,
                "Creating %s track" % self._title_case(stem_name),
            )

            song = self.song()
            song.create_audio_track(insert_index)

            # Let Live publish the newly-created Track object before accessing
            # it or asking it to create an Arrangement clip.
            self.schedule_message(
                1,
                lambda s=state, idx=insert_index, item=stem: self._populate_stem_track(
                    s,
                    idx,
                    item,
                ),
            )

        except Exception as exc:
            self._finish_import_state_failure(
                state,
                str(exc),
            )

    def _populate_stem_track(
        self,
        state,
        insert_index,
        stem,
    ):
        try:
            song = self.song()
            tracks = list(song.tracks)

            if insert_index < 0 or insert_index >= len(tracks):
                raise RuntimeError("Live did not expose the new audio track")

            new_track = tracks[insert_index]

            stem_name = str(stem.get("name") or "stem")

            pretty_name = self._title_case(stem_name)

            audio_path = os.path.abspath(str(stem.get("path") or ""))

            new_track.name = "%s - %s" % (
                state["source_name"],
                pretty_name,
            )

            color = self._stem_color(stem_name)

            if color is not None:
                try:
                    new_track.color = color
                except Exception:
                    pass

            self._write_import_progress(
                state,
                "Placing %s audio" % pretty_name,
            )

            new_track.create_audio_clip(
                audio_path,
                float(stem.get("start_beat", state["start_beat"])),
            )

            # Clip creation and warping changes may be deferred by Live.
            self.schedule_message(
                1,
                lambda s=state, idx=insert_index, name=pretty_name: self._finish_stem_clip(
                    s,
                    idx,
                    name,
                ),
            )

        except Exception as exc:
            self._finish_import_state_failure(
                state,
                str(exc),
            )

    def _finish_stem_clip(
        self,
        state,
        insert_index,
        pretty_name,
    ):
        try:
            song = self.song()
            tracks = list(song.tracks)

            if insert_index >= 0 and insert_index < len(tracks):
                try:
                    clips = list(tracks[insert_index].arrangement_clips)

                    if clips:
                        clips[-1].warping = False
                except Exception as exc:
                    self.log_message(
                        "StemLabRemote: could not disable Warp for %s: %s" % (pretty_name, exc)
                    )

            state["imported"] = int(state["imported"]) + 1
            state["next_index"] = int(state["next_index"]) + 1

            self._write_import_progress(
                state,
                "Imported %s" % pretty_name,
            )

            self.schedule_message(
                1,
                lambda s=state: self._create_next_stem_track(s),
            )

        except Exception as exc:
            self._finish_import_state_failure(
                state,
                str(exc),
            )

    def _finish_import_success(self, state):
        # Every path out of the chain hands the Set back, so the next Send
        # Stems is not refused by a guard nobody is holding any more.
        self._import_in_flight = None

        imported = int(state["imported"])
        source_name = str(state["source_name"])
        start_beat = float(state["start_beat"])

        message = "Imported %d stem%s under %s at beat %.3f" % (
            imported,
            "" if imported == 1 else "s",
            source_name,
            start_beat,
        )

        self._write_ack(
            manifest_path=state["manifest_path"],
            success=True,
            imported=imported,
            message=message,
            source_track=source_name,
            start_beat=start_beat,
        )

        self._write_import_progress(
            state,
            message,
            complete=True,
        )

        self._write_status(
            active=True,
            message=message,
        )

        self.log_message("StemLabRemote: " + message)

        try:
            self.show_message("StemLab: " + message)
        except Exception:
            pass

    def _finish_import_state_failure(
        self,
        state,
        message,
    ):
        self._import_in_flight = None

        self._write_ack(
            manifest_path=state["manifest_path"],
            success=False,
            imported=int(state.get("imported", 0)),
            message=message,
            source_track=str(state.get("source_name", "")),
            start_beat=float(state.get("start_beat", 0.0)),
        )

        self._write_import_progress(
            state,
            "Import failed: " + message,
            failed=True,
        )

        self._write_status(
            active=True,
            message="Import failed: " + message,
        )

        self.log_message("StemLabRemote: import failed:\n%s" % traceback.format_exc())

        try:
            self.show_message("StemLab import failed: " + message)
        except Exception:
            pass

    def _finish_import_failure(
        self,
        manifest_path,
        imported,
        message,
    ):
        self._import_in_flight = None

        self._write_ack(
            manifest_path=manifest_path,
            success=False,
            imported=imported,
            message=message,
        )

        state = {
            "manifest_path": manifest_path,
            "stems": [],
            "imported": imported,
        }

        self._write_import_progress(
            state,
            "Import failed: " + message,
            failed=True,
        )

        self.log_message("StemLabRemote: import failed: %s" % message)

    def _write_import_progress(
        self,
        state,
        message,
        complete=False,
        failed=False,
    ):
        try:
            manifest_path = os.path.abspath(state["manifest_path"])

            job_dir = os.path.dirname(manifest_path)

            path = os.path.join(
                job_dir,
                "stemlab_ableton_import_progress.json",
            )

            stems = state.get("stems") or []

            payload = {
                "protocol": "stemlab-ableton-import-progress",
                "version": 1,
                "message": str(message),
                "imported": int(state.get("imported", 0)),
                "total": int(len(stems)),
                "complete": bool(complete),
                "failed": bool(failed),
                "timestamp": time.time(),
            }

            self._atomic_write_json(
                path,
                payload,
            )

        except Exception:
            self.log_message(
                "StemLabRemote: could not write import progress:\n%s" % traceback.format_exc()
            )

    def _resolve_source_track(self, song):
        tracks = list(song.tracks)

        selected = None

        try:
            selected = song.view.selected_track
        except Exception:
            pass

        candidates = []

        for index, track in enumerate(tracks):
            if self._track_contains_stemlab(track):
                candidates.append((index, track))

        if selected is not None:
            for index, track in candidates:
                if track == selected:
                    return index, track

        if len(candidates) == 1:
            return candidates[0]

        if not candidates:
            raise RuntimeError(
                "Could not find a track containing StemLab. "
                "Keep the StemLab VST on the source audio track."
            )

        raise RuntimeError(
            "Multiple StemLab instances are loaded. "
            "Select the source track before pressing Separate/Retry Import."
        )

    def _track_contains_stemlab(self, track):
        try:
            devices = list(track.devices)
        except Exception:
            return False

        for device in devices:
            if self._device_tree_contains_stemlab(device):
                return True

        return False

    def _device_tree_contains_stemlab(self, device):
        labels = []

        for attribute in (
            "name",
            "class_name",
            "class_display_name",
        ):
            try:
                labels.append(str(getattr(device, attribute)))
            except Exception:
                pass

        searchable = " ".join(labels).lower()

        if any(token in searchable for token in FI_STEM_DEVICE_TOKENS):
            return True

        # Also support StemLab placed inside an Audio Effect Rack.
        try:
            chains = list(device.chains)
        except Exception:
            chains = []

        for chain in chains:
            try:
                nested_devices = list(chain.devices)
            except Exception:
                nested_devices = []

            for nested in nested_devices:
                if self._device_tree_contains_stemlab(nested):
                    return True

        return False

    # ------------------------------------------------------------------
    # Ack / status files
    # ------------------------------------------------------------------

    def _write_ack(
        self,
        manifest_path,
        success,
        imported,
        message,
        source_track="",
        start_beat=0.0,
    ):
        try:
            job_dir = os.path.dirname(os.path.abspath(manifest_path))

            ack_path = os.path.join(
                job_dir,
                "stemlab_ableton_ack.json",
            )

            payload = {
                "protocol": ACK_PROTOCOL,
                "version": 2,
                "transport": "remote-script",
                "success": bool(success),
                "imported": int(imported),
                "message": str(message),
                "source_track": str(source_track),
                "start_beat": float(start_beat),
                "timestamp": time.time(),
            }

            self._atomic_write_json(
                ack_path,
                payload,
            )

        except Exception:
            self.log_message("StemLabRemote: could not write ack:\n%s" % traceback.format_exc())

    def _write_status(self, active, message):
        try:
            # Through the same helper the plugin's own answer comes from.
            # This file is the only sign the VST3 has that the script is
            # installed and loaded, and it looks for it under the Documents
            # folder Windows reports - which stopped being
            # %USERPROFILE%\Documents the moment OneDrive's Known Folder Move
            # redirected it.
            status_dir = os.path.join(
                self._documents_folder(),
                "StemLab",
                "Ableton",
            )

            if not os.path.isdir(status_dir):
                os.makedirs(status_dir)

            path = os.path.join(
                status_dir,
                "stemlab_remote_status.json",
            )

            payload = {
                "protocol": "stemlab-remote-status",
                "version": 1,
                "active": bool(active),
                "message": str(message),
                "port": PORT,
                "timestamp": time.time(),
            }

            self._atomic_write_json(
                path,
                payload,
            )

        except Exception:
            pass

    @staticmethod
    def _documents_folder():
        """The Documents folder Windows reports, rather than a guess at it.

        The plugin resolves its half through JUCE, which asks the shell for
        CSIDL_PERSONAL, so this side has to ask the same question to land in
        the same place: under OneDrive's Known Folder Move that answer is
        %OneDrive%\\Documents, and with OneDrive merely installed it is still
        %USERPROFILE%\\Documents even though an %OneDrive%\\Documents usually
        exists beside it to be mistaken for the redirect.
        """
        if os.name == "nt":
            try:
                import ctypes

                # MAX_PATH, which is what SHGetSpecialFolderPathW writes into.
                buffer = ctypes.create_unicode_buffer(260)

                # CSIDL_PERSONAL, do not create: the same call with the same
                # arguments as JUCE's getSpecialLocation makes on the plugin
                # side, so neither can drift away from the other.
                if ctypes.windll.shell32.SHGetSpecialFolderPathW(None, buffer, 5, False):
                    if buffer.value:
                        return buffer.value
            except Exception:
                # A shell that will not answer leaves the guess below, which
                # is still better than nothing to write the heartbeat into.
                pass

        one_drive = os.environ.get("OneDriveCommercial") or os.environ.get("OneDrive")

        if one_drive:
            redirected = os.path.join(
                one_drive,
                "Documents",
            )

            if os.path.isdir(redirected):
                return redirected

        return os.path.join(
            os.environ.get("USERPROFILE", os.path.expanduser("~")),
            "Documents",
        )

    @staticmethod
    def _atomic_write_json(path, payload):
        temp = path + ".tmp"

        with open(
            temp,
            "w",
            encoding="utf-8",
        ) as handle:
            json.dump(
                payload,
                handle,
                ensure_ascii=False,
                indent=2,
            )

        os.replace(temp, path)

    @staticmethod
    def _load_json(path):
        with open(
            path,
            "r",
            encoding="utf-8",
        ) as handle:
            return json.load(handle)

    @staticmethod
    def _safe_name(obj, fallback):
        try:
            value = str(obj.name)
        except Exception:
            value = ""

        return value if value else fallback

    @staticmethod
    def _title_case(value):
        value = str(value or "")

        if not value:
            return "Stem"

        return value[:1].upper() + value[1:]

    @staticmethod
    def _stem_color(name):
        # RGB values. Live snaps these to its nearest available track color.
        colors = {
            "vocals": 0xF15BAA,
            "drums": 0xFF9A42,
            "bass": 0x34D2FF,
            "guitar": 0x46E797,
            "piano": 0x8466FF,
            "other": 0xDCEAF4,
        }

        return colors.get(str(name or "").lower())
