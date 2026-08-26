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

FI_STEM_DEVICE_TOKENS = ("stemlab", "fistem", "stemlab")


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
    is to receive completed StemLab manifests over localhost and perform the
    Live Object Model operations that a VST3 cannot perform itself.
    """

    def __init__(self, c_instance):
        ControlSurface.__init__(self, c_instance)

        self._stemlab_running = True
        self._socket = None
        self._listener_thread = None
        self._last_manifest = None
        self._last_midi_manifest = None

        # Tracks recent Use Live Clip requests so the modern + legacy
        # compatibility messages do not trigger duplicate work.
        # 0.9.5 referenced this dictionary without creating it, causing every
        # clip request to raise AttributeError in the UDP listener thread.
        self._recent_clip_requests = {}

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
        self._listener_thread = thread
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
        if text == "stemlab_toggle_transport":
            self.schedule_message(1, self._toggle_transport)
            return

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

            self._last_midi_manifest = manifest_path
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

            self._last_manifest = manifest_path

            # Live's Song/Track API must only be touched on Live's main thread.
            self.schedule_message(
                1,
                lambda path=manifest_path: self._import_manifest_on_live_thread(path),
            )
            return

        if text.startswith("stemlab_get_clip "):
            payload = text[len("stemlab_get_clip ") :].strip()
            parts = payload.split(" ", 1)

            request_id = parts[0].strip() if parts else ""
            reply_path = ""

            if len(parts) > 1 and parts[1].strip():
                try:
                    reply_path = bytes.fromhex(parts[1].strip()).decode(
                        "utf-8",
                        errors="strict",
                    )
                except Exception as exc:
                    self.log_message("StemLabRemote: invalid clip reply path: %s" % exc)
                    return

            if request_id:
                now = time.time()
                previous = self._recent_clip_requests.get(request_id)

                # The VST deliberately sends a legacy fallback immediately
                # after the modern request. If this Remote Script already saw
                # the modern request with an explicit reply path, ignore the
                # duplicate fallback. Old Remote Scripts do not have this
                # dedupe and simply answer the second compatible request.
                if (
                    previous
                    and previous.get("reply_path")
                    and not reply_path
                    and now - previous.get("time", 0.0) < 1.0
                ):
                    return

                self._recent_clip_requests[request_id] = {
                    "time": now,
                    "reply_path": reply_path,
                }

                self.schedule_message(
                    1,
                    lambda rid=request_id, rp=reply_path: self._reply_with_source_clip(rid, rp),
                )

    def _toggle_transport(self):
        """Toggle Live on its main thread; VST3 has no reliable write API for this."""
        try:
            song = self.song()
            if song.is_playing:
                song.stop_playing()
            else:
                song.start_playing()
        except Exception:
            self.log_message("StemLabRemote: transport toggle failed:\n%s" % traceback.format_exc())

    # ------------------------------------------------------------------
    # Source clip lookup
    # ------------------------------------------------------------------

    def _reply_with_source_clip(self, request_id, reply_path=""):
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

            self._write_clip_reply(
                request_id=request_id,
                success=False,
                message=message,
                reply_path=reply_path,
            )

            self.log_message(
                "StemLabRemote: source clip lookup failed:\n%s" % traceback.format_exc()
            )

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

            if clips:
                try:
                    song_time = float(song.current_song_time)
                except Exception:
                    song_time = -1.0

                if song_time >= 0.0:
                    for clip in clips:
                        try:
                            if float(clip.start_time) <= song_time < float(clip.end_time):
                                return selected_track, clip
                        except Exception:
                            pass

                if len(clips) == 1:
                    return selected_track, clips[0]

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

        # Best case: the user has clicked the desired Arrangement clip and it
        # is the current Detail clip.
        try:
            detail_clip = song.view.detail_clip
        except Exception:
            detail_clip = None

        if detail_clip is not None:
            for clip in clips:
                try:
                    if clip == detail_clip:
                        return clip
                except Exception:
                    pass

        # Second choice: whichever clip is currently underneath Live's
        # Arrangement playhead.
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
        if reply_path:
            reply_path = os.path.abspath(str(reply_path))
            folder = os.path.dirname(reply_path)

            if folder and not os.path.isdir(folder):
                os.makedirs(folder)
        else:
            documents = self._documents_folder()

            folder = os.path.join(
                documents,
                "StemLab",
                "Ableton",
            )

            if not os.path.isdir(folder):
                os.makedirs(folder)

            reply_path = os.path.join(
                folder,
                "stemlab_clip_reply.json",
            )

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

    def _import_manifest_on_live_thread(self, manifest_path):
        """Validate once, then mutate Live one small step per UI tick.

        Live's Object Model is main-thread-only and some mutations are deferred.
        Creating every track and clip in one callback can make the operation
        brittle. 0.9.4 deliberately yields between track creation, clip
        creation, and clip-property updates.
        """
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
            documents = os.path.join(
                os.path.expanduser("~"),
                "Documents",
            )

            status_dir = os.path.join(
                documents,
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
        # OneDrive commonly redirects Documents. Prefer the actual environment
        # path when present, otherwise retain the normal Windows fallback.
        user_profile = os.environ.get(
            "USERPROFILE",
            os.path.expanduser("~"),
        )

        one_drive = os.environ.get("OneDriveCommercial") or os.environ.get("OneDrive")

        if one_drive:
            redirected = os.path.join(
                one_drive,
                "Documents",
            )

            if os.path.isdir(redirected):
                return redirected

        return os.path.join(
            user_profile,
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
        # RGB values. Live snaps these to its nearest available track colour.
        colors = {
            "vocals": 0xF15BAA,
            "drums": 0xFF9A42,
            "bass": 0x34D2FF,
            "guitar": 0x46E797,
            "piano": 0x8466FF,
            "other": 0xDCEAF4,
        }

        return colors.get(str(name or "").lower())
