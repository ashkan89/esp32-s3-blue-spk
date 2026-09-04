#pragma once

#include <Arduino.h>

static const char DASHBOARD_ICON[] PROGMEM = R"ICON(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64"><defs><linearGradient id="g" x2="1" y2="1"><stop stop-color="#72f1b8"/><stop offset="1" stop-color="#58a6ff"/></linearGradient></defs><rect width="64" height="64" rx="17" fill="#10161f"/><path d="M18 25v14h8l12 9V16l-12 9h-8zm25 1c3 4 3 8 0 12m5-17c7 7 7 15 0 22" fill="none" stroke="url(#g)" stroke-width="5" stroke-linecap="round" stroke-linejoin="round"/></svg>)ICON";

static const char DASHBOARD_HTML[] PROGMEM = R"DASH(
<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover"><meta name="theme-color" content="#0b1017"><title>esp32-blue-spk</title><link rel="icon" href="/favicon.svg"><style>
:root{color-scheme:dark;--bg:#0a0f15;--panel:#111923;--panel2:#151f2b;--line:#223040;--text:#f2f7fb;--muted:#8fa0b3;--mint:#72f1b8;--blue:#65a9ff;--red:#ff6b7a;--amber:#f8c76a;--shadow:0 24px 70px #0007;--r:22px}*{box-sizing:border-box}html{scroll-behavior:smooth}body{margin:0;background:radial-gradient(800px 500px at 72% -5%,#183041 0,#0a0f1500 70%),var(--bg);color:var(--text);font:14px/1.5 Inter,ui-sans-serif,system-ui,-apple-system,"Segoe UI",sans-serif;min-height:100vh}button,input,select{font:inherit}button{color:inherit}.shell{display:grid;grid-template-columns:250px 1fr;min-height:100vh}.side{position:fixed;inset:0 auto 0 0;width:250px;padding:24px 20px;overflow-y:auto;border-right:1px solid #1a2633;background:#0b1119dd;backdrop-filter:blur(22px);z-index:20}.brand{display:flex;align-items:center;gap:12px;padding:0 8px 27px}.brandmark{width:42px;height:42px;border-radius:14px;background:linear-gradient(140deg,var(--mint),var(--blue));display:grid;place-items:center;box-shadow:0 12px 35px #61dcb844;color:#071019}.brandmark svg{width:23px}.brand b{font-size:16px;letter-spacing:.1px}.brand small{display:block;color:var(--muted);font-size:11px;margin-top:1px}.nav{display:grid;gap:3px}.nav button{border:0;background:transparent;padding:9px 13px;border-radius:13px;text-align:left;color:var(--muted);display:flex;align-items:center;gap:12px;cursor:pointer;transition:.2s}.nav button:hover{background:#151e28;color:var(--text)}.nav button.active{background:linear-gradient(100deg,#1b2b32,#172330);color:var(--mint);box-shadow:inset 0 0 0 1px #294049}.nav svg{width:19px;height:19px}.sidefoot{position:absolute;bottom:24px;left:28px;right:28px;color:var(--muted);font-size:12px}.online{display:flex;gap:8px;align-items:center}.dot{width:8px;height:8px;border-radius:99px;background:var(--muted)}.dot.good{background:var(--mint);box-shadow:0 0 13px var(--mint)}.main{grid-column:2;padding:30px 36px 70px;max-width:1500px;width:100%;margin:auto}.top{height:54px;display:flex;align-items:flex-start;justify-content:space-between;margin-bottom:24px}.eyebrow{text-transform:uppercase;letter-spacing:.17em;font-size:10px;color:var(--mint);font-weight:700}.top h1{font-size:26px;line-height:1.2;margin:4px 0 0;letter-spacing:-.7px}.topright{display:flex;gap:9px;align-items:center}.pill{border:1px solid var(--line);background:#111923aa;border-radius:99px;padding:9px 13px;color:var(--muted);display:flex;gap:8px;align-items:center;font-size:12px}.pill strong{color:var(--text);font-weight:600}.iconbtn,.btn{border:1px solid var(--line);background:#141e29;border-radius:12px;min-height:40px;padding:0 15px;cursor:pointer;transition:.18s;display:inline-flex;align-items:center;justify-content:center;gap:8px}.iconbtn{width:42px;padding:0}.iconbtn:hover,.btn:hover{transform:translateY(-1px);border-color:#3c5369;background:#192633}.btn.primary{background:linear-gradient(120deg,var(--mint),#69cde2);border:0;color:#07131b;font-weight:750;box-shadow:0 10px 28px #65d6bf2c}.btn.danger{border-color:#59323b;color:#ff9ba6;background:#241820}.btn.ghost{background:transparent}.btn:disabled{opacity:.45;cursor:not-allowed;transform:none}.page{display:none;animation:rise .3s ease}.page.active{display:block}@keyframes rise{from{opacity:0;transform:translateY(7px)}}.grid{display:grid;gap:18px}.overview{grid-template-columns:minmax(0,1.55fr) minmax(300px,.8fr)}.card{background:linear-gradient(145deg,#131c27e8,#0f1720e8);border:1px solid #1f2d3b;border-radius:var(--r);box-shadow:0 12px 40px #0002}.player{min-height:410px;padding:28px;position:relative;overflow:hidden}.player:before{content:"";position:absolute;width:330px;height:330px;border-radius:50%;right:-120px;top:-150px;background:#57bbff22;filter:blur(4px)}.nowtop{display:flex;justify-content:space-between;color:var(--muted);font-size:12px}.live{display:flex;align-items:center;gap:7px;color:var(--mint)}.live i{display:block;width:7px;height:7px;border-radius:9px;background:currentColor;box-shadow:0 0 12px currentColor}.track{margin-top:55px;position:relative}.track .label{color:var(--muted);font-size:12px}.track h2{font-size:32px;line-height:1.15;letter-spacing:-1.1px;margin:9px 0 7px;max-width:640px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.track p{margin:0;color:#aebccc;font-size:15px}.progress{margin-top:38px}.bar{height:5px;border-radius:9px;background:#263443;overflow:hidden;cursor:pointer}.bar i{height:100%;display:block;width:0;background:linear-gradient(90deg,var(--mint),var(--blue));border-radius:9px;box-shadow:0 0 12px #67dccc}.times{display:flex;justify-content:space-between;color:var(--muted);font-variant-numeric:tabular-nums;margin-top:8px;font-size:11px}.controls{display:flex;align-items:center;justify-content:center;gap:10px;margin-top:24px}.control{border:0;background:#1a2632;width:45px;height:45px;border-radius:50%;cursor:pointer;display:grid;place-items:center;transition:.18s}.control:hover{transform:scale(1.06);background:#223241}.control.mainctl{width:62px;height:62px;background:var(--text);color:#0a1118;box-shadow:0 10px 28px #0005}.control svg{width:21px;height:21px}.volume{display:grid;grid-template-columns:24px 1fr 45px;gap:11px;align-items:center;margin:24px auto 0;max-width:460px;color:var(--muted)}input[type=range]{appearance:none;width:100%;height:5px;border-radius:9px;background:#273644;outline:0}input[type=range]::-webkit-slider-thumb{appearance:none;width:17px;height:17px;border-radius:50%;background:var(--mint);box-shadow:0 0 0 5px #72f1b820}.stack{display:grid;gap:18px}.metriccard{padding:21px}.metrichead{display:flex;align-items:center;justify-content:space-between;margin-bottom:19px}.metrichead span:first-child{color:var(--muted)}.badge{font-size:10px;text-transform:uppercase;letter-spacing:.1em;padding:5px 8px;border-radius:99px;background:#26313d;color:var(--muted)}.badge.good{background:#123329;color:var(--mint)}.bigmetric{font-size:23px;font-weight:680;letter-spacing:-.5px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.submetric{color:var(--muted);font-size:12px;margin-top:5px}.meter{height:4px;background:#263441;margin-top:17px;border-radius:9px;overflow:hidden}.meter i{display:block;height:100%;background:linear-gradient(90deg,var(--mint),var(--blue));width:50%}.stats{grid-template-columns:repeat(4,1fr);margin-top:18px}.stat{padding:18px 20px}.stat .k{font-size:10px;text-transform:uppercase;letter-spacing:.11em;color:var(--muted)}.stat .v{font-size:20px;font-weight:680;margin-top:8px}.sectionhead{display:flex;align-items:end;justify-content:space-between;margin:3px 0 20px}.sectionhead h2{margin:0;font-size:22px;letter-spacing:-.5px}.sectionhead p{margin:4px 0 0;color:var(--muted)}.two{grid-template-columns:1fr 1fr}.panel{padding:24px}.panel h3{font-size:15px;margin:0 0 5px}.panel>p{color:var(--muted);margin:0 0 22px}.device{display:flex;align-items:center;gap:15px;padding:16px 0;border-top:1px solid var(--line)}.device:first-of-type{border-top:0}.avatar{width:46px;height:46px;border-radius:15px;background:#1d2b37;display:grid;place-items:center;color:var(--blue)}.avatar svg{width:22px}.deviceinfo{min-width:0;flex:1}.deviceinfo b,.deviceinfo small{display:block;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.deviceinfo small{color:var(--muted);margin-top:3px}.row{display:flex;gap:10px;align-items:center}.row.wrap{flex-wrap:wrap}.field{display:grid;gap:7px;margin:15px 0}.field label{color:#aab8c7;font-size:12px}.input{width:100%;border:1px solid var(--line);background:#0e151e;color:var(--text);padding:12px 13px;border-radius:12px;outline:0}.input:focus{border-color:#4d7f8a;box-shadow:0 0 0 3px #72f1b812}.hint{color:var(--muted);font-size:11px}.network{display:grid;grid-template-columns:1fr auto;gap:5px 12px;align-items:center;border-top:1px solid var(--line);padding:13px 2px;cursor:pointer}.network:first-child{border:0}.network:hover b{color:var(--mint)}.network small{color:var(--muted)}.signal{grid-row:1/3;grid-column:2;color:var(--muted)}.drop{border:1px dashed #35495b;border-radius:17px;padding:28px;text-align:center;background:#0c141c;transition:.2s;cursor:pointer}.drop.over{border-color:var(--mint);background:#11241f}.drop b{display:block;margin:10px}.drop span{color:var(--muted);font-size:12px}.release{padding:20px;border-radius:17px;background:linear-gradient(135deg,#172b2c,#172433);border:1px solid #294044}.release h3{margin:8px 0 4px;font-size:19px}.release p{margin:0;color:var(--muted)}.updatebar{margin:20px 0 8px;height:7px;background:#283744;border-radius:10px;overflow:hidden}.updatebar i{display:block;width:0;height:100%;background:linear-gradient(90deg,var(--mint),var(--blue));transition:width .3s}.callout{border-left:3px solid var(--amber);padding:10px 13px;background:#281f12;color:#d9c59a;border-radius:0 10px 10px 0;font-size:12px;margin:14px 0}.switch{display:flex;align-items:center;justify-content:space-between;padding:14px 0;border-top:1px solid var(--line)}.switch input{width:42px;height:23px;appearance:none;border-radius:20px;background:#344150;position:relative;transition:.2s}.switch input:after{content:"";position:absolute;width:17px;height:17px;background:white;border-radius:50%;left:3px;top:3px;transition:.2s}.switch input:checked{background:#3fbf92}.switch input:checked:after{transform:translateX(19px)}.screenpick{display:grid;grid-template-columns:repeat(4,1fr);gap:8px;margin-top:15px}.screenpick button{padding:11px 6px;border:1px solid var(--line);background:#101821;color:var(--muted);border-radius:11px;cursor:pointer}.screenpick button:disabled{cursor:default}.screenpick button.on{border-color:#2f5c52;background:#12241f;color:var(--mint)}.modepick{grid-template-columns:1fr;gap:9px}.modepick button{text-align:left;padding:12px 14px;line-height:1.35}.modepick button b{display:block;color:var(--text);font-weight:650}.modepick button.on b{color:var(--mint)}.modepick button small{display:block;font-size:11px;margin-top:2px}.modepick button:disabled:not(.on){opacity:.4}.control:disabled{opacity:.32;cursor:not-allowed}.control:disabled:hover{transform:none;background:#1a2632}.control.mainctl:disabled:hover{background:var(--text)}input[type=range]:disabled{opacity:.4}.dangerzone{border-color:#4e2931}.dangerzone h3{color:#ff9ba6}.empty{text-align:center;color:var(--muted);padding:35px 10px}.toast{position:fixed;right:25px;bottom:25px;z-index:80;background:#17222d;border:1px solid #304254;box-shadow:var(--shadow);border-radius:14px;padding:13px 16px;min-width:260px;transform:translateY(120px);opacity:0;transition:.3s}.toast.show{transform:none;opacity:1}.toast.error{border-color:#6c3540;color:#ffb1b9}.modal{position:fixed;inset:0;display:none;place-items:center;background:#05080cb8;backdrop-filter:blur(10px);z-index:100;padding:20px}.modal.show{display:grid}.dialog{width:min(430px,100%);padding:28px;background:#111a24;border:1px solid #283746;border-radius:24px;box-shadow:var(--shadow)}.dialog h2{margin:0 0 8px;font-size:22px}.dialog p{color:var(--muted);margin:0 0 20px}.loginlogo{width:57px;height:57px;border-radius:18px;background:linear-gradient(135deg,var(--mint),var(--blue));display:grid;place-items:center;color:#081119;margin-bottom:20px}.mobilebar{display:none}.spinner{width:16px;height:16px;border:2px solid #ffffff40;border-top-color:currentColor;border-radius:50%;animation:spin .7s linear infinite}@keyframes spin{to{transform:rotate(360deg)}}@media(max-width:1000px){.overview,.two{grid-template-columns:1fr}.stats{grid-template-columns:repeat(2,1fr)}}@media(max-width:720px){.shell{display:block}.side{display:none}.main{padding:22px 16px 98px}.topright .pill{display:none}.top h1{font-size:23px}.player{padding:22px;min-height:390px}.track{margin-top:44px}.track h2{font-size:26px}.stats{gap:10px}.stat{padding:15px}.mobilebar{display:grid;grid-template-columns:repeat(7,1fr);position:fixed;left:10px;right:10px;bottom:max(10px,env(safe-area-inset-bottom));z-index:40;background:#111923ee;border:1px solid #283746;border-radius:18px;box-shadow:0 15px 45px #000a;backdrop-filter:blur(18px);padding:6px}.mobilebar button{border:0;background:none;color:var(--muted);display:grid;place-items:center;gap:2px;font-size:9px;padding:7px 0;border-radius:11px}.mobilebar button.active{color:var(--mint);background:#1c2933}.mobilebar svg{width:19px}.screenpick{grid-template-columns:repeat(2,1fr)}}.badge.warn{background:#33270f;color:var(--amber)}.badge.bad{background:#331519;color:#ff9ba6}.meter.tall{height:9px;border-radius:6px}.meter i.warn{background:linear-gradient(90deg,var(--amber),#f0b15e)}.meter i.bad{background:linear-gradient(90deg,#ff6b7a,#ff9a6a)}.meter i.flat{background:#3b4a5a}.pick2{grid-template-columns:repeat(2,1fr)}.pick3{grid-template-columns:repeat(3,1fr)}.screenpick button.warn{border-color:#5c4a24;background:#241d0f;color:var(--amber)}.fxpick{grid-template-columns:repeat(3,1fr)}.fxpick button{font-size:12px;padding:11px 4px}input[type=color]{-webkit-appearance:none;appearance:none;width:100%;height:46px;padding:0;border:1px solid var(--line);border-radius:12px;background:#101821;cursor:pointer}input[type=color]::-webkit-color-swatch-wrapper{padding:6px}input[type=color]::-webkit-color-swatch{border:0;border-radius:8px}input[type=color]::-moz-color-swatch{border:0;border-radius:8px}.swatches{display:flex;flex-wrap:wrap;gap:9px;margin-top:16px}.swatches button{width:31px;height:31px;padding:0;border-radius:10px;border:1px solid #2b3947;cursor:pointer;transition:.15s}.swatches button:hover{transform:scale(1.12)}.inline{display:grid;grid-template-columns:1fr auto;gap:10px;align-items:end}.pair{display:grid;grid-template-columns:1fr 1fr;gap:12px}.kv{display:grid;grid-template-columns:1fr auto;gap:7px 14px;font-size:12px;margin-top:14px;border-top:1px solid var(--line);padding-top:13px}.kv span{color:var(--muted)}.kv b{text-align:right;font-weight:620}.mini{min-height:34px;padding:0 11px;font-size:12px;border-radius:10px}.led i{display:inline-block;width:9px;height:9px;border-radius:50%;background:#2c3a49;vertical-align:-1px;margin-right:7px}.led i.on{background:var(--mint);box-shadow:0 0 9px var(--mint)}@media(max-width:720px){.pair{grid-template-columns:1fr}.pick3{grid-template-columns:repeat(3,1fr)}}.btn svg,.iconbtn svg{width:17px;height:17px;flex:0 0 auto}.loginlogo svg{width:26px;height:26px}.brandmark svg{flex:0 0 auto}.iconbtn,.btn{white-space:nowrap;line-height:1.15}.clockpill{font-variant-numeric:tabular-nums;gap:9px}.clockpill strong{letter-spacing:.3px}.clockpill span{color:var(--muted)}.switch input{flex:0 0 42px}.switch>span{min-width:0}.timeout{display:grid;grid-template-columns:1fr auto;gap:10px;align-items:center;margin-top:14px}.timeout select{width:auto}.timeout.off,.field.off{opacity:.45;pointer-events:none}@media(max-width:720px){.topright .pill.clockpill{display:flex;padding:8px 11px}.clockpill span{display:none}}.filepick{grid-template-columns:repeat(auto-fill,minmax(88px,1fr));max-height:270px;overflow-y:auto;padding:2px 4px 2px 2px}.filepick button{padding:9px 4px;line-height:1.25}.filepick button b{display:block;color:var(--text);font-weight:650;font-size:13px}.filepick button small{display:block;font-size:10px;margin-top:1px;color:var(--muted)}.filepick button.on b,.filepick button.on small{color:var(--mint)}.dfnow{display:flex;align-items:center;justify-content:space-between;gap:12px;flex-wrap:wrap;border-top:1px solid var(--line);margin-top:16px;padding-top:14px}.dfnow .controls{margin:0;gap:8px}.dfnow .control{width:38px;height:38px}.dfnow .control.mainctl{width:48px;height:48px}

/* --- the equaliser ------------------------------------------------------ */
/* Vertical sliders, because a tone control that does not look like a tone
   control is a tone control nobody understands. The rotation is done with a
   transform rather than with `writing-mode`, which Safari still gets wrong. */
.eqbank{display:flex;gap:6px;justify-content:space-between;align-items:flex-end;padding:10px 2px 0}
.eqband{flex:1;display:flex;flex-direction:column;align-items:center;gap:8px;min-width:0}
.eqslot{height:132px;display:grid;place-items:center;width:100%}
.eqband input[type=range]{width:128px;transform:rotate(-90deg);margin:0}
.eqband .eqhz{font-size:11px;color:var(--muted);white-space:nowrap}
.eqband .eqdb{font-size:12px;font-weight:650;font-variant-numeric:tabular-nums}
.eqband .eqdb.up{color:var(--mint)}.eqband .eqdb.down{color:var(--blue)}
.eqcurve{width:100%;height:74px;margin-top:6px;border-radius:12px;background:#0d151d;border:1px solid var(--line)}

/* --- graphs ------------------------------------------------------------- */
.chart{width:100%;height:150px;border-radius:14px;background:#0d151d;border:1px solid var(--line);display:block}
.chartwrap{position:relative}
.chartwrap .axis{position:absolute;right:10px;top:8px;font-size:11px;color:var(--muted);font-variant-numeric:tabular-nums;text-align:right;line-height:1.35;pointer-events:none}
.chartempty{display:grid;place-items:center;height:150px;color:var(--muted);font-size:13px;border-radius:14px;background:#0d151d;border:1px dashed var(--line)}

/* --- lists that are rows of a thing with buttons on the right ----------- */
.rowlist{display:grid;gap:8px}
.rowitem{display:flex;align-items:center;gap:12px;padding:11px 13px;border-radius:14px;background:var(--panel2);border:1px solid var(--line)}
.rowitem.on{border-color:#2e5b52;background:linear-gradient(100deg,#14262c,#131e29)}
.rowitem .grow{flex:1;min-width:0}
.rowitem b{display:block;font-size:13.5px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.rowitem small{display:block;color:var(--muted);font-size:11px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.rowitem .acts{display:flex;gap:6px;flex-shrink:0}
.rowitem .acts .btn{padding:7px 10px;font-size:12px}
.daypick{display:flex;gap:5px;flex-wrap:wrap}
.daypick button{width:36px;padding:8px 0;border-radius:11px;border:1px solid var(--line);background:var(--panel2);color:var(--muted);cursor:pointer;font-size:11.5px;font-weight:650}
.daypick button.on{border-color:#2e5b52;background:#16303a;color:var(--mint)}
.buffbar{height:8px;border-radius:99px;background:#16202b;overflow:hidden;border:1px solid var(--line)}
.buffbar i{display:block;height:100%;background:linear-gradient(90deg,var(--blue),var(--mint));transition:width .3s}
.buffbar.warn i{background:linear-gradient(90deg,#f8c76a,#ff6b7a)}
</style></head><body>
<div class="shell"><aside class="side"><div class="brand"><div class="brandmark" data-icon="speaker"></div><div><b>esp32-blue-spk</b><small id="sideVersion">ESP32 management</small></div></div><nav class="nav" id="nav"><button class="active" data-page="overview" data-icon="home">Overview</button><button data-page="devices" data-icon="bluetooth">Devices</button><button data-page="media" data-icon="sdcard">Media</button><button data-page="radio" data-icon="radio">Radio</button><button data-page="sound" data-icon="tune">Sound</button><button data-page="alarms" data-icon="alarm">Alarms</button><button data-page="lighting" data-icon="bulb">Lighting</button><button data-page="graphs" data-icon="chart">Graphs</button><button data-page="wifi" data-icon="wifi">Wi-Fi</button><button data-page="hass" data-icon="hass">Home Assistant</button><button data-page="updates" data-icon="download">Updates</button><button data-page="settings" data-icon="settings">Settings</button></nav><div class="sidefoot"><div class="online"><i class="dot" id="sideDot"></i><span id="sideStatus">Waiting for device</span></div></div></aside>
<main class="main"><header class="top"><div><div class="eyebrow" id="eyebrow">Your audio, at a glance</div><h1 id="pageTitle">Overview</h1></div><div class="topright"><div class="pill clockpill" id="clockPill" title="Speaker clock"><i class="dot" id="clockDot"></i><span id="topDate">—</span><strong id="topClock">--:--</strong></div><div class="pill"><i class="dot" id="wifiDot"></i><strong id="topWifi">Offline</strong></div><button class="iconbtn" title="Refresh" id="refresh" data-icon="refresh"></button></div></header>
<section class="page active" id="page-overview"><div class="grid overview"><article class="card player"><div class="nowtop"><span class="live"><i></i><span id="streamLabel">Ready to play</span></span><span id="codec">44.1 kHz</span></div><div class="track"><span class="label">NOW PLAYING</span><h2 id="title">Nothing playing</h2><p id="artist">Connect a Bluetooth device to begin</p></div><div class="progress"><div class="bar"><i id="trackBar"></i></div><div class="times"><span id="position">0:00</span><span id="duration">0:00</span></div></div><div class="controls"><button class="control" data-media="previous" title="Previous" data-icon="previous"></button><button class="control" data-media="rewind" title="Rewind" data-icon="rewind"></button><button class="control mainctl" data-media="toggle" title="Play / pause" id="playButton" data-icon="play"></button><button class="control" data-media="forward" title="Fast forward" data-icon="forward"></button><button class="control" data-media="next" title="Next" data-icon="next"></button><button class="control" data-media="stop" title="Stop" data-icon="stop"></button></div><div class="volume"><button class="control" style="width:24px;height:24px;background:none" data-media="mute" title="Mute" data-icon="volume"></button><input id="volume" type="range" min="0" max="127" value="80"><span id="volumeText">63%</span></div><p class="hint" id="controlHint" style="text-align:center;margin:16px 0 0"></p></article>
<div class="stack"><article class="card metriccard" id="btCard"><div class="metrichead"><span>Bluetooth</span><span class="badge" id="btBadge">Waiting</span></div><div class="bigmetric" id="btDevice">No device</div><div class="submetric" id="btDetail">Discoverable and ready to pair</div></article><article class="card metriccard"><div class="metrichead"><span>Radio mode</span><span class="badge" id="modeBadge">—</span></div><div class="bigmetric" id="modeName">—</div><div class="submetric" id="modeDetail">One 2.4 GHz radio, three ways to share it.</div><div class="screenpick modepick"><button data-mode="0"><b>Wi-Fi only</b><small>Dashboard only · nothing can pair</small></button><button data-mode="1"><b>Bluetooth only</b><small>Audio only · no dashboard</small></button><button data-mode="2"><b>DFPlayer + Wi-Fi</b><small>Play from SD or USB · no Bluetooth at all</small></button></div><div class="hint" id="modeNote" style="margin-top:12px"></div></article><article class="card metriccard" id="dfCard" style="display:none"><div class="metrichead"><span>DFPlayer</span><span class="badge" id="dfBadge">Starting</span></div><div class="bigmetric" id="dfNow">Waiting for the module</div><div class="submetric" id="dfDetail">Audio comes from the card or a USB drive.</div><div class="screenpick pick3" id="dfQuickSource"><button data-dfsource="sd">SD card</button><button data-dfsource="usb">USB drive</button><button data-dfsource="flash">Flash</button></div><div class="hint" id="dfHint" style="margin-top:12px"></div></article><article class="card metriccard" id="radioCard" style="display:none"><div class="metrichead"><span>Internet radio</span><span class="badge" id="radioOvBadge">&mdash;</span></div><div class="bigmetric" id="radioOvName">&mdash;</div><div class="submetric" id="radioOvDetail">&nbsp;</div><div class="buffbar" id="radioOvBufWrap" style="margin-top:12px"><i id="radioOvBuf" style="width:0"></i></div><div class="row wrap" style="margin-top:14px"><button class="btn" data-page="radio">Open the station list</button></div></article><article class="card metriccard" id="alarmCard" style="display:none"><div class="metrichead"><span>Alarm</span><span class="badge" id="alarmOvBadge">&mdash;</span></div><div class="bigmetric" id="alarmOvBig">&mdash;</div><div class="submetric" id="alarmOvDetail">&nbsp;</div><div class="row wrap" style="margin-top:14px"><button class="btn primary" id="alarmOvStop" style="display:none">Stop</button><button class="btn" id="alarmOvSnooze" style="display:none">Snooze</button><button class="btn ghost" id="alarmOvSleepCancel" style="display:none">Cancel sleep timer</button><button class="btn ghost" data-page="alarms">Alarms &amp; sleep</button></div></article><article class="card metriccard" id="batteryCard" style="display:none"><div class="metrichead"><span>Battery</span><span class="badge" id="batBadge">—</span></div><div class="bigmetric" id="batPercent">—</div><div class="submetric" id="batDetail">Cell voltage and charge state.</div><div class="meter tall"><i id="batMeter"></i></div><div class="hint" id="batHint" style="margin-top:12px"></div></article><article class="card metriccard"><div class="metrichead"><span>Network</span><span class="badge" id="wifiBadge">Setup AP</span></div><div class="bigmetric" id="wifiName">—</div><div class="submetric" id="wifiDetail">Open Wi-Fi settings to connect</div><div class="meter"><i id="signalMeter"></i></div></article><article class="card metriccard"><div class="metrichead"><span>Firmware</span><span class="badge good" id="updateBadge">Current</span></div><div class="bigmetric" id="firmwareVersion">—</div><div class="submetric" id="firmwareDetail">A/B update protection enabled</div><div class="row wrap" id="overviewInstallRow" style="display:none;margin-top:16px"><button class="btn primary" id="overviewInstall" data-icon="download">Install update</button></div></article></div></div>
<div class="grid stats"><article class="card stat"><div class="k">Uptime</div><div class="v" id="uptime">—</div></article><article class="card stat"><div class="k">Free memory</div><div class="v" id="heap">—</div></article><article class="card stat"><div class="k">Wi-Fi signal</div><div class="v" id="rssi">—</div></article><article class="card stat"><div class="k">AP clients</div><div class="v" id="apClients">0</div></article></div></section>
<section class="page" id="page-devices"><div class="sectionhead"><div><h2>Bluetooth devices</h2><p>The active phone and devices this speaker remembers.</p></div><button class="btn" id="reloadDevices" data-icon="refresh">Refresh</button></div><div class="grid two"><article class="card panel"><h3>Connected now</h3><p>A2DP supports one active audio source at a time.</p><div id="currentDevice" class="empty">No phone connected</div><div id="btHeldOff" class="callout" style="display:none">The Bluetooth stack is not running in this mode, so nothing can pair. Switch to <b>Bluetooth only</b> under <b>Radio mode</b> on the Overview page. It takes Wi-Fi away entirely, so this dashboard goes with it — hold BOOT on the speaker to cycle back.</div></article><article class="card panel"><h3>Paired devices</h3><p>Forgetting a device removes its bond keys and requires pairing again.</p><div id="pairedDevices"><div class="empty">Loading devices…</div></div></article></div></section>
<section class="page" id="page-wifi"><div class="sectionhead"><div><h2>Wi-Fi management</h2><p>Connect the dashboard to your network or use its private setup hotspot.</p></div><button class="btn primary" id="scanWifi" data-icon="scan">Scan networks</button></div><div class="grid two"><article class="card panel"><h3>Nearby networks</h3><p>A scan can briefly share radio time with Bluetooth audio.</p><div id="networks"><div class="empty">Select “Scan networks” to discover access points.</div></div></article><article class="card panel"><h3>Join a network</h3><p>Credentials are stored in device flash and applied after restart.</p><form id="wifiForm"><div class="field"><label>Network name (SSID)</label><input class="input" id="wifiSsid" maxlength="32" required placeholder="Choose or type a network"></div><div class="field"><label>Password</label><input class="input" id="wifiPassword" type="password" placeholder="Leave empty for an open network"></div><button class="btn primary" type="submit">Save & restart</button></form><div class="callout">If the connection fails, the setup hotspot returns automatically after 15 seconds.</div></article></div></section>
<section class="page" id="page-updates"><div class="sectionhead"><div><h2>Firmware updates</h2><p>Safe A/B installation from GitHub Releases or a local firmware file.</p></div><button class="btn" id="checkUpdate" data-icon="refresh">Check GitHub</button></div><div class="grid two"><article class="card panel"><div class="release"><div class="eyebrow">LATEST RELEASE</div><h3 id="releaseName">Not checked yet</h3><p id="releaseAsset">Configure a GitHub repository in Settings.</p><div class="updatebar"><i id="updateProgress"></i></div><p id="updateMessage">Ready</p></div><div class="row wrap" style="margin-top:18px"><button class="btn primary" id="installUpdate" disabled data-icon="download">Install update</button><a class="btn ghost" id="releaseLink" href="#" target="_blank" rel="noopener" style="text-decoration:none;display:none">Release notes</a></div><div class="callout">Playback pauses during installation. Power loss is safe because the running firmware is never overwritten.</div></article><article class="card panel"><h3>Upload firmware</h3><p>Use the <code>firmware.bin</code> produced by the <code>esp32dev</code> build.</p><input type="file" id="firmwareFile" accept=".bin,application/octet-stream" hidden><div class="drop" id="dropzone" tabindex="0"><div style="font-size:27px">⇧</div><b>Drop firmware.bin here</b><span>or click to choose a file</span></div><div class="row" style="margin-top:17px"><button class="btn primary" id="uploadFirmware" disabled>Upload & install</button><span class="hint" id="fileName">No file selected</span></div></article></div></section>
<section class="page" id="page-media"><div class="sectionhead"><div><h2>DFPlayer library</h2><p>The card, the USB drive and every control the module exposes.</p></div><button class="btn" id="dfRefresh" data-icon="refresh">Re-read media</button></div><div id="dfOffline" class="callout" style="display:none">The DFPlayer only runs in <b>DFPlayer + Wi-Fi</b> mode. Pick it under <b>Radio mode</b> on the Overview page: the speaker restarts, both Bluetooth radios stay off, and audio comes from the module’s own card or USB drive instead of over the air. Wi-Fi and this dashboard work exactly as they do now, including the setup hotspot when no network is saved.</div><div id="dfPanels" class="grid two" style="display:none"><div class="stack"><article class="card panel"><h3>Library</h3><p>The module has no directory listing — its protocol answers in counts and indices and never in names. The closest thing is asking each of the 99 folders how many files it holds, which is what a scan does. It takes about twelve seconds, and the answer is kept until the card or the source changes.</p><div class="row wrap"><button class="btn" id="dfScan" data-icon="refresh">Scan library</button><span class="hint" id="dfScanState">Not scanned yet</span></div><div class="updatebar" id="dfScanBarWrap" style="display:none"><i id="dfScanBar"></i></div><div class="field"><label>Folders</label><div class="screenpick filepick" id="dfFolderGrid"></div><span class="hint" id="dfFolderEmpty">Scan the library and the folders that hold files appear here.</span></div><div class="field" id="dfTrackPane" style="display:none;margin-bottom:0"><label id="dfTrackLabel">Tracks</label><div class="screenpick filepick" id="dfTracks"></div><span class="hint">Track numbers are positions in the folder, so <code>/01/003.mp3</code> is folder 01, track 3.</span></div><div class="dfnow"><div><b id="dfNowLine">Nothing playing</b><small class="hint" style="display:block" id="dfNowSub">&nbsp;</small></div><div class="controls"><button class="control" data-media="previous" title="Previous" data-icon="previous"></button><button class="control mainctl" id="dfToggle" data-media="toggle" title="Play / pause" data-icon="play"></button><button class="control" data-media="next" title="Next" data-icon="next"></button><button class="control" data-media="stop" title="Stop" data-icon="stop"></button></div></div></article><article class="card panel"><h3>Source</h3><p>Which library the module plays from. Switching re-reads the file count.</p><div class="screenpick pick2" id="dfSourcePick"><button data-dfsrc="sd">SD card</button><button data-dfsrc="usb">USB drive</button><button data-dfsrc="flash">On-board flash</button><button data-dfsrc="aux">AUX input</button></div><div class="kv"><span>SD card</span><b id="dfSdState">—</b><span>USB drive</span><b id="dfUsbState">—</b><span>On-board flash</span><b id="dfFlashState">—</b><span>Files on this source</span><b id="dfFiles">—</b><span>Folders on the card</span><b id="dfFolders">—</b></div><div id="dfPc" class="callout" style="display:none">A computer is plugged into the module’s USB port, so the microSD card is mounted over there as a drive. Copy your files, eject it, and unplug the cable — playback from the card resumes when you do.</div></article><article class="card panel"><h3>Play something</h3><p>The module works in numbers, not filenames: its protocol reports counts and indices and never a name. Folder mode is the reliable one — the flat index follows FAT directory order, which changes when you re-copy the card.</p><div class="field"><label>Folder and track — <code>/01/003.mp3</code></label><div class="row"><input class="input" id="dfFolderNo" type="number" min="1" max="99" value="1" style="max-width:96px"><input class="input" id="dfFileNo" type="number" min="1" max="255" value="1" style="max-width:110px"><button class="btn primary" id="dfPlayFolder">Play</button></div><span class="hint" id="dfFolderCount">Enter a folder to see how many tracks it holds.</span></div><div class="field"><label>Track number, flat index over the whole source</label><div class="inline"><input class="input" id="dfTrackNo" type="number" min="1" max="2999" value="1"><button class="btn" id="dfPlayTrack">Play</button></div></div><div class="field"><label>MP3 folder — <code>/MP3/0007.mp3</code></label><div class="inline"><input class="input" id="dfMp3No" type="number" min="1" max="3000" value="1"><button class="btn" id="dfPlayMp3">Play</button></div></div><div class="field"><label>Announcement — <code>/ADVERT/0001.mp3</code></label><div class="row"><input class="input" id="dfAdvertNo" type="number" min="1" max="3000" value="1"><button class="btn" id="dfAdvert">Interrupt</button><button class="btn ghost" id="dfAdvertStop">Resume</button></div><span class="hint">Plays over the current track and returns to it. Something has to be playing for the module to accept it.</span></div></article></div><div class="stack"><article class="card panel"><h3>Sound</h3><p>All of it lives in the module and none of it survives a power cut, which is what the startup defaults in Settings are for.</p><div class="field"><label>Module volume <span id="dfVolText">—</span></label><div class="row"><input type="range" id="dfVolume" min="0" max="30" value="20"><button class="btn mini" id="dfVolDown">−</button><button class="btn mini" id="dfVolUp">+</button></div><span class="hint">The module has 31 steps. The Overview slider is the same setting on a 0–100% scale.</span></div><div class="field"><label>Equaliser</label><div class="screenpick pick3" id="dfEqPick"><button data-dfeq="0">Normal</button><button data-dfeq="1">Pop</button><button data-dfeq="2">Rock</button><button data-dfeq="3">Jazz</button><button data-dfeq="4">Classic</button><button data-dfeq="5">Bass</button></div></div><div class="field"><label>Repeat</label><div class="screenpick pick3" id="dfLoopPick"><button data-dfloop="off">Off</button><button data-dfloop="track">One track</button><button data-dfloop="folder">Folder</button><button data-dfloop="all">Everything</button><button data-dfloop="random">Shuffle</button></div><span class="hint">Folder repeat uses the folder number above. Shuffle starts playing immediately.</span></div><label class="switch"><span><b>Module output enabled</b><small class="hint" style="display:block">The module’s own DAC mute. Silences it at the source without stopping playback.</small></span><input type="checkbox" id="dfDac" checked></label></article><article class="card panel"><h3>Hardware pins</h3><p>The module’s own button and ADC-key inputs, pressed from here by pulling them to ground. Useful when the serial link is suspect: these work even if it is not.</p><div class="screenpick pick2" id="dfPins"><button data-dfpin="io1">IO1 — previous</button><button data-dfpin="io1" data-dflong="1">IO1 held — volume down</button><button data-dfpin="io2">IO2 — next</button><button data-dfpin="io2" data-dflong="1">IO2 held — volume up</button><button data-dfpin="adkey1">ADKEY1 — track 1</button><button data-dfpin="adkey2">ADKEY2 — track 11</button></div><div class="kv"><span>BUSY pin</span><b id="dfBusyPin">—</b></div><div class="field" style="margin-bottom:0"><label>Status LED</label><div class="screenpick" id="dfLedPick"><button data-dfled="auto">Follow BUSY</button><button data-dfled="on">On</button><button data-dfled="off">Off</button><button data-dfled="blink">Blink</button></div><span class="hint led"><i id="dfLedDot"></i><span id="dfLedState">—</span></span></div></article><article class="card panel"><h3>Module</h3><div class="kv" style="border:0;padding-top:0"><span>Serial link</span><b id="dfLink">—</b><span>Firmware version</span><b id="dfVersion">—</b><span>Current track</span><b id="dfTrackNow">—</b><span>Tracks finished this boot</span><b id="dfFinished">—</b><span>Last error</span><b id="dfError">None</b></div><div class="row wrap" style="margin-top:17px"><button class="btn" id="dfReset">Reset module</button><button class="btn ghost" id="dfStandby">Standby</button><button class="btn ghost" id="dfWake">Wake</button><button class="btn primary" id="dfSaveDefaults">Save as startup defaults</button></div><div class="callout">A reset re-runs the whole start-up sequence and is the way out of a module that has stopped answering. Standby powers its decoder down — worth about 20 mA, which matters on a battery.</div></article></div></div></section><section class="page" id="page-lighting"><div class="sectionhead"><div><h2>WS2812 ring</h2><p>Fifteen effects and a music sync driven by the same analysis the spectrum screen draws. <span id="ledPins"></span>.</p></div><label class="switch" style="border:0;padding:0"><span><b>Ring on</b></span><input type="checkbox" id="ledEnabled"></label></div><article class="card panel" id="ledUnwired" style="display:none"><h3>No ring on this build</h3><p>This firmware was built with the lighting switched off, or with no data pin (<b>LEDS_ENABLED</b> is 0, or <b>PIN_LEDS</b> is -1). Set both in <b>src/hw_config.h</b> and reflash; nothing else on the speaker changes.</p></article><div id="ledBody"><div class="grid two"><div class="stack"><article class="card panel"><h3>Effect</h3><p id="ledHint">&nbsp;</p><div class="screenpick fxpick" id="ledEffects"></div></article><article class="card panel"><h3>Colour</h3><p>The primary colour is what most effects are built from. The secondary is the other end of the gradients, the second spark in the twinkle, and the background of the chase. The effects that generate their own colours &mdash; rainbow, spectrum, colour cycle &mdash; ignore both, and fire borrows the primary&rsquo;s hue so you can have a blue one.</p><div class="pair"><div class="field"><label>Primary</label><input type="color" id="ledColor" value="#00E0FF"></div><div class="field"><label>Secondary</label><input type="color" id="ledColor2" value="#FF0080"></div></div><div class="swatches" id="ledSwatchRow"></div></article></div><div class="stack"><article class="card panel"><h3>Levels</h3><div class="field"><label>Brightness <span id="ledBrightValue">&mdash;</span></label><input type="range" id="ledBrightness" min="0" max="255" value="160"><span class="hint">Capped again in firmware by <b>LED_BRIGHTNESS_MAX</b>, which is the current budget: seven pixels at full white draw about 420 mA.</span></div><div class="field"><label>Speed <span id="ledSpeedValue">&mdash;</span></label><input type="range" id="ledSpeed" min="0" max="255" value="128"></div><div class="field"><label>Music reaction <span id="ledReactValue">&mdash;</span></label><input type="range" id="ledReactivity" min="0" max="100" value="55"><span class="hint">How much the music is allowed to show. At 0 every effect runs exactly as it would in silence, which is what you want from a lamp; at 100 a quiet passage is genuinely dim. It rides on top of <i>every</i> effect, not just the reactive ones.</span></div></article><article class="card panel"><h3>Music sync</h3><p id="ledAudio">&nbsp;</p><p class="hint">Changes here apply within a frame and are saved once you stop moving the controls, so there is nothing to press.</p></article><article class="card panel"><h3>Resting</h3><p>The ring can put itself out when the speaker is not being used, and come straight back on the first note or the first change made here. This is not the same as switching it off above: the effect and the colours are kept, and the ring simply waits.</p><label class="switch" style="border-top:0;padding-top:0"><span><b>Rest when idle</b><small class="hint" style="display:block">Off keeps the ring lit for as long as the speaker is powered.</small></span><input type="checkbox" id="ledIdleOff"></label><div class="timeout" id="ledIdleRow"><label for="ledIdleAfter" style="color:#aab8c7;font-size:12px">Rest after</label><select class="input" id="ledIdleAfter"></select></div><span class="hint" id="ledIdleState" style="display:block;margin-top:12px"></span></article></div></div></div></section>
<section class="page" id="page-radio"><div class="sectionhead"><div><h2>Internet radio</h2><p>Stations from the network, decoded on the speaker itself.</p></div><button class="btn" id="radioRefresh" data-icon="refresh">Refresh</button></div>
<div id="radioOffline" class="callout" style="display:none">Internet radio needs Wi-Fi, and <b>Bluetooth only</b> mode never starts it &mdash; one antenna, one radio. Pick <b>Wi-Fi only</b> or <b>DFPlayer + Wi-Fi</b> under <b>Radio mode</b> on the Overview page and the station list below comes back.</div>
<div id="radioPanels" class="grid two"><div class="stack">
<article class="card panel"><h3>Now playing</h3>
<div class="dfnow" style="border-top:0;margin-top:0;padding-top:0"><div class="grow" style="min-width:0"><b id="radioNow" style="font-size:15px">Nothing playing</b><small class="hint" style="display:block" id="radioNowSub">&nbsp;</small></div><div class="controls"><button class="control" id="radioPrev" title="Previous station" data-icon="previous"></button><button class="control mainctl" id="radioToggle" title="Play / stop" data-icon="play"></button><button class="control" id="radioNext" title="Next station" data-icon="next"></button></div></div>
<div class="field" style="margin-top:16px"><label>Buffer <span id="radioBufText">&mdash;</span></label><div class="buffbar" id="radioBufWrap"><i id="radioBuf" style="width:0"></i></div><span class="hint" id="radioBufHint">The jitter buffer holds about a second and a half of a 128&nbsp;kbps stream. Playback starts once it is 55% full, and the number falling towards zero is the connection failing to keep up rather than the speaker.</span></div>
<div class="kv"><span>Status</span><b id="radioState">&mdash;</b><span>Stream</span><b id="radioFormat">&mdash;</b><span>Underruns</span><b id="radioUnder">0</b><span>Reconnects</span><b id="radioRecon">0</b><span>Received</span><b id="radioBytes">&mdash;</b></div>
<div class="field" style="margin-top:16px;margin-bottom:0"><label>Volume <span id="radioVolText">&mdash;</span></label><input type="range" id="radioVolume" min="0" max="127" value="90"></div>
<label class="switch" style="margin-top:14px"><span><b>Start playing at boot</b><small class="hint" style="display:block">The speaker resumes the last station after a power cut. Set automatically whenever you press play.</small></span><input type="checkbox" id="radioAutostart"></label>
</article>
<article class="card panel"><h3>Play an address</h3><p>For trying a station before you keep it. It has to be the stream itself &mdash; a <code>.mp3</code>, <code>.aac</code> or Icecast mount point &mdash; not the page you found it on.</p><div class="row"><input class="input" id="radioUrl" placeholder="http://ice1.somafm.com/groovesalad-128-mp3"><button class="btn primary" id="radioPlayUrl">Play</button></div><span class="hint">MP3 and AAC only &mdash; Ogg and Opus have no decoder that fits in the memory this chip has left once Wi-Fi has taken its share. <b>Prefer <code>http://</code> over <code>https://</code></b>: TLS needs about 45&nbsp;kB more than this board has spare while the dashboard is up, and most stations serve both.</span></article>
</div>
<div class="stack"><article class="card panel"><h3>Favourites</h3><p>Up to twelve. The first one is what an alarm set to &ldquo;radio&rdquo; plays unless you pick another.</p><div class="rowlist" id="radioList"></div><span class="hint" id="radioListEmpty" style="display:none">No stations yet. Add one below.</span>
<h3 style="margin-top:24px" id="radioEditTitle">Add a station</h3><div class="field"><label>Name</label><input class="input" id="radioName" maxlength="39" placeholder="SomaFM Groove Salad"></div><div class="field"><label>Stream address</label><input class="input" id="radioEditUrl" maxlength="159" placeholder="http://&hellip;"></div><div class="row wrap"><button class="btn primary" id="radioSave">Save station</button><button class="btn ghost" id="radioCancel" style="display:none">Cancel</button></div></article></div></div></section>

<section class="page" id="page-sound"><div class="sectionhead"><div><h2>Sound</h2><p>The tone stack, the output level and what the speaker says out loud.</p></div><button class="btn" id="soundRefresh" data-icon="refresh">Refresh</button></div>
<div class="grid two"><div class="stack">
<article class="card panel"><h3>Equaliser</h3><p>Five biquad sections per channel, in the sample path of whatever is playing. The two ends are shelves and the three in the middle are peaking filters &mdash; the arrangement a physical tone stack uses, and for the same reason.</p>
<div id="eqHardware" class="callout" style="display:none">This mode plays through the DFPlayer, which has its own equaliser and its own audio output &mdash; the samples never pass through this chip at all. The curve you pick here is mapped to the module&rsquo;s nearest hardware preset instead, and the sliders are a preview of the shape rather than what is being applied.</div>
<label class="switch"><span><b>Equaliser on</b><small class="hint" style="display:block" id="eqActiveHint">&nbsp;</small></span><input type="checkbox" id="eqEnabled"></label>
<div class="field" style="margin-top:14px"><label>Preset</label><div class="screenpick pick3" id="eqPresets"></div></div>
<div class="eqbank" id="eqBank"></div>
<canvas class="eqcurve" id="eqCurve" width="600" height="150"></canvas>
<div class="field" style="margin-top:16px"><label>Preamp <span id="eqPreampText">0 dB</span></label><input type="range" id="eqPreamp" min="-12" max="6" value="0"></div>
<label class="switch"><span><b>Give headroom back automatically</b><small class="hint" style="display:block">Pulls the whole signal down by however much the largest boost adds, so a boosted band arrives without the limiter working. Turn it off for loudness and let the soft knee round off what goes over.</small></span><input type="checkbox" id="eqAuto"></label>
<div class="row wrap" style="margin-top:16px"><button class="btn primary" id="eqSave">Save</button><button class="btn ghost" id="eqFlat">Flatten</button></div></article>

<article class="card panel"><h3>Output</h3><p>The same controls as the Overview page, aimed at whichever source this mode is running.</p><div class="dfnow" style="border-top:0;margin-top:0;padding-top:0"><div class="grow"><b id="soundNow">&mdash;</b><small class="hint" style="display:block" id="soundNowSub">&nbsp;</small></div><div class="controls"><button class="control" data-media="previous" title="Previous" data-icon="previous"></button><button class="control mainctl" data-media="toggle" title="Play / pause" data-icon="play"></button><button class="control" data-media="next" title="Next" data-icon="next"></button><button class="control" data-media="stop" title="Stop" data-icon="stop"></button></div></div>
<div class="volume" style="margin-top:16px"><button class="control" style="width:24px;height:24px;background:none" data-media="mute" title="Mute" data-icon="volume"></button><input id="soundVolume" type="range" min="0" max="127" value="80"><span id="soundVolumeText">&mdash;</span></div></article>
</div>

<div class="stack"><article class="card panel"><h3>Spoken announcements</h3><p>Recorded speech held in flash, mixed over the music with the music ducked underneath. There is no synthesiser on the chip: everything the speaker can say was written into <code>scripts/voice_phrases.txt</code> and rendered at build time, which is also how you add your own.</p>
<label class="switch"><span><b>Announcements on</b></span><input type="checkbox" id="voiceEnabled"></label>
<div class="field" style="margin-top:14px"><label>Announcement volume <span id="voiceVolText">&mdash;</span></label><input type="range" id="voiceVolume" min="0" max="100" value="70"></div>
<div class="field"><label>Duck the music by <span id="voiceDuckText">&mdash;</span></label><input type="range" id="voiceDuck" min="0" max="100" value="75"><span class="hint">How far the music drops while the speaker is talking. The change ramps over about 60&nbsp;ms in and 170&nbsp;ms out, so it reads as somebody turning the music down rather than as a fault.</span></div>
<div class="field"><label>What may speak</label><div class="rowlist" id="voiceCats"></div></div>
<div class="row wrap"><button class="btn primary" id="voiceSave">Save</button></div>
<h3 style="margin-top:24px">Announce a device by name</h3><p>The speaker cannot read out a name it has never been given. Add the phrase to <code>scripts/voice_phrases.txt</code>, run <code>python scripts/make_voice_clips.py</code>, rebuild &mdash; then point a phone&rsquo;s Bluetooth address at it here. One per line, <code>address = phrase</code>.</p>
<div class="field"><label>Mappings</label><textarea class="input" id="voiceDevices" rows="3" style="font-family:ui-monospace,monospace;font-size:12px" placeholder="a4:83:e7:11:22:33 = dev_phone"></textarea><span class="hint" id="voiceDevHint">Addresses are on the Devices page. Anything unmapped gets the generic &ldquo;Device connected&rdquo;.</span></div>
<h3 style="margin-top:24px">Everything it can say</h3><p>Press one to hear it.</p><div class="rowlist" id="voiceClips"></div></article></div></div></section>

<section class="page" id="page-alarms"><div class="sectionhead"><div><h2>Alarms &amp; sleep</h2><p>Wake to the radio, the card or the chime. Fall asleep to any of them.</p></div><button class="btn" id="alarmRefresh" data-icon="refresh">Refresh</button></div>
<div class="grid two"><div class="stack">
<article class="card panel"><h3>Alarms</h3><p id="alarmNextLine" class="hint" style="margin-bottom:14px">&mdash;</p><div class="rowlist" id="alarmList"></div><span class="hint" id="alarmEmpty" style="display:none">No alarms yet.</span><div class="row wrap" style="margin-top:16px"><button class="btn primary" id="alarmAdd">Add an alarm</button><button class="btn ghost" id="alarmDismiss" style="display:none">Stop the alarm</button><button class="btn ghost" id="alarmSnooze" style="display:none">Snooze</button></div></article>

<article class="card panel" id="alarmEditor" style="display:none"><h3 id="alarmEditTitle">Edit alarm</h3>
<div class="row wrap"><div class="field" style="flex:1;min-width:150px"><label>Time</label><input class="input" id="alarmTime" type="time" value="07:00"></div><div class="field" style="flex:2;min-width:200px"><label>Label</label><input class="input" id="alarmLabel" maxlength="23" placeholder="Weekdays"></div></div>
<div class="field"><label>Days</label><div class="daypick" id="alarmDays"></div><span class="hint">With none selected it rings once, at the next time it comes round, and then switches itself off.</span></div>
<div class="field"><label>Wake me with</label><div class="screenpick pick3" id="alarmSource"><button data-src="0">Chime</button><button data-src="1">Internet radio</button><button data-src="2">Card folder</button></div><span class="hint" id="alarmSourceHint">&nbsp;</span></div>
<div class="field" id="alarmTargetRow" style="display:none"><label id="alarmTargetLabel">Station</label><select class="input" id="alarmTarget"></select></div>
<div class="row wrap"><div class="field" style="flex:1;min-width:180px"><label>Volume <span id="alarmVolText">&mdash;</span></label><input type="range" id="alarmVolume" min="0" max="127" value="90"></div><div class="field" style="flex:1;min-width:180px"><label>Fade up over <span id="alarmFadeText">&mdash;</span></label><input type="range" id="alarmFade" min="0" max="600" step="15" value="60"></div></div>
<div class="row wrap"><div class="field" style="flex:1;min-width:180px"><label>Stop after</label><select class="input" id="alarmDuration"><option value="300">5 minutes</option><option value="600">10 minutes</option><option value="1800" selected>30 minutes</option><option value="3600">1 hour</option></select></div><div class="field" style="flex:1;min-width:180px"><label>Snooze for</label><select class="input" id="alarmSnoozeMins"><option value="5">5 minutes</option><option value="9" selected>9 minutes</option><option value="15">15 minutes</option><option value="20">20 minutes</option></select></div></div>
<label class="switch"><span><b>Skip the next one</b><small class="hint" style="display:block">Sits out tomorrow and then carries on as normal. Cleared automatically once that occurrence has passed.</small></span><input type="checkbox" id="alarmSkip"></label>
<div class="row wrap" style="margin-top:16px"><button class="btn primary" id="alarmSave">Save alarm</button><button class="btn ghost" id="alarmTest">Test it now</button><button class="btn ghost" id="alarmCancel">Cancel</button></div>
<div class="callout" style="margin-top:16px">If the station will not connect, the chime takes over after twenty seconds. An alarm that depends on the internet has to have somewhere to fall back to, or &ldquo;the router rebooted overnight&rdquo; and &ldquo;you are late&rdquo; become the same event.</div></article>
</div>

<div class="stack"><article class="card panel"><h3>Sleep timer</h3><p>Plays for as long as you say, fades out over the last minute, and stops. It works with whatever is playing &mdash; the radio, the card or a phone.</p>
<div id="sleepRunning" style="display:none"><div class="bigmetric" id="sleepLeft" style="font-size:34px">&mdash;</div><div class="submetric" id="sleepSub">&nbsp;</div><div class="meter tall" style="margin-top:12px"><i id="sleepMeter" style="width:0"></i></div><div class="row wrap" style="margin-top:16px"><button class="btn" id="sleepPlus">+15 minutes</button><button class="btn ghost" id="sleepCancel">Cancel</button></div></div>
<div id="sleepIdle"><div class="screenpick pick3" id="sleepPresets"><button data-min="15">15 min</button><button data-min="30">30 min</button><button data-min="45">45 min</button><button data-min="60">1 hour</button><button data-min="90">1&frac12; hours</button><button data-min="120">2 hours</button></div>
<label class="switch" style="margin-top:14px"><span><b>Go to standby at the end</b><small class="hint" style="display:block">Not just stop the music &mdash; power the speaker down properly, which is what you want overnight on a battery. It cannot be woken over the network from there.</small></span><input type="checkbox" id="sleepStandby"></label>
<div class="row" style="margin-top:14px"><input class="input" id="sleepCustom" type="number" min="1" max="600" placeholder="Minutes" style="max-width:130px"><button class="btn primary" id="sleepStart">Start</button></div></div></article>

<article class="card panel"><h3>Time zone</h3><p>An alarm that fires an hour late on the last Sunday in March is not a clock that drifted &mdash; it is an alarm that failed. Picking a zone here installs its daylight-saving rule, and the speaker follows the changes on its own.</p><div class="field"><label>Zone</label><select class="input" id="clockZoneRule"></select></div><div class="kv"><span>In force now</span><b id="clockZoneNow">&mdash;</b><span>Daylight saving</span><b id="clockZoneDst">&mdash;</b><span>Offset from UTC</span><b id="clockZoneOffset">&mdash;</b><span>Clock source</span><b id="clockZoneSource">&mdash;</b></div><span class="hint">There is no time-zone database on the speaker and there is not going to be one &mdash; it is 700&nbsp;kB and it goes stale. The list above lives in this page, so a firmware update refreshes it.</span></article></div></div></section>

<section class="page" id="page-graphs"><div class="sectionhead"><div><h2>Graphs</h2><p>Two hours of history, sampled once a minute.</p></div><button class="btn" id="graphRefresh" data-icon="refresh">Refresh</button></div>
<div class="grid stats"><article class="card stat"><div class="k">Uptime</div><div class="v" id="gUptime">&mdash;</div></article><article class="card stat"><div class="k">Total runtime</div><div class="v" id="gRuntime">&mdash;</div></article><article class="card stat"><div class="k">Boots</div><div class="v" id="gBoots">&mdash;</div></article><article class="card stat"><div class="k">Largest free block</div><div class="v" id="gBlock">&mdash;</div></article></div>
<div class="grid two" style="margin-top:20px"><article class="card panel"><h3>Battery voltage</h3><p id="gVoltsNow">&mdash;</p><div class="chartwrap"><canvas class="chart" id="gVoltsChart"></canvas><div class="axis" id="gVoltsAxis"></div></div><span class="hint">The shaded stretches are where something was playing, which is usually the explanation for a curve that suddenly steepens.</span></article>
<article class="card panel"><h3>Chip temperature</h3><p id="gTempNow">&mdash;</p><div class="chartwrap"><canvas class="chart" id="gTempChart"></canvas><div class="axis" id="gTempAxis"></div></div><span class="hint">This is the die, not the room. On the original ESP32 silicon it is an undocumented sensor with a coarse step and a per-chip offset nobody calibrated, and it reads well above ambient after the radio has been busy. Useful as a trend &mdash; a speaker cooking in the sun shows up clearly &mdash; and not as a thermometer. Nothing in the firmware decides anything from it.</span></article>
<article class="card panel"><h3>Free memory</h3><p id="gHeapNow">&mdash;</p><div class="chartwrap"><canvas class="chart" id="gHeapChart"></canvas><div class="axis" id="gHeapAxis"></div></div><span class="hint">The one number here that is only ever diagnostic as a slope. A line drifting steadily downwards is a leak, and it is the difference between a mysterious reboot tonight and a known cause.</span></article>
<article class="card panel"><h3>Wi-Fi signal</h3><p id="gRssiNow">&mdash;</p><div class="chartwrap"><canvas class="chart" id="gRssiChart"></canvas><div class="axis" id="gRssiAxis"></div></div><span class="hint">Below about &minus;75&nbsp;dBm an internet radio stream starts running out of buffer before it runs out of bandwidth.</span></article></div></section>

<section class="page" id="page-hass"><div class="sectionhead"><div><h2>Home Assistant</h2><p>MQTT, with the discovery documents that make the speaker appear by itself.</p></div><button class="btn" id="mqttRefresh" data-icon="refresh">Refresh</button></div>
<div id="mqttOffline" class="callout" style="display:none">MQTT is TCP, so it needs Wi-Fi. <b>Bluetooth only</b> mode never starts the network &mdash; switch to <b>Wi-Fi only</b> or <b>DFPlayer + Wi-Fi</b> on the Overview page.</div>
<div class="grid two"><div class="stack"><article class="card panel"><h3>Broker</h3><p>The machine running Mosquitto, which is usually the one running Home Assistant itself.</p>
<label class="switch"><span><b>Publish to MQTT</b><small class="hint" style="display:block" id="mqttStateHint">&nbsp;</small></span><input type="checkbox" id="mqttEnabled"></label>
<div class="row wrap" style="margin-top:14px"><div class="field" style="flex:2;min-width:200px"><label>Address</label><input class="input" id="mqttHost" placeholder="homeassistant.local"></div><div class="field" style="flex:1;min-width:110px"><label>Port</label><input class="input" id="mqttPort" type="number" min="1" max="65535" value="1883"></div></div>
<div class="row wrap"><div class="field" style="flex:1;min-width:180px"><label>User name</label><input class="input" id="mqttUser" autocomplete="off"></div><div class="field" style="flex:1;min-width:180px"><label>Password</label><input class="input" id="mqttPassword" type="password" placeholder="Leave blank to keep current"></div></div>
<div class="field"><label>Topic prefix</label><input class="input" id="mqttTopic" maxlength="47"><span class="hint">Everything this speaker publishes sits under it. Two speakers on one broker need two prefixes.</span></div>
<label class="switch"><span><b>Publish discovery documents</b><small class="hint" style="display:block">What makes the speaker appear in Home Assistant complete, with the right names and units, instead of needing eighty lines of YAML per entity.</small></span><input type="checkbox" id="mqttDiscovery"></label>
<div class="row wrap"><div class="field" style="flex:1;min-width:180px"><label>Discovery prefix</label><input class="input" id="mqttDiscoveryPrefix" maxlength="47"></div><div class="field" style="flex:1;min-width:180px"><label>Publish every</label><select class="input" id="mqttPublish"><option value="5">5 seconds</option><option value="15" selected>15 seconds</option><option value="30">30 seconds</option><option value="60">1 minute</option><option value="300">5 minutes</option></select></div></div>
<span class="hint">Every reading is also published the moment it changes, so this is a heartbeat rather than a poll.</span>
<div class="row wrap" style="margin-top:16px"><button class="btn primary" id="mqttSave">Save</button><button class="btn ghost" id="mqttAnnounce">Resend to Home Assistant</button></div></article></div>
<div class="stack"><article class="card panel"><h3>Connection</h3><div class="kv"><span>State</span><b id="mqttState">&mdash;</b><span>Connected for</span><b id="mqttUptime">&mdash;</b><span>Connections</span><b id="mqttConnects">0</b><span>Messages sent</span><b id="mqttPublished">0</b><span>Commands acted on</span><b id="mqttReceived">0</b><span>Discovery</span><b id="mqttDisc">&mdash;</b></div><div id="mqttError" class="callout" style="display:none;margin-top:14px"></div></article>
<article class="card panel"><h3>What appears in Home Assistant</h3><p>One device, with the entities below. The list follows the firmware rather than a configuration file, so adding a radio station or an alarm changes what Home Assistant sees the next time the speaker connects.</p><div class="kv"><span>Controls</span><b>Volume, mute, play/pause, next, previous</b><span></span><b>Equaliser preset, radio station</b><span></span><b>LED ring as a light, sleep timer</b><span></span><b>One switch per alarm, and standby</b><span>Readings</span><b>Source, now playing, connected device</b><span></span><b>Battery, voltage, charging, temperature</b><span></span><b>Free memory, Wi-Fi signal, uptime, runtime</b><span></span><b>Radio status and buffer level</b></div><div class="callout" style="margin-top:14px">The standby switch turns off and cannot turn back on. That is the honest shape for it: a chip in deep sleep is not listening to MQTT, and no amount of protocol will change that.</div></article></div></div></section>
<section class="page" id="page-settings"><div class="sectionhead"><div><h2>esp32-blue-spk settings</h2><p>Identity, access, display, release source, and system maintenance.</p></div><button class="btn primary" id="saveSettings">Save settings</button></div><div class="grid two"><div class="stack"><article class="card panel"><h3>Identity & access</h3><p>Identity changes take effect after a restart.</p><div class="field"><label>Bluetooth speaker name</label><input class="input" id="deviceName" maxlength="31"></div><div class="field"><label>DHCP network hostname</label><input class="input" id="hostname" maxlength="31"></div><div class="field"><label>New dashboard password</label><input class="input" id="adminPassword" type="password" minlength="6" placeholder="Leave blank to keep current"><span class="hint">User name is always <b>admin</b>.</span></div><div class="field"><label>New setup hotspot password</label><input class="input" id="apPassword" type="password" minlength="8" placeholder="Leave blank to keep current"></div><label class="switch"><span><b>Always keep setup hotspot on</b><small class="hint" style="display:block">Useful for direct access, but adds radio traffic.</small></span><input type="checkbox" id="apAlways"></label></article><article class="card panel"><h3>OLED display</h3><p>Changes apply immediately and wake the screen.</p><div class="screenpick"><button data-screen="0">Now playing</button><button data-screen="1">Spectrum</button><button data-screen="2">VU meters</button><button data-screen="3">Scope</button><button data-screen="4">Waterfall</button><button data-screen="5">Radio</button><button data-screen="6">Clock</button><button data-screen="7">Info</button><button data-display="auto">Auto rotate</button></div><div class="field"><label>Brightness <span id="brightnessValue">Auto</span></label><input type="range" id="brightness" min="0" max="255" value="0"></div><button class="btn ghost" data-display="next">Next screen</button><h3 style="margin-top:26px">Switch the panel off</h3><p style="margin-bottom:0">Dimming and the screensaver only move the lit pixels around. This turns the display off outright, which is what actually stops an OLED ageing — and gets back the ~15 mA it draws.</p><div class="screenpick modepick" id="oledBlank"><button data-blank="0"><b>Always on</b><small>The panel never switches itself off</small></button><button data-blank="1"><b>Off when idle</b><small>Off once nothing is playing and nobody has touched it</small></button><button data-blank="2"><b>Off on a timer</b><small>Off after the timeout even while a track is playing</small></button></div><div class="timeout" id="oledTimeoutRow"><label for="oledTimeout" style="color:#aab8c7;font-size:12px">Switch off after</label><select class="input" id="oledTimeout"></select></div><span class="hint" id="oledBlankState" style="display:block;margin-top:10px"></span></article><article class="card panel"><h3>DFPlayer startup defaults</h3><p>The module keeps nothing across a power cut — not the volume, not the source, not the equaliser — so the firmware sends these at every boot. Changing them here does not touch what is playing now; the Media page does that.</p><div class="pair"><div class="field"><label>Source</label><select class="input" id="dfDefSource"><option value="2">SD card</option><option value="1">USB drive</option><option value="5">On-board flash</option><option value="3">AUX input</option></select></div><div class="field"><label>Volume <span id="dfDefVolText">—</span></label><input type="range" id="dfDefVolume" min="0" max="30" value="20"></div></div><div class="pair"><div class="field"><label>Equaliser</label><select class="input" id="dfDefEq"><option value="0">Normal</option><option value="1">Pop</option><option value="2">Rock</option><option value="3">Jazz</option><option value="4">Classic</option><option value="5">Bass</option></select></div><div class="field"><label>Repeat</label><select class="input" id="dfDefLoop"><option value="0">Off</option><option value="1">One track</option><option value="2">Folder</option><option value="3">Everything</option><option value="4">Shuffle</option></select></div></div><div class="field"><label>Folder for folder-repeat</label><input class="input" id="dfDefLoopFolder" type="number" min="1" max="99" value="1"></div><label class="switch"><span><b>Start playing at power-on</b><small class="hint" style="display:block">Waits for the card to mount and report its file count, then plays track 1. Gives up after 15 seconds rather than sitting on a command the module would refuse.</small></span><input type="checkbox" id="dfDefAutoplay"></label></article></div><div class="stack"><article class="card panel"><h3>Battery</h3><p id="batSensePin">Reads a divider on an ADC1 pin. These numbers describe the pack and the resistors, so they are applied immediately rather than at the next restart.</p><label class="switch" style="border:0;padding-top:0"><span><b>Battery gauge enabled</b><small class="hint" style="display:block">Off hides the indicator everywhere and stops the low-battery LED pattern.</small></span><input type="checkbox" id="batEnabled"></label><div class="pair"><div class="field"><label>Series cells</label><input class="input" id="batCells" type="number" min="1" max="4" value="1"></div><div class="field"><label>Divider ratio</label><input class="input" id="batDivider" type="number" step="0.01" min="1" max="20" value="2"><span class="hint">2.0 for 100k/100k.</span></div></div><div class="pair"><div class="field"><label>Full, per cell (V)</label><input class="input" id="batFull" type="number" step="0.01" min="3.4" max="4.5" value="4.20"><span class="hint">4.20 normally; 4.10 if you charge low for longevity.</span></div><div class="field"><label>Empty, per cell (V)</label><input class="input" id="batEmpty" type="number" step="0.01" min="2.5" max="4.2" value="3.30"></div></div><div class="pair"><div class="field"><label>Low warning (%)</label><input class="input" id="batLow" type="number" min="1" max="90" value="20"></div><div class="field"><label>Critical (%)</label><input class="input" id="batCritical" type="number" min="1" max="50" value="7"></div></div><div class="field"><label>Calibrate against a meter</label><div class="inline"><input class="input" id="batActual" type="number" step="0.01" placeholder="Measured pack voltage"><button class="btn" id="batCalibrate">Apply trim</button></div><span class="hint" id="batTrim">Put a meter across the pack, type what it says, and the correction factor is computed and stored.</span></div><span class="hint" id="batPack"></span><div class="kv"><span>Reading at the pin</span><b id="batPin">—</b><span>Charger status pins</span><b id="batChargePins">—</b></div></article><article class="card panel"><h3>GitHub Releases</h3><p>The latest matching <code>.bin</code> asset can be installed directly.</p><div class="field"><label>Repository (owner/name)</label><input class="input" id="githubRepo" placeholder="your-name/esp32-blue-spk"></div><div class="field"><label>Firmware asset pattern</label><input class="input" id="githubAsset" placeholder="*.bin"><span class="hint">Supports <code>*</code> and <code>?</code>; bootloader and partition files are excluded.</span></div><div class="field"><label>GitHub token (private repositories only)</label><input class="input" id="githubToken" type="password" placeholder="Leave blank to keep current"><label class="hint"><input type="checkbox" id="clearGithubToken"> Remove stored token</label></div></article><article class="card panel"><h3>Clock</h3><p>The speaker keeps its own time, and the display and the file timestamps both use it. These apply immediately.</p><div class="kv" style="margin-top:0;border-top:0;padding-top:0"><span>Speaker time</span><b id="clockNow">—</b><span>Time source</span><b id="clockSource">—</b><span>Time zone</span><b id="clockZone">—</b></div><div class="field"><label>Time format</label><div class="screenpick pick2" id="clockFormat"><button data-clockfmt="24">24-hour<small class="hint" style="display:block">18:51</small></button><button data-clockfmt="12">12-hour<small class="hint" style="display:block">6:51 PM</small></button></div></div><label class="switch"><span><b>Sync over the internet</b><small class="hint" style="display:block">While Wi-Fi is connected the speaker asks a public time server and corrects itself, which is also what keeps the update check's certificate validation working. Off keeps whatever time you set here.</small></span><input type="checkbox" id="clockAutoSync" checked></label><div class="row wrap" style="margin-top:16px"><button class="btn" id="syncClock" data-icon="clock">Sync browser time</button></div><span class="hint" style="display:block;margin-top:10px">Sending the browser's time also stores its UTC offset, which is what the network sync uses to land on the right wall clock.</span></article><article class="card panel"><h3>Power</h3><p>One switch for everything on this board that costs current and is not the speaker: the ring, the panel, the indicator LED, and a Wi-Fi radio that otherwise never sleeps between beacons.</p><div class="screenpick modepick" id="powerMode"><button data-power="0"><b>Off</b><small>Nothing is saved; every setting is the one you chose</small></button><button data-power="1"><b>Always saving</b><small>On mains as well as on battery</small></button><button data-power="2"><b>Automatic</b><small>On battery at or below a level you set, and never while charging</small></button></div><div class="field" id="powerThresholdRow" style="margin-bottom:0"><label>Start saving at <span id="powerThresholdText">20%</span> of charge</label><input type="range" id="powerThreshold" min="0" max="100" value="20"><span class="hint">Charging outranks this: a pack at 8% with a charger on it is getting better, so nothing is saved whatever the slider says. Coming back out needs 5 points more than going in, because switching the ring off changes the load and so changes the reading.</span></div><div class="kv"><span>Right now</span><b id="powerState">—</b></div><span class="hint" id="powerReason" style="display:block;margin-top:10px"></span><h3 style="margin-top:26px">Standby</h3><p style="margin-bottom:0">Saving leaves a working speaker that costs less to run. Standby stops being a speaker: everything goes dark, both radios come down, the core drops to 10&nbsp;MHz, and it waits for the BOOT button — then restarts, which is how the radios and the audio path come back.</p><div class="screenpick modepick" id="sleepMode"><button data-sleep="0"><b>Never</b><small>The speaker stays up until you say otherwise</small></button><button data-sleep="1"><b>After the timeout</b><small>Whenever nothing has played and nobody has touched it</small></button><button data-sleep="2"><b>Only while saving</b><small>The same, but only when power saving is actually on</small></button></div><div class="timeout" id="sleepTimeoutRow"><label for="sleepAfter" style="color:#aab8c7;font-size:12px">Stand by after</label><select class="input" id="sleepAfter"></select></div><div class="row wrap" style="margin-top:16px"><button class="btn" id="standbyNow">Stand by now</button><span class="hint" id="sleepState"></span></div><div class="callout">Standby is about 15&nbsp;mA, not the tens of microamps a real deep sleep would be. <b>esp_deep_sleep_start()</b> has to run with the flash cache off, so it lives in IRAM and wants ~1.8&nbsp;KB of it — this firmware has 653&nbsp;bytes left, because the Bluetooth controller alone holds 33&nbsp;KB there. It overflows the segment by 1012&nbsp;bytes and will not link. A build with Bluetooth compiled out has the room for the real thing.</div><div class="callout">Saving switches the ring off, holds the panel off, quiets the indicator LED, and puts the Wi-Fi radio into modem sleep at reduced transmit power. It writes none of those settings — all four come back exactly as you left them. The CPU clock is deliberately left alone: SBC decode has little headroom at 240 MHz and none below it, and a power mode whose symptom is crackle is not one anybody would leave on.</div></article><article class="card panel"><h3>Backup & restore</h3><p>Every stored setting in one file, for a board that has been reflashed or replaced.</p><div class="field"><label>Backup passphrase</label><input class="input" id="backupPass" type="password" minlength="8" placeholder="Leave blank to leave the secrets out"><span class="hint">Encrypts the Wi-Fi passphrase, both passwords and the GitHub token into the file (AES-256-GCM). You need this passphrase to restore them and it cannot be recovered from the speaker — leave it blank and those four are simply not in the file.</span></div><div class="row wrap"><button class="btn" id="backupSettings">Download backup</button><span class="hint">Everything else — identity, Wi-Fi network, GitHub, display, power, battery, lighting and clock — stays readable.</span></div><input type="file" id="restoreFile" accept=".json,application/json" hidden><div class="drop" id="restoreDrop" tabindex="0" style="margin-top:16px;padding:20px"><b>Drop a settings backup here</b><span>or click to choose a .json file</span></div><div class="field" id="restorePassRow" style="margin-top:14px;display:none"><label>Passphrase for this backup</label><input class="input" id="restorePass" type="password" placeholder="The passphrase this file was downloaded with"></div><div class="row" style="margin-top:14px"><button class="btn danger" id="restoreSettings" disabled>Restore & restart</button><span class="hint" id="restoreFileName">No file selected</span></div><div class="callout">Restoring overwrites the Wi-Fi network and both passwords, then restarts the speaker. A wrong passphrase changes nothing at all.</div></article><article class="card panel dangerzone"><h3>System</h3><p>Restart safely, or clear all Wi-Fi, dashboard, and Bluetooth pairing data.</p><div class="row wrap"><button class="btn" id="reboot">Restart speaker</button><button class="btn danger" id="factoryReset">Factory reset</button></div></article></div></div></section>
</main></div>
<nav class="mobilebar" id="mobileNav"><button class="active" data-page="overview" data-icon="home">Home</button><button data-page="devices" data-icon="bluetooth">Devices</button><button data-page="media" data-icon="sdcard">Media</button><button data-page="lighting" data-icon="bulb">Lights</button><button data-page="wifi" data-icon="wifi">Wi-Fi</button><button data-page="updates" data-icon="download">Update</button><button data-page="settings" data-icon="settings">Settings</button></nav>
<div class="modal show" id="loginModal"><form class="dialog" id="loginForm"><div class="loginlogo" data-icon="speaker"></div><h2>Welcome back</h2><p>Sign in to manage your speaker. On first setup, the password is <b>admin</b>.</p><div class="field"><label>Dashboard password</label><input class="input" id="loginPassword" type="password" required autofocus autocomplete="current-password"></div><button class="btn primary" style="width:100%" type="submit">Open console</button><div class="hint" id="loginError" style="color:var(--red);margin-top:12px"></div></form></div>
<div class="modal" id="confirmModal"><div class="dialog"><h2 id="confirmTitle">Are you sure?</h2><p id="confirmText"></p><div class="row" style="justify-content:flex-end"><button class="btn ghost" id="cancelConfirm">Cancel</button><button class="btn danger" id="acceptConfirm">Continue</button></div></div></div><div class="toast" id="toast"></div>
<script>
const $=s=>document.querySelector(s),$$=s=>[...document.querySelectorAll(s)];
const paths={speaker:'<path d="M4 9v6h4l5 4V5L8 9H4m13 1c1.5 2 1.5 4 0 6m3-9c3.5 3.5 3.5 6.5 0 10"/>',home:'<path d="M3 11 12 3l9 8v9a1 1 0 0 1-1 1h-5v-7H9v7H4a1 1 0 0 1-1-1z"/>',bluetooth:'<path d="m7 7 10 10-5 4V3l5 4L7 17"/>',wifi:'<path d="M3 8.5a14 14 0 0 1 18 0M6.5 12a9 9 0 0 1 11 0M10 15.5a4 4 0 0 1 4 0M12 19h.01"/>',download:'<path d="M12 3v12m-5-5 5 5 5-5M4 21h16"/>',settings:'<circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.7 1.7 0 0 0 .34 1.88l.06.06-2.83 2.83-.06-.06a1.7 1.7 0 0 0-1.88-.34 1.7 1.7 0 0 0-1.03 1.56V21h-4v-.08A1.7 1.7 0 0 0 9 19.37a1.7 1.7 0 0 0-1.88.34l-.06.06-2.83-2.83.06-.06A1.7 1.7 0 0 0 4.63 15 1.7 1.7 0 0 0 3.08 14H3v-4h.08A1.7 1.7 0 0 0 4.63 9a1.7 1.7 0 0 0-.34-1.88l-.06-.06 2.83-2.83.06.06A1.7 1.7 0 0 0 9 4.63h.02A1.7 1.7 0 0 0 10 3.08V3h4v.08a1.7 1.7 0 0 0 1 1.55 1.7 1.7 0 0 0 1.88-.34l.06-.06 2.83 2.83-.06.06A1.7 1.7 0 0 0 19.37 9v.02A1.7 1.7 0 0 0 20.92 10H21v4h-.08A1.7 1.7 0 0 0 19.4 15z"/>',refresh:'<path d="M20 6v5h-5M4 18v-5h5M18.5 9a7 7 0 0 0-12-2L4 11m16 2-2.5 4a7 7 0 0 1-12-2"/>',scan:'<path d="M3 8V3h5m8 0h5v5M3 16v5h5m8 0h5v-5M8 12h8"/>',play:'<path fill="currentColor" stroke="none" d="m8 5 11 7-11 7z"/>',pause:'<path fill="currentColor" stroke="none" d="M7 5h4v14H7zm7 0h4v14h-4z"/>',stop:'<rect x="6" y="6" width="12" height="12" rx="1" fill="currentColor" stroke="none"/>',previous:'<path d="M19 20 9 12l10-8zM5 19V5"/>',next:'<path d="m5 4 10 8-10 8zm14 1v14"/>',rewind:'<path d="m11 19-9-7 9-7zm11 0-9-7 9-7z"/>',forward:'<path d="m13 5 9 7-9 7zM2 5l9 7-9 7z"/>',volume:'<path d="M4 10v4h4l5 4V6l-5 4zm13 0c1.5 1.5 1.5 2.5 0 4m2-7c4 3.5 4 6.5 0 10"/>',clock:'<circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/>',sdcard:'<path d="M7 3h7l4 4v13a1 1 0 0 1-1 1H7a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1z"/><path d="M10 6v3m2.5-3v3M15 6v3"/>',battery:'<rect x="2" y="8" width="17" height="9" rx="2"/><path d="M21 11v3" stroke-width="3"/>',tune:'<path d="M4 6h10M18 6h2M4 12h4M12 12h8M4 18h12M20 18h0"/><circle cx="16" cy="6" r="2"/><circle cx="10" cy="12" r="2"/><circle cx="18" cy="18" r="2"/>',radio:'<path d="M12 11v10M8 21h8"/><path d="M8.5 7.5a5 5 0 0 1 7 0M5.5 4.5a9 9 0 0 1 13 0"/><circle cx="12" cy="11" r="1.6"/>',alarm:'<path d="M12 8v5l3 2"/><circle cx="12" cy="13" r="8"/><path d="m5 3 3 2M19 3l-3 2"/>',chart:'<path d="M4 20V6M4 20h16"/><path d="m7 15 4-5 3 3 5-7"/>',hass:'<path d="m3 11 9-8 9 8v9a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1z"/><path d="M12 9v5m-3-2h6"/>',bulb:'<path d="M9 18h6m-5 3h4M12 3a6 6 0 0 1 3.7 10.7c-.5.4-.7 1-.7 1.6V17H9v-1.7c0-.6-.2-1.2-.7-1.6A6 6 0 0 1 12 3z"/>'};
function icons(){$$('[data-icon]').forEach(e=>{let n=e.dataset.icon;if(paths[n]&&!e.querySelector('svg'))e.insertAdjacentHTML('afterbegin',`<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">${paths[n]}</svg>`)})}icons();
const MODES=[{name:'Wi-Fi only',badge:'Wi-Fi',hint:'The dashboard has the radio to itself. Bluetooth is not running, so nothing can pair.',warn:'The speaker restarts and rejoins your network; this dashboard comes back in a few seconds.'},{name:'Bluetooth only',badge:'Bluetooth',hint:'The A2DP sink has the radio to itself. Wi-Fi is never started in this mode.',warn:'The speaker restarts, Wi-Fi shuts down and this dashboard becomes unreachable. Hold BOOT on the speaker to cycle back.'},{name:'DFPlayer + Wi-Fi',badge:'SD + Wi-Fi',hint:'A DFPlayer Mini plays from its own microSD card or a USB drive and this dashboard drives it. Neither Bluetooth radio is started, so the whole controller is handed back — the largest heap saving any mode makes.',warn:'The speaker restarts with both Bluetooth radios off, so anything paired stops working until you switch back. Wi-Fi behaves exactly as it does now, including the setup hotspot when no network is saved, so this dashboard comes back in a few seconds.'}];
let auth=sessionStorage.getItem('speakerAuth')||'',status=null,settings=null,selectedFile=null,confirmAction=null,volumeTimer=0,pollTimer=0;
const esc=s=>String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
async function api(url,opt={}){opt.headers={...(opt.headers||{}),Authorization:auth};if(opt.body&&!(opt.body instanceof FormData)){opt.headers['Content-Type']='application/json';opt.body=JSON.stringify(opt.body)}let r=await fetch(url,opt);let d={};try{d=await r.json()}catch{}if(r.status===401){sessionStorage.removeItem('speakerAuth');$('#loginModal').classList.add('show');throw Error('Sign in required')}if(!r.ok)throw Error(d.error||`Request failed (${r.status})`);return d}
function toast(msg,error=false){let t=$('#toast');t.textContent=msg;t.className='toast show'+(error?' error':'');clearTimeout(t._timer);t._timer=setTimeout(()=>t.className='toast',3200)}
function fmtTime(ms){let s=Math.max(0,Math.floor((ms||0)/1000)),m=Math.floor(s/60);return `${m}:${String(s%60).padStart(2,'0')}`}
function fmtUp(ms){let m=Math.floor(ms/60000);if(m<60)return `${m}m`;let h=Math.floor(m/60);if(h<48)return `${h}h ${m%60}m`;return `${Math.floor(h/24)}d ${h%24}h`}
function fmtBytes(n){if(n>1048576)return`${(n/1048576).toFixed(1)} MB`;return`${Math.round(n/1024)} KB`}
function render(s){status=s;let w=s.wifi,b=s.bluetooth,m=s.media,u=s.update;$('#sideVersion').textContent=`Firmware ${s.firmware.version}`;$('#firmwareVersion').textContent=`v${s.firmware.version}`;$('#uptime').textContent=fmtUp(s.system.uptimeMs);$('#heap').textContent=fmtBytes(s.system.heapFree);$('#rssi').textContent=w.connected?`${w.rssi} dBm`:'—';$('#apClients').textContent=w.apClients;$('#wifiDot').className='dot '+(w.connected?'good':'');$('#sideDot').className='dot '+(b.connected?'good':'');$('#sideStatus').textContent=s.system.powerSaving?'Power saving':(s.mode&&s.mode.dfplayer)?(!s.dfplayer||!s.dfplayer.running?'DFPlayer starting':(s.dfplayer.asleep?'DFPlayer in standby':(!s.dfplayer.online?'DFPlayer not answering':(s.dfplayer.busy?'Playing from '+s.dfplayer.sourceName:(s.dfplayer.pc?'Card on a computer':'Ready to play'))))):(b.connected?(b.streaming?'Streaming audio':'Bluetooth connected'):(b.active?'Ready to pair':'Wi-Fi only, Bluetooth off'));$('#topWifi').textContent=w.connected?w.ssid:(w.apRunning?w.apSsid:'Offline');$('#wifiName').textContent=w.connected?w.ssid:(w.apSsid||'Offline');$('#wifiBadge').textContent=w.connected?'Connected':(w.apRunning?'Setup AP':'Offline');$('#wifiBadge').className='badge '+((w.connected||w.apRunning)?'good':'');$('#wifiDetail').textContent=w.connected?`${w.ip} · ${w.rssi} dBm`:(w.apRunning?`${w.apIp} · ${w.apClients} client${w.apClients===1?'':'s'}`:'No network');$('#signalMeter').style.width=w.connected?`${Math.max(5,Math.min(100,2*(w.rssi+100)))}%`:'0';$('#btDevice').textContent=b.connected?(b.device||'Connected device'):(b.active?'No device':'Bluetooth off');$('#btDetail').textContent=b.connected?`${b.address} · ${b.sampleRate/1000} kHz`:(b.active?'Discoverable and ready to pair':'Not running in Wi-Fi only mode');$('#btBadge').textContent=b.streaming?'Streaming':(b.connected?'Connected':(b.active?'Waiting':'Off'));$('#btBadge').className='badge '+(b.connected?'good':'');renderMode(s.mode||{},b,s.dfplayer||{});renderBattery(s.battery||{});if($('#page-media').classList.contains('active')){renderDfPage(s.dfplayer||{});dfNowLine()}$('#btHeldOff').style.display=b.active?'none':'';$('#title').textContent=m.title||'Nothing playing';let df=s.dfplayer||{},viaDf=!!(s.mode&&s.mode.dfplayer);$('#artist').textContent=m.artist||(m.title?'':(viaDf?(df.running?(df.online?'Pick a track on the Media page':'The module is not answering — check TX and RX'):'The DFPlayer driver did not start'):(b.connected?'Waiting for track information':'Connect a Bluetooth device to begin')));$('#streamLabel').textContent=viaDf?(df.busy?'Playing from the card':(DFLABEL[df.state]||'Ready to play')):(b.streaming?'Live audio':(b.connected?'Connected':'Ready to play'));let hz=viaDf?(df.running&&df.online?44100:0):(b.sampleRate||0);$('#codec').textContent=hz?`${(hz/1000).toFixed(1)} kHz`:'—';$('#position').textContent=fmtTime(m.positionMs);$('#duration').textContent=fmtTime(m.durationMs);$('#trackBar').style.width=m.durationMs?`${Math.min(100,m.positionMs/m.durationMs*100)}%`:'0';if(document.activeElement!==$('#volume')){$('#volume').value=m.volume;$('#volumeText').textContent=`${Math.round(m.volume/127*100)}%`}let pb=$('#playButton');pb.dataset.icon=m.state==='playing'?'pause':'play';pb.querySelector('svg').outerHTML=`<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8">${paths[pb.dataset.icon]}</svg>`;if(settings&&settings.power&&$('#page-settings').classList.contains('active')){const on=!!s.system.powerSaving;if(on!==settings.power.saving)loadSettings()}$('#updateBadge').textContent=u.available?'Available':(u.busy?'Working':'Current');$('#updateBadge').className='badge '+(u.available?'':(!u.busy?'good':''));$('#firmwareDetail').textContent=u.busy?u.message:(u.available?`${u.tag||u.releaseName} is newer and ready to install`:(u.tag?`Latest on GitHub is ${u.tag} · nothing newer to install`:'A/B update protection enabled'));$('#overviewInstallRow').style.display=(u.available&&!u.busy)?'':'none';renderUpdate(u);renderCurrentDevice(b);clockFromStatus(s.system)}
function renderMode(md,b,d){let id=md.id||0,cur=MODES[id]||MODES[0];$('#modeName').textContent=cur.name;$('#modeDetail').textContent=cur.hint;$('#modeBadge').textContent=cur.badge;$('#modeBadge').className='badge '+(id===2?'good':'');let dfBuilt=md.dfBuilt!==false;$$('[data-mode]').forEach(x=>{let m=+x.dataset.mode;x.classList.toggle('on',m===id);x.disabled=m===id||(m===2&&!dfBuilt)});$('#modeNote').textContent=dfBuilt?'':'This firmware was built with the DFPlayer driver switched off.';let dfm=!!md.dfplayer;$('#btCard').style.display=dfm?'none':'';$('#dfCard').style.display=dfm?'':'none';if(dfm)renderDf(d);let live,ctl,skip,seek;if(dfm){live=!!(d.running&&d.online&&!d.asleep);ctl=live;skip=live;seek=false}else{live=!!b.active;ctl=live&&!!b.avrcp;skip=ctl;seek=ctl}$$('[data-media]').forEach(x=>{let a=x.dataset.media;x.disabled=(a==='volume'||a==='mute')?!live:(SEEKS.includes(a)?!seek:(SKIPS.includes(a)?!skip:!ctl))});$('#volume').disabled=!live;$('#controlHint').textContent=dfm?(!d.running?'The DFPlayer driver did not start this boot; check the serial log.':(d.asleep?'The module is in standby. Wake it on the Media page.':(!d.online?'The module is not answering. The usual cause is TX and RX swapped; the hardware pin buttons on the Media page work regardless.':(d.pc?'A computer has the card mounted. Unplug the USB cable to play from it again.':(!d.totalTracks?'No files found on this source. Check the card, or pick another source on the Media page.':'Seeking needs a position, and the module reports none — use next and previous.'))))):(!live?'Playback control needs the Bluetooth stack running. Choose Bluetooth only under Radio mode.':(!b.connected?'Pair a phone to control playback from here.':(!ctl?'This device offers no AVRCP channel, so only volume can be set from here.':'')))}const SKIPS=['next','previous'],SEEKS=['forward','rewind'];const DFLABEL={stopped:'Stopped',playing:'Playing',paused:'Paused',standby:'Standby'};
function renderDf(d){if(d.stale)return;let bad=!d.running||(!d.online&&!d.asleep);$('#dfBadge').textContent=!d.running?'Off':(d.asleep?'Standby':(!d.online?'No reply':(DFLABEL[d.state]||d.state||'Idle')));$('#dfBadge').className='badge '+(bad?'bad':(d.asleep?'warn':(d.busy?'good':'')));$('#dfNow').textContent=!d.running?'Driver not started':(d.asleep?'Module in standby':(!d.online?'Module not answering':(d.pc?'Card on a computer':(d.track?(d.folder?`Folder ${d.folder} · track ${d.track}`:`Track ${d.track}`):'Nothing playing'))));$('#dfDetail').textContent=bad?'':`${d.sourceName||''}`+(d.totalTracks?` · ${d.totalTracks} file${d.totalTracks===1?'':'s'}`:' · no files found')+(d.eqName?` · ${d.eqName} EQ`:'')+(d.loop&&d.loopName!=='off'?` · repeat ${d.loopName}`:'');$$('[data-dfsource]').forEach(x=>{let k=x.dataset.dfsource;x.classList.toggle('on',SRCID[k]===d.source);x.disabled=bad||SRCID[k]===d.source||(k==='sd'&&!d.sd)||(k==='usb'&&!d.usb)||(k==='flash'&&!d.flash)});$('#dfHint').textContent=!d.running?'Check the serial log: the driver failed to claim UART2 or ran out of memory.':(d.asleep?'Its decoder is powered down — worth about 20 mA. Wake it on the Media page to play again.':(!d.online?'No frame has arrived from the module. Check TX/RX (they cross over), the 1k resistor on its RX pin, and that it has 5 V.':(d.error||'')))}const SRCID={usb:1,sd:2,aux:3,flash:5};function renderDfPage(d){if(d.stale)return;let up=!!(d.running&&d.online&&!d.asleep);$('#dfOffline').style.display=d.running?'none':'';$('#dfPanels').style.display=d.running?'':'none';if(!d.running)return;$$('[data-dfsrc]').forEach(x=>{let k=x.dataset.dfsrc;x.classList.toggle('on',SRCID[k]===d.source);x.disabled=!up||SRCID[k]===d.source});let media=(ok,label)=>ok?'Present':label;$('#dfSdState').textContent=media(d.sd,'Not detected');$('#dfUsbState').textContent=media(d.usb,'Not detected');$('#dfFlashState').textContent=media(d.flash,'Not on this module');$('#dfFiles').textContent=d.totalTracks?String(d.totalTracks):'Unknown';$('#dfFolders').textContent=d.folders?String(d.folders):'Unknown';$('#dfPc').style.display=d.pc?'':'none';if(d.queriedFolder)$('#dfFolderCount').textContent=d.folderTracks?`Folder ${d.queriedFolder} holds ${d.folderTracks} track${d.folderTracks===1?'':'s'}.`:`Folder ${d.queriedFolder} is empty, or the module has no such folder.`;if(document.activeElement!==$('#dfVolume')){$('#dfVolume').value=d.volume;$('#dfVolume').max=d.volumeMax||30}$('#dfVolText').textContent=`${d.volume} / ${d.volumeMax||30}`;$$('[data-dfeq]').forEach(x=>x.classList.toggle('on',+x.dataset.dfeq===d.eq));$$('[data-dfloop]').forEach(x=>x.classList.toggle('on',x.dataset.dfloop===d.loopName));if(document.activeElement!==$('#dfDac'))$('#dfDac').checked=d.dac!==false;let pins=d.pins||{};$$('[data-dfpin]').forEach(x=>{let ok=pins[x.dataset.dfpin]!==false;x.disabled=!ok;x.title=ok?'':'Not wired on this board — see hw_config.h'});$('#dfBusyPin').textContent=pins.busy===false?'Not wired':(d.busy?'Low — playing':'High — idle');$$('[data-dfled]').forEach(x=>{x.classList.toggle('on',x.dataset.dfled===LEDKEY[d.led]);x.disabled=pins.led===false});$('#dfLedDot').className=d.ledOn?'on':'';$('#dfLedState').textContent=pins.led===false?'No DFPlayer LED wired on this board':`Currently ${d.ledOn?'lit':'dark'}, mode ${LEDKEY[d.led]||'auto'}`;$('#dfLink').textContent=d.asleep?'Standby (nothing is asked of it)':(d.online?'Answering':'No reply');$('#dfVersion').textContent=d.version?String(d.version):'Not reported';$('#dfTrackNow').textContent=d.track?(d.folder?`${d.folder} / ${d.track}`:String(d.track)):'—';$('#dfFinished').textContent=String(d.finished||0);$('#dfError').textContent=d.error||'None';$$('#dfPanels button').forEach(x=>{if(x.dataset.dfpin!==undefined||x.dataset.dfsrc!==undefined||x.dataset.dfled!==undefined||x.id==='dfReset'||x.id==='dfWake')return;x.disabled=!up});$('#dfVolume').disabled=!up;$('#dfDac').disabled=!up}const LEDKEY={0:'auto',1:'off',2:'on',3:'blink'};
function renderBattery(b){let card=$('#batteryCard');card.style.display=b.wired===false?'none':'';if(!b.enabled){$('#batBadge').textContent='Gauge off';$('#batBadge').className='badge';$('#batPercent').textContent='Off';$('#batDetail').textContent='The sense pin is configured but the gauge is not switched on.';$('#batMeter').style.width='0';$('#batMeter').className='flat';$('#batHint').textContent='Turn it on under Settings → Battery. It starts off so a board with no divider fitted cannot invent a flat battery and sit there flashing the status LED about it.';return}if(!b.present){$('#batBadge').textContent='Not detected';$('#batBadge').className='badge bad';$('#batPercent').textContent='—';$('#batDetail').textContent=`Sense pin reads ${b.pinMillivolts||0} mV, too low for a cell`;$('#batMeter').style.width='0';$('#batMeter').className='flat';$('#batHint').textContent='Check the divider and that the pack is connected. The ratio and the trim are on the Settings page.';return}let st=b.state||'unknown',crit=st==='critical',low=st==='low';$('#batBadge').textContent={charging:'Charging',full:'Full',low:'Low',critical:'Critical',discharging:'On battery'}[st]||'Unknown';$('#batBadge').className='badge '+(crit?'bad':(low?'warn':(st==='charging'||st==='full'?'good':'')));$('#batPercent').textContent=`${b.percent}%`;$('#batDetail').textContent=`${(+b.volts).toFixed(2)} V`+(b.cells>1?` · ${(+b.cellVolts).toFixed(2)} V per cell · ${b.cells} cells`:'')+(b.chargeDone?' · charge complete':(b.charging?' · charging':''));$('#batMeter').style.width=`${Math.max(2,b.percent)}%`;$('#batMeter').className=crit?'bad':(low?'warn':'');$('#batHint').textContent=crit?`Below ${b.criticalPercent}% — the status LED is flashing about it. Nothing is switched off automatically; that is deliberate, so the speaker never cuts out mid-track on a reading that sagged under load.`:(low?`Below ${b.lowPercent}% — time to charge.`:(b.chargePins?'':'No charger status pins are wired, so charging cannot be detected — the voltage and the percentage are still right.'))}
function renderUpdate(u){$('#releaseName').textContent=u.releaseName||u.tag||'Not checked yet';$('#releaseAsset').textContent=u.asset||'Configure a GitHub repository in Settings.';$('#updateMessage').textContent=u.message||u.phase;let pct=u.total?Math.min(100,u.done/u.total*100):0;$('#updateProgress').style.width=`${pct}%`;$('#installUpdate').disabled=u.busy||!u.asset||!u.available;$('#checkUpdate').disabled=u.busy;$('#uploadFirmware').disabled=u.busy||!selectedFile;let link=$('#releaseLink');link.href=u.releaseUrl||'#';link.style.display=u.releaseUrl?'inline-flex':'none'}
function renderCurrentDevice(b){let box=$('#currentDevice');if(!b.connected){box.className='empty';box.textContent='No phone connected';return}box.className='device';box.innerHTML=`<div class="avatar"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8">${paths.bluetooth}</svg></div><div class="deviceinfo"><b>${esc(b.device||'Connected device')}</b><small>${esc(b.address)} · ${b.streaming?'Streaming audio':'Idle'}</small></div><button class="btn danger" data-disconnect>Disconnect</button>`;box.querySelector('[data-disconnect]').onclick=()=>confirmDo('Disconnect device?','Playback will stop, but the phone will stay paired.',()=>deviceAction('disconnect'))}
/*
 * The speaker's clock, drawn from the epoch the status poll carries rather than
 * from this browser's own time: the two are only the same if somebody has
 * pressed Sync, and the whole point of showing it is to make that visible.
 * Polls land every two seconds, so the seconds are ticked locally from the
 * moment the epoch arrived -- the reading never drifts from the speaker, and it
 * never sits still between polls either.
 */
let devClock=null;const CLK_MON=['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec'],CLK_DAY=['Sun','Mon','Tue','Wed','Thu','Fri','Sat'],CLK_SRC={ntp:'Network time (NTP)',ds3231:'DS3231 module',nvs:'Restored after power cut',set:'Set by hand',build:'Build timestamp (unconfirmed)'};
function clockFromStatus(sys){if(!sys||!sys.epoch)return;devClock={epoch:sys.epoch,offset:sys.tzOffsetMinutes||0,h24:sys.clock24h!==false,trusted:sys.clockTrusted!==false,source:sys.clockSource||''};devClock.at=Date.now();drawClock()}
// The offset is already in the epoch we add it to, so every reader below is a
// UTC one: getHours() would apply the *browser's* zone a second time.
function clockText(d,h24,secs){let h=d.getUTCHours(),m=String(d.getUTCMinutes()).padStart(2,'0'),s=String(d.getUTCSeconds()).padStart(2,'0'),ap='';if(h24)h=String(h).padStart(2,'0');else{ap=h<12?' AM':' PM';h=h%12||12}return `${h}:${m}${secs?':'+s:''}${ap}`}
function drawClock(){if(!devClock)return;let d=new Date(devClock.epoch*1000+(Date.now()-devClock.at)+devClock.offset*60000);$('#topDate').textContent=`${CLK_DAY[d.getUTCDay()]} ${d.getUTCDate()} ${CLK_MON[d.getUTCMonth()]}`;$('#topClock').textContent=clockText(d,devClock.h24,true);$('#clockDot').className='dot '+(devClock.trusted?'good':'');$('#clockPill').title=`Speaker clock · ${CLK_SRC[devClock.source]||devClock.source||'unknown source'}`;$('#clockNow').textContent=`${d.getUTCFullYear()}-${String(d.getUTCMonth()+1).padStart(2,'0')}-${String(d.getUTCDate()).padStart(2,'0')} ${clockText(d,devClock.h24,true)}`}
setInterval(drawClock,1000);

/*
 * The timeout menus for the panel and the ring. One list, because the two
 * settings mean the same thing and offering different numbers for each would
 * only invite the question of why.
 */
const TIMEOUTS=[[10,'10 seconds'],[30,'30 seconds'],[60,'1 minute'],[120,'2 minutes'],[300,'5 minutes'],[600,'10 minutes'],[900,'15 minutes'],[1800,'30 minutes'],[3600,'1 hour'],[7200,'2 hours'],[14400,'4 hours'],[43200,'12 hours']];
function fillTimeouts(sel,lo,hi){if(sel.options.length)return;sel.innerHTML=TIMEOUTS.filter(([v])=>v>=(lo||0)&&v<=(hi||86400)).map(([v,t])=>`<option value="${v}">${t}</option>`).join('')}
// A stored value that is not one of the offered steps -- set over the API, or
// left behind by a firmware with a different list -- gets an option of its own
// rather than being silently rounded to whichever one happens to be selected.
function setTimeout_(sel,secs){if(!sel.querySelector(`option[value="${secs}"]`)){const o=document.createElement('option');o.value=secs;o.textContent=fmtSecs(secs);sel.insertBefore(o,sel.firstChild)}sel.value=String(secs)}
function fmtIdle(s){s=Math.max(0,Math.round(s));if(s<60)return `${s}s`;const m=Math.floor(s/60);return m<60?`${m}m ${s%60}s`:`${Math.floor(m/60)}h ${m%60}m`}
function fmtSecs(s){if(s<60)return `${s} seconds`;if(s<3600)return `${Math.round(s/60)} minutes`;return `${(s/3600).toFixed(s%3600?1:0)} hours`}

async function refresh(show=false){try{let s=await api('/api/status');render(s);ledLiveStatus(s.leds);renderRadioOverview(s);renderAlarmOverview(s);refreshActivePage(s);if(show)toast('Dashboard refreshed')}catch(e){if(show)toast(e.message,true)}}
async function loadSettings(){try{settings=await api('/api/settings');paintZoneStatus();$('#deviceName').value=settings.deviceName;$('#hostname').value=settings.hostname;$('#apAlways').checked=settings.apAlways;$('#githubRepo').value=settings.githubRepo;$('#githubAsset').value=settings.githubAsset;$('#wifiSsid').value=settings.savedSsid;$('#clockAutoSync').checked=settings.clockAutoSync!==false;let pw=settings.power||{};renderPowerMode(pw.mode|0);if(document.activeElement!==$('#powerThreshold')){$('#powerThreshold').value=pw.threshold??20;$('#powerThresholdText').textContent=`${pw.threshold??20}%`}$('#powerState').textContent=pw.saving?'Saving':'Not saving';$('#powerState').style.color=pw.saving?'var(--mint)':'';$('#powerReason').textContent=pw.reason||'';$('#powerThresholdRow').classList.toggle('off',(pw.mode|0)!==2);fillTimeouts($('#sleepAfter'),pw.sleepMinSeconds,pw.sleepMaxSeconds);setTimeout_($('#sleepAfter'),pw.sleepAfterSeconds??1800);renderSleepMode(pw.sleepMode|0);$('#standbyNow').disabled=pw.sleepPossible===false;$('#sleepState').textContent=pw.sleepPossible===false?'No wake button is compiled in, so there would be nothing to bring it back.':((pw.sleepMode|0)===0?'Nothing has played and nobody has touched it for '+fmtIdle(pw.idleSeconds||0)+'.':`Idle for ${fmtIdle(pw.idleSeconds||0)} of ${fmtSecs(pw.sleepAfterSeconds??1800)}.`);let od=settings.display||{};fillTimeouts($('#oledTimeout'),od.blankMinSeconds,od.blankMaxSeconds);setTimeout_($('#oledTimeout'),od.blankAfterSeconds??300);renderOledBlank(od.blankMode|0);$('#oledBlankState').innerHTML=od.present===false?'No panel was found on the I2C bus, so these have nothing to act on.':(od.blanked?'The panel is off right now. Press BOOT, or change anything here, to bring it back.':`The panel is on. Nothing has played for <b>${fmtIdle(od.idleSeconds||0)}</b>; nobody has touched it for <b>${fmtIdle(od.untouchedSeconds||0)}</b>.<br>Analyser peak <b>${od.audioPeakDb??'—'} dBFS</b>${od.dfBusy?' · the DFPlayer says it is playing':''} — silence reads about −78, so anything near that with the first timer stuck is a fault worth reporting.`);let h24=settings.clock24h!==false;$$('[data-clockfmt]').forEach(x=>x.classList.toggle('on',(x.dataset.clockfmt==='24')===h24));let om=settings.clockOffsetMinutes||0,oa=Math.abs(om);$('#clockZone').textContent=`UTC${om<0?'-':'+'}${String(Math.floor(oa/60)).padStart(2,'0')}:${String(oa%60).padStart(2,'0')}`;$('#clockSource').textContent=(CLK_SRC[settings.clockSource]||settings.clockSource||'—')+(settings.clockNetworkSynced?' · synced':'');let d=settings.dfplayer||{};$('#dfDefSource').value=String(d.source||2);$('#dfDefVolume').max=d.volumeMax||30;$('#dfDefVolume').value=d.volume??20;$('#dfDefVolText').textContent=`${d.volume??20} / ${d.volumeMax||30}`;$('#dfDefEq').value=String(d.eq||0);$('#dfDefLoop').value=String(d.loop||0);$('#dfDefLoopFolder').value=d.loopFolder||1;$('#dfDefAutoplay').checked=!!d.autoplay;let b=settings.battery||{};$('#batEnabled').checked=b.enabled!==false;$('#batCells').value=b.cells||1;$('#batDivider').value=b.divider??2;$('#batFull').value=b.full??4.2;$('#batEmpty').value=b.empty??3.3;$('#batPack').textContent=(b.cells||1)>1?`That is ${((b.full??4.2)*(b.cells||1)).toFixed(2)} V to ${((b.empty??3.3)*(b.cells||1)).toFixed(2)} V across the whole pack.`:'';$('#batLow').value=b.low??20;$('#batCritical').value=b.critical??7;$('#batTrim').textContent=`Trim in force: ${(+(b.calibration??1)).toFixed(4)}. Put a meter across the pack, type what it says, and the correction is computed and stored.`;$('#batChargePins').textContent=b.chargePins?'Wired':'Not wired';if(b.sensePin!==undefined&&b.sensePin<0)$('#batSensePin').textContent='This firmware was built with no battery sense pin (PIN_BATTERY_SENSE is -1), so these settings have no effect until one is configured in hw_config.h.';if(status&&status.battery)$('#batPin').textContent=`${status.battery.pinMillivolts||0} mV`;if(settings.defaultAdminPassword)toast('Change the default dashboard password',true)}catch(e){toast(e.message,true)}}
/*
 * The DFPlayer library browser.
 *
 * Everything here is numbers, because that is all the module has: folders 01-99
 * and a file count for each, learned one round trip at a time by a scan. The
 * index is fetched on opening the Media page and then only while a scan is
 * running -- it changes at no other time, and the two-second status poll has no
 * business carrying ninety-nine numbers it does not need.
 */
let dfLib=null,dfOpenFolder=0,dfLibTimer=null,dfStarted=null;
async function loadDfLibrary(){try{dfLib=await api('/api/dfplayer/library');renderDfLibrary()}catch(e){dfLib=null}}
function dfLibPoll(on){clearInterval(dfLibTimer);dfLibTimer=on?setInterval(loadDfLibrary,1500):null}
function renderDfLibrary(){const l=dfLib;if(!l)return;
 const pct=l.scanTotal?Math.round(l.scanDone/l.scanTotal*100):0;
 $('#dfScanBarWrap').style.display=l.scanning?'':'none';$('#dfScanBar').style.width=`${pct}%`;$('#dfScan').disabled=!!l.scanning;
 $('#dfScanState').textContent=l.scanning?`Asking folder ${l.scanDone} of ${l.scanTotal}…`:(l.knownFolders?`${l.knownFolders} folder${l.knownFolders===1?'':'s'} with files on the ${l.source}${l.totalTracks?` · ${l.totalTracks} files in total`:''}`:(l.scanned?'The scan finished and found no numbered folders. Files in the root are reachable by their flat index below.':'Not scanned yet.'));
 // Polling stops itself the moment the scan does, rather than being left to a
 // timer somewhere else to notice.
 dfLibPoll(!!l.scanning);
 const fs=l.folders||[];
 $('#dfFolderEmpty').style.display=fs.length?'none':'';
 $('#dfFolderGrid').innerHTML=fs.map(f=>`<button data-dffolder="${f.folder}" class="${f.folder===dfOpenFolder?'on':''}"><b>${String(f.folder).padStart(2,'0')}</b><small>${f.files} file${f.files===1?'':'s'}</small></button>`).join('');
 $$('[data-dffolder]').forEach(b=>b.onclick=()=>dfOpen(+b.dataset.dffolder));
 dfRenderTracks();dfNowLine()}
function dfOpen(folder){dfOpenFolder=dfOpenFolder===folder?0:folder;$$('[data-dffolder]').forEach(b=>b.classList.toggle('on',+b.dataset.dffolder===dfOpenFolder));dfRenderTracks()}
function dfRenderTracks(){const pane=$('#dfTrackPane');const f=(dfLib&&(dfLib.folders||[]).find(x=>x.folder===dfOpenFolder));
 if(!f){pane.style.display='none';return}
 pane.style.display='';$('#dfTrackLabel').textContent=`Folder ${String(f.folder).padStart(2,'0')} — ${f.files} file${f.files===1?'':'s'}`;
 let html='';for(let i=1;i<=f.files;i++){const on=dfStarted&&dfStarted.folder===f.folder&&dfStarted.file===i;html+=`<button data-dftrack="${i}" class="${on?'on':''}"><b>${String(i).padStart(3,'0')}</b></button>`}
 $('#dfTracks').innerHTML=html;
 $$('[data-dftrack]').forEach(b=>b.onclick=()=>dfPlayFrom(dfOpenFolder,+b.dataset.dftrack))}
async function dfPlayFrom(folder,file){dfStarted={folder,file};dfRenderTracks();dfNowLine();await df('folder',{folder,file})}
/*
 * What is playing, in the module's own terms.
 *
 * `track` is a flat index over the whole source and is the only position the
 * module reports -- it does not say which folder a file came from. So the
 * folder and track shown are the ones the dashboard itself started, and are
 * dropped as soon as the module stops, rather than being left to describe
 * something that finished ten tracks ago.
 */
function dfNowLine(){const d=(status&&status.dfplayer)||{},l=dfLib||{};
 if(!d.busy)dfStarted=null;
 $('#dfNowLine').textContent=!d.running?'The driver did not start':(!d.online?'The module is not answering':(d.busy?(dfStarted?`Folder ${String(dfStarted.folder).padStart(2,'0')} · track ${String(dfStarted.file).padStart(3,'0')}`:`Track ${d.track||l.track||'?'}`):'Nothing playing'));
 $('#dfNowSub').textContent=d.busy?`Playing from the ${d.sourceName||l.source||'card'}${d.track?` · file ${d.track} of ${d.totalTracks||'?'} on this source`:''}`:'Pick a folder above, then a track.';const t=$('#dfToggle'),want=d.busy?'pause':'play';if(t.dataset.icon!==want){t.dataset.icon=want;t.querySelector('svg').outerHTML=`<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8">${paths[want]}</svg>`}}
async function loadDevices(){try{let d=await api('/api/devices'),box=$('#pairedDevices');if(!d.devices.length){box.innerHTML='<div class="empty">No paired devices yet</div>';return}box.innerHTML=d.devices.map(x=>`<div class="device"><div class="avatar"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8">${paths.bluetooth}</svg></div><div class="deviceinfo"><b>${esc(x.name)}</b><small>${esc(x.address)}${x.connected?' · Connected':''}</small></div><button class="btn danger" data-forget="${esc(x.address)}">Forget</button></div>`).join('');$$('[data-forget]').forEach(b=>b.onclick=()=>confirmDo('Forget this device?','Its pairing keys will be removed from the speaker.',()=>deviceAction('forget',b.dataset.forget)))}catch(e){toast(e.message,true)}}
async function deviceAction(action,address){try{await api('/api/devices',{method:'POST',body:{action,address}});toast(action==='forget'?'Device forgotten':'Device disconnected');setTimeout(()=>{refresh();loadDevices()},700)}catch(e){toast(e.message,true)}}
let ledCfg=null,ledPatch={},ledTimer=null;const LED_SWATCHES=['#FF3B30','#FF9500','#FFD60A','#34C759','#00E0FF','#0A84FF','#7A5CFF','#FF2D96','#FFFFFF'];function ledLabels(l){$('#ledBrightValue').textContent=Math.round(l.brightness/255*100)+'%';$('#ledSpeedValue').textContent=Math.round(l.speed/255*100)+'%';$('#ledReactValue').textContent=l.reactivity+'%'}function ledAudioLine(l){let e=$('#ledAudio');if(!e)return;if(l.audioPath===false){e.innerHTML='This speaker is in <b>DFPlayer mode</b>. That module decodes its own card and hands out analog audio that never passes through the ESP32, so there is nothing for the analyser to hear and the reactive effects rest at their idle brightness. Every other mode feeds it.';return}e.innerHTML=l.hearingAudio?'<b>Hearing audio.</b> The reactive effects are live.':'<b>Silent.</b> Play something and the reactive effects pick it up within a frame.'}function renderLighting(l){ledCfg=l;$('#ledUnwired').style.display=l.wired?'none':'';$('#ledBody').style.display=l.wired?'':'none';if(!l.wired)return;$('#ledEnabled').checked=!!l.enabled;$('#ledColor').value=l.color;$('#ledColor2').value=l.color2;$('#ledBrightness').value=l.brightness;$('#ledSpeed').value=l.speed;$('#ledReactivity').value=l.reactivity;ledLabels(l);$('#ledPins').textContent=`${l.count} pixel${l.count===1?'':'s'} on GPIO${l.pin}`;$('#ledEffects').innerHTML=(l.effects||[]).map((e,i)=>`<button data-fx="${i}" class="${i===l.effect?'on':''}">${esc(e.name)}</button>`).join('');$$('[data-fx]').forEach(b=>b.onclick=()=>ledPick(+b.dataset.fx));$('#ledHint').textContent=((l.effects||[])[l.effect]||{}).hint||'';fillTimeouts($('#ledIdleAfter'),l.idleMinSeconds,l.idleMaxSeconds);setTimeout_($('#ledIdleAfter'),l.idleAfterSeconds??300);$('#ledIdleOff').checked=!!l.idleOff;$('#ledIdleRow').classList.toggle('off',!l.idleOff);ledAudioLine(l);ledRestLine(l)}function ledRestLine(l){$('#ledIdleState').textContent=!l.idleOff?'The ring stays lit for as long as the speaker is powered.':(l.resting?'The ring is resting now. The first note, or any change here, brings it back.':`The ring is lit. Nothing has played and nothing has changed for ${fmtIdle(l.idleSeconds||0)}.`)}function ledPick(i){if(!ledCfg)return;ledCfg.effect=i;$$('[data-fx]').forEach(x=>x.classList.toggle('on',+x.dataset.fx===i));$('#ledHint').textContent=((ledCfg.effects||[])[i]||{}).hint||'';sendLeds({effect:i})}function sendLeds(patch){Object.assign(ledPatch,patch);if(ledTimer)return;ledTimer=setTimeout(async()=>{let body=ledPatch;ledPatch={};ledTimer=null;try{let r=await api('/api/leds',{method:'POST',body});if(ledCfg){Object.assign(ledCfg,body);ledCfg.hearingAudio=r.hearingAudio;ledCfg.resting=r.resting;ledAudioLine(ledCfg);ledRestLine(ledCfg)}}catch(e){toast(e.message,true)}},120)}function ledLiveStatus(l){if(!l||!ledCfg)return;ledCfg.hearingAudio=l.hearingAudio;ledCfg.resting=l.resting;ledAudioLine(ledCfg);ledRestLine(ledCfg)}async function loadLighting(){try{settings=await api('/api/settings');renderLighting(settings.leds||{wired:false})}catch(e){toast(e.message,true)}}$('#ledSwatchRow').innerHTML=LED_SWATCHES.map(c=>`<button data-swatch="${c}" style="background:${c}" title="${c}"></button>`).join('');$$('[data-swatch]').forEach(b=>b.onclick=()=>{$('#ledColor').value=b.dataset.swatch;sendLeds({color:b.dataset.swatch})});$('#ledEnabled').onchange=e=>sendLeds({enabled:e.target.checked});$('#ledColor').oninput=e=>sendLeds({color:e.target.value});$('#ledColor2').oninput=e=>sendLeds({color2:e.target.value});$('#ledBrightness').oninput=e=>{if(ledCfg){ledCfg.brightness=+e.target.value;ledLabels(ledCfg)}sendLeds({brightness:+e.target.value})};$('#ledSpeed').oninput=e=>{if(ledCfg){ledCfg.speed=+e.target.value;ledLabels(ledCfg)}sendLeds({speed:+e.target.value})};$('#ledReactivity').oninput=e=>{if(ledCfg){ledCfg.reactivity=+e.target.value;ledLabels(ledCfg)}sendLeds({reactivity:+e.target.value})};

/* =====================================================================
   Sound, Radio, Alarms, Graphs and Home Assistant.

   Each page keeps its own last-fetched document in a module-level variable and
   re-renders from it, rather than reading values back out of the DOM. That is
   the same shape the Lighting and Media pages already use here, and it is what
   makes a page safe to repaint underneath somebody who is halfway through
   editing: the editor writes to the document, the document paints the page.
   ===================================================================== */

let audioCfg=null,radioCfg=null,alarmCfg=null,mqttCfg=null,graphData=null;
let radioEditIndex=-1,alarmEditIndex=-1,alarmDraft=null;
let radioVolBusy=0,soundVolBusy=0;

/* --- the equaliser ---------------------------------------------------- */

const EQ_LABELS=hz=>hz>=1000?`${hz/1000}k`:`${hz}`;

function eqDraft(){
  /* The sliders are the truth while the page is open; the saved document is
     only the starting point. Reading them back here keeps "move three sliders
     then press Save" from posting the first one three times. */
  const gain=$$('#eqBank input[type=range]').map(i=>+i.value);
  return {enabled:$('#eqEnabled').checked,preamp:+$('#eqPreamp').value,
          autoPreamp:$('#eqAuto').checked,gain};
}

function eqPaintCurve(){
  /* A drawn approximation of the response, not a computed one.

     Each band is a bell (or a shelf at the ends) whose height is its gain, and
     the curve is their sum. That is not the transfer function -- the real one
     depends on Q and on how the sections interact -- but it is the shape the
     sliders describe, and the point of the picture is to make five numbers
     legible at a glance rather than to be a measurement. */
  const c=$('#eqCurve');if(!c)return;const ctx=c.getContext('2d');
  const w=c.width,h=c.height,mid=h/2;
  ctx.clearRect(0,0,w,h);
  ctx.strokeStyle='#223040';ctx.lineWidth=1;
  for(let db=-12;db<=12;db+=6){const y=mid-(db/14)*mid;ctx.globalAlpha=db===0?.9:.35;
    ctx.beginPath();ctx.moveTo(0,y);ctx.lineTo(w,y);ctx.stroke()}
  ctx.globalAlpha=1;
  const gains=$$('#eqBank input[type=range]').map(i=>+i.value);
  const bands=(audioCfg&&audioCfg.eq.bands)||[60,250,1000,4000,12000];
  if(!gains.length)return;
  /* Log frequency axis from 20 Hz to 20 kHz, which is the only axis on which a
     tone control looks like the thing it does. */
  const lo=Math.log10(20),hi=Math.log10(20000);
  const xOf=hz=>((Math.log10(hz)-lo)/(hi-lo))*w;
  ctx.beginPath();
  for(let x=0;x<=w;x++){
    const hz=Math.pow(10,lo+(x/w)*(hi-lo));
    let db=0;
    bands.forEach((bhz,i)=>{
      const oct=Math.log2(hz/bhz);
      if(i===0&&hz<bhz)db+=gains[i];
      else if(i===bands.length-1&&hz>bhz)db+=gains[i];
      else db+=gains[i]*Math.exp(-(oct*oct)/1.6);
    });
    const y=mid-(db/14)*mid;
    x?ctx.lineTo(x,y):ctx.moveTo(x,y);
  }
  ctx.strokeStyle=$('#eqEnabled').checked?'#72f1b8':'#4b5b6b';
  ctx.lineWidth=2;ctx.stroke();
}

function eqPaintLabels(){
  const gains=$$('#eqBank input[type=range]');
  gains.forEach(i=>{
    const v=+i.value,out=i.parentElement.parentElement.querySelector('.eqdb');
    out.textContent=(v>0?'+':'')+v;
    out.classList.toggle('up',v>0);out.classList.toggle('down',v<0);
  });
  $('#eqPreampText').textContent=`${$('#eqPreamp').value>0?'+':''}${$('#eqPreamp').value} dB`;
  eqPaintCurve();
}

function eqMarkCustom(){
  /* Moving a slider is what makes the curve custom. Reflected in the picker
     immediately rather than waiting for the round trip, because a preset that
     stays highlighted while the sliders no longer match it is a lie. */
  if(!audioCfg)return;
  const custom=audioCfg.eq.presets.length-1;
  $$('#eqPresets button').forEach((b,i)=>b.classList.toggle('on',i===custom));
}

function renderAudio(d){
  audioCfg=d;
  const eq=d.eq,v=d.voice;

  $('#eqHardware').style.display=eq.inHardware?'':'none';
  $('#eqEnabled').checked=eq.enabled;
  $('#eqPreamp').value=eq.preamp;
  $('#eqAuto').checked=eq.autoPreamp;
  $('#eqActiveHint').textContent=!eq.enabled?'Bypassed.'
    :eq.active?(+eq.headroomDb<0?`Active. ${eq.headroomDb} dB of level given back for headroom.`:'Active.')
    :'On, but every band is at zero, so nothing is being changed.';

  if(!$('#eqPresets').children.length){
    $('#eqPresets').innerHTML=eq.presets.map((pr,i)=>`<button data-preset="${i}">${esc(pr.name)}</button>`).join('');
    $$('#eqPresets button').forEach(b=>b.onclick=()=>{
      const pr=audioCfg.eq.presets[+b.dataset.preset];
      $$('#eqBank input[type=range]').forEach((inp,i)=>inp.value=pr.gain[i]);
      $$('#eqPresets button').forEach(o=>o.classList.toggle('on',o===b));
      $('#eqEnabled').checked=true;
      eqPaintLabels();
      saveAudio({eq:{...eqDraft(),preset:+b.dataset.preset}});
    });
  }
  $$('#eqPresets button').forEach((b,i)=>b.classList.toggle('on',i===eq.preset));

  if(!$('#eqBank').children.length){
    $('#eqBank').innerHTML=eq.bands.map((hz,i)=>
      `<div class="eqband"><span class="eqdb">0</span><div class="eqslot"><input type="range" min="${eq.gainMin}" max="${eq.gainMax}" step="1" value="0" data-band="${i}"></div><span class="eqhz">${EQ_LABELS(hz)}</span></div>`).join('');
    $$('#eqBank input[type=range]').forEach(inp=>{
      inp.oninput=()=>{eqPaintLabels();eqMarkCustom()};
      inp.onchange=()=>saveAudio({eq:eqDraft()});
    });
  }
  $$('#eqBank input[type=range]').forEach((inp,i)=>inp.value=eq.gain[i]);
  eqPaintLabels();

  $('#voiceEnabled').checked=v.enabled;
  $('#voiceVolume').value=v.volume;$('#voiceVolText').textContent=`${v.volume}%`;
  $('#voiceDuck').value=v.duck;$('#voiceDuckText').textContent=v.duck?`${v.duck}%`:'not at all';
  $('#voiceDevices').value=(v.devices||'').split(',').filter(Boolean)
    .map(x=>x.replace(':',' = ').trim()).join('\n');

  const CATS=[[1,'System','Boot, shutdown and the radio mode.'],
              [2,'Connections','A phone or the network coming and going. Off by default: a phone at the edge of range would otherwise have the speaker talking to an empty room.'],
              [4,'Battery','Low, critical, charging and full. The one worth interrupting music for.'],
              [8,'Internet radio','Connecting to a station, and failing to.'],
              [16,'Alarms','The alarm, snooze and the sleep timer.']];
  $('#voiceCats').innerHTML=CATS.map(([bit,name,hint])=>
    `<label class="rowitem" style="cursor:pointer"><input type="checkbox" data-cat="${bit}" ${v.categories&bit?'checked':''}><span class="grow"><b>${name}</b><small>${hint}</small></span></label>`).join('');
  $$('#voiceCats input').forEach(i=>i.onchange=()=>saveAudio({voice:voiceDraft()}));

  $('#voiceClips').innerHTML=(v.clips||[]).map(c=>
    `<div class="rowitem"><span class="grow"><b>${esc(c.text)}</b><small>${esc(c.id)}</small></span><span class="acts"><button class="btn ghost" data-say="${esc(c.id)}">Play</button></span></div>`).join('');
  $$('#voiceClips [data-say]').forEach(b=>b.onclick=async()=>{
    try{await api('/api/audio',{method:'POST',body:{action:'say',clip:b.dataset.say}});toast('Playing')}
    catch(e){toast(e.message,true)}
  });
}

function voiceDraft(){
  let bits=0;$$('#voiceCats input:checked').forEach(i=>bits|=+i.dataset.cat);
  /* The mapping is typed as "address = phrase" a line at a time because that is
     readable; the firmware stores it as one comma-separated string because that
     is one NVS key. This is the only place the two forms meet. */
  const devices=$('#voiceDevices').value.split('\n').map(l=>l.trim()).filter(Boolean)
    .map(l=>{const [mac,clip]=l.split('=').map(x=>(x||'').trim());
             return mac&&clip?`${mac.replace(/[:\-]/g,'').toLowerCase()}:${clip}`:''})
    .filter(Boolean).join(',');
  return {enabled:$('#voiceEnabled').checked,volume:+$('#voiceVolume').value,
          duck:+$('#voiceDuck').value,categories:bits,devices};
}

async function loadAudio(){try{renderAudio(await api('/api/audio'))}catch(e){toast(e.message,true)}}
async function saveAudio(body){try{renderAudio(await api('/api/audio',{method:'POST',body}))}catch(e){toast(e.message,true)}}

/* --- internet radio --------------------------------------------------- */

function fmtStreamBytes(n){
  if(!n)return '0 B';
  if(n<1024)return `${n} B`;
  if(n<1048576)return `${(n/1024).toFixed(0)} kB`;
  return `${(n/1048576).toFixed(1)} MB`;
}

function renderRadio(d){
  radioCfg=d;
  const off=!d.available;
  $('#radioOffline').style.display=off?'':'none';
  $('#radioPanels').style.display=off?'none':'';
  if(off){
    $('#radioOffline').innerHTML=d.modeHasWifi
      ?'Internet radio did not start this boot &mdash; there was not enough memory left for its buffer. The serial log says so at boot; a restart usually clears it.'
      :'Internet radio needs Wi-Fi, and <b>Bluetooth only</b> mode never starts it &mdash; one antenna, one radio. Pick <b>Wi-Fi only</b> or <b>DFPlayer + Wi-Fi</b> under <b>Radio mode</b> on the Overview page and the station list comes back.';
    return;
  }

  const n=d.now||{},live=n.state==='playing'||n.state==='buffering';
  $('#radioNow').textContent=n.name||'Nothing playing';
  $('#radioNowSub').textContent=n.title||n.error||{connecting:'Opening the stream…',buffering:'Filling the buffer…',reconnecting:'The stream dropped; trying again…',idle:'Pick a station below.'}[n.state]||' ';
  $('#radioState').textContent={playing:'Playing',buffering:'Buffering',connecting:'Connecting',reconnecting:'Reconnecting',error:'Stopped',idle:'Idle'}[n.state]||'—';
  $('#radioFormat').textContent=n.sampleRate?`${n.codec} ${n.bitrate?n.bitrate+' kbps ':''}${(n.sampleRate/1000).toFixed(1)} kHz ${n.channels===1?'mono':'stereo'}`:'—';
  $('#radioUnder').textContent=n.underruns||0;
  $('#radioRecon').textContent=n.reconnects||0;
  $('#radioBytes').textContent=fmtStreamBytes(n.bytes||0);
  $('#radioBuf').style.width=`${n.buffer||0}%`;
  $('#radioBufText').textContent=live?`${n.buffer||0}%`:'—';
  /* Amber below a third: at that level a stream is about to run dry, and the
     colour is the only warning that arrives before the sound stops. */
  $('#radioBufWrap').classList.toggle('warn',live&&(n.buffer||0)<33);
  $('#radioToggle').dataset.icon=live?'stop':'play';
  $('#radioToggle').innerHTML='';icons();
  if(!radioVolBusy){$('#radioVolume').value=d.volume;$('#radioVolText').textContent=`${Math.round(d.volume/1.27)}%`}
  $('#radioAutostart').checked=!!d.autostart;

  const list=d.stations||[];
  $('#radioListEmpty').style.display=list.length?'none':'';
  $('#radioList').innerHTML=list.map((st,i)=>
    `<div class="rowitem${n.station===i&&live?' on':''}"><span class="grow"><b>${esc(st.name)}</b><small>${esc(st.url)}</small></span><span class="acts"><button class="btn" data-play="${i}">${n.station===i&&live?'Stop':'Play'}</button><button class="btn ghost" data-edit="${i}" title="Edit">Edit</button><button class="btn ghost" data-up="${i}" title="Move up">&uarr;</button><button class="btn ghost" data-del="${i}" title="Remove">&times;</button></span></div>`).join('');
  $$('#radioList [data-play]').forEach(b=>b.onclick=()=>{
    const i=+b.dataset.play;
    radioPost(n.station===i&&live?{action:'stop'}:{action:'play',index:i});
  });
  $$('#radioList [data-edit]').forEach(b=>b.onclick=()=>{
    const i=+b.dataset.edit;radioEditIndex=i;
    $('#radioName').value=list[i].name;$('#radioEditUrl').value=list[i].url;
    $('#radioEditTitle').textContent=`Edit ${list[i].name}`;
    $('#radioCancel').style.display='';
    $('#radioName').focus();
  });
  $$('#radioList [data-up]').forEach(b=>b.onclick=()=>radioPost({action:'move',index:+b.dataset.up,up:true}));
  $$('#radioList [data-del]').forEach(b=>b.onclick=()=>{
    confirmDo('Remove this station?',`${esc(list[+b.dataset.del].name)} will be deleted from the favourites.`,
      ()=>radioPost({action:'delete',index:+b.dataset.del}));
  });
}

function radioClearEditor(){
  radioEditIndex=-1;$('#radioName').value='';$('#radioEditUrl').value='';
  $('#radioEditTitle').textContent='Add a station';$('#radioCancel').style.display='none';
}
async function loadRadio(){try{renderRadio(await api('/api/radio'))}catch(e){toast(e.message,true)}}
async function radioPost(body){try{renderRadio(await api('/api/radio',{method:'POST',body}))}catch(e){toast(e.message,true)}}

/* --- alarms and the sleep timer ---------------------------------------- */

const DAY_NAMES=['Sun','Mon','Tue','Wed','Thu','Fri','Sat'];
const ALARM_SOURCES=['Chime','Internet radio','Card folder'];

function fmtCountdown(sec){
  if(!sec)return '—';
  const h=Math.floor(sec/3600),m=Math.floor((sec%3600)/60);
  if(h>=24)return `${Math.floor(h/24)}d ${h%24}h`;
  if(h)return `${h}h ${m}m`;
  return m?`${m}m`:`${sec}s`;
}
function fmtDays(days){
  if(!days)return 'Once';
  if(days===0x7F)return 'Every day';
  if(days===0x3E)return 'Weekdays';
  if(days===0x41)return 'Weekends';
  return DAY_NAMES.filter((_,i)=>days&(1<<i)).join(' ');
}
function fmtWallTime(h,m){
  const d=new Date();d.setHours(h,m,0,0);
  return d.toLocaleTimeString([],{hour:'2-digit',minute:'2-digit'});
}

function renderAlarms(d){
  alarmCfg=d;
  const now=d.now||{},list=d.alarms||[];
  const ringing=now.state==='ringing',snoozed=now.state==='snoozed';

  $('#alarmDismiss').style.display=(ringing||snoozed)?'':'none';
  $('#alarmSnooze').style.display=ringing?'':'none';
  $('#alarmNextLine').innerHTML=ringing?'<b style="color:var(--mint)">An alarm is going off now.</b>'
    :snoozed?`Snoozed &mdash; back in ${fmtCountdown(now.snoozeLeftSeconds)}.`
    :now.next>=0?`Next alarm in ${fmtCountdown(now.nextInSeconds)}.`
    :'No alarm is armed.';

  $('#alarmEmpty').style.display=list.length?'none':'';
  $('#alarmAdd').style.display=list.length>=d.max?'none':'';
  $('#alarmList').innerHTML=list.map(a=>{
    const bits=[fmtDays(a.days),ALARM_SOURCES[a.source]||'Chime'];
    if(a.fadeSeconds)bits.push(`fades up over ${a.fadeSeconds<60?a.fadeSeconds+'s':Math.round(a.fadeSeconds/60)+' min'}`);
    if(a.skipNext)bits.push('skipping the next one');
    return `<div class="rowitem${a.enabled?' on':''}"><label class="switch" style="padding:0;border:0;background:none;width:auto;margin:0"><input type="checkbox" data-arm="${a.index}" ${a.enabled?'checked':''}></label><span class="grow"><b>${fmtWallTime(a.hour,a.minute)}${a.label?' · '+esc(a.label):''}</b><small>${esc(bits.join(' · '))}</small></span><span class="acts"><button class="btn ghost" data-aedit="${a.index}">Edit</button><button class="btn ghost" data-adel="${a.index}">&times;</button></span></div>`;
  }).join('');
  $$('#alarmList [data-arm]').forEach(i=>i.onchange=()=>alarmPost({action:'save',index:+i.dataset.arm,enabled:i.checked}));
  $$('#alarmList [data-aedit]').forEach(b=>b.onclick=()=>alarmEdit(+b.dataset.aedit));
  $$('#alarmList [data-adel]').forEach(b=>b.onclick=()=>confirmDo('Delete this alarm?','It will be removed from the speaker.',()=>{alarmCloseEditor();alarmPost({action:'delete',index:+b.dataset.adel})}));

  $('#sleepRunning').style.display=now.sleepRunning?'':'none';
  $('#sleepIdle').style.display=now.sleepRunning?'none':'';
  if(now.sleepRunning){
    const left=now.sleepLeftSeconds,total=now.sleepTotalSeconds||1;
    $('#sleepLeft').textContent=fmtCountdown(left);
    $('#sleepSub').textContent=left<=60?'Fading out…':`of ${Math.round(total/60)} minutes`;
    $('#sleepMeter').style.width=`${Math.max(0,Math.min(100,100*left/total))}%`;
  }
  $('#sleepStandby').checked=!!d.sleepStandbyDefault;

  if(alarmEditIndex>=-1&&alarmDraft)alarmPaintEditor();
}

function alarmEdit(index){
  const found=(alarmCfg.alarms||[]).find(a=>a.index===index);
  alarmEditIndex=index;
  alarmDraft=found?{...found}:{index:255,enabled:true,hour:7,minute:0,days:0x3E,source:0,
    target:0,volume:90,fadeSeconds:60,durationSeconds:1800,snoozeMinutes:9,skipNext:false,label:''};
  $('#alarmEditor').style.display='';
  $('#alarmEditTitle').textContent=found?'Edit alarm':'New alarm';
  $('#alarmTest').style.display=found?'':'none';
  alarmPaintEditor();
  $('#alarmEditor').scrollIntoView({behavior:'smooth',block:'nearest'});
}
function alarmCloseEditor(){alarmEditIndex=-1;alarmDraft=null;$('#alarmEditor').style.display='none'}

function alarmPaintEditor(){
  const a=alarmDraft;if(!a)return;
  $('#alarmTime').value=`${String(a.hour).padStart(2,'0')}:${String(a.minute).padStart(2,'0')}`;
  $('#alarmLabel').value=a.label||'';
  $('#alarmDays').innerHTML=DAY_NAMES.map((n,i)=>`<button data-day="${i}" class="${a.days&(1<<i)?'on':''}">${n[0]}${n[1]}</button>`).join('')
    +`<button data-daypreset="62" style="width:auto;padding:8px 12px">Weekdays</button><button data-daypreset="65" style="width:auto;padding:8px 12px">Weekends</button><button data-daypreset="127" style="width:auto;padding:8px 12px">Every day</button><button data-daypreset="0" style="width:auto;padding:8px 12px">Once</button>`;
  $$('#alarmDays [data-day]').forEach(b=>b.onclick=()=>{a.days^=(1<<+b.dataset.day);alarmPaintEditor()});
  $$('#alarmDays [data-daypreset]').forEach(b=>b.onclick=()=>{a.days=+b.dataset.daypreset;alarmPaintEditor()});

  $$('#alarmSource button').forEach(b=>b.classList.toggle('on',+b.dataset.src===a.source));
  const radioOff=!alarmCfg.radioAvailable,dfOff=!alarmCfg.dfAvailable;
  $('#alarmSourceHint').innerHTML=a.source===1
    ?(radioOff?'This mode has no internet radio, so this alarm would fall straight through to the chime.':'If the station will not connect within twenty seconds, the chime takes over.')
    :a.source===2
    ?(dfOff?'No DFPlayer is running in this mode, so this alarm would fall straight through to the chime.':'Plays the first track in the folder you pick.')
    :'A repeating two-note figure through the speaker itself. Works in every mode and needs nothing.';

  const targetRow=$('#alarmTargetRow');
  targetRow.style.display=a.source===0?'none':'';
  if(a.source===1){
    $('#alarmTargetLabel').textContent='Station';
    $('#alarmTarget').innerHTML=(alarmCfg.stations||[]).map((n,i)=>`<option value="${i}">${esc(n)}</option>`).join('')||'<option value="0">No stations saved</option>';
  }else if(a.source===2){
    $('#alarmTargetLabel').textContent='Folder on the card';
    $('#alarmTarget').innerHTML=Array.from({length:99},(_,i)=>`<option value="${i+1}">${String(i+1).padStart(2,'0')}</option>`).join('');
  }
  $('#alarmTarget').value=String(a.target||(a.source===2?1:0));

  $('#alarmVolume').value=a.volume;$('#alarmVolText').textContent=`${Math.round(a.volume/1.27)}%`;
  $('#alarmFade').value=a.fadeSeconds;
  $('#alarmFadeText').textContent=a.fadeSeconds?(a.fadeSeconds<60?`${a.fadeSeconds} seconds`:`${Math.round(a.fadeSeconds/60)} minutes`):'no fade';
  $('#alarmDuration').value=String(a.durationSeconds);
  $('#alarmSnoozeMins').value=String(a.snoozeMinutes);
  $('#alarmSkip').checked=!!a.skipNext;
}

async function loadAlarms(){try{renderAlarms(await api('/api/alarms'))}catch(e){toast(e.message,true)}}
async function alarmPost(body){try{renderAlarms(await api('/api/alarms',{method:'POST',body}))}catch(e){toast(e.message,true)}}

/* --- graphs ------------------------------------------------------------ */

function drawChart(id,axisId,values,flags,opts){
  const c=$(id);if(!c)return;
  /* Sized from the element rather than from an attribute, so the curve is not
     stretched on a narrow window or blurred on a high-density screen. */
  const rect=c.getBoundingClientRect(),dpr=window.devicePixelRatio||1;
  c.width=Math.max(1,Math.round(rect.width*dpr));
  c.height=Math.max(1,Math.round(rect.height*dpr));
  const ctx=c.getContext('2d');ctx.scale(dpr,dpr);
  const w=rect.width,h=rect.height,pad=6;
  ctx.clearRect(0,0,w,h);

  const real=values.filter(v=>v!==null&&v!==undefined);
  if(real.length<2){
    $(axisId).textContent='';
    ctx.fillStyle='#8fa0b3';ctx.font='13px system-ui';ctx.textAlign='center';
    ctx.fillText('Not enough history yet',w/2,h/2);
    return;
  }
  let lo=Math.min(...real),hi=Math.max(...real);
  /* A flat line needs a band around it or it sits on the floor of the chart and
     reads as zero. */
  if(hi-lo<opts.minSpan){const mid=(hi+lo)/2;lo=mid-opts.minSpan/2;hi=mid+opts.minSpan/2}
  const pad2=(hi-lo)*0.12;lo-=pad2;hi+=pad2;
  const xOf=i=>pad+(i/(values.length-1))*(w-pad*2);
  const yOf=v=>h-pad-((v-lo)/(hi-lo))*(h-pad*2);

  /* The stretches where something was playing, behind the curve. On a battery
     graph this is usually the whole explanation for a curve that steepens. */
  if(flags&&opts.shade){
    ctx.fillStyle='rgba(101,169,255,.10)';
    let start=-1;
    for(let i=0;i<=flags.length;i++){
      const on=i<flags.length&&(flags[i]&2);
      if(on&&start<0)start=i;
      if(!on&&start>=0){ctx.fillRect(xOf(start),pad,Math.max(1,xOf(i-1)-xOf(start)),h-pad*2);start=-1}
    }
  }

  ctx.strokeStyle='#223040';ctx.lineWidth=1;
  for(let k=0;k<=2;k++){const y=pad+((h-pad*2)*k)/2;ctx.beginPath();ctx.moveTo(pad,y);ctx.lineTo(w-pad,y);ctx.stroke()}

  ctx.beginPath();let started=false;
  values.forEach((v,i)=>{
    if(v===null||v===undefined){started=false;return}
    const x=xOf(i),y=yOf(v);
    started?ctx.lineTo(x,y):ctx.moveTo(x,y);started=true;
  });
  ctx.strokeStyle=opts.color;ctx.lineWidth=2;ctx.lineJoin='round';ctx.stroke();

  /* A dot on the newest sample, because "which end is now" is otherwise a
     convention the reader has to remember. */
  const lastIdx=values.length-1-[...values].reverse().findIndex(v=>v!==null&&v!==undefined);
  if(lastIdx>=0&&lastIdx<values.length){
    ctx.fillStyle=opts.color;ctx.beginPath();
    ctx.arc(xOf(lastIdx),yOf(values[lastIdx]),3,0,7);ctx.fill();
  }
  $(axisId).innerHTML=`${opts.fmt(hi)}<br>${opts.fmt(lo)}`;
}

function renderGraphs(d){
  graphData=d;
  const now=d.now||{};
  $('#gUptime').textContent=fmtUp(d.uptimeSeconds*1000);
  $('#gRuntime').textContent=d.runtimeSeconds>=3600?`${Math.floor(d.runtimeSeconds/3600)} h`:`${Math.round(d.runtimeSeconds/60)} min`;
  $('#gBoots').textContent=d.bootCount||'—';
  $('#gBlock').textContent=now.largestBlock?`${now.largestBlock} kB`:'—';

  const volts=(d.volts||[]).map(v=>v===null?null:+v);
  $('#gVoltsNow').textContent=now.volts?`${(+now.volts).toFixed(2)} V · ${now.percent}%${now.charging?' · charging':''}`:'No battery gauge is fitted or enabled.';
  drawChart('#gVoltsChart','#gVoltsAxis',volts,d.flags,{color:'#72f1b8',minSpan:0.15,shade:true,fmt:v=>`${v.toFixed(2)} V`});

  const temp=(d.temperature||[]).map(v=>v===null?null:+v);
  $('#gTempNow').textContent=now.temperature?`${(+now.temperature).toFixed(1)} °C at the die`:'The sensor did not answer.';
  drawChart('#gTempChart','#gTempAxis',temp,d.flags,{color:'#f8c76a',minSpan:2,shade:true,fmt:v=>`${v.toFixed(1)} °C`});

  $('#gHeapNow').textContent=now.heap?`${now.heap} kB free · ${now.heapLow} kB was the lowest since boot`:'—';
  drawChart('#gHeapChart','#gHeapAxis',d.heap||[],d.flags,{color:'#65a9ff',minSpan:8,shade:false,fmt:v=>`${Math.round(v)} kB`});

  const rssi=(d.rssi||[]).map(v=>v===null?null:+v);
  $('#gRssiNow').textContent=now.rssi?`${now.rssi} dBm`:'Not associated with a network.';
  drawChart('#gRssiChart','#gRssiAxis',rssi,d.flags,{color:'#ff6b7a',minSpan:6,shade:false,fmt:v=>`${Math.round(v)} dBm`});
}

async function loadGraphs(){try{renderGraphs(await api('/api/telemetry'))}catch(e){toast(e.message,true)}}

/* --- Home Assistant ---------------------------------------------------- */

function renderMqtt(d){
  mqttCfg=d;
  $('#mqttOffline').style.display=d.modeHasWifi?'none':'';
  $('#mqttEnabled').checked=d.enabled;
  $('#mqttHost').value=d.host||'';
  $('#mqttPort').value=d.port||1883;
  $('#mqttUser').value=d.user||'';
  $('#mqttPassword').placeholder=d.passwordSet?'Leave blank to keep current':'No password';
  $('#mqttTopic').value=d.baseTopic||'';
  $('#mqttDiscovery').checked=d.discovery;
  $('#mqttDiscoveryPrefix').value=d.discoveryPrefix||'homeassistant';
  $('#mqttPublish').value=String(d.publishSeconds||15);

  const label={off:'Off',unavailable:'Waiting for the network',connecting:'Connecting…',connected:'Connected',failed:'Could not connect'}[d.state]||d.state;
  $('#mqttState').textContent=label;
  $('#mqttStateHint').textContent=d.state==='connected'?'Connected to the broker.':label;
  $('#mqttUptime').textContent=d.connectedForSeconds?fmtUp(d.connectedForSeconds*1000):'—';
  $('#mqttConnects').textContent=d.connects||0;
  $('#mqttPublished').textContent=d.published||0;
  $('#mqttReceived').textContent=d.received||0;
  $('#mqttDisc').textContent=!d.discovery?'Switched off':d.discoveryDone?'Sent':'Pending';
  $('#mqttError').style.display=d.error?'':'none';
  $('#mqttError').textContent=d.error||'';
}
async function loadMqtt(){try{renderMqtt(await api('/api/mqtt'))}catch(e){toast(e.message,true)}}

/* --- the time-zone list ------------------------------------------------ */
/*
   Kept in the page rather than on the device. There is no zone database in the
   firmware -- it is 700 kB and it goes stale -- so the rules live in the half of
   the system that is rewritten by every firmware update anyway, and the speaker
   stores the one string it was handed.
*/
const ZONES=[['','Fixed offset (no daylight saving)'],
['GMT0BST,M3.5.0/1,M10.5.0','United Kingdom'],
['CET-1CEST,M3.5.0,M10.5.0/3','Central Europe (Paris, Berlin, Madrid)'],
['EET-2EEST,M3.5.0/3,M10.5.0/4','Eastern Europe (Athens, Helsinki)'],
['WET0WEST,M3.5.0/1,M10.5.0','Portugal'],
['MSK-3','Moscow'],
['EST5EDT,M3.2.0,M11.1.0','US Eastern'],
['CST6CDT,M3.2.0,M11.1.0','US Central'],
['MST7MDT,M3.2.0,M11.1.0','US Mountain'],
['MST7','Arizona'],
['PST8PDT,M3.2.0,M11.1.0','US Pacific'],
['AKST9AKDT,M3.2.0,M11.1.0','Alaska'],
['HST10','Hawaii'],
['AST4ADT,M3.2.0,M11.1.0','Atlantic Canada'],
['NST3:30NDT,M3.2.0,M11.1.0','Newfoundland'],
['<-03>3','Brazil (São Paulo)'],
['<-03>3','Argentina'],
['<-05>5','Colombia, Peru'],
['<-06>6','Mexico City'],
['<+0330>-3:30<+0430>,J80/0,J264/0','Iran'],
['<+04>-4','Dubai'],
['<+05>-5','Pakistan'],
['IST-5:30','India, Sri Lanka'],
['<+06>-6','Bangladesh'],
['<+07>-7','Thailand, Vietnam'],
['CST-8','China, Singapore, Hong Kong'],
['JST-9','Japan'],
['KST-9','Korea'],
['AEST-10AEDT,M10.1.0,M4.1.0/3','Sydney, Melbourne'],
['AEST-10','Brisbane'],
['ACST-9:30ACDT,M10.1.0,M4.1.0/3','Adelaide'],
['AWST-8','Perth'],
['NZST-12NZDT,M9.5.0,M4.1.0/3','New Zealand'],
['SAST-2','South Africa'],
['EAT-3','East Africa'],
['WAT-1','West Africa'],
['<+02>-2','Israel'],
['<+03>-3','Turkey, Saudi Arabia'],
['UTC0','UTC']];

function paintZones(){
  const sel=$('#clockZoneRule');if(!sel||sel.children.length)return;
  sel.innerHTML=ZONES.map(([tz,name])=>`<option value="${esc(tz)}">${esc(name)}</option>`).join('');
  sel.onchange=async()=>{
    try{await api('/api/clock',{method:'POST',body:{zone:sel.value}});toast('Time zone set');loadSettings()}
    catch(e){toast(e.message,true);loadSettings()}
  };
}
function paintZoneStatus(){
  if(!settings)return;
  paintZones();
  const sel=$('#clockZoneRule');
  if(sel&&document.activeElement!==sel)sel.value=settings.clockZone||'';
  const mins=settings.clockOffsetMinutes||0,sign=mins<0?'-':'+',abs=Math.abs(mins);
  $('#clockZoneNow').textContent=settings.clockZone?(settings.clockZoneAbbrev||'—'):'Fixed offset';
  $('#clockZoneDst').textContent=!settings.clockZone?'No rule installed':settings.clockDst?'In force':'Not in force';
  $('#clockZoneOffset').textContent=`UTC${sign}${String(Math.floor(abs/60)).padStart(2,'0')}:${String(abs%60).padStart(2,'0')}`;
  $('#clockZoneSource').textContent=settings.clockSource||'—';
}

/* --- wiring ------------------------------------------------------------ */

$('#soundRefresh').onclick=()=>loadAudio();
$('#eqEnabled').onchange=()=>saveAudio({eq:eqDraft()});
$('#eqAuto').onchange=()=>saveAudio({eq:eqDraft()});
$('#eqPreamp').oninput=()=>eqPaintLabels();
$('#eqPreamp').onchange=()=>saveAudio({eq:eqDraft()});
$('#eqSave').onclick=()=>saveAudio({eq:eqDraft()});
$('#eqFlat').onclick=()=>saveAudio({eq:{...eqDraft(),preset:0}});
$('#voiceEnabled').onchange=()=>saveAudio({voice:voiceDraft()});
$('#voiceVolume').oninput=()=>$('#voiceVolText').textContent=`${$('#voiceVolume').value}%`;
$('#voiceVolume').onchange=()=>saveAudio({voice:voiceDraft()});
$('#voiceDuck').oninput=()=>{const v=+$('#voiceDuck').value;$('#voiceDuckText').textContent=v?`${v}%`:'not at all'};
$('#voiceDuck').onchange=()=>saveAudio({voice:voiceDraft()});
$('#voiceSave').onclick=()=>saveAudio({voice:voiceDraft()});
$('#soundVolume').oninput=()=>{soundVolBusy=Date.now();$('#soundVolumeText').textContent=`${Math.round($('#soundVolume').value/1.27)}%`};
$('#soundVolume').onchange=async()=>{
  try{await api('/api/media',{method:'POST',body:{action:'volume',value:+$('#soundVolume').value}})}
  catch(e){toast(e.message,true)}
  soundVolBusy=0;refresh();
};

$('#radioRefresh').onclick=()=>loadRadio();
$('#radioToggle').onclick=()=>radioPost({action:'toggle'});
$('#radioNext').onclick=()=>radioPost({action:'next'});
$('#radioPrev').onclick=()=>radioPost({action:'previous'});
$('#radioVolume').oninput=()=>{radioVolBusy=Date.now();$('#radioVolText').textContent=`${Math.round($('#radioVolume').value/1.27)}%`};
$('#radioVolume').onchange=()=>{radioVolBusy=0;radioPost({action:'volume',value:+$('#radioVolume').value})};
$('#radioAutostart').onchange=()=>radioPost({action:'autostart',value:$('#radioAutostart').checked});
$('#radioPlayUrl').onclick=()=>{
  const url=$('#radioUrl').value.trim();
  if(!url)return toast('Paste a stream address first',true);
  radioPost({action:'url',url});
};
$('#radioSave').onclick=()=>{
  const url=$('#radioEditUrl').value.trim();
  if(!url)return toast('A station needs a stream address',true);
  radioPost({action:'save',index:radioEditIndex<0?255:radioEditIndex,
             name:$('#radioName').value.trim(),url}).then(radioClearEditor);
};
$('#radioCancel').onclick=()=>radioClearEditor();

$('#alarmRefresh').onclick=()=>loadAlarms();
$('#alarmAdd').onclick=()=>alarmEdit(255);
$('#alarmCancel').onclick=()=>alarmCloseEditor();
$('#alarmDismiss').onclick=()=>alarmPost({action:'dismiss'});
$('#alarmSnooze').onclick=()=>alarmPost({action:'snooze'});
$('#alarmTest').onclick=()=>{if(alarmEditIndex>=0&&alarmEditIndex<255)alarmPost({action:'test',index:alarmEditIndex})};
$$('#alarmSource button').forEach(b=>b.onclick=()=>{
  if(!alarmDraft)return;
  alarmDraft.source=+b.dataset.src;
  alarmDraft.target=alarmDraft.source===2?1:0;
  alarmPaintEditor();
});
$('#alarmTarget').onchange=()=>{if(alarmDraft)alarmDraft.target=+$('#alarmTarget').value};
$('#alarmVolume').oninput=()=>{if(alarmDraft){alarmDraft.volume=+$('#alarmVolume').value;$('#alarmVolText').textContent=`${Math.round(alarmDraft.volume/1.27)}%`}};
$('#alarmFade').oninput=()=>{if(alarmDraft){alarmDraft.fadeSeconds=+$('#alarmFade').value;
  $('#alarmFadeText').textContent=alarmDraft.fadeSeconds?(alarmDraft.fadeSeconds<60?`${alarmDraft.fadeSeconds} seconds`:`${Math.round(alarmDraft.fadeSeconds/60)} minutes`):'no fade'}};
$('#alarmSkip').onchange=()=>{if(alarmDraft)alarmDraft.skipNext=$('#alarmSkip').checked};
$('#alarmSave').onclick=()=>{
  if(!alarmDraft)return;
  const [h,m]=($('#alarmTime').value||'07:00').split(':').map(Number);
  alarmPost({action:'save',index:alarmEditIndex<0?255:alarmEditIndex,enabled:true,
    hour:h,minute:m,days:alarmDraft.days,source:alarmDraft.source,
    target:+$('#alarmTarget').value||0,volume:+$('#alarmVolume').value,
    fadeSeconds:+$('#alarmFade').value,durationSeconds:+$('#alarmDuration').value,
    snoozeMinutes:+$('#alarmSnoozeMins').value,skipNext:$('#alarmSkip').checked,
    label:$('#alarmLabel').value.trim()}).then(alarmCloseEditor);
};
$$('#sleepPresets button').forEach(b=>b.onclick=()=>alarmPost({action:'sleep',minutes:+b.dataset.min,standby:$('#sleepStandby').checked}));
$('#sleepStart').onclick=()=>{
  const m=+$('#sleepCustom').value;
  if(!m)return toast('How many minutes?',true);
  alarmPost({action:'sleep',minutes:m,standby:$('#sleepStandby').checked});
};
$('#sleepPlus').onclick=()=>alarmPost({action:'sleepExtend',minutes:15});
$('#sleepCancel').onclick=()=>alarmPost({action:'sleepCancel'});
$('#sleepStandby').onchange=()=>alarmPost({action:'sleepStandbyDefault',value:$('#sleepStandby').checked});

$('#alarmOvStop').onclick=()=>alarmPost({action:'dismiss'});
$('#alarmOvSnooze').onclick=()=>alarmPost({action:'snooze'});
$('#alarmOvSleepCancel').onclick=()=>alarmPost({action:'sleepCancel'});
$('#graphRefresh').onclick=()=>loadGraphs();
$('#mqttRefresh').onclick=()=>loadMqtt();
$('#mqttAnnounce').onclick=async()=>{
  try{await api('/api/mqtt',{method:'POST',body:{action:'announce'}});toast('Sent to Home Assistant')}
  catch(e){toast(e.message,true)}
};
$('#mqttSave').onclick=async()=>{
  const body={enabled:$('#mqttEnabled').checked,host:$('#mqttHost').value.trim(),
    port:+$('#mqttPort').value||1883,user:$('#mqttUser').value.trim(),
    baseTopic:$('#mqttTopic').value.trim(),discovery:$('#mqttDiscovery').checked,
    discoveryPrefix:$('#mqttDiscoveryPrefix').value.trim(),publishSeconds:+$('#mqttPublish').value};
  /* Only send the password when one was typed: an empty field means "leave the
     stored one alone", and the field starts empty on every load because the
     firmware never sends it back. */
  const pw=$('#mqttPassword').value;
  if(pw)body.password=pw;
  try{renderMqtt(await api('/api/mqtt',{method:'POST',body}));$('#mqttPassword').value='';toast('Saved')}
  catch(e){toast(e.message,true)}
};
$('#mqttEnabled').onchange=()=>$('#mqttSave').click();

/* Charts are drawn at the size of their element, so a resized window needs a
   repaint. Debounced, because a drag fires this continuously. */
let graphResize;
addEventListener('resize',()=>{
  clearTimeout(graphResize);
  graphResize=setTimeout(()=>{if(graphData&&$('#page-graphs').classList.contains('active'))renderGraphs(graphData)},200);
});


/* --- keeping the new pages live ---------------------------------------- */
/*
   The two-second status poll already runs; these hang off it rather than
   starting timers of their own, so a dashboard left open on the Radio page
   makes two requests every two seconds instead of one plus a second poll that
   nobody remembered to stop when the page changed.

   Only the visible page is fetched. The graphs are the exception: two hours of
   history is the largest document this firmware assembles, and refetching it
   every two seconds would have the speaker building 4 kB of JSON continuously
   for a picture that changes once a minute.
*/
let pageTick=0;
function refreshActivePage(s){
  if($('#page-sound').classList.contains('active')){
    /* The transport on the Sound page mirrors the Overview's, and both are
       driven from the status document rather than from a second request. */
    const src=s.radio&&s.radio.state&&s.radio.state!=='idle'?'radio'
             :(s.mode&&s.mode.dfplayer)?'card':'bluetooth';
    const m=s.media||{};
    $('#soundNow').textContent=m.title||{radio:'Internet radio',card:'The card',bluetooth:'Bluetooth'}[src];
    $('#soundNowSub').textContent=m.artist||(s.radio&&s.radio.name)||' ';
    if(!soundVolBusy||Date.now()-soundVolBusy>2500){
      const v=src==='radio'&&s.radio?s.radio.volume:m.volume;
      if(typeof v==='number'){$('#soundVolume').value=v;$('#soundVolumeText').textContent=`${Math.round(v/1.27)}%`}
    }
  }
  /* The page you are looking at, and only that page. The Radio and Alarms
     documents are small and carry numbers that move by the second -- a buffer
     level, a countdown -- so they keep the two-second cadence. The broker's
     connection statistics do not move like that, and two hours of history moves
     once a minute, so both are fetched far more slowly. */
  if($('#page-radio').classList.contains('active'))loadRadio();
  if($('#page-alarms').classList.contains('active'))loadAlarms();
  if($('#page-hass').classList.contains('active')&&(pageTick%5)===0)loadMqtt();
  if($('#page-graphs').classList.contains('active')&&(pageTick%30)===0)loadGraphs();
  pageTick++;
}

/* --- the radio on the Overview ----------------------------------------- */
function renderRadioOverview(s){
  const r=s.radio||{};
  const card=$('#radioCard');
  if(!card)return;
  /* Only shown in the modes that have a radio, and only once there is
     something to say -- an empty card in Bluetooth mode is a card that has to
     be explained. */
  const show=r.available&&r.state&&r.state!=='idle';
  card.style.display=show?'':'none';
  if(!show)return;
  const live=r.state==='playing'||r.state==='buffering';
  $('#radioOvName').textContent=r.name||'Internet radio';
  $('#radioOvDetail').textContent=r.title||r.error||
    {connecting:'Opening the stream…',buffering:`Buffering — ${r.buffer||0}% of the way there`,
     reconnecting:'The stream dropped; trying again…'}[r.state]||
    (r.bitrate?`${r.codec} · ${r.bitrate} kbps`:'Playing');
  $('#radioOvBadge').textContent={playing:'Playing',buffering:'Buffering',connecting:'Connecting',
    reconnecting:'Reconnecting',error:'Stopped'}[r.state]||r.state;
  $('#radioOvBadge').className='badge '+(r.state==='playing'?'good':'');
  $('#radioOvBuf').style.width=`${r.buffer||0}%`;
  $('#radioOvBufWrap').classList.toggle('warn',live&&(r.buffer||0)<33);
}

/* --- the alarm and sleep timer on the Overview -------------------------- */
function renderAlarmOverview(s){
  const a=s.alarm||{},card=$('#alarmCard');
  if(!card)return;
  const show=a.state!=='idle'||a.sleepRunning||a.next>=0;
  card.style.display=show?'':'none';
  if(!show)return;
  if(a.state==='ringing'){
    $('#alarmOvBig').textContent='Alarm';
    $('#alarmOvDetail').textContent='Ringing now.';
    $('#alarmOvBadge').textContent='Ringing';
    $('#alarmOvBadge').className='badge good';
  }else if(a.state==='snoozed'){
    $('#alarmOvBig').textContent=fmtCountdown(a.snoozeLeftSeconds);
    $('#alarmOvDetail').textContent='Snoozed.';
    $('#alarmOvBadge').textContent='Snoozed';
    $('#alarmOvBadge').className='badge';
  }else if(a.sleepRunning){
    $('#alarmOvBig').textContent=fmtCountdown(a.sleepLeftSeconds);
    $('#alarmOvDetail').textContent=a.sleepLeftSeconds<=60?'Sleep timer — fading out…':'Left on the sleep timer.';
    $('#alarmOvBadge').textContent='Sleep timer';
    $('#alarmOvBadge').className='badge good';
  }else{
    $('#alarmOvBig').textContent=fmtCountdown(a.nextInSeconds);
    $('#alarmOvDetail').textContent='Until the next alarm.';
    $('#alarmOvBadge').textContent='Armed';
    $('#alarmOvBadge').className='badge';
  }
  $('#alarmOvStop').style.display=(a.state==='ringing'||a.state==='snoozed')?'':'none';
  $('#alarmOvSnooze').style.display=a.state==='ringing'?'':'none';
  $('#alarmOvSleepCancel').style.display=a.sleepRunning?'':'none';
}

function page(name){$$('[data-page]').forEach(b=>b.classList.toggle('active',b.dataset.page===name));$$('.page').forEach(p=>p.classList.toggle('active',p.id===`page-${name}`));$('#pageTitle').textContent=name[0].toUpperCase()+name.slice(1);$('#eyebrow').textContent={overview:'Your audio, at a glance',devices:'Pairing and connections',media:'The card, the drive and the module',wifi:'Network management',updates:'Reliable A/B firmware',settings:'Make it yours',lighting:'Colour, motion and music',sound:'Tone, level and what it says out loud',radio:'Stations from the network',alarms:'Waking up, and going to sleep',graphs:'Two hours of history',hass:'MQTT and home automation'}[name];
if(name==='hass')$('#pageTitle').textContent='Home Assistant';if(name==='devices')loadDevices();if(name==='settings')loadSettings();if(name==='lighting')loadLighting();
if(name==='sound')loadAudio();
if(name==='radio')loadRadio();
if(name==='alarms'){loadAlarms();loadSettings()}
if(name==='graphs')loadGraphs();
if(name==='hass')loadMqtt();if(name==='media'){if(status)renderDfPage(status.dfplayer||{});refresh();loadDfLibrary()}else{dfLibPoll(false)}if(name==='settings'&&settings&&settings.power&&settings.power.wokeFromSleep)toast('This speaker woke from standby')}
$$('[data-page]').forEach(b=>b.onclick=()=>page(b.dataset.page));
$('#loginForm').onsubmit=async e=>{e.preventDefault();let raw=`admin:${$('#loginPassword').value}`;auth='Basic '+btoa(unescape(encodeURIComponent(raw)));try{await api('/api/auth');sessionStorage.setItem('speakerAuth',auth);$('#loginModal').classList.remove('show');$('#loginError').textContent='';await refresh();loadSettings();clearInterval(pollTimer);pollTimer=setInterval(refresh,2000)}catch(err){auth='';$('#loginError').textContent='Incorrect password. Please try again.'}};
async function media(action,value){try{await api('/api/media',{method:'POST',body:{action,value}});setTimeout(refresh,180)}catch(e){toast(e.message,true)}}
$$('[data-media]').forEach(b=>b.onclick=()=>media(b.dataset.media));$('#volume').oninput=e=>{$('#volumeText').textContent=`${Math.round(e.target.value/127*100)}%`;clearTimeout(volumeTimer);volumeTimer=setTimeout(()=>media('volume',+e.target.value),80)};$('#refresh').onclick=()=>refresh(true);async function df(action,extra={}){try{await api('/api/dfplayer',{method:'POST',body:{action,...extra}});setTimeout(refresh,220)}catch(e){toast(e.message,true)}}$$('[data-dfsource]').forEach(b=>b.onclick=()=>dfSource(b.dataset.dfsource));$$('[data-dfsrc]').forEach(b=>b.onclick=()=>dfSource(b.dataset.dfsrc));async function dfSource(v){dfOpenFolder=0;dfStarted=null;await df('source',{value:v});setTimeout(loadDfLibrary,600)}$$('[data-dfeq]').forEach(b=>b.onclick=()=>df('eq',{value:+b.dataset.dfeq}));$$('[data-dfloop]').forEach(b=>b.onclick=()=>df('loop',{value:b.dataset.dfloop,folder:+$('#dfFolderNo').value||1}));$$('[data-dfpin]').forEach(b=>b.onclick=()=>df('pin',{pin:b.dataset.dfpin,long:!!b.dataset.dflong}));$$('[data-dfled]').forEach(b=>b.onclick=()=>df('led',{value:b.dataset.dfled}));$('#dfRefresh').onclick=()=>df('refresh');$('#dfScan').onclick=async()=>{dfOpenFolder=0;await df('scan');toast('Scanning the card, folder by folder…');setTimeout(loadDfLibrary,300);dfLibPoll(true)};$('#dfReset').onclick=()=>confirmDo('Reset the DFPlayer?','Playback stops and the module re-runs its whole start-up sequence, which takes a couple of seconds.',()=>df('reset'));$('#dfStandby').onclick=()=>df('standby');$('#dfWake').onclick=()=>df('wake');$('#dfPlayTrack').onclick=()=>df('track',{value:+$('#dfTrackNo').value});$('#dfPlayMp3').onclick=()=>df('mp3',{value:+$('#dfMp3No').value});$('#dfAdvert').onclick=()=>df('advert',{value:+$('#dfAdvertNo').value});$('#dfAdvertStop').onclick=()=>df('advertStop');$('#dfPlayFolder').onclick=()=>df('folder',{folder:+$('#dfFolderNo').value,file:+$('#dfFileNo').value});$('#dfFolderNo').oninput=e=>{clearTimeout(e.target._t);e.target._t=setTimeout(()=>{let f=+e.target.value;if(f>=1&&f<=99)df('queryFolder',{folder:f})},450)};$('#dfVolume').oninput=e=>{$('#dfVolText').textContent=`${e.target.value} / ${e.target.max}`;clearTimeout(e.target._t);e.target._t=setTimeout(()=>df('volumeRaw',{value:+e.target.value}),110)};$('#dfVolUp').onclick=()=>df('volumeStep',{up:true});$('#dfVolDown').onclick=()=>df('volumeStep',{up:false});$('#dfDac').onchange=e=>df('dac',{value:e.target.checked});$('#dfSaveDefaults').onclick=async()=>{await df('saveDefaults',{autoplay:$('#dfDefAutoplay').checked});toast('Saved. These are sent to the module at every boot.');loadSettings()};$('#dfDefVolume').oninput=e=>$('#dfDefVolText').textContent=`${e.target.value} / ${e.target.max}`;$('#batCalibrate').onclick=async()=>{let v=+$('#batActual').value;if(!v)return toast('Type the voltage your meter reads first',true);try{let r=await api('/api/battery',{method:'POST',body:{action:'calibrate',volts:v}});toast(`Trim ${(+r.calibration).toFixed(4)} stored`);$('#batActual').value='';loadSettings();refresh()}catch(e){toast(e.message,true)}};
$('#reloadDevices').onclick=loadDevices;$('#scanWifi').onclick=async()=>{let b=$('#scanWifi'),old=b.textContent;b.disabled=true;b.innerHTML='<i class="spinner"></i> Scanning';try{let d=await api('/api/wifi/scan'),box=$('#networks');box.innerHTML=d.networks.length?d.networks.map(n=>`<div class="network" data-ssid="${esc(n.ssid)}"><b>${esc(n.ssid||'(hidden network)')}</b><small>Channel ${n.channel} · ${n.secure?'Secured':'Open'}</small><span class="signal">${n.rssi} dBm</span></div>`).join(''):'<div class="empty">No networks found</div>';$$('[data-ssid]').forEach(n=>n.onclick=()=>{$('#wifiSsid').value=n.dataset.ssid;$('#wifiPassword').focus()})}catch(e){toast(e.message,true)}finally{b.disabled=false;b.textContent=old;icons()}};
$('#wifiForm').onsubmit=e=>{e.preventDefault();confirmDo('Change Wi-Fi network?',`The speaker will restart and connect to “${$('#wifiSsid').value}”.`,async()=>{try{await api('/api/wifi',{method:'POST',body:{ssid:$('#wifiSsid').value,password:$('#wifiPassword').value}});toast('Saved. Speaker is restarting…')}catch(e){toast(e.message,true)}})};
$('#checkUpdate').onclick=()=>updateAction('check');$('#overviewInstall').onclick=()=>{page('updates');confirmInstall()};$('#installUpdate').onclick=confirmInstall;function confirmInstall(){confirmDo('Install firmware update?','Playback will pause and the speaker will restart when installation completes.',()=>updateAction('install'))}
async function updateAction(kind){try{await api(`/api/update/${kind}`,{method:'POST'});toast(kind==='check'?'Checking GitHub…':'Update started');refresh()}catch(e){toast(e.message,true)}}
const dz=$('#dropzone'),file=$('#firmwareFile');dz.onclick=()=>file.click();dz.onkeydown=e=>{if(e.key==='Enter'||e.key===' ')file.click()};['dragenter','dragover'].forEach(n=>dz.addEventListener(n,e=>{e.preventDefault();dz.classList.add('over')}));['dragleave','drop'].forEach(n=>dz.addEventListener(n,e=>{e.preventDefault();dz.classList.remove('over')}));dz.ondrop=e=>selectFile(e.dataTransfer.files[0]);file.onchange=e=>selectFile(e.target.files[0]);function selectFile(f){if(!f)return;if(!f.name.toLowerCase().endsWith('.bin'))return toast('Choose a .bin firmware file',true);selectedFile=f;$('#fileName').textContent=`${f.name} · ${fmtBytes(f.size)}`;$('#uploadFirmware').disabled=false}
$('#uploadFirmware').onclick=()=>confirmDo('Install uploaded firmware?','The file will be written to the inactive OTA slot, then the speaker will restart.',uploadFirmware);function uploadFirmware(){let x=new XMLHttpRequest,form=new FormData;form.append('firmware',selectedFile);x.open('POST','/api/update/upload');x.setRequestHeader('Authorization',auth);x.setRequestHeader('X-Firmware-Size',selectedFile.size);x.upload.onprogress=e=>{if(e.lengthComputable){$('#updateProgress').style.width=`${e.loaded/e.total*100}%`;$('#updateMessage').textContent=`Uploading ${Math.round(e.loaded/e.total*100)}%`}};x.onload=()=>{let d={};try{d=JSON.parse(x.responseText)}catch{};x.status<300?toast('Firmware installed. Restarting…'):toast(d.error||'Upload failed',true)};x.onerror=()=>toast('Upload connection failed',true);x.send(form)}
$('#saveSettings').onclick=async()=>{let body={deviceName:$('#deviceName').value,hostname:$('#hostname').value,apAlways:$('#apAlways').checked,githubRepo:$('#githubRepo').value.trim(),githubAsset:$('#githubAsset').value.trim()||'*.bin',clearGithubToken:$('#clearGithubToken').checked,dfSource:+$('#dfDefSource').value,dfVolume:+$('#dfDefVolume').value,dfEq:+$('#dfDefEq').value,dfLoop:+$('#dfDefLoop').value,dfLoopFolder:+$('#dfDefLoopFolder').value,dfAutoplay:$('#dfDefAutoplay').checked,batteryEnabled:$('#batEnabled').checked,batteryCells:+$('#batCells').value,batteryDivider:+$('#batDivider').value,batteryFull:+$('#batFull').value,batteryEmpty:+$('#batEmpty').value,batteryLow:+$('#batLow').value,batteryCritical:+$('#batCritical').value};if($('#adminPassword').value)body.adminPassword=$('#adminPassword').value;if($('#apPassword').value)body.apPassword=$('#apPassword').value;if($('#githubToken').value)body.githubToken=$('#githubToken').value;try{await api('/api/settings',{method:'POST',body});toast('Settings saved. Restart to apply identity changes.');$('#adminPassword').value=$('#apPassword').value=$('#githubToken').value='';loadSettings()}catch(e){toast(e.message,true)}};
/*
 * Settings backup and restore.
 *
 * The download cannot be a plain link, and is not a GET: every endpoint here
 * wants the dashboard password in an Authorization header, which a link would
 * arrive without, and the backup passphrase must not travel in a query string
 * where it would land in browser history and in logs. So it is a POST read back
 * as a blob, named here rather than from the Content-Disposition header.
 */
$('#backupSettings').onclick=async()=>{const b=$('#backupSettings'),old=b.textContent,pass=$('#backupPass').value;if(pass&&pass.length<8)return toast('A backup passphrase must be at least 8 characters',true);b.disabled=true;b.innerHTML='<i class="spinner"></i> '+(pass?'Encrypting':'Preparing');try{const r=await fetch('/api/settings/backup',{method:'POST',headers:{Authorization:auth,'Content-Type':'application/json'},body:JSON.stringify({passphrase:pass})});if(r.status===401){sessionStorage.removeItem('speakerAuth');$('#loginModal').classList.add('show');throw Error('Sign in required')}if(!r.ok){let d={};try{d=await r.json()}catch{}throw Error(d.error||`Backup failed (${r.status})`)}const text=await r.text(),name=`${(settings&&settings.hostname)||'esp32-blue-spk'}-settings-${new Date().toISOString().slice(0,10)}.json`,url=URL.createObjectURL(new Blob([text],{type:'application/json'})),a=document.createElement('a');a.href=url;a.download=name;document.body.appendChild(a);a.click();a.remove();setTimeout(()=>URL.revokeObjectURL(url),1000);toast(pass?`Saved ${name} — keep the passphrase, it cannot be recovered`:`Saved ${name} — without the secrets`)}catch(e){toast(e.message,true)}finally{b.disabled=false;b.textContent=old}};
/*
 * The file is read and parsed here before anything is sent, so the wrong file
 * picked by mistake costs nothing -- and so the card can say which speaker and
 * which firmware the backup came from, and whether it needs a passphrase, while
 * there is still time to change your mind. The parsed object goes to the API,
 * not the raw text, because api() stringifies whatever it is given and a string
 * would arrive double-encoded.
 */
let restoreDoc=null;const rdz=$('#restoreDrop'),rfile=$('#restoreFile');rdz.onclick=()=>rfile.click();rdz.onkeydown=e=>{if(e.key==='Enter'||e.key===' ')rfile.click()};['dragenter','dragover'].forEach(n=>rdz.addEventListener(n,e=>{e.preventDefault();rdz.classList.add('over')}));['dragleave','drop'].forEach(n=>rdz.addEventListener(n,e=>{e.preventDefault();rdz.classList.remove('over')}));rdz.ondrop=e=>pickRestore(e.dataTransfer.files[0]);rfile.onchange=e=>pickRestore(e.target.files[0]);
async function pickRestore(f){restoreDoc=null;$('#restoreSettings').disabled=true;$('#restorePassRow').style.display='none';$('#restoreFileName').textContent='No file selected';if(!f)return;let text;try{text=await f.text()}catch{return toast('That file could not be read',true)}let d;try{d=JSON.parse(text)}catch{return toast('That file is not valid JSON',true)}if(!d||typeof d!=='object'||!d.settings)return toast('That file is not a settings backup',true);restoreDoc=d;$('#restoreSettings').disabled=false;const enc=!!d.secrets;$('#restorePassRow').style.display=enc?'':'none';const when=d.createdEpoch?new Date(d.createdEpoch*1000).toISOString().slice(0,10):'date unknown';$('#restoreFileName').textContent=`${d.device||f.name} · firmware ${d.firmware||'?'} · ${when} · ${enc?'encrypted secrets':'no secrets'}`}
$('#restoreSettings').onclick=()=>{if(restoreDoc&&restoreDoc.secrets&&!$('#restorePass').value)return toast('This backup is encrypted — enter its passphrase',true);confirmDo('Restore these settings?','Every stored setting the file carries is overwritten — the Wi-Fi network and, if the backup is encrypted, both passwords — and the speaker restarts. If the backup carries a different dashboard password, signing back in needs that one.',doRestore)};
async function doRestore(){const b=$('#restoreSettings'),old=b.textContent;b.disabled=true;b.innerHTML='<i class="spinner"></i> Restoring';try{const body={...restoreDoc};if(restoreDoc.secrets)body.passphrase=$('#restorePass').value;const r=await api('/api/settings/restore',{method:'POST',body});$('#restorePass').value='';toast(r.message||'Settings restored; restarting…')}catch(e){toast(e.message,true)}finally{b.disabled=false;b.textContent=old}}
function renderPowerMode(mode){$$('[data-power]').forEach(b=>b.classList.toggle('on',+b.dataset.power===mode));$('#powerThresholdRow').classList.toggle('off',mode!==2)}
async function powerPatch(body){try{await api('/api/settings',{method:'POST',body})}catch(e){toast(e.message,true)}finally{loadSettings()}}
$$('[data-power]').forEach(b=>b.onclick=()=>{const m=+b.dataset.power;renderPowerMode(m);powerPatch({powerMode:m,powerThreshold:+$('#powerThreshold').value});toast(m===0?'Power saving off':(m===1?'Power saving on':'Power saving follows the battery'))});
// Debounced like every other slider here: dragging it should not be one POST
// per pixel, and the effects apply the moment the value lands anyway.
$('#powerThreshold').oninput=e=>{$('#powerThresholdText').textContent=`${e.target.value}%`;clearTimeout(e.target._t);e.target._t=setTimeout(()=>powerPatch({powerMode:2,powerThreshold:+e.target.value}),260)};
function renderSleepMode(mode){$$('[data-sleep]').forEach(b=>b.classList.toggle('on',+b.dataset.sleep===mode));$('#sleepTimeoutRow').classList.toggle('off',mode===0)}
$$('[data-sleep]').forEach(b=>b.onclick=()=>{const m=+b.dataset.sleep;renderSleepMode(m);powerPatch({sleepMode:m,sleepAfterSeconds:+$('#sleepAfter').value});toast(m===0?'Standby off':(m===1?'Standby after the timeout':'Standby only while saving'))});
$('#sleepAfter').onchange=e=>powerPatch({sleepAfterSeconds:+e.target.value});
$('#standbyNow').onclick=()=>confirmDo('Put the speaker into standby?','Everything goes dark, both radios come down and playback stops. Press BOOT on the speaker to bring it back — it restarts, so give it a few seconds.',async()=>{try{const r=await api('/api/system',{method:'POST',body:{action:'standby'}});toast(r.message||'Going into standby')}catch(e){toast(e.message,true)}});
function renderOledBlank(mode){$$('[data-blank]').forEach(b=>b.classList.toggle('on',+b.dataset.blank===mode));$('#oledTimeoutRow').classList.toggle('off',mode===0)}
// The panel settings go through /api/settings rather than /api/display, which
// only carries the actions that change nothing on disk. They still apply at
// once, so the Save button is not part of the deal.
async function oledBlank(body){try{await api('/api/settings',{method:'POST',body});loadSettings()}catch(e){toast(e.message,true);loadSettings()}}
$$('[data-blank]').forEach(b=>b.onclick=()=>{const m=+b.dataset.blank;renderOledBlank(m);oledBlank({oledBlankMode:m,oledBlankAfterSeconds:+$('#oledTimeout').value});toast(m===0?'The panel stays on':(m===1?'The panel switches off when idle':'The panel switches off on a timer'))});
$('#oledTimeout').onchange=e=>oledBlank({oledBlankMode:+($$('[data-blank]').find(b=>b.classList.contains('on'))||{dataset:{blank:0}}).dataset.blank,oledBlankAfterSeconds:+e.target.value});
$('#ledIdleOff').onchange=e=>{$('#ledIdleRow').classList.toggle('off',!e.target.checked);if(ledCfg)ledCfg.idleOff=e.target.checked;sendLeds({idleOff:e.target.checked,idleAfterSeconds:+$('#ledIdleAfter').value});toast(e.target.checked?'The ring rests when idle':'The ring stays lit')};
$('#ledIdleAfter').onchange=e=>sendLeds({idleOff:$('#ledIdleOff').checked,idleAfterSeconds:+e.target.value});
$$('[data-screen]').forEach(b=>b.onclick=()=>display('screen',+b.dataset.screen));$$('[data-display]').forEach(b=>b.onclick=()=>display(b.dataset.display));async function display(action,value){try{await api('/api/display',{method:'POST',body:{action,value}});toast('Display updated')}catch(e){toast(e.message,true)}}$('#brightness').oninput=e=>{$('#brightnessValue').textContent=+e.target.value?e.target.value:'Auto';clearTimeout(e.target._t);e.target._t=setTimeout(()=>display('brightness',+e.target.value),130)};
async function clockPref(body,msg){try{await api('/api/clock',{method:'POST',body});if(msg)toast(msg)}catch(e){toast(e.message,true)}finally{refresh();loadSettings()}}$('#syncClock').onclick=()=>{let d=new Date;clockPref({year:d.getFullYear(),month:d.getMonth()+1,day:d.getDate(),hour:d.getHours(),minute:d.getMinutes(),second:d.getSeconds(),offsetMinutes:-d.getTimezoneOffset()},'Clock synchronized')};$$('[data-clockfmt]').forEach(b=>b.onclick=()=>{let h24=b.dataset.clockfmt==='24';$$('[data-clockfmt]').forEach(x=>x.classList.toggle('on',x===b));if(devClock){devClock.h24=h24;drawClock()}clockPref({use24h:h24},h24?'24-hour clock':'12-hour clock')});$('#clockAutoSync').onchange=e=>clockPref({autoSync:e.target.checked},e.target.checked?'Network time sync on':'Network time sync off; the speaker keeps the time you set');$$('[data-mode]').forEach(el=>el.onclick=()=>{let n=+el.dataset.mode,t=MODES[n];confirmDo('Switch to '+t.name+'?',t.warn,async()=>{try{let r=await api('/api/system',{method:'POST',body:{action:'mode',mode:n}});toast(r.message)}catch(e){n===1?toast('Switching to Bluetooth mode; this page is now offline.'):toast(e.message,true)}})});$('#reboot').onclick=()=>confirmDo('Restart speaker?','Bluetooth and the dashboard will be unavailable briefly.',()=>system('reboot'));$('#factoryReset').onclick=()=>confirmDo('Factory reset everything?','This permanently clears Wi-Fi credentials, dashboard settings, and every Bluetooth bond.',()=>system('factoryReset'));async function system(action){try{await api('/api/system',{method:'POST',body:{action}});toast('Speaker is restarting…')}catch(e){toast(e.message,true)}}
function confirmDo(title,text,fn){$('#confirmTitle').textContent=title;$('#confirmText').textContent=text;confirmAction=fn;$('#confirmModal').classList.add('show')}$('#cancelConfirm').onclick=()=>$('#confirmModal').classList.remove('show');$('#acceptConfirm').onclick=()=>{let fn=confirmAction;$('#confirmModal').classList.remove('show');confirmAction=null;if(fn)fn()};
if(auth){api('/api/auth').then(()=>{$('#loginModal').classList.remove('show');refresh();loadSettings();pollTimer=setInterval(refresh,2000)}).catch(()=>{})}
</script></body></html>
)DASH";
