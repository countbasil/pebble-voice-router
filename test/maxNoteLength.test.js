// Tests for the continuous-dictation max note length setting and the
// per-platform note buffer sizes it relies on.
// Run: node test/maxNoteLength.test.js

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

section('watch mirroring');
check('MAX_NOTE_MIN is a declared message key',
      packageJson.pebble.messageKeys.indexOf('MAX_NOTE_MIN') >= 0, true);
check('defaults to one minute', context.watchMirroredSettings({}).MAX_NOTE_MIN, 1);
check('saved choice is sent', context.watchMirroredSettings({ max_note_min: 10 }).MAX_NOTE_MIN, 10);
check('string choice is parsed', context.watchMirroredSettings({ max_note_min: '5' }).MAX_NOTE_MIN, 5);
check('unknown choice falls back to one minute',
      context.watchMirroredSettings({ max_note_min: 99 }).MAX_NOTE_MIN, 1);

sentToWatch = [];
listeners.webviewclosed({ response: encodeURIComponent(JSON.stringify({ max_note_min: 15 })) });
check('saving settings sends it to the watch', sentToWatch[sentToWatch.length - 1].MAX_NOTE_MIN, 15);

section('settings page');
listeners.showConfiguration();
var html = decodeURIComponent(openedUrl.replace(/^data:text\/html;charset=utf-8,/, ''));
check('picker shows the saved choice',
      /<select id="max_note_min">[\s\S]*?<option value="15" selected>/.test(html), true);
check('page saves the picker',
      html.indexOf('max_note_min:parseInt(document.getElementById("max_note_min").value,10)') >= 0, true);

section('watch side');
var clamp = watchSource.match(/max_note_min_clamp\(int v\) \{\s*return \(v >= (\d+) && v <= (\d+)\)/);
check('watch accepts every choice',
      context.MAX_NOTE_MIN_CHOICES.every(function(n) { return n >= +clamp[1] && n <= +clamp[2]; }), true);
check('watch default matches the phone default',
      +watchSource.match(/#define MAX_NOTE_MIN_DEFAULT\s+(\d+)/)[1], context.maxNoteMinutes({}));
check('chaining stops at the configured length',
      /s_cfg_max_note_min \* 60000/.test(watchSource), true);

section('history holds a full note');
// HISTORY_FULL_CHUNKS × 200 must cover NOTE_BUF_SIZE on the big-RAM platforms,
// so a long note's history entry isn't cut short.
var big = watchSource.match(/#if defined\(PBL_PLATFORM_EMERY\)[\s\S]*?#else/)[0];
var noteBig = +big.match(/#define NOTE_BUF_SIZE\s+(\d+)/)[1];
var chunksBig = +big.match(/#define HISTORY_FULL_CHUNKS\s+(\d+)/)[1];
check('emery/gabbro history fits a full note', chunksBig * 200 >= noteBig, true);
check('history full text is heap-allocated',
      /static char\s+\(\*s_history_full\)\[HISTORY_FULL_LEN\];/.test(watchSource), true);

process.stdout.write('\n' + passed + ' passed, ' + failed + ' failed\n');
if (failed) process.exit(1);
