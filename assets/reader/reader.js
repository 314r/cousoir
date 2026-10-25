// Cousoir's reader, run in each page of a book it shows: the page laid out
// in columns, one or two a screen, or as a scroll, in the type the reader
// picks; turned by the keys, clicks and the wheel; and positions found and
// gone back to as Calibre's viewer writes them.
//
// The CFI code is ported from src/pyj/read_book/cfi.pyj of Calibre 9.16,
// Copyright (C) 2016 Kovid Goyal, GPL v3: encode(), decode() and their
// helpers, its highlight wrappers (data-calibre-range-wrapper) skipped as it
// skips them.
//
// It tells the window what happens by console messages, "cousoir:" and JSON:
// { event: "position", cfi, fraction, page, pages, layout } (layout "1" or
// "2", the columns a screen, or "scroll"), { event: "next" } and
// { event: "previous" } at the ends of the page, { event: "key", key, ctrl,
// alt, shift }, { event: "menu" } for a click in the middle,
// { event: "selection", empty }, { event: "highlight", uuid } when one is
// clicked, and { event: "layout" } once laid out again for a new size.
(function () {
    'use strict';
    if (window.cousoir)
        return;

    const XHTML = 'http://www.w3.org/1999/xhtml';
    // What the window asks for, by setup(): the page's colours, its margin
    // in pixels, how many columns a screen holds, a scroll rather than
    // pages, and the type: a font family, a line height and an alignment
    // ("start", "justify"), empty for the book's own.
    const settings = {
        margin: 40, background: '#ffffff', color: '#1d1d1f', link: '#2f6db5', columns: 1, scroll: false,
        fontFamily: '', lineHeight: 0, align: '',
    };
    let styleElement = null;
    let lastCfi = '';

    // Each message says which page it is from: a page being left may still
    // tell something.
    function post(message) {
        message.page_url = location.href.split('#')[0];
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
            // Over the page's own transparent backgrounds, as Calibre's
            // highlight_style_as_css() makes it important.
            span.style.setProperty('background-color', color, 'important');
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

    // --- A search's places, marked without changing the page ---------------

    // The places of a search on this page, { start, end }, the one at
    // `current` marked more; by the CSS Custom Highlight API, which adds no
    // node to the page, so that no CFI changes.
    function markSearch(list, current) {
        if (!window.CSS || !CSS.highlights)
            return 0;
        const others = new Highlight();
        const shown = new Highlight();
        let count = 0;
        (list || []).forEach((place, i) => {
            const range = rangeOf(place.start, place.end);
            if (!range)
                return;
            (i === current ? shown : others).add(range);
            ++count;
        });
        CSS.highlights.set('cousoir-search', others);
        CSS.highlights.set('cousoir-search-shown', shown);
        return count;
    }

    // --- Pages ------------------------------------------------------------

    function scroller() {
        return document.scrollingElement || document.documentElement;
    }

    // Pages go across, a screen at a time; in a scroll, down.
    function scrolled() {
        return !!settings.scroll;
    }

    function pageWidth() {
        return window.innerWidth;
    }

    // What a page is: a screen's width, or in a scroll its height.
    function pageStep() {
        return scrolled() ? window.innerHeight : pageWidth();
    }

    // A right to left page: its columns go leftward, its scroll position from
    // 0 down.
    function rtl() {
        return !scrolled() && getComputedStyle(document.body || document.documentElement).direction === 'rtl';
    }

    // A scroll made here, not by the reader: what it shows is told, but the
    // place kept through layouts stays.
    let ownScroll = -1;

    function setScrollTop(top) {
        scroller().scrollTop = top;
        ownScroll = scroller().scrollTop;
    }

    function atScrollEnd() {
        const s = scroller();
        return s.scrollTop + window.innerHeight >= s.scrollHeight - 2;
    }

    function pages() {
        const s = scroller();
        // The last column of a spread may stand alone: a page all the same.
        const extent = scrolled() ? s.scrollHeight : s.scrollWidth;
        return Math.max(1, Math.ceil((extent - 2) / pageStep()));
    }

    function page() {
        if (scrolled())
            return atScrollEnd() ? pages() - 1 : Math.floor((scroller().scrollTop + 1) / pageStep());
        return Math.round(Math.abs(scroller().scrollLeft) / pageWidth());
    }

    // The paragraphs the book aligns to the start or justifies, which the
    // reader's alignment then sets; those centred or to the end stay so.
    let marked = false;

    function markParagraphs() {
        marked = true;
        if (!document.body)
            return;
        for (const element of document.body.querySelectorAll('p, li, dd, dt, blockquote, div')) {
            const align = getComputedStyle(element).textAlign;
            if (align === 'start' || align === 'left' || align === 'justify' || align === '-webkit-auto')
                element.dataset.cousoirAlign = '';
        }
    }

    function typeRules() {
        let rules = '';
        if (settings.fontFamily) {
            rules += `body, body * { font-family: ${settings.fontFamily} !important; }
                      body code, body pre, body kbd, body samp, body tt, body code *, body pre * {
                          font-family: monospace !important; }`;
        }
        if (settings.lineHeight) {
            rules += `body, body p, body div, body li, body blockquote, body dd, body dt, body td, body th {
                          line-height: ${settings.lineHeight} !important; }`;
        }
        if (settings.align === 'justify')
            rules += '[data-cousoir-align] { text-align: justify !important; }';
        else if (settings.align === 'start')
            rules += '[data-cousoir-align] { text-align: start !important; }';
        return rules;
    }

    function layout() {
        const width = window.innerWidth;
        const height = window.innerHeight;
        const side = Math.max(12, Math.round(settings.margin || 0));
        const top = Math.max(16, Math.round(height * 0.04));
        const columns = scrolled() ? 1 : Math.max(1, settings.columns || 1);
        if (!styleElement) {
            styleElement = document.createElementNS(XHTML, 'style');
            (document.head || document.documentElement).appendChild(styleElement);
        }
        // A screen holds `columns` columns: each its width and the gap after
        // it, the margins of the body making the gap at the screen's edges.
        const page = scrolled()
            ? `html {
                   width: ${width}px !important; height: auto !important;
                   overflow-x: hidden !important; overflow-y: auto !important; scrollbar-width: none !important;
               }
               body {
                   margin: ${top}px ${side}px !important;
                   width: auto !important; max-width: none !important; min-width: 0 !important; height: auto !important;
                   columns: auto !important;
               }`
            : `html { width: ${width}px !important; height: ${height}px !important; overflow: hidden !important; }
               body {
                   height: ${height - 2 * top}px !important;
                   width: auto !important; max-width: none !important; min-width: 0 !important;
                   margin: ${top}px ${side}px !important;
                   column-width: ${Math.floor(width / columns) - 2 * side}px !important;
                   column-gap: ${2 * side}px !important; column-fill: auto !important;
               }`;
        styleElement.textContent = `
            ${page}
            html {
                margin: 0 !important; padding: 0 !important;
                background: ${settings.background} !important; color: ${settings.color} !important;
            }
            html::-webkit-scrollbar { display: none; }
            body {
                box-sizing: content-box !important; padding: 0 !important; overflow: visible !important;
                background: transparent !important; color: ${settings.color} !important;
                overflow-wrap: break-word;
            }
            body * { color: inherit !important; background-color: transparent !important; }
            a, a * { color: ${settings.link} !important; }
            img, svg, video { max-width: 100% !important; max-height: ${height - 2 * top}px !important; object-fit: contain; }
            ::selection { background: ${settings.link}; color: ${settings.background}; }
            ::highlight(cousoir-search) { background-color: color-mix(in srgb, ${settings.link} 30%, transparent); }
            ::highlight(cousoir-search-shown) { background-color: ${settings.link}; color: ${settings.background}; }
            ${typeRules()}
        `;
    }

    function show(n, keepAnchor) {
        if (scrolled()) {
            setScrollTop(Math.max(0, n) * pageStep());
        } else {
            const at = Math.max(0, Math.min(n, pages() - 1)) * pageWidth();
            scroller().scrollLeft = rtl() ? -at : at;
        }
        report(keepAnchor);
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
                    // In a scroll, the first line whole in sight.
                    if (!rect || (rect.right > 0 && rect.left < width && rect.top < height
                                  && (scrolled() ? rect.top >= -1 : rect.bottom > 0)))
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

    // Where the reader is, gone back to when the page is laid out again: the
    // place gone to, or the start of the page turned to, and not the start
    // of the page a new layout puts it on, which would creep back a little
    // with every one.
    let anchor = '';

    function report(keepAnchor) {
        lastCfi = currentCfi();
        if (!keepAnchor)
            anchor = lastCfi;
        const count = pages();
        const fraction = scrolled() ? scroller().scrollTop / Math.max(1, scroller().scrollHeight)
                                    : count > 1 ? page() / count : 0;
        post({ event: 'position', cfi: lastCfi, fraction: fraction, page: page(), pages: count,
               layout: scrolled() ? 'scroll' : String(Math.max(1, settings.columns || 1)) });
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
        if (scrolled())
            return Math.floor((rect.top + scroller().scrollTop) / pageStep());
        // Where it is with the page at its start.
        const x = rect.left + scroller().scrollLeft;
        return rtl() ? Math.floor((pageWidth() - 1 - x) / pageWidth()) : Math.floor(x / pageWidth());
    }

    // In a scroll, the place itself at the top of the window.
    function scrollTo(node, offset) {
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
        setScrollTop(rect ? Math.max(0, scroller().scrollTop + rect.top - 4) : 0);
    }

    function goToCfi(cfi) {
        // "epubcfi(/6/2/4/1:3)": the spine's step aside.
        const inner = cfi.replace(/^epubcfi\(\/\d+/, '').replace(/\)$/, '');
        const decoded = inner ? decode(inner, document) : null;
        if (!decoded || !decoded.node || decoded.node === document) {
            show(0);
            return false;
        }
        if (scrolled()) {
            scrollTo(decoded.node, decoded.offset);
            report(true);
        } else {
            show(pageOf(decoded.node, decoded.offset), true);
        }
        anchor = inner;
        return true;
    }

    function goToFragment(fragment) {
        const target = fragment ? document.getElementById(fragment) : null;
        if (target && scrolled()) {
            scrollTo(target);
            report();
            return;
        }
        show(target ? pageOf(target) : 0);
    }

    function goToEnd() {
        if (scrolled()) {
            setScrollTop(scroller().scrollHeight);
            report();
        } else {
            show(pages() - 1);
        }
    }

    // How far into the page's file, from 0 to 1.
    function goToFraction(fraction) {
        fraction = Math.max(0, Math.min(1, fraction || 0));
        if (scrolled()) {
            setScrollTop(fraction * scroller().scrollHeight);
            report();
        } else {
            show(Math.min(pages() - 1, Math.floor(fraction * pages())));
        }
    }

    // A scroll goes a window down, but for a line or two kept in sight.
    function scrollBy(screens) {
        const style = getComputedStyle(document.body || document.documentElement);
        // "normal" is about 1.2 times the font's size.
        const line = parseFloat(style.lineHeight) || 1.2 * parseFloat(style.fontSize) || 20;
        const step = Math.max(40, window.innerHeight - 2 * line);
        setScrollTop(scroller().scrollTop + screens * step);
        report();
    }

    function next() {
        if (scrolled()) {
            if (atScrollEnd())
                post({ event: 'next' });
            else
                scrollBy(1);
        } else if (page() < pages() - 1) {
            show(page() + 1);
        } else {
            post({ event: 'next' });
        }
    }

    function previous() {
        if (scrolled()) {
            if (scroller().scrollTop <= 1)
                post({ event: 'previous' });
            else
                scrollBy(-1);
        } else if (page() > 0) {
            show(page() - 1);
        } else {
            post({ event: 'previous' });
        }
    }

    // A few lines down or up in a scroll, on to the next file at its end.
    function lines(count) {
        if (count > 0 && atScrollEnd())
            post({ event: 'next' });
        else if (count < 0 && scroller().scrollTop <= 1)
            post({ event: 'previous' });
        else {
            setScrollTop(scroller().scrollTop + count * 40);
            report();
        }
    }

    document.addEventListener('keydown', event => {
        const key = event.key;
        if (event.ctrlKey || event.altKey || event.metaKey) {
            post({ event: 'key', key: key, ctrl: event.ctrlKey, alt: event.altKey, shift: event.shiftKey });
            return;
        }
        if (scrolled() && (key === 'ArrowDown' || key === 'ArrowUp'))
            lines(key === 'ArrowDown' ? 1 : -1);
        else if (key === 'ArrowRight' || key === 'ArrowDown' || key === 'PageDown' || (key === ' ' && !event.shiftKey))
            next();
        else if (key === 'ArrowLeft' || key === 'ArrowUp' || key === 'PageUp' || (key === ' ' && event.shiftKey))
            previous();
        else if (key === 'Home')
            show(0);
        else if (key === 'End')
            goToEnd();
        else {
            post({ event: 'key', key: key, ctrl: false, alt: false, shift: event.shiftKey });
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

    // The wheel turns a page at a time; in a scroll it scrolls, and goes on
    // to the next file or back to the one before once the end was reached a
    // moment before.
    let wheelLock = 0;
    let endReached = 0;
    document.addEventListener('wheel', event => {
        const now = Date.now();
        if (scrolled()) {
            const atEnd = event.deltaY > 0 && atScrollEnd();
            const atStart = event.deltaY < 0 && scroller().scrollTop <= 1;
            if (!atEnd && !atStart) {
                endReached = 0;
                return;
            }
            event.preventDefault();
            if (!endReached)
                endReached = now;
            if (now - endReached > 400 && now >= wheelLock) {
                wheelLock = now + 600;
                endReached = 0;
                post({ event: atEnd ? 'next' : 'previous' });
            }
            return;
        }
        event.preventDefault();
        if (now < wheelLock || Math.abs(event.deltaY) < 1)
            return;
        wheelLock = now + 250;
        if (event.deltaY > 0)
            next();
        else
            previous();
    }, { passive: false });

    let scrollTimer = 0;
    window.addEventListener('scroll', () => {
        if (!scrolled() || Math.abs(scroller().scrollTop - ownScroll) < 1)
            return;
        clearTimeout(scrollTimer);
        scrollTimer = setTimeout(() => report(), 120);
    });

    let selectionTimer = 0;
    document.addEventListener('selectionchange', () => {
        clearTimeout(selectionTimer);
        selectionTimer = setTimeout(() => post({ event: 'selection', empty: window.getSelection().isCollapsed }), 150);
    });

    // Laid out again, and back where the reader was.
    function relayout() {
        layout();
        if (anchor)
            goToCfi('epubcfi(/2' + anchor + ')');
    }

    window.addEventListener('resize', () => {
        relayout();
        post({ event: 'layout' });
    });

    window.cousoir = {
        // The page's colours, margin, columns, scroll and type, from the
        // window; again when the reader changes them, the place kept.
        setup(options) {
            Object.assign(settings, options || {});
            if (!marked)
                markParagraphs();
            relayout();
            return true;
        },
        goToStart() { show(0); },
        goToEnd: goToEnd,
        goToCfi: goToCfi,
        goToFragment: goToFragment,
        goToFraction: goToFraction,
        next: next,
        previous: previous,
        // The page of each of these ids, -1 for one the page lacks.
        pagesOf(ids) {
            const pagesById = {};
            for (const id of ids || []) {
                const target = document.getElementById(id);
                pagesById[id] = target ? pageOf(target) : -1;
            }
            return pagesById;
        },
        // The page of each of these CFIs within the page, -1 for one it
        // lacks.
        pagesOfCfis(cfis) {
            return (cfis || []).map(cfi => {
                const decoded = decode(cfi, document);
                return decoded && decoded.node && decoded.node !== document ? pageOf(decoded.node, decoded.offset) : -1;
            });
        },
        applyHighlights: applyHighlights,
        markSearch: markSearch,
        removeHighlight: removeHighlight,
        selection: selection,
        clearSelection() { window.getSelection().removeAllRanges(); },
        // The CFI of a place, and the text between two CFIs: what Cousoir
        // works out from a page's XML is checked against them.
        encode(node, offset) { return encode(document, node, offset); },
        rangeText(start, end) {
            const range = rangeOf(start, end);
            return range ? range.toString() : null;
        },
    };
})();
