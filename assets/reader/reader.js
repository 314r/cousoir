// Cousoir's reader, run in each page of a book it shows: the page laid out
// in columns a screen wide, turned by the keys, clicks and the wheel, and
// positions found and gone back to as Calibre's viewer writes them.
//
// The CFI code is ported from src/pyj/read_book/cfi.pyj of Calibre 9.16,
// Copyright (C) 2016 Kovid Goyal, GPL v3: encode(), decode() and their
// helpers, its highlight wrappers (data-calibre-range-wrapper) skipped as it
// skips them.
//
// It tells the window what happens by console messages, "cousoir:" and JSON:
// { event: "position", cfi, fraction, page, pages }, { event: "next" } and
// { event: "previous" } at the ends of the page, { event: "key", key },
// { event: "selection", empty }, { event: "highlight", uuid } when one is
// clicked, and { event: "layout" } once laid out again for a new size.
(function () {
    'use strict';
    if (window.cousoir)
        return;

    const XHTML = 'http://www.w3.org/1999/xhtml';
    const settings = { margin: 0.06, background: '#ffffff', color: '#1d1d1f', link: '#2f6db5' };
    let styleElement = null;
    let lastCfi = '';

    function post(message) {
        console.log('cousoir:' + JSON.stringify(message));
    }

    // --- CFI, after Calibre's cfi.pyj -------------------------------------

    const escapePattern = /[\[\],^();~@!]/g;
    const unescapePattern = /[\^](.)/g;

    function escapeForCfi(raw) {
        return (raw || '').replace(escapePattern, '^$&');
    }

    function unescapeFromCfi(raw) {
        return (raw || '').replace(unescapePattern, '$1');
    }

    function idIsUnique(id) {
        try {
            const found = document.querySelectorAll('[id=' + window.CSS.escape(id) + ']');
            return found && found.length < 2;
        } catch (e) {
            return false;
        }
    }

    function isText(node) {
        return node.nodeType === Node.TEXT_NODE || node.nodeType === Node.CDATA_SECTION_NODE;
    }

    function isWrapper(node) {
        return node && node.nodeType === Node.ELEMENT_NODE && node.dataset && node.dataset.calibreRangeWrapper;
    }

    function textLengthInWrapper(node) {
        let length = 0;
        for (let p = node.firstChild; p; p = p.nextSibling) {
            if (isText(p))
                length += p.nodeValue.length;
            else if (isWrapper(p))
                length += textLengthInWrapper(p);
        }
        return length;
    }

    function adjustNodeForTextOffset(node) {
        if (node.parentNode && isWrapper(node.parentNode))
            node = node.parentNode;
        let offset = 0;
        let adjusted = false;
        for (;;) {
            const p = node.previousSibling;
            if (!p || p.nodeType > Node.COMMENT_NODE)
                break;
            if (isText(p))
                offset += p.nodeValue.length;
            else if (isWrapper(p))
                offset += textLengthInWrapper(p);
            node = p;
            adjusted = true;
        }
        if (adjusted && isWrapper(node)) {
            if (!node.firstChild || node.firstChild.nodeType !== Node.TEXT_NODE)
                node.insertBefore(document.createTextNode(''), node.firstChild || null);
            node = node.firstChild;
        }
        return [node, offset, adjusted];
    }

    function unwrappedNodes(wrapper) {
        let nodes = [];
        for (const child of wrapper.childNodes) {
            if (isWrapper(child))
                nodes = nodes.concat(unwrappedNodes(child));
            else
                nodes.push(child);
        }
        return nodes;
    }

    function incrementIndexForChild(child, index, sentinel) {
        if (isWrapper(child))
            return incrementIndexForChildren(unwrappedNodes(child), index, sentinel);
        index |= 1;
        if (child.nodeType === Node.ELEMENT_NODE)
            index += 1;
        return index;
    }

    function incrementIndexForChildren(children, index, sentinel) {
        for (let i = 0; i < children.length; ++i) {
            const child = children[i];
            index = incrementIndexForChild(child, index, sentinel);
            if (child === sentinel)
                break;
        }
        return index;
    }

    function nonWrapperParent(node) {
        let p = node.parentNode;
        let child = node;
        while (p && isWrapper(p)) {
            child = p;
            p = p.parentNode;
        }
        return [p, child];
    }

    function encode(doc, node, offset) {
        let cfi = '';
        if (node.nodeType === Node.ELEMENT_NODE) {
            if (typeof offset === 'number') {
                const q = node.childNodes.item(offset);
                if (q && q.nodeType === Node.ELEMENT_NODE) {
                    node = q;
                    if (isWrapper(node)) {
                        if (!node.firstChild)
                            node.appendChild(document.createTextNode(''));
                        node = node.firstChild;
                    }
                }
            } else if (isWrapper(node)) {
                if (!node.firstChild)
                    node.appendChild(document.createTextNode(''));
                node = node.firstChild;
            }
        }
        if (isText(node)) {
            offset = offset || 0;
            const [adjustedNode, additional, adjusted] = adjustNodeForTextOffset(node);
            if (adjusted) {
                node = adjustedNode;
                offset += additional;
            }
            cfi = ':' + offset + cfi;
        }
        let first = true;
        while (node !== doc) {
            const [p, sentinel] = nonWrapperParent(node);
            if (!p)
                break;
            let index = incrementIndexForChildren(p.childNodes, 0, sentinel);
            if (first) {
                first = false;
                if (isWrapper(node))
                    index -= 1;
            }
            const id = node.id;
            const idspec = id && idIsUnique(id) ? '[' + escapeForCfi(id) + ']' : '';
            cfi = '/' + index + idspec + cfi;
            node = p;
        }
        return cfi;
    }

    function nodeAtIndex(nodes, target, index, textNodes) {
        for (let i = 0; i < nodes.length; ++i) {
            const node = nodes[i];
            const element = node.nodeType === Node.ELEMENT_NODE;
            if (element && isWrapper(node)) {
                const [q, next] = nodeAtIndex(unwrappedNodes(node), target, index, textNodes);
                if (q)
                    return [q, next];
                index = next;
                continue;
            }
            if ((textNodes && !isText(node)) || (!textNodes && !element))
                continue;
            if (index === target)
                return [node, index];
            index += 1;
        }
        return [null, index];
    }

    function nodeForPathStep(parent, target, assertion) {
        if (assertion) {
            const q = document.getElementById(assertion);
            if (q && idIsUnique(assertion))
                return q;
        }
        const element = target % 2 === 0;
        target = Math.floor(target / 2);
        if (element && target > 0)
            target -= 1;
        return nodeAtIndex(parent.childNodes, target, 0, !element)[0];
    }

    function nodeForTextOffset(nodes, offset, firstNode) {
        let lastText = null;
        let seenFirst = false;
        for (let i = 0; i < nodes.length; ++i) {
            const node = nodes[i];
            if (!seenFirst) {
                if (!firstNode || node.isSameNode(firstNode))
                    seenFirst = true;
                else
                    continue;
            }
            if (isText(node)) {
                const length = node.nodeValue.length;
                if (offset <= length)
                    return [node, offset, true];
                lastText = node;
                offset -= length;
            } else if (isWrapper(node)) {
                const [q, rest, ok] = nodeForTextOffset(unwrappedNodes(node), offset);
                if (ok)
                    return [q, rest, true];
                offset = rest;
            }
        }
        return [lastText, offset, false];
    }

    function decode(cfi, doc) {
        doc = doc || document;
        let node = doc;
        const step = /^\/(\d+)(\[[^\]]*\])?/;
        while (cfi.length > 0) {
            const r = cfi.match(step);
            if (!r)
                break;
            const target = parseInt(r[1], 10);
            let assertion = r[2];
            if (assertion)
                assertion = unescapeFromCfi(assertion.slice(1, assertion.length - 1));
            const q = nodeForPathStep(node, target, assertion);
            if (q)
                node = q;
            else if (target)
                return null;
            cfi = cfi.substr(r[0].length);
        }
        const decoded = {};
        const r = cfi.match(/^:(\d+)/);
        if (r) {
            let offset = parseInt(r[1], 10);
            if (node.parentNode && isWrapper(node.parentNode))
                node = node.parentNode;
            const [q, rest] = nodeForTextOffset(node.parentNode.childNodes, offset, node);
            if (q)
                node = q;
            decoded.offset = rest;
        }
        decoded.node = node;
        return decoded;
    }

    // --- Highlights, wrapped as Calibre's viewer wraps them -----------------

    let wrapperCount = 0;

    // A Range of two CFIs within the page, or null.
    function rangeOf(start, end) {
        const a = decode(start, document);
        const b = decode(end, document);
        if (!a || !b || !a.node || !b.node)
            return null;
        try {
            const range = document.createRange();
            range.setStart(a.node, a.offset !== undefined ? a.offset : 0);
            range.setEnd(b.node, b.offset !== undefined ? b.offset : 0);
            return range.collapsed ? null : range;
        } catch (e) {
            return null;
        }
    }

    function wrapRange(range, uuid, color) {
        const root = range.commonAncestorContainer.nodeType === Node.ELEMENT_NODE ? range.commonAncestorContainer
                                                                                 : range.commonAncestorContainer.parentNode;
        const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
        const nodes = [];
        for (let node = walker.nextNode(); node; node = walker.nextNode()) {
            if (range.intersectsNode(node) && node.nodeValue.length)
                nodes.push(node);
        }
        const count = ++wrapperCount;
        for (let node of nodes) {
            const start = node === range.startContainer ? range.startOffset : 0;
            const end = node === range.endContainer ? range.endOffset : node.nodeValue.length;
            if (start >= end)
                continue;
            if (end < node.nodeValue.length)
                node.splitText(end);
            if (start > 0)
                node = node.splitText(start);
            const span = document.createElementNS(XHTML, 'span');
            span.dataset.calibreRangeWrapper = String(count);
            span.dataset.annotId = uuid;
            span.style.backgroundColor = color;
            node.parentNode.insertBefore(span, node);
            span.appendChild(node);
        }
    }

    // Wraps the highlights of this page: { uuid, start, end, color }. Their
    // ranges are all found first, then wrapped, so that a wrapper never
    // changes the text a later CFI points to.
    function applyHighlights(list) {
        const found = [];
        for (const highlight of list || []) {
            const range = rangeOf(highlight.start, highlight.end);
            if (range)
                found.push([range, highlight]);
        }
        for (const [range, highlight] of found)
            wrapRange(range, highlight.uuid, highlight.color);
        return found.length;
    }

    function removeHighlight(uuid) {
        for (const span of document.querySelectorAll('span[data-annot-id="' + uuid.replace(/"/g, '') + '"]')) {
            const parent = span.parentNode;
            while (span.firstChild)
                parent.insertBefore(span.firstChild, span);
            parent.removeChild(span);
            parent.normalize();
        }
    }

    // The selection as Calibre's cfi_for_selection() gives it, and its text.
    function selection() {
        const selected = window.getSelection();
        if (!selected || selected.isCollapsed || !selected.rangeCount)
            return null;
        const range = selected.getRangeAt(0);
        const position = (container, offset) => {
            if (container.nodeType === Node.ELEMENT_NODE && offset && container.childNodes.length > offset)
                return [container.childNodes[offset], 0];
            return [container, offset];
        };
        const [startNode, startOffset] = position(range.startContainer, range.startOffset);
        const [endNode, endOffset] = position(range.endContainer, range.endOffset);
        return {
            start: encode(document, startNode, startOffset),
            end: encode(document, endNode, endOffset),
            text: selected.toString(),
        };
    }

    // --- Pages ------------------------------------------------------------

    function scroller() {
        return document.scrollingElement || document.documentElement;
    }

    function pageWidth() {
        return window.innerWidth;
    }

    function pages() {
        return Math.max(1, Math.round(scroller().scrollWidth / pageWidth()));
    }

    function page() {
        return Math.round(scroller().scrollLeft / pageWidth());
    }

    function layout() {
        const width = window.innerWidth;
        const height = window.innerHeight;
        const side = Math.max(16, Math.round(width * settings.margin));
        const top = Math.max(16, Math.round(height * 0.05));
        if (!styleElement) {
            styleElement = document.createElementNS(XHTML, 'style');
            (document.head || document.documentElement).appendChild(styleElement);
        }
        styleElement.textContent = `
            html {
                width: ${width}px !important; height: ${height}px !important;
                margin: 0 !important; padding: 0 !important; overflow: hidden !important;
                background: ${settings.background} !important; color: ${settings.color} !important;
            }
            body {
                box-sizing: content-box !important; height: ${height - 2 * top}px !important;
                width: auto !important; max-width: none !important; min-width: 0 !important;
                margin: ${top}px ${side}px !important; padding: 0 !important;
                column-width: ${width - 2 * side}px !important; column-gap: ${2 * side}px !important;
                column-fill: auto !important; overflow: visible !important;
                background: transparent !important; color: ${settings.color} !important;
                overflow-wrap: break-word;
            }
            body * { color: inherit !important; background-color: transparent !important; }
            a, a * { color: ${settings.link} !important; }
            img, svg, video { max-width: 100% !important; max-height: ${height - 2 * top}px !important; object-fit: contain; }
            ::selection { background: ${settings.link}; color: ${settings.background}; }
        `;
    }

    function show(n) {
        scroller().scrollLeft = Math.max(0, Math.min(n, pages() - 1)) * pageWidth();
        report();
    }

    // The first visible character, as a CFI: scanned from the top left of
    // the page as Calibre's at_current() scans.
    function currentCfi() {
        const width = window.innerWidth;
        const height = window.innerHeight;
        for (let y = 2; y < height; y += 16) {
            for (let x = 2; x < width; x += 16) {
                const range = document.caretRangeFromPoint ? document.caretRangeFromPoint(x, y) : null;
                if (!range)
                    continue;
                const node = range.startContainer;
                if (isText(node) && node.nodeValue.trim().length) {
                    const rects = range.getClientRects();
                    const rect = rects.length ? rects[0] : null;
                    if (!rect || (rect.right > 0 && rect.left < width))
                        return encode(document, node, range.startOffset);
                }
                const element = document.elementFromPoint(x, y);
                if (element && element !== document.documentElement && element !== document.body
                    && (element.localName === 'img' || element.localName === 'svg' || element.localName === 'image'))
                    return encode(document, element);
            }
        }
        return encode(document, document.body || document.documentElement);
    }

    function report() {
        lastCfi = currentCfi();
        const count = pages();
        post({ event: 'position', cfi: lastCfi, fraction: count > 1 ? page() / count : 0, page: page(), pages: count });
    }

    // The page a node, at a character, shows on.
    function pageOf(node, offset) {
        let rect = null;
        if (isText(node)) {
            const range = document.createRange();
            const at = Math.min(offset || 0, node.nodeValue.length);
            range.setStart(node, at);
            range.setEnd(node, Math.min(at + 1, node.nodeValue.length));
            const rects = range.getClientRects();
            rect = rects.length ? rects[0] : range.getBoundingClientRect();
        } else if (node.getBoundingClientRect) {
            rect = node.getBoundingClientRect();
        }
        if (!rect)
            return 0;
        return Math.floor((rect.left + scroller().scrollLeft) / pageWidth());
    }

    function goToCfi(cfi) {
        // "epubcfi(/6/2/4/1:3)": the spine's step aside.
        const inner = cfi.replace(/^epubcfi\(\/\d+/, '').replace(/\)$/, '');
        const decoded = inner ? decode(inner, document) : null;
        if (!decoded || !decoded.node || decoded.node === document) {
            show(0);
            return false;
        }
        show(pageOf(decoded.node, decoded.offset));
        return true;
    }

    function goToFragment(fragment) {
        const target = fragment ? document.getElementById(fragment) : null;
        show(target ? pageOf(target) : 0);
    }

    function next() {
        if (page() < pages() - 1)
            show(page() + 1);
        else
            post({ event: 'next' });
    }

    function previous() {
        if (page() > 0)
            show(page() - 1);
        else
            post({ event: 'previous' });
    }

    document.addEventListener('keydown', event => {
        const key = event.key;
        if (event.ctrlKey || event.altKey || event.metaKey) {
            post({ event: 'key', key: key, ctrl: event.ctrlKey, shift: event.shiftKey });
            return;
        }
        if (key === 'ArrowRight' || key === 'ArrowDown' || key === 'PageDown' || (key === ' ' && !event.shiftKey))
            next();
        else if (key === 'ArrowLeft' || key === 'ArrowUp' || key === 'PageUp' || (key === ' ' && event.shiftKey))
            previous();
        else if (key === 'Home')
            show(0);
        else if (key === 'End')
            show(pages() - 1);
        else {
            post({ event: 'key', key: key, ctrl: false, shift: event.shiftKey });
            return;
        }
        event.preventDefault();
    }, true);

    document.addEventListener('click', event => {
        if (event.target.closest && event.target.closest('a[href]'))
            return;
        const wrapper = event.target.closest ? event.target.closest('span[data-annot-id]') : null;
        if (wrapper && window.getSelection().isCollapsed) {
            post({ event: 'highlight', uuid: wrapper.dataset.annotId });
            return;
        }
        if (!window.getSelection().isCollapsed)
            return;
        const x = event.clientX;
        if (x < window.innerWidth / 3)
            previous();
        else if (x > window.innerWidth * 2 / 3)
            next();
        else
            post({ event: 'menu' });
    });

    let wheelLock = 0;
    document.addEventListener('wheel', event => {
        event.preventDefault();
        const now = Date.now();
        if (now < wheelLock || Math.abs(event.deltaY) < 1)
            return;
        wheelLock = now + 250;
        if (event.deltaY > 0)
            next();
        else
            previous();
    }, { passive: false });

    let selectionTimer = 0;
    document.addEventListener('selectionchange', () => {
        clearTimeout(selectionTimer);
        selectionTimer = setTimeout(() => post({ event: 'selection', empty: window.getSelection().isCollapsed }), 150);
    });

    window.addEventListener('resize', () => {
        const cfi = lastCfi;
        layout();
        goToCfi('epubcfi(/2' + cfi + ')');
        post({ event: 'layout' });
    });

    window.cousoir = {
        setup(options) {
            Object.assign(settings, options || {});
            layout();
            return true;
        },
        goToStart() { show(0); },
        goToEnd() { show(pages() - 1); },
        goToCfi: goToCfi,
        goToFragment: goToFragment,
        // The page of each of these ids, -1 for one the page lacks.
        pagesOf(ids) {
            const pagesById = {};
            for (const id of ids || []) {
                const target = document.getElementById(id);
                pagesById[id] = target ? pageOf(target) : -1;
            }
            return pagesById;
        },
        next: next,
        previous: previous,
        currentCfi: currentCfi,
        applyHighlights: applyHighlights,
        removeHighlight: removeHighlight,
        selection: selection,
        clearSelection() { window.getSelection().removeAllRanges(); },
        encode: encode,
        decode: decode,
        page: page,
        pages: pages,
    };
})();
