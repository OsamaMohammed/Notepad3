import { spawn } from 'node:child_process';
import assert from 'node:assert/strict';
import { setTimeout as delay } from 'node:timers/promises';
import net from 'node:net';

const [exe, dll] = process.argv.slice(2);
const portServer = net.createServer();
await new Promise(resolve => portServer.listen(0, '127.0.0.1', resolve));
const port = portServer.address().port;
await new Promise(resolve => portServer.close(resolve));
const child = spawn(exe, [dll, '--serve'], {
    windowsHide: true,
    env: { ...process.env, WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS: `--remote-debugging-port=${port}` }
});
let output = '';
child.stdout.on('data', data => { output += data; });
child.stderr.on('data', data => { output += data; });
let exitCode;
child.on('exit', code => { exitCode = code; });
child.on('error', error => { output += error.message; exitCode = 1; });
let socket;
let checks = 0;
function check(value, name) { ++checks; assert.ok(value, name); }
try {
    const deadline = Date.now() + 40000;
    while (!output.includes('READY') && exitCode === undefined && Date.now() < deadline) await delay(100);
    check(output.includes('READY'), `preview ready: ${output}`);
    const pages = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
    const page = pages.find(item => item.url.startsWith('https://np3-preview.invalid/'));
    check(page, 'preview debugging target');
    socket = new WebSocket(page.webSocketDebuggerUrl);
    await new Promise((resolve, reject) => { socket.addEventListener('open', resolve, {once:true}); socket.addEventListener('error', reject, {once:true}); });
    let nextId = 0;
    const pending = new Map();
    socket.addEventListener('message', event => {
        const response = JSON.parse(event.data);
        if (pending.has(response.id)) {
            const {resolve,reject} = pending.get(response.id); pending.delete(response.id);
            response.error ? reject(new Error(JSON.stringify(response.error))) : resolve(response.result);
        }
    });
    function call(method, params) {
        const id = ++nextId;
        return new Promise((resolve,reject) => { pending.set(id,{resolve,reject}); socket.send(JSON.stringify({id,method,params})); });
    }
    async function evaluate(expression) {
        const response = await call('Runtime.evaluate', {expression,returnByValue:true,awaitPromise:true});
        if (response.exceptionDetails) throw new Error(JSON.stringify(response.exceptionDetails));
        return response.result.value;
    }
    const rendered = await evaluate(`({
        table: document.querySelectorAll('table td').length,
        checked: document.querySelector('input[type=checkbox]')?.checked,
        disabled: document.querySelector('input[type=checkbox]')?.disabled,
        svg: Boolean(document.querySelector('.mermaid-block svg')),
        invalid: document.querySelectorAll('.render-error').length,
        raw: document.body.textContent.includes('<script>window.np3SourceExecutedSentinel=true</script>'),
        injected: Boolean(window.np3SourceExecutedSentinel), text: document.body.textContent.slice(-160)
    })`);
    check(rendered.table === 2, 'table cells rendered');
    check(rendered.checked && rendered.disabled, 'task list read-only');
    check(rendered.svg, 'valid Mermaid SVG');
    check(rendered.invalid === 2, 'invalid Mermaid and configuration override rejected');
    check(rendered.raw && !rendered.injected, `raw HTML stays visible and inert: ${JSON.stringify(rendered)}`);
    const hit = await evaluate(`(async () => {
        const {sourceHitAt} = await import('/preview.js');
        const words = [...document.querySelectorAll('[data-run]')].filter(node => node.textContent === 'same');
        const box = words[1].getBoundingClientRect();
        return sourceHitAt(box.left + 1, box.top + box.height / 2);
    })()`);
    check(Number.isInteger(hit.runId) && hit.utf16Offset === 0, 'DOM hit testing locates second repeated word');
    await evaluate(`window.chrome.webview.postMessage({version:1,generation:'999999',kind:'source',runId:0,utf16Offset:0});
        window.chrome.webview.postMessage({version:1,generation:'42',kind:'source',runId:-1,utf16Offset:0});
        window.chrome.webview.postMessage({version:1,generation:'42',kind:'source',runId:999999,utf16Offset:0}); true`);
    await delay(100);
    check(!output.includes('SOURCE'), 'invalid and stale source messages ignored');
    await evaluate(`(() => {
        const words = [...document.querySelectorAll('[data-run]')].filter(node => node.textContent === 'same');
        const box = words[1].getBoundingClientRect();
        words[1].dispatchEvent(new MouseEvent('dblclick',{bubbles:true,clientX:box.left+1,clientY:box.top+box.height/2}));
        return true;
    })()`);
    const end = Date.now() + 10000;
    while (exitCode === undefined && Date.now() < end) await delay(50);
    check(output.includes('SOURCE 22'), `double-click returned exact UTF-8 source offset: ${output}`);
    check(exitCode === 0, `native callback lifecycle checks: ${output}`);
    console.log(`PASS: ${checks} browser DOM assertions`);
} finally {
    socket?.close();
    if (exitCode === undefined) child.kill();
}
