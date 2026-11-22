// Execute the real frontend's capability-response branch against a provider reply.
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const server = process.argv[2];
const source = fs.readFileSync(path.join(server, 'tmpl/simple/devVideo.js.tmpl'), 'utf8');
const marker = '} else if( message.type == "CFAgentCapabilities" ) {';
const start = source.indexOf(marker);
assert.ok(start >= 0, 'capability consumer not found');
const end = source.indexOf('} else if( message.type == "BuildInformation" ) {', start);
assert.ok(end > start, 'capability consumer boundary not found');
const handler = new Function('message', 'cf', 'CFInput', 'let capabilities;\n' + source.slice(start + marker.length, end));
const message = JSON.parse(fs.readFileSync(0, 'utf8'));
const expectedTextReady = Number(process.argv[3] || 0);
let applied = 0, reported = 0;
const cf = {inputOptions:{}, wallBridge:{reportInputState(){reported++;}}, logNotice(){}};
const input = {apply(cf){
    assert.equal(cf.tapTextInitialized, expectedTextReady);
    assert.equal(cf.tapTextCapable, expectedTextReady);
    applied++;
}};
handler({...message, tag:{silent:1}}, cf, input);
handler({...message, tag:{}}, cf, input);
assert.equal(applied, 2);
assert.equal(reported, 2);
console.log('PASS real frontend capability handler: silent and visible replies');
