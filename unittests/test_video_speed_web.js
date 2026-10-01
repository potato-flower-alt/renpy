/* Run with node unittests/test_video_speed_web.js. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');

const node = () => ({connect() {}, disconnect() {}, gain: {
    value: 1, cancelScheduledValues() {}, setValueAtTime() {},
}});
const sandbox = {
    HTMLVideoElement: class {},
    AudioContext: class {
        createStereoPanner() { return node(); }
        createGain() { return node(); }
        createMediaElementSource() { return node(); }
    },
    URL,
    window: {location: 'https://example.test/'},
    console: {debug() {}},
    document: {
        body: {appendChild() {}},
        createElement() { return {style: {}, addEventListener() {}, currentTime: 0, play: () => Promise.resolve()}; },
    },
};
vm.createContext(sandbox);
vm.runInContext(fs.readFileSync(path.join(__dirname, '../renpy/common/_audio.js'), 'utf8'), sandbox);
sandbox.renpyAudio.getFilter = () => null;
const run = code => vm.runInContext(code, sandbox);
run('get_channel(0).video = true');
run('renpyAudio.queue(0, "url:first.mp4", "first", false, 0, false, 0, -1, 1, null, 2)');
assert.equal(run('get_channel(0).video_el.playbackRate'), 2);
assert.equal(run('get_channel(0).video_el.defaultPlaybackRate'), 2);
assert.equal(run('get_channel(0).video_el.preservesPitch'), true);
run('renpyAudio.queue(0, "url:second.mp4", "second", false, 0, false, 0, -1, 1, null, 0.5)');
assert.equal(run('get_channel(0).video_el.playbackRate'), 2);
run('on_video_end(get_channel(0))');
assert.equal(run('get_channel(0).video_el.playbackRate'), 0.5);
run('renpyAudio.queue(0, "url:third.mp4", "third", false, 0, false, 0, -1, 1, null)');
run('on_video_end(get_channel(0))');
assert.equal(run('get_channel(0).video_el.playbackRate'), 1);
console.log('Web video speed, pitch, queue, and default-rate checks passed.');
