/* Exercise the actual browser adapter with a small DOM/MSE stub and synthetic
 * MPEG-TS packets. No browser, network, recording fixture or npm install needed.
 */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const PACKET = 188, WRAP = 2 ** 33;

function packet(pid, payload, key = false) {
    const b = new Uint8Array(PACKET).fill(255);
    b.set([0x47, 0x40 | (pid >> 8), pid & 255, key ? 0x30 : 0x10]);
    if (key) b.set([1, 0x40], 4);
    b.set(payload, key ? 6 : 4);
    return b;
}
function section(bytes) {
    let crc = 0xffffffff;
    for (const byte of bytes) {
        crc ^= byte << 24;
        for (let i = 0; i < 8; i++) crc = (crc << 1) ^ (crc & 0x80000000 ? 0x04c11db7 : 0);
    }
    return [0, ...bytes, crc >>> 24, (crc >>> 16) & 255, (crc >>> 8) & 255, crc & 255];
}
function fixture(gop = 10) {
    const packets = [
        packet(0, section([0, 0xb0, 13, 0, 1, 0xc1, 0, 0, 0, 1, 0xf0, 0])),
        packet(4096, section([2, 0xb0, 18, 0, 1, 0xc1, 0, 0, 0xe1, 1, 0xf0, 0, 0x1b, 0xe1, 1, 0xf0, 0])),
    ];
    const keys = new Map();
    for (let i = 0; i < 6000; i++) {
        const n = BigInt((WRAP - 45000 + i * 9000) % WRAP);
        const pts = [0x21 | Number((n >> 29n) & 14n), Number((n >> 22n) & 255n),
            Number((n >> 14n) & 254n) | 1, Number((n >> 7n) & 255n), Number((n << 1n) & 254n) | 1];
        if (i % gop === 0) keys.set(i / 10, packets.length * PACKET);
        packets.push(packet(257, [0, 0, 1, 0xe0, 0, 0, 0x80, 0x80, 5, ...pts], i % gop === 0));
        // Deliberately variable bitrate to exercise interpolation refinement.
        for (let j = 0; j < (i < 2000 ? 1 : i < 4000 ? 6 : 2); j++) {
            const padding = new Uint8Array(PACKET).fill(255);
            padding.set([0x47, 0x1f, 0xff, 0x10]); packets.push(padding);
        }
    }
    const data = new Uint8Array(packets.length * PACKET);
    packets.forEach((p, i) => data.set(p, i * PACKET));
    return {data, keys};
}
function harness(data) {
    const elements = new Map(), requests = [], events = new Map(), players = [];
    const video = new EventTarget();
    Object.assign(video, {paused:true, currentTime:0, duration:Infinity, buffered:{length:0}, plays:0,
        play() { this.paused = false; this.plays++; this.dispatchEvent(new Event('play')); return Promise.resolve(); },
        pause() { this.paused = true; this.dispatchEvent(new Event('pause')); }, canPlayType() { return ''; }});
    for (const id of ['recording-player', 'recording-play', 'recording-seek', 'recording-time',
                      'recording-status', 'recording-fullscreen', 'recording-controls']) {
        elements.set(id, {dataset:{url:'/file?path=test.ts', error:'ERROR', loading:'LOADING',
            unavailable:'UNAVAILABLE', seekError:'SEEK ERROR', play:'Play', pause:'Pause'}, addEventListener() {}, value:0});
    }
    elements.set('recording-video', video);
    const mpegts = {BaseLoader:class {destroy() {}}, LoaderStatus:{kBuffering:1, kComplete:2, kError:3},
        LoaderErrors:{EXCEPTION:1}, Events:{ERROR:'error'}, isSupported:() => false,
        createPlayer:() => {
            const callbacks = new Map();
            const p = {callbacks, destroyed:0, on:(name, f) => callbacks.set(name, f), attachMediaElement() {},
                load() {}, unload() {}, detachMediaElement() {}, destroy() {this.destroyed++;}};
            players.push(p); return p;
        }};
    const window = {mpegts, addEventListener:(name, f) => events.set(name, f), location:{reload() {window.reloaded = true;}}};
    const ctx = vm.createContext({document:{getElementById:id => elements.get(id)}, window, mpegts,
        console:{error() {}, warn() {}}, DOMException, AbortController, Uint8Array, setTimeout,
        fetch:async (url, options) => {
            if (ctx.respond) return ctx.respond(url, options);
            if (options.signal?.aborted) throw new DOMException('aborted', 'AbortError');
            if (options.method === 'HEAD') return {ok:true, headers:new Map([['Content-Length', String(data.length)]])};
            const match = /^bytes=(\d+)-(\d+)$/.exec(options.headers.Range);
            assert(match, 'every GET must request a bounded range');
            const start = Number(match[1]), end = Number(match[2]);
            requests.push([start, end]);
            return {status:206, headers:new Map([['Content-Range', `bytes ${start}-${end}/${ctx.api?.file.size || data.length}`],
                ['Content-Length', String(end - start + 1)]]), arrayBuffer:async () => data.slice(start, end + 1).buffer};
        }});
    let source = fs.readFileSync(process.argv[2], 'utf8');
    const end = '\n})();';
    assert.equal(source.split(end).length, 2, 'one adapter closure for test instrumentation');
    source = source.replace(end, '\n globalThis.api={file,read,scan,initialise,locate,load,loaderFor,finishSeek,failure,close,' +
        'state:()=>({pendingTime,pendingPlay,seeking,closed})};' + end);
    vm.runInContext(source, ctx);
    return {ctx, api:ctx.api, video, elements, requests, events, window, players};
}

(async () => {
    const f = fixture(), h = harness(f.data), {api, video, elements} = h;
    await api.initialise(new AbortController().signal);
    assert.equal(api.file.pid, 257);
    assert(Math.abs(api.file.duration - 600) < .001, 'PTS wrap preserves the complete timeline');
    assert(h.requests.reduce((n, [a,b]) => n + b-a+1, 0) < f.data.length / 4, 'bounded initial probes');
    for (const target of [480.37, 120.37, 599.8, .37]) {
        const p = await api.locate(target, new AbortController().signal);
        assert(p.key && p.seconds <= target && target - p.seconds < 3);
        assert.equal(p.offset, f.keys.get(p.seconds), 'actual keyframe selected at known byte position');
    }
    const cancelled = new AbortController(); cancelled.abort();
    api.file.samples = api.file.samples.filter(p => p.seconds < 1 || p.seconds > 590);
    await assert.rejects(api.locate(310.37, cancelled.signal), {name:'AbortError'});
    api.scan(f.data, 0);
    assert(api.file.samples.length <= 4096);
    assert.equal(api.file.samples.filter(p => p.key).length, 600, 'thinning retains available keyframes');
    const limited = harness(f.data); await limited.api.initialise(new AbortController().signal);
    limited.api.file.samples = [limited.api.file.samples[0], limited.api.file.samples.at(-1)];
    let probed = 0;
    limited.ctx.respond = async (url, options) => {
        const [a,b] = options.headers.Range.slice(6).split('-').map(Number); probed += b-a+1;
        const bytes = new Uint8Array(b-a+1);
        if (bytes.length >= PACKET) bytes.set(f.data.subarray(PACKET*2, PACKET*3));
        return {status:206, headers:new Map([['Content-Range', `bytes ${a}-${b}/${f.data.length}`],
            ['Content-Length', String(b-a+1)]]), arrayBuffer:async () => bytes.buffer};
    };
    await assert.rejects(limited.api.locate(310.37, new AbortController().signal), /probe budget/);
    assert(probed <= f.data.length, 'aggregate seek reads are bounded, including ineffective probes');

    const longGop = fixture(50), small = harness(longGop.data);
    await small.api.initialise(new AbortController().signal);
    const longSeek = await small.api.locate(314.37, new AbortController().signal);
    assert.equal(longSeek.seconds, 310, 'small recording with long GOP reaches backwards keyframe scan');
    assert.equal(longSeek.offset, longGop.keys.get(310));

    await api.load(250.37, true);
    assert(api.state().seeking && api.state().pendingPlay);
    const seek = elements.get('recording-seek');
    seek.value = 300.37; seek.onchange();
    assert(api.state().pendingPlay, 'overlapping seek retains playing intent while old video is paused');
    await new Promise(r => setTimeout(r, 0));
    const play = elements.get('recording-play');
    play.onclick(); assert(!api.state().pendingPlay);
    play.onclick(); assert(api.state().pendingPlay);
    assert.equal(video.plays, 0, 'Play during lookup waits for requested frame');
    video.buffered = {length:1, start:() => 0, end:() => 10};
    api.finishSeek();
    assert(!api.state().seeking && !video.paused && video.plays === 1);
    assert(Math.abs(video.currentTime - .37) < .001);

    elements.get('recording-player').requestFullscreen = () => Promise.reject(new Error('Denied'));
    elements.get('recording-fullscreen').onclick();
    await new Promise(r => setTimeout(r, 0));
    assert(!video.paused && !play.disabled, 'fullscreen denial preserves playback');
    api.failure(new Error('decoder failed')); api.finishSeek();
    assert(video.paused && play.disabled && video.plays === 1, 'terminal failure cannot resume a pending seek');
    h.events.get('pagehide')(); h.events.get('pageshow')({persisted:true});
    assert(h.window.reloaded, 'back-forward cache restore reopens the recording');

    const recover = harness(f.data); await recover.api.initialise(new AbortController().signal);
    await recover.api.load(0, false);
    recover.video.buffered = {length:1, start:() => 0, end:() => 2}; recover.api.finishSeek();
    recover.api.file.samples = [recover.api.file.samples[0], recover.api.file.samples.at(-1)];
    recover.ctx.respond = async () => {throw new Error('Probe failed');};
    await recover.api.load(314.37, true);
    assert(!recover.api.state().seeking && !recover.video.paused && !recover.elements.get('recording-play').disabled,
        'failed seek retains the previous playable window and playback intent');
    assert.equal(recover.players.length, 1, 'failed probe never recreates the retained player');
    assert.equal(recover.players[0].destroyed, 0);
    assert.equal(recover.elements.get('recording-status').textContent, 'SEEK ERROR');
    const nearby = recover.elements.get('recording-seek'); nearby.value = .5; nearby.onchange();
    assert.equal(recover.elements.get('recording-status').textContent, '', 'successful in-window seek clears the warning');
    recover.players[0].unload = () => {throw new Error('unload failed');};
    recover.players[0].callbacks.get('error')('IOError', 'failed', {});
    assert(recover.video.paused && recover.elements.get('recording-play').disabled,
        'retained player errors are still handled after recovery');
    assert.equal(recover.elements.get('recording-status').textContent, 'ERROR');
    assert.equal(recover.players[0].destroyed, 1);
    recover.api.close();
    assert.equal(recover.players[0].destroyed, 1, 'destroy remains idempotent');

    const broken = harness(f.data); let cancelledBody = false, readBody = false;
    broken.api.file.size = f.data.length;
    broken.ctx.respond = async () => ({status:200, headers:new Map(), body:{cancel:async () => {cancelledBody=true;}},
        arrayBuffer:async () => {readBody=true;}});
    await assert.rejects(broken.api.read(0, 188), /bounded byte range/);
    assert(cancelledBody && !readBody, 'ignored Range is cancelled before downloading its body');

    const stalled = harness(f.data); stalled.api.file.size = 128 * 1024 * 1024;
    let fetched = 0;
    stalled.ctx.respond = async (url, options) => {
        const [a,b] = options.headers.Range.slice(6).split('-').map(Number); fetched += b-a+1;
        return {status:206, headers:new Map([['Content-Range', `bytes ${a}-${b}/${stalled.api.file.size}`],
            ['Content-Length', String(b-a+1)]]), arrayBuffer:async () => new ArrayBuffer(b-a+1)};
    };
    const Loader = stalled.api.loaderFor({offset:0, seconds:0}), loader = new Loader();
    await new Promise((resolve, reject) => {
        loader._onDataArrival = () => {};
        loader._onError = (type, info) => {assert.match(info.msg, /read budget/); resolve();};
        loader._onComplete = () => reject(new Error('Undecodable stream downloaded completely'));
        loader.open({}, {from:0, to:-1});
    });
    assert(fetched < 34 * 1024 * 1024, 'no buffered progress cannot fetch the entire file');
    loader.destroy();
    console.log('PASS TS program/PTS wrap/VBR keyframe lookup, bounded reads, cancellation, pending play, fullscreen, history and decoder budget');
})().catch(e => {console.error(e); process.exitCode=1;});
