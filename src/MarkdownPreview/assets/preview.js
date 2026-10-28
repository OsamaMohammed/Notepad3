import mermaid from './vendor/mermaid/mermaid.esm.min.mjs';

const root = document.getElementById('document');
let generation = '';
let renderSequence = 0;
let previousPayload;

function send(kind, fields = {}) {
    window.chrome.webview.postMessage({ version: 1, generation, kind, ...fields });
}

export function sourceHitAt(x, y) {
    const target = document.elementFromPoint(x, y);
    const visual = target?.closest('img, .mermaid-block, .missing-image');
    if (visual) {
        return { blockId: Number(visual.closest('[data-block]')?.dataset.block) };
    }
    let node, offset;
    if (document.caretPositionFromPoint) {
        const caret = document.caretPositionFromPoint(x, y);
        node = caret?.offsetNode;
        offset = caret?.offset;
    } else {
        const caret = document.caretRangeFromPoint(x, y);
        node = caret?.startContainer;
        offset = caret?.startOffset;
    }
    const element = node?.nodeType === Node.TEXT_NODE ? node.parentElement : node;
    const run = element?.closest?.('[data-run]');
    if (run && node) {
        const prefix = document.createRange();
        prefix.setStart(run, 0);
        prefix.setEnd(node, offset);
        return { runId: Number(run.dataset.run), utf16Offset: prefix.toString().length };
    }
    const block = target?.closest('[data-block]');
    return block ? { blockId: Number(block.dataset.block) } : null;
}

export async function renderPreview(payload) {
    const sequence = ++renderSequence;
    previousPayload = payload;
    generation = payload.generation;
    document.documentElement.classList.toggle('dark', Boolean(payload.dark));
    // HTML comes only from the native Markdown renderer, which escapes source HTML.
    root.innerHTML = payload.html;
    const usedIds = new Map();
    for (const heading of root.querySelectorAll('h1,h2,h3,h4,h5,h6')) {
        const slug = heading.textContent.trim().toLowerCase().replace(/[^\p{L}\p{N}_\-\s]/gu, '').replace(/\s+/g, '-');
        const count = usedIds.get(slug) || 0;
        usedIds.set(slug, count + 1);
        heading.id = count ? `${slug}-${count}` : slug;
    }
    for (const image of root.querySelectorAll('img')) {
        image.addEventListener('error', () => {
            const fallback = document.createElement('span');
            fallback.className = 'missing-image';
            fallback.dataset.block = image.dataset.block;
            fallback.textContent = image.alt ? `Image unavailable: ${image.alt}` : 'Image unavailable';
            image.replaceWith(fallback);
        }, { once: true });
    }
    mermaid.initialize({
        startOnLoad: false,
        securityLevel: 'strict',
        theme: payload.dark ? 'dark' : 'default',
        maxTextSize: 50000,
        maxEdges: 500,
        htmlLabels: false,
        flowchart: { htmlLabels: false },
        suppressErrorRendering: true,
        secure: ['secure', 'securityLevel', 'startOnLoad', 'maxTextSize', 'maxEdges',
            'theme', 'themeVariables', 'themeCSS', 'fontFamily', 'htmlLabels', 'flowchart']
    });
    for (const diagram of payload.diagrams) {
        if (sequence !== renderSequence) return;
        const container = root.querySelector(`[data-diagram="${diagram.id}"]`);
        if (!container) continue;
        try {
            if (diagram.code.length > 50000 || /^\s*---/.test(diagram.code) || /(^|\n)\s*%%\s*\{/.test(diagram.code)) {
                throw new Error('Diagram is too large or contains a configuration override.');
            }
            const result = await mermaid.render(`diagram-${sequence}-${diagram.id}`, diagram.code);
            if (sequence !== renderSequence) return;
            container.innerHTML = result.svg;
        } catch (error) {
            const message = document.createElement('p');
            message.className = 'render-error';
            message.textContent = `Mermaid: ${error.message || 'Unable to render diagram'}`;
            const code = document.createElement('pre');
            code.textContent = diagram.code;
            container.replaceChildren(message, code);
        }
    }
    if (sequence === renderSequence) send('ready');
}

root.addEventListener('dblclick', event => {
    event.preventDefault();
    const hit = sourceHitAt(event.clientX, event.clientY);
    if (hit && Object.values(hit).every(Number.isSafeInteger)) send('source', hit);
});
root.addEventListener('click', event => {
    const link = event.target.closest('a');
    if (!link) return;
    event.preventDefault();
    const href = link.getAttribute('href');
    if (href?.startsWith('#')) {
        try { document.getElementById(decodeURIComponent(href.slice(1)))?.scrollIntoView(); } catch { /* Invalid fragment. */ }
    }
});
window.chrome.webview.addEventListener('message', event => {
    const payload = event.data;
    if (payload.kind === 'theme' && previousPayload) {
        renderPreview({ ...previousPayload, dark: payload.dark }).catch(() => send('error'));
    } else if (payload.kind === 'render') {
        renderPreview(payload).catch(() => send('error'));
    }
});
send('loaded');
