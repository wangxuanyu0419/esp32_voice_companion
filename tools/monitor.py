#!/usr/bin/env python3
"""
ESP32 Voice Companion — Serial Monitor Web Dashboard
Opens http://localhost:8765 with real-time log streaming.

Usage:
    python tools/monitor.py [--port /dev/cu.usbmodem101] [--baud 115200]
"""

import argparse
import json
import queue
import re
import signal
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer, ThreadingHTTPServer

try:
    import serial
except ImportError:
    print("pyserial not found. Install with:  pip install pyserial")
    sys.exit(1)

# ── Config ──────────────────────────────────────────────────────────────────
DEFAULT_PORT = "/dev/cu.usbmodem101"
DEFAULT_BAUD = 115200
HTTP_PORT    = 8765
MAX_HISTORY  = 500   # lines kept in memory for new browser connections

# ── Shared state ────────────────────────────────────────────────────────────
log_history: list[dict] = []          # {ts, level, tag, msg, raw}
sse_clients: list[queue.Queue] = []
state = {
    "serial_ok": False,
    "wifi":      "disconnected",
    "ws":        "disconnected",
    "app_state": "INIT",
    "device_id": "—",
    "server":    "—",
}

# ── Log parser ───────────────────────────────────────────────────────────────
LOG_RE = re.compile(r'^([IWED]) \((\d+)\) ([^:]+): (.*)')

LEVEL_MAP = {"I": "info", "W": "warn", "E": "error", "D": "debug"}

def parse_line(raw: str) -> dict:
    raw = raw.strip()
    m = LOG_RE.match(raw)
    if m:
        lvl, ts_ms, tag, msg = m.groups()
        return {"ts": int(ts_ms), "level": LEVEL_MAP.get(lvl, "info"),
                "tag": tag.strip(), "msg": msg, "raw": raw}
    return {"ts": 0, "level": "sys", "tag": "—", "msg": raw, "raw": raw}

def update_state(entry: dict):
    tag, msg = entry["tag"], entry["msg"]
    if tag == "WIFI_MANAGER":
        if "connected" in msg.lower():    state["wifi"] = "connected"
        elif "disconnect" in msg.lower(): state["wifi"] = "disconnected"
        elif "AP started" in msg:         state["wifi"] = "ap:" + msg.split(": ")[-1]
    elif tag == "WS_PROTOCOL":
        if "connected" in msg.lower():    state["ws"] = "connected"
        elif "disconnect" in msg.lower(): state["ws"] = "disconnected"
    elif tag == "APPLICATION":
        m = re.search(r'state.*?(\w+)', msg, re.IGNORECASE)
        if m: state["app_state"] = m.group(1)
    elif tag == "CONFIG_STORE":
        m = re.search(r'device=(\S+)', msg)
        if m: state["device_id"] = m.group(1)
        m = re.search(r'server=(\S+)', msg)
        if m: state["server"] = m.group(1)

# ── Serial reader thread ─────────────────────────────────────────────────────
def serial_reader(port: str, baud: int):
    while True:
        try:
            with serial.Serial(port, baud, timeout=0.5,
                               dsrdtr=False, rtscts=False) as ser:
                # Assert DTR so the ESP32-S3 USB-Serial/JTAG CDC console
                # knows a host is ready and starts flushing buffered log data.
                ser.dtr = True
                ser.rts = False
                state["serial_ok"] = True
                broadcast_event("status", state)
                print(f"[monitor] Connected to {port} @ {baud} (DTR asserted)")
                while True:
                    raw = ser.readline().decode("utf-8", errors="replace")
                    if not raw:
                        continue
                    entry = parse_line(raw)
                    update_state(entry)
                    log_history.append(entry)
                    if len(log_history) > MAX_HISTORY:
                        log_history.pop(0)
                    broadcast_event("log", entry)
        except serial.SerialException as e:
            state["serial_ok"] = False
            broadcast_event("status", state)
            print(f"[monitor] Serial error: {e}. Retrying in 0.3s…")
            time.sleep(0.3)  # Fast reconnect to catch device reboot

def broadcast_event(event: str, data: dict):
    dead = []
    payload = f"event: {event}\ndata: {json.dumps(data)}\n\n"
    for q in sse_clients:
        try:
            q.put_nowait(payload)
        except queue.Full:
            dead.append(q)
    for q in dead:
        sse_clients.remove(q)

# ── HTML page ────────────────────────────────────────────────────────────────
HTML = r"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 Voice Companion — Monitor</title>
<style>
  :root{--bg:#0d1117;--panel:#161b22;--border:#30363d;--text:#e6edf3;
        --dim:#8b949e;--info:#58a6ff;--warn:#d29922;--error:#f85149;
        --ok:#3fb950;--sys:#a5d6ff}
  *{box-sizing:border-box;margin:0;padding:0}
  body{background:var(--bg);color:var(--text);font-family:'Segoe UI',system-ui,sans-serif;
       display:flex;flex-direction:column;height:100vh;overflow:hidden}
  header{display:flex;align-items:center;gap:16px;padding:10px 18px;
         background:var(--panel);border-bottom:1px solid var(--border);flex-shrink:0}
  header h1{font-size:15px;font-weight:600}
  .dot{width:9px;height:9px;border-radius:50%;background:#555;flex-shrink:0}
  .dot.on{background:var(--ok);box-shadow:0 0 6px var(--ok)}
  .dot.warn{background:var(--warn);box-shadow:0 0 6px var(--warn)}
  .chips{display:flex;gap:8px;margin-left:auto;flex-wrap:wrap}
  .chip{padding:2px 10px;border-radius:20px;font-size:11px;font-weight:600;
        background:var(--border);color:var(--dim)}
  .chip.ok{background:#1a3a1a;color:var(--ok)}
  .chip.warn{background:#3a2a00;color:var(--warn)}
  .chip.err{background:#3a0a0a;color:var(--error)}
  main{display:flex;flex:1;overflow:hidden}
  #sidebar{width:200px;flex-shrink:0;background:var(--panel);
           border-right:1px solid var(--border);padding:12px;overflow-y:auto}
  #sidebar h2{font-size:11px;text-transform:uppercase;letter-spacing:.08em;
              color:var(--dim);margin-bottom:8px}
  .kv{display:flex;flex-direction:column;gap:6px;margin-bottom:16px}
  .kv-item{font-size:12px}
  .kv-item span:first-child{color:var(--dim);display:block;font-size:10px}
  .kv-item span:last-child{color:var(--text);word-break:break-all}
  #log-wrap{flex:1;overflow:hidden;display:flex;flex-direction:column}
  #toolbar{display:flex;gap:8px;padding:8px 12px;background:var(--panel);
           border-bottom:1px solid var(--border);flex-shrink:0;align-items:center}
  #filter{flex:1;background:var(--bg);border:1px solid var(--border);
          border-radius:6px;padding:4px 10px;color:var(--text);font-size:13px}
  .lvl-btn{padding:3px 10px;border-radius:4px;border:1px solid var(--border);
           background:none;color:var(--dim);cursor:pointer;font-size:12px}
  .lvl-btn.active{border-color:var(--info);color:var(--info)}
  #autoscroll{accent-color:var(--ok);cursor:pointer}
  #log{flex:1;overflow-y:auto;padding:4px 0;font-family:'Fira Code',monospace;font-size:12px}
  .line{display:flex;gap:8px;padding:1px 12px;border-bottom:1px solid transparent;
        line-height:1.55}
  .line:hover{background:#ffffff0a}
  .line .ts{color:var(--dim);width:70px;flex-shrink:0;font-size:11px;padding-top:1px}
  .line .tag{width:130px;flex-shrink:0;overflow:hidden;text-overflow:ellipsis;
             white-space:nowrap;color:var(--sys)}
  .line .msg{flex:1;white-space:pre-wrap;word-break:break-all}
  .info .msg{color:var(--text)}
  .warn .msg{color:var(--warn)}
  .error .msg{color:var(--error)}
  .sys .msg{color:var(--dim)}
  .debug .msg{color:#79c0ff}
  #count{font-size:11px;color:var(--dim);margin-left:auto}
</style>
</head>
<body>
<header>
  <div class="dot" id="serial-dot"></div>
  <h1>ESP32 Voice Companion</h1>
  <div class="chips">
    <div class="chip" id="chip-serial">serial: —</div>
    <div class="chip" id="chip-wifi">wifi: —</div>
    <div class="chip" id="chip-ws">ws: —</div>
    <div class="chip" id="chip-state">—</div>
  </div>
</header>
<main>
  <div id="sidebar">
    <h2>Device</h2>
    <div class="kv">
      <div class="kv-item"><span>ID</span><span id="kv-id">—</span></div>
      <div class="kv-item"><span>Server</span><span id="kv-server">—</span></div>
    </div>
    <h2>Filters (click tag)</h2>
    <div class="kv" id="tag-list"></div>
  </div>
  <div id="log-wrap">
    <div id="toolbar">
      <input id="filter" placeholder="Filter logs…" oninput="applyFilter()">
      <button class="lvl-btn active" data-lvl="all" onclick="setLevel(this)">ALL</button>
      <button class="lvl-btn" data-lvl="warn" onclick="setLevel(this)">W</button>
      <button class="lvl-btn" data-lvl="error" onclick="setLevel(this)">E</button>
      <label style="font-size:12px;color:var(--dim);display:flex;gap:4px;align-items:center">
        <input type="checkbox" id="autoscroll" checked> Auto-scroll
      </label>
      <button class="lvl-btn" onclick="clearLog()" style="margin-left:4px">Clear</button>
      <span id="count">0 lines</span>
    </div>
    <div id="log"></div>
  </div>
</main>
<script>
const logEl = document.getElementById('log');
let lines = [], filterText = '', filterLevel = 'all', tagFilter = null;
let lineCount = 0;
const tagCounts = {};

function fmtTs(ms){ return ms ? (ms/1000).toFixed(1)+'s' : '—' }

function matchLine(e){
  if(filterLevel==='warn' && e.level!=='warn' && e.level!=='error') return false;
  if(filterLevel==='error' && e.level!=='error') return false;
  if(tagFilter && e.tag!==tagFilter) return false;
  if(filterText && !e.raw.toLowerCase().includes(filterText)) return false;
  return true;
}

function renderLine(e){
  const d=document.createElement('div');
  d.className='line '+e.level;
  d.dataset.tag=e.tag; d.dataset.level=e.level; d.dataset.raw=e.raw.toLowerCase();
  d.innerHTML=`<span class="ts">${fmtTs(e.ts)}</span>`+
    `<span class="tag" title="${e.tag}">${e.tag}</span>`+
    `<span class="msg">${escHtml(e.msg)}</span>`;
  return d;
}

function escHtml(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;')}

function addEntry(e){
  lines.push(e);
  lineCount++;
  document.getElementById('count').textContent=lineCount+' lines';
  tagCounts[e.tag]=(tagCounts[e.tag]||0)+1;
  updateTagList();
  if(!matchLine(e)) return;
  const el=renderLine(e);
  logEl.appendChild(el);
  if(document.getElementById('autoscroll').checked)
    logEl.scrollTop=logEl.scrollHeight;
}

function applyFilter(){
  filterText=document.getElementById('filter').value.toLowerCase();
  rebuildLog();
}
function setLevel(btn){
  document.querySelectorAll('.lvl-btn').forEach(b=>b.classList.remove('active'));
  btn.classList.add('active'); filterLevel=btn.dataset.lvl; rebuildLog();
}
function clearLog(){ lines=[]; lineCount=0; logEl.innerHTML='';
  document.getElementById('count').textContent='0 lines'; }

function rebuildLog(){
  logEl.innerHTML='';
  const frag=document.createDocumentFragment();
  lines.filter(matchLine).forEach(e=>frag.appendChild(renderLine(e)));
  logEl.appendChild(frag);
  if(document.getElementById('autoscroll').checked)
    logEl.scrollTop=logEl.scrollHeight;
}

function updateTagList(){
  const el=document.getElementById('tag-list');
  el.innerHTML='';
  Object.entries(tagCounts).sort((a,b)=>b[1]-a[1]).forEach(([tag,cnt])=>{
    const d=document.createElement('div');
    d.className='kv-item'; d.style.cursor='pointer';
    if(tagFilter===tag) d.style.color='var(--info)';
    d.innerHTML=`<span>${tag}</span><span>${cnt}</span>`;
    d.onclick=()=>{ tagFilter=(tagFilter===tag)?null:tag; updateTagList(); rebuildLog(); };
    el.appendChild(d);
  });
}

function updateStatus(s){
  const dot=document.getElementById('serial-dot');
  const cs=document.getElementById('chip-serial');
  if(s.serial_ok){ dot.className='dot on'; cs.className='chip ok'; cs.textContent='serial: OK'; }
  else { dot.className='dot warn'; cs.className='chip warn'; cs.textContent='serial: lost'; }

  const cw=document.getElementById('chip-wifi');
  if(s.wifi==='connected'){ cw.className='chip ok'; cw.textContent='wifi: connected'; }
  else if(s.wifi.startsWith('ap:')){ cw.className='chip warn'; cw.textContent='wifi: AP '+s.wifi.slice(3); }
  else { cw.className='chip err'; cw.textContent='wifi: off'; }

  const cws=document.getElementById('chip-ws');
  if(s.ws==='connected'){ cws.className='chip ok'; cws.textContent='ws: connected'; }
  else { cws.className='chip err'; cws.textContent='ws: off'; }

  document.getElementById('chip-state').textContent=s.app_state||'—';
  document.getElementById('kv-id').textContent=s.device_id||'—';
  document.getElementById('kv-server').textContent=s.server||'—';
}

// ── SSE connection ───────────────────────────────────────────────────────────
function connect(){
  const es=new EventSource('/events');
  es.addEventListener('log',    e=>addEntry(JSON.parse(e.data)));
  es.addEventListener('status', e=>updateStatus(JSON.parse(e.data)));
  es.addEventListener('history',e=>JSON.parse(e.data).forEach(addEntry));
  es.onerror=()=>{ console.warn('SSE error, reconnecting…'); es.close(); setTimeout(connect,2000); };
}
connect();
</script>
</body>
</html>"""

# ── HTTP handler ─────────────────────────────────────────────────────────────
class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args): pass   # silence request logs

    def do_GET(self):
        if self.path == "/":
            body = HTML.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        elif self.path == "/events":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "keep-alive")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()

            q: queue.Queue = queue.Queue(maxsize=200)
            sse_clients.append(q)

            # send history + current status immediately
            try:
                hist_payload = f"event: history\ndata: {json.dumps(log_history[-MAX_HISTORY:])}\n\n"
                self.wfile.write(hist_payload.encode())
                stat_payload = f"event: status\ndata: {json.dumps(state)}\n\n"
                self.wfile.write(stat_payload.encode())
                self.wfile.flush()
            except BrokenPipeError:
                sse_clients.remove(q)
                return

            while True:
                try:
                    msg = q.get(timeout=15)
                    self.wfile.write(msg.encode())
                    self.wfile.flush()
                except queue.Empty:
                    # keepalive comment
                    try:
                        self.wfile.write(b": keepalive\n\n")
                        self.wfile.flush()
                    except BrokenPipeError:
                        break
                except BrokenPipeError:
                    break

            if q in sse_clients:
                sse_clients.remove(q)
        else:
            self.send_error(404)

# ── Main ─────────────────────────────────────────────────────────────────────
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=DEFAULT_PORT)
    ap.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    ap.add_argument("--http", type=int, default=HTTP_PORT)
    args = ap.parse_args()

    t = threading.Thread(target=serial_reader, args=(args.port, args.baud), daemon=True)
    t.start()

    ThreadingHTTPServer.allow_reuse_address = True
    server = ThreadingHTTPServer(("127.0.0.1", args.http), Handler)
    signal.signal(signal.SIGINT, lambda *_: (server.shutdown(), sys.exit(0)))

    url = f"http://localhost:{args.http}"
    print(f"\n  ESP32 Monitor  →  {url}\n")
    try:
        import webbrowser
        webbrowser.open(url)
    except Exception:
        pass

    server.serve_forever()

if __name__ == "__main__":
    main()
