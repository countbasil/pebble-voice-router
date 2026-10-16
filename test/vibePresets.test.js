// Tests for the mic start/stop vibration presets on the settings page.
// Run: node test/vibePresets.test.js

var fs = require('fs');
var path = require('path');
var vm = require('vm');

var pkjsPath = path.join(__dirname, '..', 'src', 'pkjs', 'index.js');
var watchPath = path.join(__dirname, '..', 'src', 'c', 'main.c');
var packagePath = path.join(__dirname, '..', 'package.json');
var pkjsSource = fs.readFileSync(pkjsPath, 'utf8');
var watchSource = fs.readFileSync(watchPath, 'utf8');
var packageJson = JSON.parse(fs.readFileSync(packagePath, 'utf8'));

var listeners = {};
var storage = {};
var sentToWatch = [];
var openedUrl = '';

var context = {
    console: { log: function() {} },
    setTimeout: function(callback) { callback(); },
    localStorage: {
        getItem: function(key) {
            return Object.prototype.hasOwnProperty.call(storage, key) ? storage[key] : null;
        },
        setItem: function(key, value) { storage[key] = String(value); },
        removeItem: function(key) { delete storage[key]; }
    },
    Pebble: {
        addEventListener: function(name, handler) { listeners[name] = handler; },
        sendAppMessage: function(message, onSuccess) {
            sentToWatch.push(message);
            if (onSuccess) onSuccess();
        },
        openURL: function(url) { openedUrl = url; }
    }
};

vm.createContext(context);
vm.runInContext(pkjsSource, context, { filename: pkjsPath });

var passed = 0;
var failed = 0;

function check(label, got, expected) {
    if (got === expected) {
        passed++;
        process.stdout.write('  ✓ ' + label + '\n');
    } else {
        failed++;
        process.stdout.write('  ✗ ' + label + '\n      got: ' + got + '\n expected: ' + expected + '\n');
    }
}

function section(name) {
    process.stdout.write('\n' + name + '\n');
}

function saveSettings(cfg) {
    sentToWatch = [];
    listeners.webviewclosed({ response: encodeURIComponent(JSON.stringify(cfg)) });
    return sentToWatch[sentToWatch.length - 1];
}

section('watch mirroring');
var mirrored = context.watchMirroredSettings({});
check('start defaults to 3 pulses', mirrored.VIBE_START, 2);
check('stop defaults to the long buzz', mirrored.VIBE_STOP, 2);
check('unknown preset falls back to the default',
      context.watchMirroredSettings({ vibe_start: 'bogus' }).VIBE_START, 2);
check('off is index 0', context.watchMirroredSettings({ vibe_stop: 'off' }).VIBE_STOP, 0);
['VIBE_START', 'VIBE_STOP', 'VIBE_PREVIEW'].forEach(function(key) {
    check(key + ' is a declared message key', packageJson.pebble.messageKeys.indexOf(key) >= 0, true);
});

section('preview on save');
var msg = saveSettings({ vibe_start: 'long', vibe_stop: 'long' });
check('changed start is sent', msg.VIBE_START, 3);
check('changed start asks the watch to play it', msg.VIBE_PREVIEW, 1);
msg = saveSettings({ vibe_start: 'long', vibe_stop: 'long', skip_confirm: true });
check('unchanged vibes do not play', msg.VIBE_PREVIEW, undefined);
msg = saveSettings({ vibe_start: 'long', vibe_stop: 'off' });
check('changed stop asks the watch to play it', msg.VIBE_PREVIEW, 1);

section('settings page');
listeners.showConfiguration();
var html = decodeURIComponent(openedUrl.replace(/^data:text\/html;charset=utf-8,/, ''));
check('start picker shows the saved preset',
      /<select id="vibe_start">[\s\S]*?<option value="long" selected>/.test(html), true);
check('stop picker shows the saved preset',
      /<select id="vibe_stop">[\s\S]*?<option value="off" selected>/.test(html), true);
check('page saves both pickers',
      html.indexOf('vibe_start:document.getElementById("vibe_start").value') >= 0 &&
      html.indexOf('vibe_stop:document.getElementById("vibe_stop").value') >= 0, true);
check('page declares UTF-8 so labels like "3 × 100ms" render',
      openedUrl.indexOf('data:text/html;charset=utf-8,') === 0 && html.indexOf('<meta charset="utf-8">') >= 0, true);
check('page points to the System vibe intensity setting',
      html.indexOf('Sounds &amp; Haptics &rarr; System') >= 0, true);

section('watch presets match the labels');
var presetCount = +watchSource.match(/#define VIBE_PRESETS\s+(\d+)/)[1];
check('start preset count', context.VIBE_START_PRESETS.length, presetCount);
check('stop preset count', context.VIBE_STOP_PRESETS.length, presetCount);
check('default index', context.VIBE_PRESET_DEFAULT,
      +watchSource.match(/#define VIBE_PRESET_DEFAULT\s+(\d+)/)[1]);

var startBody = watchSource.match(/s_vibe_start_ms\[VIBE_PRESETS\]\[2\] = \{[^\n]*\n([^\n]*)/)[1];
var startPulses = (startBody.match(/\{\s*(\d+),/g) || []).map(function(m) { return +m.match(/\d+/)[0]; });
context.VIBE_START_PRESETS.forEach(function(p, i) {
    var ms = p.id === 'off' ? 0 : +p.label.match(/(\d+)ms/)[1];
    check('start "' + p.label + '" is ' + ms + 'ms on the watch', startPulses[i], ms);
});
var stopValues = watchSource.match(/s_vibe_stop_ms\[VIBE_PRESETS\] = \{([^}]*)\}/)[1]
    .split(',').map(function(v) { return +v.trim(); });
context.VIBE_STOP_PRESETS.forEach(function(p, i) {
    var ms = p.id === 'off' ? 0 : Math.round(parseFloat(p.label.match(/\(([\d.]+)s\)/)[1]) * 1000);
    check('stop "' + p.label + '" is ' + ms + 'ms on the watch', stopValues[i], ms);
});

process.stdout.write('\n' + passed + ' passed, ' + failed + ' failed\n');
if (failed) process.exit(1);
