"""
MapTrack — Image Gallery Backend (in-memory, no local disk storage)
═══════════════════════════════════════════════════════════════
Images are kept entirely in memory (a dict of id -> bytes) instead
of being written to disk. This keeps the server stateless and
portable to cloud hosts with ephemeral/read-only filesystems —
nothing to clean up on disk, nothing left behind on redeploy or
restart.

Images auto-expire and are dropped from memory 5 minutes after
upload. Restarting the server also clears everything immediately,
which is fine since nothing is meant to outlive 5 minutes anyway.

Timestamps are stored as timezone-aware UTC datetimes and serialized
with an explicit UTC offset (e.g. "...+00:00"), so the browser's
JavaScript Date parser interprets them correctly regardless of the
viewer's local timezone. A naive (timezone-less) timestamp here was
previously causing the expiry countdown to appear to expire images
instantly for anyone not in the server's own timezone.

Endpoints:
  POST /upload       -> ESP32 (or browser) sends a JPEG here.
                         Query params: ?device=..&lat=..&lng=..
  GET  /images       -> JSON list of currently-live image metadata
  GET  /image/<id>   -> raw JPEG bytes for one image
  GET  /             -> serves index.html (dashboard)
  GET  /gallery      -> serves image_gallery.html

The dashboard (index.html) pulls its GPS trail data straight from
Google Sheets (CSV export + Apps Script) in the browser via config.csvUrl
and config.scriptUrl — Sheets is the database, this server never touches
that data. This server's only job is the image gallery: receiving
uploads, holding them in memory, and serving both HTML pages.

Run locally:
    pip install flask
    python gallery_server.py
Then open http://<this-machine's-IP>:5000/ — dashboard at /, gallery at
/gallery. In production (Render etc.) this is run via gunicorn instead;
see the deploy instructions.
"""

from flask import Flask, request, jsonify, send_from_directory, Response
from datetime import datetime, timezone
import os
import threading
import time
import uuid

app = Flask(__name__)
BASE_DIR = os.path.dirname(os.path.abspath(__file__))

EXPIRY_SECONDS = 5 * 60  # images live for 5 minutes

# In-memory registry only — nothing touches disk.
# { id: {data: bytes, timestamp: aware datetime (UTC), lat: float|None, lng: float|None, device: str|None} }
images = {}
lock = threading.Lock()


@app.route("/upload", methods=["POST"])
def upload():
    data = request.get_data()
    if not data:
        return jsonify({"status": "error", "message": "empty body"}), 400

    lat = request.args.get("lat")
    lng = request.args.get("lng")
    device = request.args.get("device")  # which physical unit uploaded this

    img_id = uuid.uuid4().hex[:10]
    with lock:
        images[img_id] = {
            "data": data,
            "timestamp": datetime.now(timezone.utc),
            "lat": float(lat) if lat else None,
            "lng": float(lng) if lng else None,
            "device": device or "Unknown device",
        }

    print(f"[upload] {img_id} ({len(data)} bytes) device={device} lat={lat} lng={lng}")
    return jsonify({"status": "ok", "id": img_id})


@app.route("/images", methods=["GET"])
def list_images():
    cleanup_expired()
    with lock:
        result = [
            {
                "id": img_id,
                # isoformat() on a UTC-aware datetime yields a "+00:00" suffix,
                # which JS's Date parser reads as UTC unambiguously.
                "timestamp": meta["timestamp"].isoformat(timespec="seconds"),
                "lat": meta["lat"],
                "lng": meta["lng"],
                "device": meta.get("device") or "Unknown device",
            }
            for img_id, meta in images.items()
        ]
    result.sort(key=lambda m: m["timestamp"], reverse=True)
    return jsonify(result)


@app.route("/image/<img_id>")
def serve_image(img_id):
    with lock:
        meta = images.get(img_id)
    if not meta:
        return "Not found or expired", 404
    return Response(meta["data"], mimetype="image/jpeg")


@app.route("/")
@app.route("/index.html")
def dashboard_page():
    # Dashboard is the landing page. It reads/writes its data via the
    # Google Sheets CSV export + Apps Script endpoints configured in its
    # own Config tab — no server-side involvement needed for that part.
    return send_from_directory(BASE_DIR, "index.html")


@app.route("/gallery")
@app.route("/image_gallery.html")
def gallery_page():
    return send_from_directory(BASE_DIR, "image_gallery.html")


def cleanup_expired():
    now = datetime.now(timezone.utc)
    with lock:
        expired = [i for i, m in images.items()
                   if (now - m["timestamp"]).total_seconds() > EXPIRY_SECONDS]
        for img_id in expired:
            images.pop(img_id, None)
            print(f"[expired] removed {img_id}")


def cleanup_loop():
    while True:
        cleanup_expired()
        time.sleep(10)


# Started at import time (not inside `if __name__=="__main__"`) so this
# also runs correctly when a production server like gunicorn imports
# this module directly instead of executing it as a script.
threading.Thread(target=cleanup_loop, daemon=True).start()


if __name__ == "__main__":
    port = int(os.environ.get("PORT", 5000))  # Render (and similar hosts) inject PORT
    app.run(host="0.0.0.0", port=port, debug=False)
