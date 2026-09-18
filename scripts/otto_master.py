#!/usr/bin/env python3
"""Tiny local master for validating EVA1 Wi-Fi/mDNS/TCP connectivity.

The firmware resolves master.local and connects to TCP/8765. Messages are
newline-delimited JSON objects. This tool intentionally does not implement
audio, MQTT, or MCP; it only verifies device discovery and round trips.

macOS uses /usr/bin/dns-sd to register the proxy record master.local without
requiring a Python mDNS package.
"""

from __future__ import annotations

import argparse
import collections
import json
import os
import socket
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any


TCP_DEFAULT_PORT = 8765
HTTP_DEFAULT_PORT = 8080
SERVICE_TYPE = "_otto-master._tcp"
PROJECT_ROOT = Path(__file__).resolve().parent.parent
FIRMWARE_PATH = PROJECT_ROOT / "build" / "xiaozhi.bin"
SENSITIVE_KEYS = {
    "api_key",
    "authorization",
    "current_token",
    "password",
    "secret",
    "token",
}


def redact(value: Any) -> Any:
    """Return a JSON-safe copy with provisioning material removed."""
    if isinstance(value, dict):
        return {
            key: "[REDACTED]" if key.lower() in SENSITIVE_KEYS else redact(item)
            for key, item in value.items()
        }
    if isinstance(value, list):
        return [redact(item) for item in value]
    return value


def local_ip() -> str:
    """Return the LAN address selected by the OS routing table."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.connect(("224.0.0.251", 5353))
        return sock.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        sock.close()


class Registry:
    def __init__(self) -> None:
        self.lock = threading.RLock()
        self.devices: dict[str, dict[str, Any]] = {}
        self.clients: dict[str, "OttoClient"] = {}
        self.events: collections.deque[dict[str, Any]] = collections.deque(maxlen=300)

    def event(self, direction: str, device_id: str, message: Any) -> None:
        safe_message = redact(message)
        item = {
            "time": time.time(),
            "direction": direction,
            "device_id": device_id,
            "message": safe_message,
        }
        with self.lock:
            self.events.append(item)
            device = self.devices.get(device_id)
            if device is not None:
                device["last_message"] = safe_message
                device["last_seen"] = time.time()

    def register(self, client: "OttoClient", hello: dict[str, Any]) -> str:
        board = hello.get("board") if isinstance(hello.get("board"), dict) else {}
        device_id = str(hello.get("mac") or f"{client.address[0]}:{client.address[1]}")
        with self.lock:
            self.clients[device_id] = client
            self.devices[device_id] = {
                "device_id": device_id,
                "name": hello.get("name", "unknown"),
                "protocol": hello.get("protocol", "unknown"),
                "mac": hello.get("mac", ""),
                "ip": client.address[0],
                "port": client.address[1],
                "board": board,
                "online": True,
                "connected_at": time.time(),
                "last_seen": time.time(),
                "last_message": hello,
            }
        self.event("in", device_id, hello)
        return device_id

    def disconnect(self, device_id: str, client: "OttoClient") -> None:
        with self.lock:
            if self.clients.get(device_id) is client:
                self.clients.pop(device_id, None)
                if device_id in self.devices:
                    self.devices[device_id]["online"] = False
                    self.devices[device_id]["last_seen"] = time.time()

    def send(self, device_id: str, message: dict[str, Any]) -> None:
        with self.lock:
            client = self.clients.get(device_id)
        if client is None:
            raise KeyError(f"device is offline: {device_id}")
        client.send(message)
        self.event("out", device_id, message)

    def snapshot(self) -> dict[str, Any]:
        with self.lock:
            devices = []
            for device in self.devices.values():
                copied = dict(device)
                copied["connected_at"] = _iso_time(copied["connected_at"])
                copied["last_seen"] = _iso_time(copied["last_seen"])
                devices.append(copied)
            events = [dict(event) for event in list(self.events)[-100:]]
        for event in events:
            event["time"] = _iso_time(event["time"])
        return {"devices": devices, "events": events}


def _iso_time(value: float) -> str:
    return time.strftime("%H:%M:%S", time.localtime(value))


REGISTRY = Registry()


class OttoClient:
    def __init__(self, connection: socket.socket, address: tuple[str, int]) -> None:
        self.connection = connection
        self.address = address
        self.send_lock = threading.Lock()
        self.device_id = ""

    def send(self, message: dict[str, Any]) -> None:
        payload = (json.dumps(message, ensure_ascii=False, separators=(",", ":")) + "\n").encode()
        with self.send_lock:
            self.connection.sendall(payload)

    def run(self) -> None:
        self.connection.settimeout(90)
        buffer = b""
        try:
            while True:
                chunk = self.connection.recv(4096)
                if not chunk:
                    break
                buffer += chunk
                if len(buffer) > 64 * 1024:
                    raise ValueError("device message buffer is too large")
                while b"\n" in buffer:
                    line, buffer = buffer.split(b"\n", 1)
                    line = line.strip()
                    if not line:
                        continue
                    message = json.loads(line.decode("utf-8"))
                    if not isinstance(message, dict):
                        continue
                    if message.get("type") == "hello":
                        self.device_id = REGISTRY.register(self, message)
                    elif self.device_id:
                        REGISTRY.event("in", self.device_id, message)
                        if message.get("type") == "heartbeat":
                            with REGISTRY.lock:
                                device = REGISTRY.devices.get(self.device_id)
                                if device:
                                    device["board"] = message.get("board", device.get("board", {}))
        except (ConnectionError, OSError, ValueError, UnicodeDecodeError, json.JSONDecodeError) as exc:
            print(f"[device {self.address[0]}] closed: {exc}")
        finally:
            if self.device_id:
                REGISTRY.disconnect(self.device_id, self)
            try:
                self.connection.close()
            except OSError:
                pass


def device_accept_loop(server_socket: socket.socket) -> None:
    while True:
        try:
            connection, address = server_socket.accept()
        except OSError:
            return
        client = OttoClient(connection, address)
        print(f"[device] TCP connection from {address[0]}:{address[1]}")
        threading.Thread(target=client.run, daemon=True).start()


HTML = r"""<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Otto Master</title>
<style>
@import url('https://fonts.googleapis.com/css2?family=Archivo+Black&family=IBM+Plex+Mono:wght@400;500;600&display=swap');
:root{--paper:#ebe6d8;--ink:#171717;--red:#d63227;--yellow:#efb72b;--blue:#1a62a0;--white:#f8f5eb;--muted:#716c60;--line:3px solid var(--ink)}
*{box-sizing:border-box}
html{background:var(--paper)}
body{margin:0;min-height:100vh;color:var(--ink);font-family:"IBM Plex Mono",ui-monospace,SFMono-Regular,Menlo,monospace;background:var(--paper);letter-spacing:.015em}
body:before{content:"";position:fixed;inset:0;z-index:-1;pointer-events:none;opacity:.18;background-image:linear-gradient(to right,transparent 31px,#766f60 32px),linear-gradient(to bottom,transparent 31px,#766f60 32px);background-size:32px 32px}
.shell{max-width:1420px;margin:0 auto;padding:26px clamp(18px,4vw,58px) 56px;position:relative}
.masthead{border-top:14px solid var(--ink);padding-top:15px;display:grid;grid-template-columns:minmax(0,1.35fr) minmax(310px,.65fr);gap:30px;align-items:end}
.eyebrow{font-size:11px;font-weight:600;letter-spacing:.16em;text-transform:uppercase;color:var(--red);margin-bottom:12px}
h1{margin:0;font-family:"Archivo Black","Arial Black",sans-serif;letter-spacing:-.08em;text-transform:uppercase;font-size:clamp(44px,8vw,112px);line-height:.86;max-width:760px}
.lede{max-width:620px;margin:18px 0 0;font-size:13px;line-height:1.7;color:var(--muted)}
.mark{display:flex;justify-content:flex-end;align-items:flex-end;gap:9px;height:120px}
.mark span{display:block;border:var(--line)}.mark .block-red{width:58px;height:94px;background:var(--red)}.mark .block-blue{width:86px;height:58px;background:var(--blue)}.mark .block-yellow{width:36px;height:36px;background:var(--yellow);align-self:flex-start}.mark .block-black{width:18px;height:120px;background:var(--ink)}
.protocol-strip{display:grid;grid-template-columns:repeat(4,1fr);gap:2px;background:var(--ink);border:var(--line);margin-top:24px}
.protocol{background:var(--paper);padding:13px 14px 12px;min-height:66px}.protocol.red{background:var(--red);color:var(--white)}.protocol.blue{background:var(--blue);color:var(--white)}.protocol.yellow{background:var(--yellow)}
.protocol label{display:block;font-size:10px;font-weight:600;letter-spacing:.1em;text-transform:uppercase;opacity:.72;margin-bottom:7px}.protocol strong{font-size:16px;font-weight:600}.protocol code{font-size:13px}
.dashboard{display:grid;grid-template-columns:minmax(0,1.55fr) minmax(290px,.45fr);gap:20px;margin-top:24px}
.panel{background:var(--white);border:var(--line);box-shadow:9px 9px 0 var(--ink);min-width:0}.panel-head{display:flex;align-items:center;justify-content:space-between;gap:20px;padding:15px 17px;border-bottom:var(--line);background:var(--white)}.panel-title{display:flex;align-items:center;gap:12px;font-family:"Archivo Black","Arial Black",sans-serif;font-size:19px;letter-spacing:-.045em;text-transform:uppercase}.section-no{display:inline-grid;place-items:center;width:32px;height:32px;background:var(--red);color:var(--white);font-family:"IBM Plex Mono",monospace;font-size:12px;letter-spacing:0}.panel-meta{font-size:10px;letter-spacing:.1em;text-transform:uppercase;color:var(--muted)}
.table-wrap{overflow:auto}table{border-collapse:collapse;width:100%;min-width:690px}th,td{padding:12px 14px;text-align:left;vertical-align:top;border-bottom:1px solid #b8b2a3}th{font-size:10px;letter-spacing:.12em;text-transform:uppercase;background:var(--ink);color:var(--white);border-bottom:0}td{font-size:12px;line-height:1.45}tbody tr:last-child td{border-bottom:0}tbody tr:hover td{background:#e9e1cb}.device-name{font-family:"Archivo Black","Arial Black",sans-serif;font-size:17px;letter-spacing:-.04em}.device-id{display:block;font-size:10px;color:var(--muted);margin-top:5px}.status{display:inline-flex;align-items:center;gap:7px;text-transform:uppercase;font-size:11px;font-weight:600}.status:before{content:"";width:9px;height:9px;background:currentColor;display:inline-block}.online{color:#147436}.offline{color:var(--red)}.ip{font-size:13px;font-weight:600}.last-message{max-width:510px;background:#e8e3d5;padding:10px;border-left:6px solid var(--blue)}pre{white-space:pre-wrap;word-break:break-word;margin:0;font-size:11px;line-height:1.5}
.network-poster{padding:17px}.poster-band{display:flex;justify-content:space-between;align-items:center;padding:10px 12px;background:var(--red);color:var(--white);font-size:10px;letter-spacing:.14em;font-weight:600}.poster-band span:last-child{font-size:9px}.unit-count{padding:22px 10px 15px;border-left:var(--line);border-right:var(--line);font-family:"Archivo Black","Arial Black",sans-serif;font-size:82px;line-height:.85;letter-spacing:-.1em;background:var(--yellow)}.unit-label{padding:9px 10px 12px;border:var(--line);border-top:0;font-size:11px;letter-spacing:.13em}.spec-list{margin:18px 0 0;border-top:var(--line)}.spec-row{display:grid;grid-template-columns:1fr 1.2fr;gap:10px;padding:10px 0;border-bottom:1px solid #b8b2a3;font-size:11px}.spec-row dt{color:var(--muted);text-transform:uppercase}.spec-row dd{margin:0;text-align:right;font-weight:600}.legend{display:grid;grid-template-columns:1fr 1fr 1fr;gap:7px;margin-top:18px}.legend span{height:9px;display:block}.legend .r{background:var(--red)}.legend .y{background:var(--yellow)}.legend .b{background:var(--blue)}
.dispatch{grid-column:1 / -1}.dispatch-body{padding:17px;display:grid;grid-template-columns:minmax(200px,.35fr) minmax(0,1fr);gap:17px}.dispatch-side{display:flex;flex-direction:column;gap:10px}.field-label{display:block;margin-bottom:7px;font-size:10px;font-weight:600;letter-spacing:.12em;text-transform:uppercase;color:var(--muted)}select,input,textarea{width:100%;border:var(--line);border-radius:0;background:#e8e3d5;color:var(--ink);font:inherit}select,input{height:44px;padding:0 10px;font-size:12px;font-weight:600}textarea{min-height:143px;padding:12px;font-size:12px;line-height:1.55;resize:vertical}.action-grid{display:grid;grid-template-columns:1.4fr 1fr 1fr;gap:6px}.quick-grid{display:grid;grid-template-columns:repeat(6,1fr);gap:8px}button{min-height:40px;padding:9px 12px;border:var(--line);border-radius:0;background:var(--ink);color:var(--white);font:600 11px "IBM Plex Mono",monospace;letter-spacing:.07em;text-transform:uppercase;cursor:pointer;box-shadow:4px 4px 0 transparent;transition:transform .12s,box-shadow .12s,background .12s}button:hover{transform:translate(-2px,-2px);box-shadow:4px 4px 0 var(--ink)}button.primary{background:var(--red)}button.secondary{background:var(--blue)}button.tertiary{background:var(--yellow);color:var(--ink)}.send-row{display:flex;align-items:center;gap:12px;margin-top:10px}.send-row button{min-width:130px}.hint{color:var(--muted);font-size:11px}.result{min-height:16px}.live-report{margin-top:11px;border:var(--line);border-left:8px solid var(--red);background:#e8e3d5;padding:11px 12px;font-size:11px;line-height:1.5}.report-head{display:flex;justify-content:space-between;gap:12px;border-bottom:1px solid #b8b2a3;padding-bottom:7px;margin-bottom:8px;color:var(--muted);font-size:9px;letter-spacing:.12em;text-transform:uppercase}.report-grid{display:grid;grid-template-columns:repeat(3,1fr);gap:10px}.report-grid span{display:block;color:var(--muted);font-size:9px;letter-spacing:.1em;text-transform:uppercase}.report-grid strong{display:block;margin-top:3px;font-size:12px}
.events{grid-column:1 / -1}.event-log{max-height:480px;overflow:auto;padding:15px 17px;background:#171717;color:#f0eadc;border-left:12px solid var(--blue)}.event-log pre{font-size:11px;line-height:1.55}.event-log::-webkit-scrollbar{width:10px}.event-log::-webkit-scrollbar-thumb{background:var(--yellow)}
@media(max-width:900px){.masthead,.dashboard{grid-template-columns:1fr}.mark{justify-content:flex-start;height:72px}.mark .block-red{height:58px}.mark .block-black{height:72px}.protocol-strip{grid-template-columns:repeat(2,1fr)}.dispatch-body{grid-template-columns:1fr}.panel{box-shadow:6px 6px 0 var(--ink)}}
@media(max-width:560px){.shell{padding:16px 12px 36px}.protocol-strip{grid-template-columns:1fr 1fr}.protocol{min-height:60px;padding:10px}.protocol strong{font-size:13px}h1{font-size:54px}.quick-grid{grid-template-columns:1fr}.panel-head{align-items:flex-start;flex-direction:column;gap:9px}}
</style></head>
<body>
<main class="shell">
  <header class="masthead">
    <div>
      <div class="eyebrow">LOCAL ROBOT CONTROL / OTTO-MASTER-01</div>
      <h1>OTTO<br>MASTER</h1>
      <p class="lede">A hard-edged control board for a soft-bodied machine collective. Discover robots by name, watch their pulse, and dispatch JSON without touching an IP address.</p>
    </div>
    <div class="mark" aria-hidden="true"><span class="block-red"></span><span class="block-blue"></span><span class="block-yellow"></span><span class="block-black"></span></div>
  </header>
  <section class="protocol-strip" aria-label="网络协议状态">
    <div class="protocol red"><label>Discovery</label><strong>mDNS / <code>master.local</code></strong></div>
    <div class="protocol blue"><label>Transport</label><strong>TCP / 8765</strong></div>
    <div class="protocol yellow"><label>Refresh</label><strong>1 SEC / LIVE</strong></div>
    <div class="protocol"><label>Protocol</label><strong>OTTO-MASTER / 1</strong></div>
  </section>
  <section class="dashboard">
    <article class="panel devices-panel">
      <div class="panel-head"><div class="panel-title"><span class="section-no">01</span>Device Formation</div><div class="panel-meta"><span id="device-count">00</span> registered units</div></div>
      <div class="table-wrap"><div id="devices">加载中…</div></div>
    </article>
    <aside class="panel network-panel">
      <div class="panel-head"><div class="panel-title"><span class="section-no">A</span>Master Link</div><div class="panel-meta">stable</div></div>
      <div class="network-poster">
        <div class="poster-band"><span>NETWORK / LIVE</span><span>● 01s POLL</span></div>
        <div class="unit-count" id="online-count">00</div>
        <div class="unit-label" id="online-label">UNITS ONLINE</div>
        <dl class="spec-list">
          <div class="spec-row"><dt>Host</dt><dd>master.local</dd></div>
          <div class="spec-row"><dt>Socket</dt><dd>TCP : 8765</dd></div>
          <div class="spec-row"><dt>Web</dt><dd>HTTP : 8080</dd></div>
          <div class="spec-row"><dt>Identity</dt><dd>MAC / UNIQUE</dd></div>
        </dl>
        <div class="legend" aria-label="颜色图例"><span class="r"></span><span class="y"></span><span class="b"></span></div>
      </div>
    </aside>
    <article class="panel dispatch">
      <div class="panel-head"><div class="panel-title"><span class="section-no">02</span>Dispatch JSON</div><div class="panel-meta">machine-readable command line</div></div>
      <div class="dispatch-body">
        <div class="dispatch-side">
          <div><label class="field-label" for="device">Target unit</label><select id="device"></select></div>
          <div><label class="field-label">Action form</label><select id="action-name"><option value="walk">walk</option><option value="turn">turn</option><option value="jump">jump</option><option value="swing">swing</option><option value="moonwalk">moonwalk</option><option value="bend">bend</option><option value="shake_leg">shake_leg</option><option value="updown">updown</option><option value="tiptoe_swing">tiptoe_swing</option><option value="jitter">jitter</option><option value="ascending_turn">ascending_turn</option><option value="crusaito">crusaito</option><option value="flapping">flapping</option><option value="home">home</option></select></div>
          <div class="action-grid"><div><label class="field-label" for="action-steps">Steps</label><input id="action-steps" type="number" value="3" min="1" max="100"></div><div><label class="field-label" for="action-speed">Speed</label><input id="action-speed" type="number" value="1000" min="500" max="1500"></div><div><label class="field-label" for="action-amount">Amount</label><input id="action-amount" type="number" value="50" min="0" max="170"></div></div>
          <div class="action-grid"><div><label class="field-label" for="action-direction">Direction</label><input id="action-direction" type="number" value="1" min="-1" max="1"></div><div style="grid-column:span 2"><button class="primary" onclick="sendActionCommand()">Move →</button></div></div>
          <div><label class="field-label">Quick forms</label><div class="quick-grid"><button class="secondary" onclick="preset('ping')">Ping</button><button class="tertiary" onclick="preset('report')">Inspect</button><button class="tertiary" onclick="preset('actions')">Actions</button><button class="primary" onclick="preset('echo')">Echo</button><button onclick="preset('status')">Status</button><button class="primary" onclick="preset('ota')">OTA</button><button onclick="preset('stop')">Stop</button></div></div>
        </div>
        <div><label class="field-label" for="message">Payload / JSON</label><textarea id="message">{"type":"ping","id":"web-1"}</textarea><div class="send-row"><button class="primary" onclick="sendMessage()">Send command →</button><div id="result" class="hint result"></div></div><div id="live-report" class="live-report">WAITING FOR A STATE REPORT / USE INSPECT…</div></div>
      </div>
    </article>
    <article class="panel events">
      <div class="panel-head"><div class="panel-title"><span class="section-no">03</span>Signal Log</div><div class="panel-meta">inbound ← / outbound →</div></div>
      <div class="event-log"><pre id="events">加载中…</pre></div>
    </article>
  </section>
</main>
<script>
let state={};
function esc(v){return String(v).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));}
function preset(type){let id='web-'+Date.now(); if(type==='echo') $('message').value=JSON.stringify({type,id,payload:{text:'hello from WebUI',time:new Date().toISOString()}},null,2); else if(type==='report') $('message').value=JSON.stringify({type:'query_state',id},null,2); else if(type==='actions') $('message').value=JSON.stringify({type:'list_actions',id},null,2); else if(type==='ota'){ $('message').value='正在读取 master 局域网地址…'; fetch('/api/ota').then(r=>r.json()).then(info=>{$('message').value=JSON.stringify({type:'upgrade',id,url:info.url},null,2)}).catch(e=>{$('message').value=JSON.stringify({type:'upgrade',id,error:String(e)},null,2)}); } else $('message').value=JSON.stringify({type,id},null,2);}
function $(id){return document.getElementById(id)}
function sendActionCommand(){let id='web-'+Date.now();let message={type:'action',id,action:$('action-name').value,steps:Number($('action-steps').value),speed:Number($('action-speed').value),direction:Number($('action-direction').value),amount:Number($('action-amount').value)};$('message').value=JSON.stringify(message,null,2);sendMessage();}
function renderReport(events){let report=[...events].reverse().find(e=>e.direction==='in'&&e.message&&e.message.runtime);if(!report){$('live-report').textContent='WAITING FOR A STATE REPORT / USE INSPECT…';return;}let runtime=report.message.runtime||{},otto=runtime.otto||{},voice=runtime.voice||{},action=otto.action||{};let actionText=action.state==='moving'?action.name:'idle';$('live-report').innerHTML=`<div class="report-head"><span>LAST REPORT / ${esc(report.time)}</span><strong>${esc(report.device_id)}</strong></div><div class="report-grid"><div><span>Action</span><strong>${esc(actionText)}</strong></div><div><span>Motion</span><strong>${esc(action.state||'unknown')}</strong></div><div><span>Queue</span><strong>${esc(otto.queue_depth??'—')}</strong></div><div><span>Voice state</span><strong>${esc(voice.device_state||'unknown')}</strong></div><div><span>Last event</span><strong>${esc(voice.last_event||'—')}</strong></div><div><span>Audio</span><strong>${voice.voice_detected?'detected':'quiet'}</strong></div></div>`;}
async function refresh(){let r=await fetch('/api/state');state=await r.json();let ds=state.devices||[];let online=ds.filter(d=>d.online).length;$('online-count').textContent=String(online).padStart(2,'0');$('online-label').textContent=online===1?'UNIT ONLINE':'UNITS ONLINE';$('device-count').textContent=String(ds.length).padStart(2,'0');let s='<table><thead><tr><th>Unit / Identity</th><th>Link</th><th>Address</th><th>Last signal</th></tr></thead><tbody>';for(const d of ds){s+=`<tr><td><span class="device-name">${esc(d.name)}</span><span class="device-id">${esc(d.device_id)}</span></td><td><span class="status ${d.online?'online':'offline'}">${d.online?'online':'offline'}</span></td><td><span class="ip">${esc(d.ip||'—')}</span><br><span class="device-id">TCP ${esc(d.port||'—')}</span></td><td><div class="last-message"><pre>${esc(JSON.stringify(d.last_message||{},null,2))}</pre></div></td></tr>`}s+='</tbody></table>';$('devices').innerHTML=ds.length?s:'<div style="padding:22px;color:var(--muted)">NO UNITS DETECTED / WAITING FOR HELLO…</div>';let old=$('device').value;$('device').innerHTML=ds.map(d=>`<option value="${esc(d.device_id)}">${esc(d.name)} · ${esc(d.device_id)}</option>`).join('');if(ds.some(d=>d.device_id===old))$('device').value=old;$('events').textContent=(state.events||[]).map(e=>`${e.time} ${e.direction==='in'?'←':'→'} ${e.device_id}\n${JSON.stringify(e.message)}`).join('\n\n')||'暂无消息';renderReport(state.events||[]);}
async function sendMessage(){try{let message=JSON.parse($('message').value);let device_id=$('device').value;if(!device_id)throw Error('请选择在线设备');let r=await fetch('/api/send',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({device_id,message})});let out=await r.json();$('result').textContent=JSON.stringify(out);setTimeout(refresh,100);}catch(e){$('result').textContent='发送失败: '+e.message;}}
refresh();setInterval(refresh,1000);
</script></body></html>"""


class WebHandler(BaseHTTPRequestHandler):
    def _firmware_url(self) -> str:
        return f"http://{local_ip()}:{self.server.server_port}/firmware/xiaozhi.bin"

    def _reply(self, status: int, content_type: str, body: bytes) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:  # noqa: N802
        if self.path == "/" or self.path.startswith("/index.html"):
            self._reply(200, "text/html; charset=utf-8", HTML.encode())
        elif self.path == "/api/state":
            self._reply(200, "application/json; charset=utf-8", json.dumps(REGISTRY.snapshot(), ensure_ascii=False).encode())
        elif self.path == "/api/ota":
            available = FIRMWARE_PATH.is_file()
            body = {
                "available": available,
                "firmware": str(FIRMWARE_PATH),
                "size": FIRMWARE_PATH.stat().st_size if available else 0,
                "url": self._firmware_url(),
            }
            self._reply(200, "application/json; charset=utf-8", json.dumps(body).encode())
        elif self.path == "/ota/manifest.json":
            # Compatible with the stock Ota::CheckVersion() flow when the
            # device's wifi.ota_url is pointed to this local endpoint.
            body = {
                "firmware": {
                    "version": "2.0.5-local",
                    "url": self._firmware_url(),
                    "force": 1,
                }
            }
            self._reply(200, "application/json; charset=utf-8", json.dumps(body).encode())
        elif self.path == "/firmware/xiaozhi.bin":
            self._send_firmware()
        else:
            self._reply(404, "application/json", b'{"error":"not found"}')

    def _send_firmware(self) -> None:
        if not FIRMWARE_PATH.is_file():
            self._reply(404, "application/json", b'{"error":"build/xiaozhi.bin not found; run idf.py build"}')
            return
        size = FIRMWARE_PATH.stat().st_size
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(size))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        try:
            with FIRMWARE_PATH.open("rb") as firmware:
                while True:
                    chunk = firmware.read(16 * 1024)
                    if not chunk:
                        break
                    self.wfile.write(chunk)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_POST(self) -> None:  # noqa: N802
        if self.path != "/api/send":
            self._reply(404, "application/json", b'{"error":"not found"}')
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if length > 64 * 1024:
                raise ValueError("request too large")
            request = json.loads(self.rfile.read(length).decode("utf-8"))
            device_id = request["device_id"]
            message = request["message"]
            if not isinstance(message, dict):
                raise ValueError("message must be a JSON object")
            REGISTRY.send(device_id, message)
            self._reply(200, "application/json", b'{"ok":true}')
        except (ValueError, TypeError, json.JSONDecodeError, UnicodeDecodeError) as exc:
            self._reply(400, "application/json", json.dumps({"ok": False, "error": str(exc)}).encode())
        except KeyError as exc:
            self._reply(409, "application/json", json.dumps({"ok": False, "error": str(exc)}).encode())

    def log_message(self, format: str, *args: Any) -> None:
        print(f"[web] {self.address_string()} - {format % args}")


def start_mdns(tcp_port: int, advertise_ip: str) -> subprocess.Popen[str] | None:
    if os.name != "posix" or not os.path.exists("/usr/bin/dns-sd"):
        print("[mDNS] /usr/bin/dns-sd not found; master.local will not be advertised automatically")
        return None
    command = [
        "/usr/bin/dns-sd", "-P", "master", SERVICE_TYPE, "local", str(tcp_port),
        "master.local", advertise_ip, "proto=otto-master", "version=1",
    ]
    process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    print(f"[mDNS] advertising master.local -> {advertise_ip}:{tcp_port}")
    return process


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tcp-port", type=int, default=TCP_DEFAULT_PORT)
    parser.add_argument("--web-port", type=int, default=HTTP_DEFAULT_PORT)
    parser.add_argument("--advertise-ip", default=None, help="LAN IP used for master.local")
    args = parser.parse_args()

    advertise_ip = args.advertise_ip or local_ip()
    tcp_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    tcp_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    tcp_socket.bind(("0.0.0.0", args.tcp_port))
    tcp_socket.listen(16)
    print(f"[tcp] listening on 0.0.0.0:{args.tcp_port}")

    mdns_process = start_mdns(args.tcp_port, advertise_ip)
    threading.Thread(target=device_accept_loop, args=(tcp_socket,), daemon=True).start()

    web_server = ThreadingHTTPServer(("0.0.0.0", args.web_port), WebHandler)
    print(f"[web] open http://127.0.0.1:{args.web_port}")
    try:
        web_server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping Otto Master")
    finally:
        web_server.shutdown()
        web_server.server_close()
        tcp_socket.close()
        if mdns_process is not None:
            mdns_process.terminate()
            try:
                mdns_process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                mdns_process.kill()


if __name__ == "__main__":
    main()
